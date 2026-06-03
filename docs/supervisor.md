# Supervisor Module — CoCo_ESP32 OSD

## Overview

The Supervisor is an **On-Screen Display (OSD) overlay** that pauses emulation and provides disk management, machine reset, and settings via a menu UI. It uses a **blue theme** (bright blue-white on dark blue).

The OSD is rendered into the FabGL VGA framebuffer through the **OSD canvas** (`src/hal/osd_canvas.{h,cpp}`). It implements the drawing primitives the supervisor uses (`fillRect`, `drawRect`, `drawFast{H,V}Line`, `setTextFont`, `setTextColor`, `setTextDatum`, `drawString`, `textWidth`, the public `textcolor`/`textbgcolor` members, plus `startWrite`/`endWrite` no-ops) on top of FabGL's `Canvas`. RGB565 color constants (`OSD_BLACK`, `OSD_WHITE`, etc.) are bit-expanded to RGB888 when forwarded to the canvas.

> **Coordinate-space caveat:** the supervisor layout was sized for 320×240. The VGA32 framebuffer is 640×200. The OSD currently sits in the upper-left region of the surface — a future revision could scale or relocate it to better fill 640×200.

**Activation:** F3 toggles the overlay on/off. While active, `supervisor_update_and_render()` returns `true`, telling the main loop to skip emulation. (F1/F2 are reserved for the CoCo 3 keyboard matrix — see `keyboard-hal.md`.)

**Machine selection:** The main menu's "Machine:" row reflects the runtime-active machine (`g_machine_type`, not the compile-time `MACHINE_NAME`). ENTER opens the `SV_MACHINE_SELECT` submenu (CoCo 2 / CoCo 3, `(current)` marker on the active one). Picking the other machine opens a confirm dialog; accepting calls `supervisor_set_machine_type()` which saves supervisor state, persists the choice in NVS (`"sv"` namespace, `"machine_type"` key), and calls `esp_restart()`. On boot, `supervisor_load_machine_type()` returns the NVS value or the compile-time `MACHINE_TYPE`. See `runtime-machine-switch.md` for the complete flow.

---

## File Map

| File | Purpose |
|---|---|
| `supervisor.h/.cpp` | Core lifecycle, state machine, public API, state persistence |
| `sv_menu.h/.cpp` | Main menu definition, actions, and key handling |
| `sv_filebrowser.h/.cpp` | SD card directory browser for mounting .DSK/.VDK images |
| `sv_disk.h/.cpp` | WD1793 FDC emulation, PSRAM disk caching, mount/eject/flush |
| `sv_render.h/.cpp` | Rendering primitives (frame, menu items, file entries, dialogs). Paints through `osd_canvas` into the FabGL framebuffer. |
| `sv_debug.h/.cpp` | Debug memory dump to serial (Motorola S-Record / Intel HEX) |

---

## State Machine

```
SV_INACTIVE ──F3──> SV_MAIN_MENU
                       │
   ┌──────────────┬────┼─────────────────┬────────────┬────────────────┐
   │              │    │                 │            │                │
SV_FILE_BROWSER  SV_MACHINE_SELECT  SV_SETTINGS  SV_ABOUT  SV_DEBUG_MENU
(Disk Manager)        │                (toggles)        │
   │                  v                           SV_DEBUG_DUMP
   │           SV_CONFIRM_DIALOG ──accept──> esp_restart()
   │                  │cancel                (new machine_type in NVS)
   │            SV_MACHINE_SELECT
   │
   └──> SV_CONFIRM_DIALOG (Reset Machine confirmation)
              │accept
         machine_reset()
```

States are defined in `supervisor.h` as `SV_State` enum:

