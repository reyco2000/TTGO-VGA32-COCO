# ESP32 CoCo Emulator — System Architecture

## Project Overview

TTGO-VGA32-COCO is a Tandy Color Computer emulator supporting both the CoCo 2 and CoCo 3, running on the **LilyGo TTGO VGA32 v1.4** board (ESP32-WROVER-E). It is derived from XRoar, Ciaran Anscomb's Dragon/CoCo emulator originally written in C for desktop platforms. The project adapts XRoar's emulation core to run on a resource-constrained embedded system with custom hardware abstraction for display, audio, keyboard, and storage.

**Target hardware:** LilyGo TTGO VGA32 v1.4 — ESP32-WROVER-E, dual-core 240 MHz, **4 MB PSRAM**, **4 MB flash**, on-board VGA DAC, PS/2 mini-DIN connector, MicroSD socket, 3.5 mm audio jack.

**Emulated machines:**
- Tandy CoCo 2 — 64 KB RAM, Color BASIC 1.3, Extended BASIC 1.1, Disk BASIC 1.1
- Tandy CoCo 3 — 512 KB RAM with MMU banking, Color BASIC 2.0, Extended BASIC 2.0, Disk BASIC 1.1; GIME (TCC1014) chip for enhanced video and interrupt control

A single firmware image contains both machine paths. The active machine is chosen at boot from a runtime variable `g_machine_type` (seeded from NVS, defaulting to `MACHINE_TYPE` in `config.h`) and can be switched at runtime from the supervisor menu → **Machine** → confirm → `esp_restart()`. See **`runtime-machine-switch.md`** for the end-to-end design.

---

## System Architecture

```
+-----------------------------------------------------------+
|              TTGO-VGA32-COCO.ino                   |
|                  (Arduino entry point)                    |
|  setup() -> hal_init, machine_init, load_roms, video_init |
|  loop()  -> hal_process_input, supervisor, machine_run    |
+-----------------------------------------------------------+
         |                    |                    |
         v                    v                    v
+----------------+  +------------------+  +----------------+
|  HAL Layer     |  |  Emulation Core  |  |  Supervisor    |
|  (hal/*.cpp)   |  |  (core/*.cpp)    |  |  (supervisor/) |
|                |  |                  |  |                |
| hal_video      |  | MC6809 CPU       |  | OSD Menu       |
|  (FabGL VGA)   |  | MC6821 PIA x2    |  | Disk Manager   |
| hal_audio      |  | MC6847 VDG       |  | File Browser   |
|  (DAC1)        |  | TCC1014 GIME     |  | FDC (WD1793)   |
| hal_keyboard   |  | SAM6883          |  | Render engine  |
|  (FabGL PS/2)  |  | Machine wiring   |  | (via osd_canvas)|
| hal_joystick   |  |                  |  |                |
|  (PS/2 mouse)  |  |                  |  |                |
| hal_storage    |  |                  |  |                |
| osd_canvas     |  |                  |  |                |
+----------------+  +------------------+  +----------------+
         |                    |                    |
         v                    v                    v
+-----------------------------------------------------------+
|        LilyGo TTGO VGA32 v1.4 Hardware                    |
|  VGA DAC | DAC1/GPIO25 | PS/2 ULP | SD HSPI | PSRAM       |
+-----------------------------------------------------------+
```

---

## Module Breakdown

### 1. Entry Point: `TTGO-VGA32-COCO.ino`

The Arduino sketch orchestrates boot and the main loop.

