# Supervisor Module — CoCo_ESP32 OSD

## Overview

The Supervisor is an **On-Screen Display (OSD) overlay** that pauses emulation and provides disk management, machine reset, and settings via a menu UI. It uses a **blue theme** (bright blue-white on dark blue).

The OSD is rendered into the FabGL VGA framebuffer through the **OSD canvas** (`src/hal/osd_canvas.{h,cpp}`). It implements the drawing primitives the supervisor uses (`fillRect`, `drawRect`, `drawFast{H,V}Line`, `setTextFont`, `setTextColor`, `setTextDatum`, `drawString`, `textWidth`, the public `textcolor`/`textbgcolor` members, plus `startWrite`/`endWrite` no-ops) on top of FabGL's `Canvas`. RGB565 color constants (`OSD_BLACK`, `OSD_WHITE`, etc.) are bit-expanded to RGB888 when forwarded to the canvas.

> **Coordinate-space caveat:** the supervisor layout was sized for 320×240. The VGA32 framebuffer is 640×200. The OSD currently sits in the upper-left region of the surface — a future revision could scale or relocate it to better fill 640×200.

**Activation:** F3 toggles the overlay on/off. While active, `supervisor_update_and_render()` returns `true`, telling the main loop to skip emulation. (F1/F2 are reserved for the CoCo 3 keyboard matrix — see `keyboard-hal.md`.)

**Machine selection:** The Settings menu's "Machine" row reflects the runtime-active machine (`g_machine_type`, not the compile-time `MACHINE_NAME`) and opens the Machine submenu; its own "Choose Machine Type" row opens the `SV_MACHINE_SELECT` popup over that list (CoCo 2 / CoCo 3, `(current)` marker on the active one); ESC returns to the submenu. Picking the other machine asks to confirm the restart in the same popup (No / Yes, defaults to No); accepting calls `supervisor_set_machine_type()` which saves supervisor state, persists the choice in NVS (`"sv"` namespace, `"machine_type"` key), and calls `esp_restart()`. On boot, `supervisor_load_machine_type()` returns the NVS value or the compile-time `MACHINE_TYPE`. See `runtime-machine-switch.md` for the complete flow.

**PMODE 4 Artifacts:** on a CoCo 3 it cycles NTSC artifact colours for PMODE 4 OFF / STANDARD / REVERSE (`tcc1014_set_artifacts()`), applied live and saved in NVS (`"sv"` / `"artifacts"`, loaded in `setup()`); see `coco3-gime.md`. On a CoCo 2 it reads ALWAYS, because the CoCo 2 renderer always shows them.

**Monitor:** on a CoCo 3 it switches RGB / COMPOSITE colours (`tcc1014_set_monitor()` and `hal_video_gime_colours_changed()`), applied live and saved in NVS (`"sv"` / `"monitor"`); see `coco3-gime.md`. On a CoCo 2 it reads N/A.

---

## File Map