| State | Description |
|---|---|
| `SV_INACTIVE` | OSD hidden, emulation running |
| `SV_MAIN_MENU` | Top-level menu with 6 items |
| `SV_FILE_BROWSER` | **Disk Manager**: 2-row drive strip (all 4 drives) + file browser below. Left/Right selects active drive; Enter mounts; U ejects; F flushes. Also used as the stand-alone file browser. |
| `SV_MACHINE_SELECT` | Machine type submenu (CoCo 2 / CoCo 3 with `(current)` marker) |
| `SV_SETTINGS` | Settings submenu: Debug Log / RS-232 Pak toggles (mutually exclusive; share UART0) |
| `SV_ABOUT` | Info screen (version, free heap) |
| `SV_DEBUG_MENU` | Debug submenu picker: CPU/GIME Status, Memory Hex Dump, RS-232 Pak |
| `SV_DEBUG_DUMP` | Active debug screen (dispatched from `SV_DEBUG_MENU`) |
| `SV_CONFIRM_DIALOG` | Yes/No dialog (used by Reset Machine and machine type change) |

**Transitions** are driven by `sv.state` assignment. `sv.prev_state` tracks where to return on ESC/cancel.

---

## Main Loop Integration

In the main loop (typically `loop()` in the .ino):

```
if (supervisor_update_and_render()) {
    return;  // skip emulation this frame
}
// ... run emulation ...
```

Key dispatch from the FabGL PS/2 keyboard backend. The keyboard HAL translates incoming VirtualKey events to USB HID usage IDs before calling `supervisor_on_key()` so the supervisor's dispatch table stays single-sourced — see `keyboard-hal.md` for the `vk_to_hid_usage()` mapping.

```
if (supervisor_is_active()) {
    supervisor_on_key(hid_usage, pressed);
} else if (hid_usage == 0x3C && pressed) {  // F3
    supervisor_toggle();
}
```

---

## Data Flow

### Supervisor_t Struct (supervisor.h)

Single global instance (`static Supervisor_t sv`). Contains:

- **state/prev_state** — current screen and return target
- **machine** — pointer to the `Machine` struct (CPU, PIA, FDC, RAM)
- **menu_cursor / menu_scroll_offset / menu_item_count** — shared cursor state reused by main menu and disk manager
- **current_path / file_cursor / file_scroll_offset / file_count / target_drive** — file browser state
- **file_entries** — heap-allocated array of `SV_FileEntry[128]` (allocated once on first browse)
- **confirm_message / confirm_callback / confirm_context** — generic confirmation dialog
- **needs_redraw** — dirty flag; rendering only happens when true

### Redraw Model

The OSD uses a **dirty-flag** redraw approach:
1. Key handlers set `sv.needs_redraw = true` on any visual change
2. `supervisor_update_and_render()` checks the flag, renders once, clears it
3. When no redraw is needed, a 16ms delay caps at ~60fps idle

---

## Module Details

### sv_menu.cpp — Main Menu

**Menu items** (static array `menu_items[]`):

| Index | Label | Action |
|---|---|---|
| 0 | Disk Manager | Opens unified Disk Manager (drive strip + file browser) |
| 1 | Machine: CoCo 3 | Opens machine-select submenu (CoCo 2 / CoCo 3) |
| 2 | Settings | Opens settings submenu (Debug Log / RS-232 Pak toggles) |
| 3 | Reset Machine | Confirm dialog → `sv_disk_flush_all()` → `machine_reset()` |
| 4 | Debug | Opens debug submenu picker (CPU/GIME Status, Hex Dump, RS-232 Pak) |
| 5 | About | Info screen (version, free heap) |

**Key handling:** Up/Down move cursor, Enter executes action, ESC/F1 close.

`execute_action()` is the dispatcher — modifies `sv.state` to transition screens. Reset uses the confirm dialog pattern with a lambda callback that flushes all dirty disk caches before resetting.

### sv_filebrowser.cpp — Disk Manager / File Browser

This module serves double duty as the **unified Disk Manager** (the `SV_FILE_BROWSER` state). The screen has two areas:

**Drive strip (top 2 rows):** shows all 4 drives in a 2×2 grid.
- Row 0: D0 (left half) / D2 (right half)
- Row 1: D1 (left half) / D3 (right half)
- Active drive is highlighted; mounted drives shown in cyan, empty in dim; a separator line runs below the strip.
- Left/Right arrows cycle the active drive without closing the screen.
- After a successful mount, the OSD **stays open** so the user can mount additional drives.