**Boot sequence (`setup`):**
1. Initialize Serial (115200 baud)
2. Apply serial-port mode from NVS (`supervisor_load_serial_mode()`) — before any debug output, so the RS-232 Pak host link is not corrupted by the boot banner
3. `hal_init()` — storage, audio, keyboard (FabGL PS/2), joystick stub (NOT video yet)
4. Seed `g_machine_type` from NVS
5. `machine_init()` — allocate RAM/ROM from PSRAM, init CPU/PIA/VDG/GIME/SAM
6. `machine_load_roms()` — load ROMs from SD card
7. **SD + ROM validation** — `hal_storage_is_ready()` → if false, call `boot_halt_screen()` with "SD card not found" message and halt; check per-machine ROM flags → if any required ROM is missing, call `boot_halt_screen()` naming the missing files and halt. `boot_halt_screen()` initializes the VGA display, paints black/red/white text at Font 2 (8×14 px), mirrors to serial, and loops forever — `setup()` never resumes.
8. `hal_video_init()` — initialize FabGL VGAController at 640×200 @ 70 Hz (only reached when validation passes)
9. `machine_reset()` — cold reset, read reset vector
10. `supervisor_init()` — OSD menu, FDC, NMI wiring
11. `supervisor_load_state()` — restore last-mounted disks from NVS

**Main loop (`loop`):**
1. `hal_process_input()` — drain FabGL VirtualKey queue, tick deferred releases
2. `supervisor_update_and_render()` — if active, render OSD and skip emulation
3. `machine_run_frame()` — execute 262 scanlines of CPU + video
4. `hal_render_frame()` — no-op on VGA32 (FabGL DMA scans the framebuffer continuously)

---

### 2. Emulation Core (`src/core/`)

The emulation core is HAL-agnostic — no source file under `src/core/` references TFT, USB, VGA, FabGL, or any board-specific API. It calls only the HAL surface declared in `src/hal/hal.h`.

#### MC6809 CPU — `mc6809.cpp` / `mc6809.h`

Full Motorola 6809 CPU emulation with all documented and undocumented opcodes.

- **Registers:** A, B (combined as D), X, Y, U (user stack), S (hardware stack), DP (direct page), CC (condition codes), PC
- **Addressing modes:** Inherent, Immediate, Direct, Extended, Indexed (all 6809 post-byte variants)
- **Interrupts:** NMI, FIRQ, IRQ with proper priority and masking
- **Special states:** CWAI (push-and-wait), SYNC, HALT
- **Cycle accuracy:** Each instruction accounts for correct cycle counts
- **Memory interface:** Function pointers `read(addr)` and `write(addr, val)` set by machine.cpp
- **Debug:** PC trace ring buffer (64 entries) dumpable to serial

#### MC6821 PIA — `mc6821.cpp` / `mc6821.h`

Two Peripheral Interface Adapters handle all I/O and interrupts.

**PIA0 ($FF00-$FF03):** Keyboard matrix, joystick comparator, vsync IRQ
- Port A (PA0-PA7): Keyboard row data input (active low)
- Port B (PB0-PB7): Keyboard column select output
- CA1: Horizontal sync (HS) from VDG
- CB1: Field sync (FS/vsync) from VDG — triggers 60 Hz IRQ
- IRQA/IRQB → CPU IRQ line

**PIA1 ($FF20-$FF23):** Sound output, VDG mode control, cartridge interrupt
- Port A bits 2-7: 6-bit DAC audio output
- Port B bit 1: Single-bit audio output
- Port B bits 3-4: VDG mode (CSS, GM selects)
- CA1/CB1: Cartridge FIRQ
- IRQA/IRQB → CPU FIRQ line

#### MC6847 VDG — `mc6847.cpp` / `mc6847.h`

Video Display Generator renders 256×192 active pixels into a line buffer for the CoCo 2.

**Text mode (AG=0):**
- 32×16 characters, 8 px wide × 12 scanlines tall
- Internal 6×12 font ROM (64 characters, PROGMEM)
- Normal, inverse, and semigraphics-4 supported

**Graphics modes (AG=1, GM=0..7):**
- 8 modes from CG1 (64×64, 4-color) to RG6 (256×192, 2-color)
- CSS bit selects palette pair (green/orange families)

**Color palette:** 12 VDG colors mapped to RGB values; in this build the HAL converts them to RGB222 raw pixel bytes for FabGL.

#### TCC1014 GIME — `tcc1014.cpp` / `tcc1014.h`

CoCo 3 GIME — see the dedicated **CoCo 3 Extensions** section below.

#### SAM6883 — `sam6883.cpp` / `sam6883.h`