| File | Purpose |
|---|---|
| `supervisor.h/.cpp` | Core lifecycle, state machine, public API, state persistence |
| `sv_menu.h/.cpp` | Main menu definition, actions, and key handling |
| `sv_filebrowser.h/.cpp` | SD card directory browser for mounting .DSK/.VDK images |
| `sv_cart.h/.cpp` | Load Cart screen: picks a .ccc/.rom cartridge ROM from the SD card, or unloads it |
| `sv_disk.h/.cpp` | WD1793 FDC emulation, PSRAM disk caching, mount/eject/flush |
| `sv_render.h/.cpp` | Rendering primitives (frame, menu items, file entries, dialogs). Paints through `osd_canvas` into the FabGL framebuffer. |
| `sv_debug.h/.cpp` | Debug screens (CPU/GIME status, hex dump, RS-232) + "Dump RAM to SD" full-memory dump to hex-text files |

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
| `SV_MAIN_MENU` | Top-level menu with 8 tiles |
| `SV_FILE_BROWSER` | **Disk Manager**: 2-row drive strip (all 4 drives) + file browser below. Left/Right selects active drive; Enter mounts; U ejects; F flushes. Also used as the stand-alone file browser. |
| `SV_MACHINE_SELECT` | Machine select: popup over the Settings list (CoCo 2 / CoCo 3, `(current)` on the active one); picking the other asks to confirm the restart in the same window, defaulting to No |
| `SV_SETTINGS` | Settings submenu, an icon list in the wide green frame: Machine, Keyboard (submenu), Joy - Mouse Sensitivity, Joy-Keyboard Emulation (OFF / RIGHT / LEFT, cycles on ENTER, live, NVS `joy_kbd`; see `joystick-hal.md`), WiFi / Debug, DriveWire, Multi-Pak - Insert Cartridges (submenu) |
| `SV_SOUND_MENU` | Settings -> Multi-Pak submenu: Orchestra-90 and Speech/Sound toggles (ON / OFF), Hard Disk (OFF / (RS-DOS) ON / (RGB-DOS) ON), RS-232 Pak toggle (live; shares UART0 with the Debug submenu's Echo Log, turning one on turns the other off) |
| `SV_CART_POPUP` | Popup over the Multi-Pak list: confirm a cartridge toggle and restart, or name its missing ROM file(s); from the Hard Disk row, the Off / RS-DOS / RGB-DOS picker |
| `SV_KEYBOARD_MENU` | Settings -> Keyboard submenu: Keyboard Language (US English / Spanish Latam, cycles on ENTER) and Key Mapper |
| `SV_JOY_SENSE` | Joy - Mouse Sensitivity: popup over the Settings list in the theme colours, with a live cursor pad tracking the PS/2 mouse, sensitivity bar (1-10), Invert-Y toggle |
| `SV_ABOUT` | Info screen (version, free heap) |
| `SV_DEBUG_MENU` | Debug submenu, an icon list in the wide green frame: CPU/GIME Status, Memory Hex Dump, RS-232 Pak, Dump RAM to SD, and the Echo Log toggle (debug output on the serial port, formerly "Debug Log" in Settings) |
| `SV_DEBUG_DUMP` | Active debug screen (dispatched from `SV_DEBUG_MENU`) |
| `SV_DEBUG_DUMP_NAME` | "Dump RAM to SD": filename entry → blocking save → result screen |
| `SV_CONFIRM_DIALOG` | Yes/No dialog (used by Reset Machine) |
| `SV_KEYMAP_LIST` | Key Mapper: a picture of the running machine's keyboard (CoCo 2 or CoCo 3 layout). Arrows move over it, ENTER remaps the selected key (a popup asks which character on two-character keys), T opens the test screen, C clears all mappings after a confirmation. Key style and the CoCo 2 geometry follow the CoCo2Keyboard on-screen keyboard from the CoCo2-CYD project (silver case, shadowed caps, red BREAK, triangle arrow glyphs, shifted character above the main one). The CoCo 3 layout was measured from a photo of the real keyboard (staggered rows, BREAK top right, arrow diamond down the right edge, F1/F2 beside the space bar). White keys are remappable, orange ones have a custom binding, cream ones are fixed; a legend is black when that character can be remapped and grey when it cannot |
| `SV_KEYMAP_CAPTURE` | Key Mapper: popup over the keyboard that waits for the next raw keypress, then asks to confirm the binding (DEL clears the binding, ESC cancels) |
| `SV_KEYMAP_TEST` | Key Mapper: shows what each pressed key would type, without injecting into the CoCo |
| `SV_CART` | Load Cart: folder browser for `.ccc` / `.rom` cartridge ROMs with an "Unload cartridge" row; a popup asks whether to restart now |

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

**Tiles** (static array `tiles[]`, 4x2 grid of 128 px tiles, each with a 64x28 icon drawn in code):

| Index | Label | Action |
|---|---|---|
| 0 | Disks | Opens unified Disk Manager (drive strip + file browser) |
| 1 | Cart | Opens the Load Cart screen (`sv_cart.cpp`, see "Cartridge ROM — Load Cart") |
| 2 | Setup | Opens settings submenu (first row "Machine" opens machine-select) |
| 3 | WiFi | Opens the WiFi / Debug screen directly; ESC returns to the main menu (to Settings when it was opened from the Settings row) |
| 4 | Reset | Confirm dialog → `sv_disk_flush_all()` → `machine_reset()` |
| 5 | Debug | Opens debug submenu (CPU/GIME Status, Hex Dump, RS-232 Pak, Dump RAM to SD, Echo Log toggle) |
| 6 | About | Info screen in the wide green frame, untitled: logo in the title row, credits, version, free heap |
| 7 | Resume | Closes the supervisor |

**Key handling:** all four arrows move through the grid, Enter executes the action, ESC/F1/F3 close.

**Look and layout:** CoCo green box with black text and a dark blue accent, 560 px wide (wider than the 256 px submenu frame: 640x200 pixels are ~2.4x taller than wide, so tiles and icons are stretched horizontally to look square). Icons are built from `fillRect`, `fillEllipse` and `drawLine` on a 32x28 unit grid with 2-pixel-wide units.

**Redraw:** `sv_menu_render()` repaints only the two tiles whose selection changed. `supervisor_update_and_render()` calls `sv_menu_invalidate()` when the main menu is entered from any other state (full repaint) and `sv_menu_clear_box()` before a submenu draws, so no green is left beside the narrower frame. The confirm dialog is left floating over the main menu.

`execute_action()` is the dispatcher — modifies `sv.state` to transition screens. Reset uses the confirm dialog pattern with a lambda callback that flushes all dirty disk caches before resetting.

### sv_filebrowser.cpp — Disk Manager / File Browser

This module is the **unified Disk Manager** (the `SV_FILE_BROWSER` state), drawn in the wide green frame shared with the main menu. The screen has two areas and a focus that moves between them.

**Drive buttons (top row):** one white button per drive (`DRIVE 0`..`DRIVE 3`) with a diskette icon (red when mounted, grey when empty) and the mounted file name on two 14-character lines without its extension. The focused button is filled with the accent colour.

With the hard disk on (Multi-Pak → Hard Disk), the row has six narrower buttons (84 px, names on two 8-character lines): `D0`..`D3`, then `HD0` and `HD1` with a hard-drive icon (red light when mounted). Internally they are slots: 0-3 are the WD1793 drives, `FB_HD_SLOT0` (4) + n is hard disk n (`vhd.cpp`). See `vhd.md`.

**File list (below):** FAT32 directory browser, 8 dense rows (`SV_FB_VISIBLE_ITEMS`, 8x8 font). Name on the left, `<DIR>` or the size right-aligned; the selected row is a full-width accent bar. A thin scrollbar appears only when the list is longer than the page.
- **scan:** `sv_fb_scan_directory()` reads directory entries into `SV_FileEntry[]` (max 128, allocated once in PSRAM)
- **sort:** `..` always first, then directories (alphabetical), then supported files (.dsk/.vdk, and .vhd while the hard disk is on), then others (dimmed, not mountable)

**Keys:**
- Tab switches between buttons and list; Up from the first file goes to the buttons, Down from the buttons returns to the list; Left/Right move between buttons.
- List: Enter on a folder descends, Backspace = parent, PgUp/PgDn/Home/End.
- Enter on a supported file opens the "Mount disk on" popup: each drive with its current contents plus Cancel, first empty drive preselected. A `.vhd` opens "Mount hard disk on" with HD0 / HD1 instead. Choosing an occupied drive asks "Replace disk?"; choosing an empty one while the image is already mounted elsewhere asks "Mount again?".
- Enter on a mounted drive button asks "Unmount disk" (eject flushes the dirty cache first).
- Every confirmation is No / Yes and defaults to No. After a mount the screen stays open.
- F flushes the dirty cache to SD: the focused drive when on the buttons (both hard disks for HD0/HD1), all drives and hard disks when in the list.
- ESC returns to the main menu; F1/F3 close the supervisor.

**Popups** are a sub-state of this screen (`pop` in `sv_filebrowser.cpp`), not an `SV_State`: white window, double accent border, accent title bar, up to two message lines, option rows. Closing one repaints the whole screen.

**Redraw:** only the two affected rows or buttons repaint on a keypress; the list repaints when it scrolls or the folder changes. `sv_filebrowser_invalidate()` forces a full repaint (called by the supervisor when the screen is entered).

**Supported formats:** `.dsk` (JVC) and `.vdk` = fully supported. `.vhd` = hard disk images, on HD0/HD1 while the hard disk is on. `.dmk` = recognized but not mountable.

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

All debug screens (the three pages and the Dump RAM to SD screens) draw in the wide green frame: headings in the accent colour, data in black, 8x8 font. The frame is painted once per screen (`dbg_frame()`); later redraws only rewrite the text, padded so shorter values erase longer ones, which keeps hex-dump scrolling and filename typing flicker-free. `sv_debug_invalidate()` forces the frame to repaint and is called by the supervisor when a debug screen is entered.

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

#### Dump RAM to SD (`SV_DEBUG_DUMP_NAME`)

The "Dump RAM to SD" row of the Debug submenu is an action, not a page: ENTER on it calls `sv_debug_begin_dump()` and enters the `SV_DEBUG_DUMP_NAME` state instead of opening a debug page. The screen has three sub-phases tracked by `SV_DumpPhase` in `SV_DebugState`:

- **`SV_DUMP_INPUT`** — filename entry. Type an up-to-8-char base name. The suggested default is the first `DUMPnnn` (`DUMP001`, `DUMP002`, …) with no `-RAM.txt` on the card, so a repeat dump doesn't overwrite the last one; ENTER keeps it, the first letter or digit typed replaces it, and BACKSPACE edits it; the supervisor HID route only delivers uppercase letters, digits, BACKSPACE, ENTER and ESC, which keeps names FAT-friendly. ENTER (non-empty) starts the save; ESC returns to the Debug submenu.
- **`SV_DUMP_SAVING`** — the render draws a "Saving…" banner first (FabGL scans the framebuffer out continuously, so it is on screen immediately) with the real sizes (64KB CPU space + the machine's RAM), then performs the **blocking** write, then flips to the result phase. A progress bar with a percentage covers both files; the write loops advance it every 4KB of memory and it is redrawn only when the percentage changes. Emulation is already paused while the supervisor is open.
- **`SV_DUMP_RESULT`** — shows the saved file paths and sizes (or an error in red). Any key returns to the Debug submenu.

**Output:** two hex-text files are written under `/DUMPS/` on the SD card (created if missing), both in the same `AAAAA: bb bb … |ascii|` layout as the serial hex dump:

| File | Contents |
|---|---|
| `/DUMPS/<NAME>-CPU.txt` | 64KB CPU address space via `machine_read($0000–$FFFF)` — RAM through the current MMU map, plus mapped ROM and I/O (what the 6809 sees now). 4-digit addresses. |
| `/DUMPS/<NAME>-RAM.txt` | Full physical RAM straight from `m->ram_physical`: the configured size on CoCo 3 (`m->ram_size`: 128KB–2MB; 5-digit addresses, 6 digits for 2MB), 64KB on CoCo 2. |

Lines are batched through a 4KB static buffer (`s_dump_buf`) so a 512KB dump becomes ~256 bulk SD writes rather than 32K per-line writes. The RAM hex file is about 4.7× the RAM size (~2.4 MB for 512KB, ~9.7 MB for 2MB), and the write blocks for the whole time; the on-screen bar and serial logging report progress.

### sv_keymap.cpp — Key Mapper

Reached from **Settings → Key Mapper**. Lets the user bind any physical key to one of the remappable CoCo keys defined in `hal_keyboard.cpp` (`KM_CHARS`): the CoCo 2 set (`! " # $ % & ' ( ) : * - = @ + ; < > , . ? /` and the four arrows) plus, on CoCo 3 only, ALT / CTRL / CLEAR / F1 / F2.

- **List screen** (`SV_KEYMAP_LIST`): two action rows (Test Mappings, Clear All Mappings) then one row per key showing its binding (`default` or the FabGL key name). ENTER on a key opens capture; ESC returns to Settings. Scrolls like the file browser.
- **Capture screen** (`SV_KEYMAP_CAPTURE`): the next key-down opens a Yes/No confirm dialog (`Map ! to key F7 ?`); on Yes the binding is applied **live** (no machine reset needed) and saved, then the list screen returns. DEL clears the binding; ESC cancels. SHIFT and the host hotkeys F3–F6 are rejected as sources. A key already bound elsewhere is stolen from that entry.
- **Test screen** (`SV_KEYMAP_TEST`): each press shows `KEYNAME -> result`, e.g. `HASH -> # (custom)`; nothing reaches the CoCo.

Capture/test receive **raw VirtualKeys**: `process_vk()` (hal_keyboard.cpp) calls `sv_keymap_on_raw_vk()` while `sv_keymap_wants_raw_vk()` is true, bypassing the lossy VK→HID-usage translation used by the other menus. At runtime the custom table is checked **before** `VK_MAP`, so a binding replaces the key's default meaning. Bindings persist immediately via `supervisor_save_keymap()` and load at boot from `setup()`.

### sv_joystick.cpp — Joy - Mouse Sensitivity

Reached from **Settings -> Joy - Mouse Sensitivity** (`SV_JOY_SENSE`). Lets the user tune the PS/2-mouse-as-joystick behavior described in `joystick-hal.md`.

- **Live cursor pad:** a bordered box shows a cursor that tracks the live mouse position via `hal_joystick_get_pos()`.
  - **Flicker-free redraw (no double-buffer):** the OSD draws straight to the VGA framebuffer, so repainting the whole frame every tick flickers. Instead, the static frame/box/bar/labels are painted **once** by `sv_render_joystick_pad()` (in `sv_render.cpp`) when `s_full_redraw` is set — on open, and on any level/Invert-Y change. Between frames the cursor moves **incrementally** via `sv_render_joystick_cursor()`, which erases only the old cursor square and draws the new one (the box border is never touched).
  - **Idle when still:** `sv_joystick_tick()` calls `hal_joystick_update()` and requests a redraw (`sv.needs_redraw`) **only when the cursor actually moved** (tracked by `s_drawn_x`/`s_drawn_y`) or a full repaint is pending. A motionless mouse leaves the screen idle, so there is nothing to flicker. `sv_joystick_tick()` runs from `supervisor_update_and_render()` before the `needs_redraw` idle check.
- **Sensitivity bar:** a 10-segment bar reflecting the current level (1-10, via `hal_joystick_get_sensitivity()`/`hal_joystick_set_sensitivity()`). Labels/values use the compact `setTextFont(1)` (8x8) to keep the screen uncluttered.
- **Invert-Y value:** shows the current Invert-Y state (`hal_joystick_get_invert_y()`/`hal_joystick_set_invert_y()`).

**Key handling:**
| Key | Action |
|---|---|
| Left/Right | Adjust sensitivity level (1..10) |
| Up/Down | Toggle Invert-Y |
| ESC | Save level + Invert-Y to NVS via `supervisor_save_joystick()`, return to Settings |

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

### Machine submenu — machine type, CPU, CoCo 3 memory

The Settings row **Machine** opens a submenu (`SV_MACHINE_MENU` state, an icon list like the Keyboard submenu) with three rows. All three are fixed at boot, so a change restarts the emulator: Choose Machine Type and CoCo 3 Memory ask to confirm (No / Yes, default No); the CPU picker restarts as soon as the other CPU is picked.

| Row | Value shown | Popup | Persisted as |
|---|---|---|---|
| Choose Machine Type | CoCo 2 / CoCo 3 | `SV_MACHINE_SELECT` | `machine_type` |
| CPU | MC6809 / HD6309 | `SV_CPU_POPUP` — picker listing 6809 and 6309, `current` on the running one; ENTER on the other saves and restarts; see `cpu.md` | `cpu_variant` |
| CoCo 3 Memory | 128K / 512K / 1024K / 2048K | `SV_RAM_SELECT` — lists the four sizes with `(current)` on the active one, then confirms; see "RAM sizes" in `coco3-gime.md` | `coco3_ram` |
| CoCo 2 Memory (on a CoCo 2, same row) | 64K | none — fixed, ENTER does nothing | — |

`supervisor_load_cpu_variant()` and `supervisor_load_coco3_ram()` seed `g_cpu_variant` and `g_coco3_ram_kb` in `setup()` before `machine_init()`. The memory size is used by the CoCo 3 only, so it is set while the CoCo 3 is running; on a CoCo 2 the row shows the fixed 64K.

### Cartridge ROM — Load Cart

The main-menu **Cart** tile opens the Load Cart screen (`SV_CART` state, `sv_cart.cpp`), a list in the wide green frame:

- Two status lines: the stored cartridge (`none` when empty, `(after restart)` while the change has not been applied, `(file missing)` when the boot fell back to Disk BASIC) and the folder on screen.
- An **Unload cartridge** row, present while a cartridge is loaded or waiting for a restart.
- The folders and the `.ccc` / `.rom` files of the current folder. The screen opens in `/carts` (the SD root if that folder does not exist) and remembers the folder until power-off.

The ROM is read once at boot, so ENTER on a file or on Unload opens a popup: **No** stores the choice for the next restart, **Yes** stores it and calls `supervisor_save_and_restart()`, ESC cancels. Choosing what this boot already loaded just stores it, with no popup. On the CoCo 3 a file of up to 128 KB (`CART_ROM_MAX_SIZE`) is accepted, on the CoCo 2 up to 16 KB (`CART_BANK_SIZE`); a larger file is refused with a message.

The choice is stored in `cart_rom` as a full SD path. `load_cart_rom()` in `machine.cpp` reads the file into the cartridge buffer: the CoCo 2 maps it at `$C000-$FEFF` (16 KB at most), and the CoCo 3 serves `$C000-$DFFF` for a ROM of up to 8 KB and `$C000-$FEFF` for a larger one (`rom_disk_end`). On the CoCo 3 a ROM of more than 16 KB is a **bank-switched cartridge** (RoboCop, Predator): it is split into 16 KB banks, the bank count rounded up to a power of two, and a write to `$FF40` selects the bank that answers every cartridge-ROM read (`cart_bank`); reset selects bank 0. While one is loaded, writes to `$FF40-$FF5F` do not reach the disk controller. Every ROM over 16 KB is treated this way, as in XRoar, so an unbanked 32 KB ROM that expects to sit at `$8000-$FEFF` does not work.

A cartridge takes the place of Disk BASIC, so the disk drives are not usable from BASIC until it is unloaded. Loading or unloading a cartridge here also turns the Orchestra-90 setting off.

### Multi-Pak — Orchestra-90, Speech/Sound and Hard Disk

See `orchestra90.md`, `ssc.md` and `vhd.md` for the cartridges themselves.

The Settings row **Multi-Pak - Insert Cartridges** (a Multi-Pak icon; called Cartridges in v0.18.0 and Sound Carts before) opens a submenu (`SV_SOUND_MENU`, an icon list like the Keyboard submenu; the Settings list has six rows) with four rows. Orchestra-90, Speech/Sound and Hard Disk are set up at boot, so their rows open a popup (`SV_CART_POPUP` state, 480 px wide, drawn with `sv_render_popup_w()` in the theme colours) before calling `supervisor_save_and_restart()`. The two ON/OFF rows share the popup code, which explains the change and asks No / Yes; each is an entry in `CART_TOGGLES` (`sv_menu.cpp`: name, enable flag, save function, ROM files and sizes, messages).

- **Orchestra-90** turns the `$FF7A`/`$FF7B` DAC ports on and puts `orch90.rom` in the cartridge slot at `$C000` in place of Disk BASIC (read once at boot by `load_cart_rom()` in `machine.cpp`). Turning it off clears the stored cartridge, so Disk BASIC is back after the restart.
- **Speech/Sound** turns the `$FF7D`/`$FF7E` ports on. It has no CoCo-side ROM, so Disk BASIC stays. It needs `pic-7040-510.bin` (4096 bytes) and `sp0256-al2.bin` (2048 bytes).
- **Turning either on** first checks its ROM file(s) (exact size where known). If one is missing, the popup names it and nothing changes.
- **Hard Disk** opens a picker instead (`hd_popup_*` in `sv_menu.cpp`, same popup state): **Off**, **On, boot RS-DOS (NitrOS-9 compatible)** (Disk BASIC, as in VCC's Multi-Pak with the FD-502 and the hard disk in two slots) and **On, boot RGB-DOS**. The current mode is preselected and marked "current"; the row value is OFF / (RS-DOS) ON / (RGB-DOS) ON. The mode comes from `vhd_en` plus whether the boot cartridge's file name is `rgbdos.rom`. RGB-DOS is marked "no ROM" unless `rgbdos.rom` is in `/roms/` or already chosen with Load Cart. Picking another mode saves `vhd_en` and `cart_rom` (RS-DOS clears it, RGB-DOS sets `rgbdos.rom` unless Load Cart already chose an RGB-DOS file) and restarts. The On modes, and Orchestra-90, turn each other off: both need the cartridge slot. The row has a hard-disk icon. Turning the hard disk off (Off, or Orchestra-90 on) also removes the saved HD0/HD1 images (`hd0_path`/`hd1_path`) from NVS in `supervisor_save_state()`, so turning it back on starts with no images mounted.
- **RS-232 Pak** (moved here from Settings) toggles at once with no restart: ON sets `SERIAL_MODE_RS232` (UART0, forcing the Debug submenu's Echo Log off), OFF sets `SERIAL_MODE_OFF`.

`supervisor_load_cart_config()` runs in `setup()` after `dw_bus_load_config()`, so the Orchestra-90 ROM replaces DriveWire's HDB-DOS ROM. While it is on there is no Disk BASIC.

A ROM that does not start with `DK` is a program cartridge: `machine.cpp` pulses CART (PIA1 CB1) once per frame, so BASIC starts it at power-on as on the real machine.

## Persistence (ESP32 Preferences)

Namespace: `"sv"`. Stored in NVS flash.

| Key | Type | Description |
|---|---|---|
| `last_dir` | String | Last browsed directory path |
| `d0_path` .. `d3_path` | String | Mounted disk paths per drive |
| `auto_mount` | Bool | Auto-mount on boot (default true) |
| `machine_type` | UChar | Runtime machine selection (3 = CoCo 2, 4 = CoCo 3) |
| `serial_mode` | UChar | UART0 ownership: 0 = off, 1 = Debug Log, 2 = RS-232 Pak |
| `kbd_layout` | UChar | PS/2 layout: 0 = US English, 1 = Spanish Latam |
| `keymap` | Bytes | Key Mapper bindings: `int16_t[KM_COUNT]` blob, one FabGL VirtualKey per remappable CoCo key (-1 = default) |
| `joyLevel` | UChar | Mouse-as-joystick sensitivity level, 1..10 (default 7) |
| `joyInv` | Bool | Mouse-as-joystick Invert-Y flag (default false) |
| `orch90` | Bool | Orchestra-90 DAC ports enabled (default false) |
| `ssc` | Bool | Speech/Sound Cartridge enabled (default false) |
| `vhd_en` | Bool | Virtual hard disk at `$FF80-$FF86` enabled (default false) |
| `hd0_path`, `hd1_path` | String | Hard disk images on HD0/HD1, auto-mounted at boot with the floppies; written only while the hard disk is on, so turning it off and on again keeps them |
| `cpu_variant` | UChar | 0 = MC6809, 1 = HD6309 (default `CPU_VARIANT`) |
| `coco3_ram` | UShort | CoCo 3 memory in KB: 128, 512, 1024 or 2048 (default `COCO3_RAM_KB`) |
| `cart_rom` | String | Cartridge ROM loaded at `$C000`: a full SD path set by the Load Cart screen, or a bare file name in `/roms/` (`orch90.rom` from the Orchestra-90 toggle, `rgbdos.rom` from Hard Disk → RGB-DOS); absent = default (Disk BASIC) |

- `supervisor_save_state()` — writes current dir + all mounted disk and hard disk paths
- `supervisor_load_state()` — restores dir + auto-mounts disks and hard disks on boot (before the CPU starts, so RGB-DOS finds the hard disk at cold start)
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