**File list (below the strip):** standard FAT32 directory browser.
- **scan:** `sv_fb_scan_directory()` reads directory entries into `SV_FileEntry[]` (max 128)
- **sort:** `..` always first, then directories (alphabetical), then supported files (.dsk/.vdk), then others
- **navigate:** Enter on dir = descend, Backspace = parent, PgUp/PgDn/Home/End for fast scrolling
- **mount:** Enter on a supported file = mount to `target_drive`, save state, stay on Disk Manager
- **eject:** U key = eject active drive (flushes dirty cache first), save state
- **flush:** F key = flush active drive's dirty cache to SD card immediately
- **SPI bus note:** the SD card is on a dedicated HSPI bus separate from the VGA output path; there is no SPI bus contention.

**Supported formats:** `.dsk` (JVC) and `.vdk` = fully supported. `.dmk` = recognized but not mountable.

Visible file window is 9 items (`SV_FB_VISIBLE_ITEMS`). Scrollbar rendered when list exceeds window.

`sv_render_drive_strip(cells, mounted, active)` in `sv_render.cpp` draws the 2-row strip using the standard OSD color palette.

### sv_disk.cpp — FDC Emulation

Full WD1793 command-level emulation mapped at `$FF40-$FF5F`:

**Address map:**
| Range | Read | Write |
|---|---|---|
| `$FF40-$FF47` | Returns 0 | DSKREG (drive select latch) |
| `$FF48` | Status | Command |
| `$FF49` | Track | Track |
| `$FF4A` | Sector | Sector |
| `$FF4B` | Data (read sector) | Data (write sector) |