Synchronous Address Multiplexer controls memory mapping and VDG display address (CoCo 2; GIME absorbs this on CoCo 3).

#### Machine Integration — `machine.cpp` / `machine.h`

Wires all components together with the active machine's memory map.

**CoCo 2 memory map:**
```
$0000-$7FFF  RAM (64 KB, SAM page select)
$8000-$9FFF  Extended BASIC ROM (8K)
$A000-$BFFF  Color BASIC ROM (8K)
$C000-$FEFF  Disk BASIC ROM (16K cartridge slot)
$FF00-$FF1F  PIA0 (mirrored every 4 bytes)
$FF20-$FF3F  PIA1 (mirrored every 4 bytes)
$FF40-$FF5F  FDC — WD1793 + DSKREG
$FF68-$FF6B  MC6551 ACIA — RS-232 Pak (only when enabled; see rs232-hal.md)
$FFC0-$FFDF  SAM control register
$FFE0-$FFFF  Vectors (from ROM)
```

**CoCo 3 memory map (512 KB RAM + MMU overlay):**
```
CPU $0000-$DFFF  Eight 8 KB MMU slots → any 8 KB block of 512 KB physical RAM
CPU $E000-$FEFF  GIME ROM/RAM window (banked via GIME INIT0/INIT1 registers)
$FF00-$FF1F      PIA0 (same as CoCo 2)
$FF20-$FF3F      PIA1 (same as CoCo 2)
$FF40-$FF5F      FDC — WD1793 + DSKREG
$FF90-$FF9F      GIME internal registers (INIT0, INIT1, IRQ, FIRQ masks, timer)
$FFA0-$FFAF      MMU task registers (8 slot registers × 2 tasks = 16 bytes)
$FFB0-$FFBF      GIME palette registers (16 × 6-bit RGB color entries)
$FFC0-$FFDF      SAM control register (CoCo 3 compatible subset)
$FFE0-$FFFF      Vectors (from ROM)
```

**IRQ routing:**
- PIA0 IRQA/IRQB → CPU IRQ (60 Hz timer, keyboard)
- PIA1 IRQA/IRQB → CPU FIRQ (cartridge)
- GIME IRQ / FIRQ → CPU (CoCo 3 only)
- MC6551 ACIA → CPU FIRQ via CART line, OR'd in per scanline when the RS-232 Pak is enabled

#### MC6551 ACIA — `mc6551.cpp` / `mc6551.h`

Optional Deluxe RS-232 Pak at `$FF68–$FF6B`, bridged to UART0 by `hal_rs232.cpp`.
Shares the single UART0 with debug output (mutually exclusive). See `rs232-hal.md`.

---

## CoCo 3 Extensions

The CoCo 3 replaces the MC6847 VDG and most of the SAM6883 with Tandy's custom GIME chip (TCC1014). The GIME provides an expanded video subsystem, a hardware MMU, and an enhanced interrupt controller.

### TCC1014 GIME Chip — `tcc1014.cpp` / `tcc1014.h`

Approximately 940 lines across the two files.

#### Memory Management Unit (MMU)

- Physical RAM: 512 KB, organised as 64 × 8 KB pages
- MMU slot registers at $FFA0-$FFAF: 16 bytes covering two tasks, each 8 slots
- Active task selected by INIT0 bit 0 (TR flag)
- Each slot register holds a 6-bit page number; the GIME concatenates it with the low 13 address bits to produce a 19-bit physical address
- CPU $0000-$DFFF pass through the MMU
- CPU $E000-$FEFF is controlled by GIME INIT0/INIT1 ROM-enable bits

#### Video / Graphics Modes

**Legacy compatibility (GIME COCO=1):** Pass-through to MC6847-compatible logic.

**Native GIME modes (GIME COCO=0):**

| Mode | Resolution | Colors | Notes |
|------|-----------|--------|-------|
| Text 32-col | 32×16 chars | 16 fg / 16 bg | Hardware attribute bytes |
| Text 40-col | 40×24 chars | 16 fg / 16 bg | |
| Text 80-col | 80×24 chars | 16 fg / 16 bg | |
| Graphics 128px | 128 wide | 16-color or 4-color | |
| Graphics 160px | 160 wide | 4-color | |
| Graphics 320px | 320 wide | 4-color or 2-color | |
| Graphics 640px | 640 wide | 2-color | Maximum horizontal resolution; 1:1 on the VGA framebuffer |

**16-Color Palette:**
- 16 palette registers at $FFB0-$FFBF
- Each register is 6 bits (RRGGBB, 2 bits per channel) — 64 possible RGB combinations
- The GIME core pre-converts pixel data to RGB565 for the line buffer (OPT-C4); the HAL converts each RGB565 pixel back to a 6-bit RGB222 raw VGA pixel byte before writing to the FabGL scanline.

#### GIME Interrupt Controller

| Bit | Source | Description |
|-----|--------|-------------|
| 5 | TMR | Programmable countdown timer underflow |
| 4 | HBORD | Horizontal border (end of active display line) |
| 3 | VBORD | Vertical border (end of active display field) |
| 2 | EI2 | Serial (unused in this port) |
| 1 | EI1 | Keyboard / joystick comparator |
| 0 | EI0 | Cartridge interrupt |

Each source can route to IRQ or FIRQ independently via the GIME enable masks at $FF92 / $FF93.

#### GIME Internal Registers

| Address | Register | Function |
|---------|----------|----------|
| $FF90 | INIT0 | COCO compat flag, MMU enable, ROM map, task select |
| $FF91 | INIT1 | Memory type (128 K / 512 K), GIME timer rate |
| $FF92 | IRQENR | GIME IRQ source enable mask |
| $FF93 | FIRQENR | GIME FIRQ source enable mask |
| $FF94 | TIMERH | Timer preload high byte (bits 11-8) |
| $FF95 | TIMERL | Timer preload low byte (bits 7-0) |
| $FF98 | VMODE | Video mode: GIME/CoCo, graphics/text, line count |
| $FF99 | VRES | Horizontal resolution, color depth |
| $FF9A | BORDER | Border color index |
| $FF9C | VSCROL | Vertical fine-scroll offset |
| $FF9D | HSCROL | Horizontal fine-scroll + row stride select |
| $FF9E | VOFFSET_H | Display start address high (bits 18-16) |
| $FF9F | VOFFSET_L | Display start address low (bits 15-9) |

---

### 3. Hardware Abstraction Layer (`src/hal/`)

#### Video — `hal_video.cpp`

FabGL `VGAController` at **640×200 @ 70 Hz**, 64-color direct mode (RGB222, 2 bits per channel).

- `hal_video_init()` brings up FabGL (`begin(R1,R0,G1,G0,B1,B0,HSYNC,VSYNC)`), sets the resolution, and binds the framebuffer to the OSD canvas used by the supervisor.
- `hal_video_render_scanline_gime()` consumes the pre-converted RGB565 line emitted by the GIME core (OPT-C4) and writes raw VGA pixel bytes via `getScanline(y)`. Each pixel is `s_vga.createRawPixel(RGB222(...))`. Widths of 320 / 640 are 1:1 or pixel-doubled to the 640 framebuffer; other widths are nearest-neighbor scaled.
- `hal_video_render_scanline()` (CoCo 2 VDG) writes each palette-index pixel through a 16-entry RGB222 cache keyed off the VDG palette, pixel-doubled to fit 256→512 on the 640 surface and vertically centered within 200 lines.
- `hal_video_present()` and `hal_video_present_gime()` are no-ops — FabGL scans the framebuffer continuously via DMA. They update the FPS counter and clear the dirty flag.
- `hal_video_get_canvas()` returns a pointer to an `OSDCanvas` instance — defined in `osd_canvas.h` — so the supervisor can draw to the OSD.

**Why VGAController (64-color direct) and not VGA16Controller (16-color paletted):** the GIME core emits pre-converted RGB565 pixels rather than palette indices, so direct color mode avoids a per-pixel RGB565 → palette-index reverse lookup and keeps the core untouched.

#### Audio — `hal_audio.cpp`