**DSKREG ($FF40) bits** — matches XRoar `rsdos.c`:
- Bits 0-2: drive select (one-hot: 0x01=D0, 0x02=D1, 0x04=D2)
- Bit 3: motor on
- Bit 5: density (XOR'd with 0x20 — inverted so normal CoCo setting enables NMI)
- Bit 7: HALT enable (DRQ -> CPU HALT for byte synchronization)

**Commands implemented:**
| Cmd | Type | Notes |
|---|---|---|
| `$00` | Restore | Track=0, check mounted |
| `$10` | Seek | Track=Data register |
| `$20-$7F` | Step/StepIn/StepOut | Updates track if bit 4 set |
| `$80/$90` | Read Sector | Copies from PSRAM cache to sector_buf |
| `$A0/$B0` | Write Sector | Fills sector_buf, then copies to PSRAM cache |
| `$E0` | Read Track | Returns immediate success (not fully emulated) |
| `$F0` | Write Track (Format) | Parses raw format stream, extracts sector data, writes to PSRAM cache. Used by DSKINI |
| `$D0` | Force Interrupt | Bit 3 = immediate INTRQ |

**INTRQ/HALT/NMI flow** (critical for DSKCON compatibility):
1. Command completes (or sector transfer finishes) -> `set_intrq(true)`
2. INTRQ asserted -> HALT disabled, CPU released
3. NMI fires if `!density && intrq` (density gating after XOR)
4. INTRQ cleared on status read (`$FF48`) or new command write
5. Sector transfers use `intrq_defer = 1` — fires INTRQ one tick later so CPU can store the last byte
6. `sv_disk_tick()` must be called once per CPU instruction to handle deferred INTRQ

**DRQ/HALT flow** (byte-level sync):
- DRQ high -> release CPU halt
- DRQ low + halt_enable -> halt CPU (waits for next byte)

**PSRAM caching:**
- Entire disk image loaded into PSRAM at mount time (~161KB for standard 35T/18S)
- All sector reads/writes go to in-memory cache (zero SD latency during emulation)
- SD -> PSRAM uses 512-byte bounce buffer (ESP32 SPI DMA can't write directly to PSRAM)
- Dirty images flushed back to SD on eject (U), explicit F key in Disk Manager, auto-flush before machine reset (Reset Machine dialog), and auto-flush before machine type change (`esp_restart()`)
- Up to 4 drives (~644KB PSRAM total)

**Geometry detection** (`sv_disk_detect_geometry`):
- VDK files: 12-byte header
- JVC/DSK files: header = `file_size % 256`
- Track count derived from data size / (sectors_per_track * sector_size)
- >80 tracks assumed double-sided

### sv_debug.cpp — Debug Memory Dump

Dumps emulated CoCo memory to the serial port in **Motorola S-Record** or **Intel HEX** format. Reads memory via `machine_read()`, so the output reflects the live SAM/ROM mapping (exactly what the 6809 CPU sees).

**UI fields** (navigate with Up/Down):

| Field | Controls | Description |
|---|---|---|
| Format | Left/Right | Toggle between `S-Record` and `Intel HEX` |
| Start Address | 0-9, A-F keys; Left/Right move nibble cursor | 4-digit hex address ($0000-$FFFF) |
| End Address | Same as above | 4-digit hex address ($0000-$FFFF) |
| Send Dump | Enter | Executes the dump to serial |

**Key bindings in debug screen:**

| Key | Code | Action |
|---|---|---|
| Up/Down | `0x52/0x51` | Move between fields |
| Left/Right | `0x50/0x4F` | Toggle format / move hex cursor |
| 0-9, A-F | `0x27,0x1E-0x26,0x04-0x09` | Enter hex digit at cursor position |
| Enter | `0x28` | Execute dump (when on "Send Dump" field) |
| ESC | `0x29` | Return to main menu |

**Output format:** A header banner is printed before the hex data:

```
========================================
  CoCo ESP32 MEMORY DUMP
  Format : Motorola S-Record
  Range  : $3C00 - $3C0F (16 bytes)
========================================
S00B0000434F434F44554D5091
S1133C008634B7FF21B7FF01B7FF03B7FF237E62xx
S9033C00xx
========== END OF DUMP =================
```

**Record details:**
- S-Record: S0 header (module name "COCODUMP"), S1 data records (16 bytes/line), S9 end record with exec address
- Intel HEX: Data records (16 bytes/line), EOF record (`:00000001FF`)
- Both formats include proper checksums
- 16 bytes per record line (`DUMP_BYTES_PER_LINE`)
- Addresses that would wrap past $FFFF are handled safely

**State struct** (`SV_DebugState` in `sv_debug.h`):
- `active_field` — which UI field has focus
- `format` — `SV_DUMP_SREC` or `SV_DUMP_IHEX`
- `start_addr` / `end_addr` — 16-bit addresses (swapped automatically if reversed)
- `hex_digit` — nibble cursor position (0-3) within address field
- `dumping` — true during serial output (shows "Dumping..." on screen)

### sv_render.cpp — Rendering Engine

Renders using Font 2 (16 px) through `src/hal/osd_canvas.{h,cpp}` (which maps Font 2 → FabGL `FONT_8x14`) into the FabGL VGA framebuffer. Layout coordinates fit inside the 256×192 area `(32,24)-(288,216)` — see the coordinate-space caveat in the Overview.

**Layout constants:**
```
SV_BORDER_X=32  SV_BORDER_Y=24  (top-left of OSD area)
SV_BORDER_W=256  SV_BORDER_H=192
SV_TITLE_H=18   SV_ITEM_H=18   SV_FOOTER_H=16
SV_CONTENT_X = BORDER_X + 8
SV_CONTENT_Y = BORDER_Y + TITLE_H + 4
```

**Color palette** (RGB565) — blue theme:
| Constant | Color | Hex | Used for |
|---|---|---|---|
| `SV_COLOR_BG` | Dark blue | `0x0008` | Background |
| `SV_COLOR_TEXT` | Bright blue-white | `0x5DDF` | Normal text |
| `SV_COLOR_HIGHLIGHT` | Bright blue-white | `0x5DDF` | Selected item bg |
| `SV_COLOR_HL_TEXT` | Dark blue | `0x0008` | Selected item text |
| `SV_COLOR_DIM` | Dim blue | `0x3B3F` | Footer, scrollbar |
| `SV_COLOR_DIR` | Yellow | `0xFFE0` | Directory icon |
| `SV_COLOR_DISK` | Cyan | `0x07FF` | Disk image icon |
| `SV_COLOR_WARN` | Red | `0xF800` | Unsupported file icon |

**Rendering functions:**
- `sv_render_frame(title, footer)` — clears area, draws border, title bar (inverse), footer with separator line
- `sv_render_menu_item(index, label, value, highlighted)` — single row with `>` cursor, optional right-aligned value
- `sv_render_file_entry(index, name, size, is_dir, is_supported, highlighted)` — file row with icon (`+`=dir, `*`=disk, `!`=unsupported) and size
- `sv_render_scrollbar(start, visible, total)` — vertical scrollbar thumb
- `sv_render_confirm_dialog(message, yes_highlighted)` — centered Yes/No dialog box
- `sv_render_status_line(text, color)` — centered status at bottom of content area

---

## Persistence (ESP32 Preferences)

Namespace: `"sv"`. Stored in NVS flash.

| Key | Type | Description |
|---|---|---|
| `last_dir` | String | Last browsed directory path |
| `d0_path` .. `d3_path` | String | Mounted disk paths per drive |
| `auto_mount` | Bool | Auto-mount on boot (default true) |

- `supervisor_save_state()` — writes current dir + all mounted disk paths
- `supervisor_load_state()` — restores dir + auto-mounts disks on boot
- `supervisor_quick_mount_last_disk()` — mounts drive 0 from saved path (used at boot)

---

## NMI/HALT Wiring

Set up in `supervisor_init()`:

```cpp
m->fdc.nmi_callback = [](void* ctx) {
    mc6809_nmi(&((Machine*)ctx)->cpu);
};
m->fdc.halt_callback = [](void* ctx, bool halted) {
    ((Machine*)ctx)->cpu.halted = halted;
};
m->fdc.callback_context = m;
```

This connects the FDC's interrupt/halt signals to the MC6809 CPU through the Machine struct.

---

## Key Bindings (HID Usage Codes)

| Key | Code | Context |
|---|---|---|
| F3 | `0x3C` | Toggle OSD on/off (global) |
| Up | `0x52` | Move cursor up |
| Down | `0x51` | Move cursor down |
| Enter | `0x28` | Select / confirm; file browser: mount selected file to active drive |
| ESC | `0x29` | Back / cancel |
| Backspace | `0x2A` | Parent directory (file browser) |
| PgUp | `0x4B` | Page up (file browser) |
| PgDn | `0x4E` | Page down (file browser) |
| Home | `0x4A` | Jump to first (file browser) |
| End | `0x4D` | Jump to last (file browser) |
| Left | `0x50` | Disk Manager: select previous drive (0→3); Yes/No toggle (confirm dialog); move nibble cursor (debug dump) |
| Right | `0x4F` | Disk Manager: select next drive (0→3); Yes/No toggle (confirm dialog); toggle format / move nibble (debug dump) |
| U | `0x18` | Eject active drive (Disk Manager) |
| F | `0x09` | Flush active drive to SD card (Disk Manager) |
| F4 | `0x3D` | Soft reset — auto-flushes all dirty disks before reset |
| 0-9 | `0x27,0x1E-0x26` | Hex digit input (debug dump address fields) |
| A-F | `0x04-0x09` | Hex digit input (debug dump address fields) |

---

## Known Limitations & TODOs

- **No framebuffer snapshot:** the framebuffer is in PSRAM and could in principle be snapshotted, but a "repaint on next frame" strategy is used instead. On OSD close, the GIME border re-fill is continuous so any flash is minimal.
- **DMK format:** Recognized in file browser but not mountable (no DMK parser).
- **menu_cursor reuse:** Main menu and submenus (machine select, settings, debug menu) share `sv.menu_cursor`; `saved_main_menu_cursor` / `saved_main_menu_count` stash and restore the main menu position on ESC.
- **Max 128 file entries:** `SV_FB_MAX_ENTRIES = 128` — directories with more entries are truncated.
- **SD scan before render:** file browser scans the directory before rendering to avoid holding the HSPI bus open while painting the OSD.

---

## Adding a New Menu Screen (Checklist)

1. Add a new `SV_State` value in `supervisor.h`
2. Add a new `SV_MenuAction` in `sv_menu.h` and a menu item in `sv_menu.cpp`
3. Write `execute_action()` case to transition: set `prev_state`, change `state`, set `needs_redraw`
4. Write `your_screen_on_key()` handler — dispatch in `supervisor_on_key()` switch
5. Write `your_screen_render()` using `sv_render_frame()` + `sv_render_menu_item()` — dispatch in `supervisor_update_and_render()` switch
6. Handle ESC to return to `SV_MAIN_MENU` (restore cursor position if needed)