ESP32 internal **DAC1 on GPIO25** driven from a hardware timer ISR.

- `hal_audio_init()` calls `dac_output_enable(DAC_CHANNEL_1)`, allocates `timerBegin(0, 80, true)` for a 1 µs timer base, and arms `timerAlarmWrite(timer, 1000000 / 22050, true)` for the 22050 Hz ISR.
- The ISR pulls one sample per fire from the active double-buffer slot and calls `dac_output_voltage(DAC_CHANNEL_1, sample)`.
- The 262-sample-per-frame scanline capture + commit-frame double-buffer model from the S3 build is preserved unchanged — the only difference is the output stage.

#### Keyboard — `hal_keyboard.cpp`

FabGL PS/2 keyboard on the board's mini-DIN connector (CLK=GPIO33, DATA=GPIO32).

- `hal_keyboard_init()` calls `PS2Controller::begin(PS2Preset::KeyboardPort0, KbdMode::CreateVirtualKeysQueue)`. The preset allocates a `Keyboard` instance internally and starts the virtual-key queue. **Note:** the explicit-pin overload of `PS2Controller::begin()` does *not* allocate the keyboard; the preset variant does.
- Layout is set via `kbd->setLayout(&fabgl::SpanishLayout)`. All built-in FabGL layouts are available.
- `hal_keyboard_tick()` drains the queue with `getNextVirtualKey()`, dispatches each `VirtualKeyItem` through `process_vk()` which:
  1. Handles supervisor hotkeys (F3 = toggle supervisor; while active, all keys are forwarded to the supervisor as HID-usage IDs via a small `VK → HID-usage` mapping for compatibility with the existing supervisor code).
  2. Handles emulation hotkeys (F4 = reset, F5 = FPS overlay, F6 = quick-mount last disk).
  3. Looks up `VK_*` in a static `VK_MAP[]` table to find the CoCo (col, row) — letters, digits, symbols, arrows, modifiers, plus the shifted-digit pre-resolved symbols (`VK_HASH`, `VK_DOLLAR`, etc.) that FabGL emits when SHIFT is held.
  4. Applies the press to `key_matrix[col]` with shift-suppression logic for symbols that already include SHIFT.
- Key releases go through the same `defer_release()` mechanism as the original USB HID path: a key stays held in the matrix for `MIN_HOLD_FRAMES` (4 frames) after PS/2 says it's released, so fast taps remain visible to BASIC's KEYIN debounce.

**CoCo keyboard matrix (8 columns × 7 rows, active-low):**
```
        PB0  PB1  PB2  PB3  PB4  PB5  PB6  PB7
PA0:     @    A    B    C    D    E    F    G
PA1:     H    I    J    K    L    M    N    O
PA2:     P    Q    R    S    T    U    V    W
PA3:     X    Y    Z   UP  DOWN LEFT RIGHT SPACE
PA4:     0    1    2    3    4    5    6    7
PA5:     8    9    :    ;    ,    -    .    /
PA6:   ENTER CLEAR BREAK ALT  CTL  F1   F2  SHIFT
```

#### Joystick — `hal_joystick.cpp`

Neutral stub. The TTGO VGA32 v1.4 has no joystick wiring in this build.

- `hal_joystick_read_axis()` returns 32 (center of 0..63)
- `hal_joystick_read_button()` returns **0** — *not pressed* under this codebase's polarity convention. The PIA0 PA read site in `machine.cpp` does `if (hal_joystick_read_button(0)) row_data &= ~0x01;`, so any non-zero return forces PA0 low and corrupts the keyboard column scan.
- `hal_joystick_compare()` returns false
- `hal_joystick_init()` / `hal_joystick_update()` are no-ops; no GPIO is claimed.

#### Storage — `hal_storage.cpp`

SD card on the dedicated HSPI bus (CS=GPIO13, MOSI=GPIO12, MISO=GPIO2, SCK=GPIO14).

> GPIO2 is a strapping pin on the ESP32 — the board must keep it floating or pulled high at boot for the bootloader.

The implementation tries 4 MHz first, falls back to 1 MHz with an SPI re-init, and finally falls back to the shared default SPI bus if dedicated HSPI fails. ROMs and disk images are loaded into PSRAM via this layer.

#### OSD Canvas — `osd_canvas.{h,cpp}`

The supervisor's draw calls go through the `OSDCanvas` class in `osd_canvas.{h,cpp}`, which provides:

- An `OSDCanvas` class with the drawing primitives the supervisor uses (`fillRect`, `drawRect`, `drawFastHLine`, `drawFastVLine`, `drawPixel`, `setTextFont`, `setTextColor`, `setTextDatum`, `drawString`, `textWidth`, public `textcolor` / `textbgcolor` fields, `startWrite`/`endWrite` no-ops)
- The `TL_DATUM` / `TC_DATUM` / `TR_DATUM` / `ML_DATUM` / etc. alignment constants
- OSD color constants (`OSD_BLACK`, `OSD_WHITE`, etc.) as RGB565

The implementation forwards each call to a FabGL `Canvas`, converting RGB565 → RGB888 with bit replication. Fonts map to FabGL's `FONT_8x8` (font 1) and `FONT_8x14` (font 2). `sv_render.h` includes `osd_canvas.h` so the supervisor's `g_canvas->...` calls resolve to this layer.

---

### 4. Supervisor / OSD (`src/supervisor/`)

State machine: `INACTIVE → MAIN_MENU → FILE_BROWSER | MACHINE_SELECT | SETTINGS | ABOUT | DEBUG_MENU | DEBUG_DUMP | CONFIRM_DIALOG`. F3 toggles, ESC/F1 closes.

- **Menu** (`sv_menu.cpp`) — 6-item main menu: Disk Manager, Machine type, Settings, Reset Machine, Debug, About. Reset flushes all dirty disks before calling `machine_reset()`.
- **Disk Manager / File Browser** (`sv_filebrowser.cpp`) — unified screen: 2-row drive strip (D0–D3) above a FAT32 file list. Left/Right select active drive; Enter mounts; U ejects; F flushes to SD. After mount, OSD stays open for additional drives.
- **Machine Select** (`sv_menu.cpp`) — CoCo 2 / CoCo 3 submenu; confirm dialog → `supervisor_set_machine_type()` flushes all dirty disks then calls `esp_restart()`.
- **Settings** (`sv_menu.cpp`) — Debug Log / RS-232 Pak toggle (mutually exclusive; both share UART0).
- **FDC** (`sv_disk.cpp`) — WD1793 command-level emulation with PSRAM cache, deferred INTRQ (byte-257 mechanism)
- **Render** (`sv_render.cpp`) — green phosphor theme; draws via `osd_canvas` into the FabGL framebuffer; `sv_render_drive_strip()` renders the 2×2 drive status grid.

The supervisor's coordinate system is the framebuffer (`DISPLAY_WIDTH` × `DISPLAY_HEIGHT` = 640 × 200 on the VGA32 build). The original layout was designed for 320 × 240; the OSD currently sits in the upper-left region of the 640 × 200 surface.

---

### 5. Configuration (`config.h`)

`BOARD_TYPE` selects the hardware target; only `BOARD_TYPE_VGA32` is shipped in this build. Compile-time settings:

| Setting | Value | Description |
|---------|-------|-------------|
| BOARD_TYPE | BOARD_TYPE_VGA32 | TTGO VGA32 v1.4 |
| MACHINE_TYPE | 4 (CoCo 3) or 3 (CoCo 2) | Compile-time default; runtime-switchable via NVS |
| CPU_VARIANT | 0 | MC6809 |
| RAM_SIZE_KB | 512 (CoCo 3) | |
| CPU_CLOCK_HZ | 895000 | 0.895 MHz NTSC |
| TARGET_FPS | 60 | NTSC timing |
| SCANLINES_PER_FRAME | 262 | NTSC |
| ACTIVE_SCANLINES | 192 | Visible display lines |
| DISPLAY_WIDTH × HEIGHT | 640 × 200 | VGA framebuffer surface |
| AUDIO_SAMPLE_RATE | 22050 | DAC ISR rate |
| USE_PSRAM | 1 | RAM/ROM in PSRAM |

---

### 6. Pin Assignments (fixed by the TTGO VGA32 v1.4 board)

| Function | GPIO | Notes |
|----------|------|-------|
| VGA R0 / R1 | 21 / 22 | Resistor-ladder DAC |
| VGA G0 / G1 | 18 / 19 | Resistor-ladder DAC |
| VGA B0 / B1 | 4 / 5 | Resistor-ladder DAC |
| VGA HSYNC | 23 | |
| VGA VSYNC | 15 | |
| PS/2 Keyboard CLK | 33 | |
| PS/2 Keyboard DATA | 32 | |
| PS/2 Mouse CLK | 26 | (unused) |
| PS/2 Mouse DATA | 27 | (unused) |
| SD Card CS | 13 | HSPI |
| SD Card MOSI | 12 | HSPI |
| SD Card MISO | 2 | HSPI (strapping pin — must not be low at boot) |
| SD Card SCK | 14 | HSPI |
| Audio | 25 | ESP32 internal DAC1 |

Joystick GPIOs are not wired on this board; the HAL stub avoids claiming any.

---

### 7. Build System

Arduino-CLI with **ESP32 core 2.0.x** (FabGL 1.0.9 is incompatible with core 3.x / IDF 5):

```bash
arduino-cli config add board_manager.additional_urls \
    https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@2.0.17
arduino-cli lib install FabGL

arduino-cli compile --fqbn esp32:esp32:esp32wrover:PartitionScheme=huge_app \
    TTGO-VGA32-COCO/
arduino-cli upload --fqbn esp32:esp32:esp32wrover:PartitionScheme=huge_app \
    -p /dev/ttyACM0 TTGO-VGA32-COCO/
```

**Dependencies:**
- **FabGL** (VGA driver, PS/2 keyboard driver, PS/2 mouse driver, sound generator, font set)
- **SD** (ships with the ESP32 core)
- **Preferences** (ships with the ESP32 core, used for NVS)

---

### 8. ROM Requirements

Proprietary ROM images required (not included in source). Place in `/roms/` on the SD card.

All files below are **required**. Missing any will trigger the boot validation halt screen naming the absent file(s).

**CoCo 2:**

| File | Size | Maps to | Description |
|------|------|---------|-------------|
| `bas13.rom` | 8 KB | $A000-$BFFF | Color BASIC 1.3 |
| `extbas11.rom` | 8 KB | $8000-$9FFF | Extended BASIC 1.1 |
| `disk11.rom` | 8 KB | $C000-$DFFF | Disk BASIC 1.1 |

**CoCo 3:**

| File | Size | Maps to | Description |
|------|------|---------|-------------|
| `coco3.rom` | 32 KB | internal | Super Extended Color BASIC 2.0 (CRC 0xb4c88d6c) |
| `disk11.rom` | 8 KB | $C000-$DFFF | Disk BASIC 1.1 |

---

### 9. Performance Characteristics

- CPU emulation: ~20-25 fps on CoCo 3 GIME hi-res content (sustained by emulation core, not display)
- Display: FabGL DMA scans 640×200 @ 70 Hz continuously — no SPI push step on the critical path
- Audio: Real-time via DAC1 timer ISR at 22050 Hz; pitch-corrected scanline buffer
- Memory: ~160 KB SRAM free, ~3.5 MB PSRAM free after init

---

### 10. Data Flow Diagrams

#### Frame Execution Flow
```
machine_run_frame()
  for scanline = 0..261:
    sv_disk_tick()              // Deferred INTRQ countdown
    mc6809_run(~57 cycles)      // CPU executes instructions
      check_interrupts()        //   NMI > FIRQ > IRQ dispatch
      execute_one()             //   Fetch-decode-execute
        machine_read/write()    //   Memory/IO dispatch
    if scanline < active_height:
      tcc1014_render_scanline() // GIME renders line to RGB565 buffer (CoCo 3)
       OR
      mc6847_render_scanline()  // VDG renders line to palette-index buffer (CoCo 2)
      hal_video_render_scanline_gime / hal_video_render_scanline
                                // Convert + write into FabGL scanline
    advance_video_counters()
  // No "present" step — FabGL DMA is already scanning the framebuffer.
```

#### Keyboard Input Flow
```
PS/2 Keyboard (physical)
  → ESP32 ULP scancode shifter (FabGL)
  → Scancode-to-VK converter task (FabGL, Core 0)
  → VirtualKey queue
  → hal_keyboard_tick() on Core 1 (called from hal_process_input each frame)
  → process_vk(VirtualKeyItem)
  → Layer 1: supervisor / hotkey gate (F3, F4, F5, F6)
  → Layer 2: VK_MAP[] lookup → (CoCo col, CoCo row, needs_shift, suppress_shift)
  → set_key(col, row, pressed) modifies key_matrix[]
  → defer_release(col, row) keeps key held for MIN_HOLD_FRAMES on release
  → PIA0 reads key_matrix[col] when CPU scans the keyboard
```

#### Disk Read Flow (DSKCON)
```
BASIC: LOAD"FILE"
  → Disk BASIC ROM reads directory (T17, S3-11)
  → For each sector:
    1. CPU writes track/sector to FDC registers ($FF49/$FF4A)
    2. CPU writes READ SECTOR command to $FF48
       → fdc_execute_command(): reads 256 bytes from SD into sector_buf
       → Sets reading=true, drq=true, data=sector_buf[0]
    3. CPU writes DSKREG ($FF40) to enable HALT + NMI
    4. CPU enters tight loop: LDA $FF4B / STA ,X+ / BRA loop
       → Each LDA $FF4B calls fdc_read_data()
       → Returns current byte, advances buf_pos, pre-loads next
    5. After 256th byte: sets intrq_defer=1 (not immediate NMI)
    6. CPU executes STA ,X+ (stores last byte)
    7. Next scanline: sv_disk_tick() fires set_intrq() → NMI
    8. NMI handler returns to DSKCON, which sets up next sector
```

---

### 11. Source File Reference

```
TTGO-VGA32-COCO/
  TTGO-VGA32-COCO.ino  Main sketch (setup + loop)
  config.h                    Hardware config, pin assignments, BOARD_TYPE switch

  src/core/
    machine.cpp/.h            System integration, memory map, frame loop
    mc6809.cpp/.h             MC6809 CPU emulation
    mc6809_opcodes.h          Opcode tables
    mc6821.cpp/.h             MC6821 PIA (2 instances)
    mc6847.cpp/.h             MC6847 VDG (CoCo 2)
    sam6883.cpp/.h            SAM6883 address mux (CoCo 2)
    tcc1014.cpp/.h            TCC1014 GIME (MMU, video, interrupts; CoCo 3)

  src/hal/
    hal.h                     HAL interface
    hal.cpp                   hal_init, hal_process_input
    hal_video.cpp             FabGL VGAController @ 640×200 @ 70 Hz
    hal_audio.cpp             Internal DAC1 (GPIO25), timer ISR
    hal_keyboard.cpp          FabGL PS/2 → CoCo matrix + deferred release
    hal_joystick.cpp          Neutral stub (hardware not wired)
    hal_storage.cpp           SD card on HSPI
    osd_canvas.h/.cpp         OSD drawing canvas for supervisor (backed by FabGL Canvas)

  src/supervisor/
    supervisor.cpp/.h         OSD lifecycle, state machine
    sv_menu.cpp/.h            Main menu rendering + key handling
    sv_filebrowser.cpp/.h     SD card directory browser
    sv_disk.cpp/.h            WD1793 FDC emulation + INTRQ/NMI
    sv_render.cpp/.h          OSD rendering engine (green phosphor theme)

  src/tests/
    integration_test.cpp/.h   Automated test framework

  src/utils/
    debug.h                   DEBUG_PRINT / DEBUG_PRINTF macros

  src/roms/
    rom_loader.cpp/.h         ROM file loading utilities
```
