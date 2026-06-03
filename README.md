# TTGO-VGA32-COCO — CoCo 2 & CoCo 3 Emulator for the LilyGo TTGO VGA32

A full **TRS-80 Color Computer** (CoCo 2 and CoCo 3) emulator running on the **LilyGo TTGO VGA32 v1.4** board (ESP32-WROVER). Ported from the [XRoar](http://www.6809.org.uk/xroar/) emulator by Ciaran Anscomb.

**Beta-3 — May 2026** (LilyGo TTGO VGA32 port)

## Features

- **Complete MC6809 CPU** emulation with accurate cycle counts
- **CoCo 2 mode:** MC6847 VDG video — text and all semigraphics/graphics modes
- **CoCo 3 mode:** TCC1014 GIME — 512 KB RAM with MMU, 16-color palette, native graphics up to 640px
- **Dual 6821 PIA** chips for keyboard, joystick, and audio I/O
- **SAM6883** address multiplexer (CoCo 2) / GIME integrated MMU (CoCo 3)
- **WD1793 floppy disk controller** with `.DSK` and `.VDK` image support
- **VGA output** at 640×200 @ 70 Hz via FabGL (64-color direct mode)
- **PS/2 keyboard** input with selectable layout (Spanish by default; US/UK/German/Italian/French/etc. supported)
- **Audio** via the ESP32 internal 8-bit DAC on GPIO25 (3.5 mm jack)
- **On-Screen Display (OSD)** supervisor for disk mounting, machine reset, and status
- **PSRAM disk caching** — entire disk images loaded into PSRAM for zero-latency access
- **Runtime CoCo 2 / CoCo 3 switching** — chosen at boot from NVS, settable from the supervisor menu

## Hardware Requirements

- **LilyGo TTGO VGA32 v1.4** (ESP32-WROVER-E, 4 MB PSRAM, 4 MB flash)
- **VGA monitor** capable of 640×200 @ 70 Hz (most VGA CRTs and adapters; some modern LCDs accept this mode, others won't sync)
- **PS/2 keyboard** plugged into the board's mini-DIN PS/2 jack
- **MicroSD card** (FAT32 formatted) inserted in the on-board socket
- **3.5 mm audio output** (mono) on the board's jack
- **5 V USB-C** for power and serial programming

> The board provides VGA, PS/2, audio jack, and SD socket on-board — no extra wiring is required. **Joystick hardware is not wired** on the v1.4 board for this build; the joystick HAL is a neutral stub.

## Fixed Pin Map (TTGO VGA32 v1.4)

| Function | Pin(s) | Notes |
|---|---|---|
| VGA Red (R0, R1) | GPIO21, GPIO22 | Resistor-ladder DAC |
| VGA Green (G0, G1) | GPIO18, GPIO19 | Resistor-ladder DAC |
| VGA Blue (B0, B1) | GPIO4, GPIO5 | Resistor-ladder DAC |
| VGA HSYNC | GPIO23 | |
| VGA VSYNC | GPIO15 | |
| PS/2 Keyboard CLK | GPIO33 | |
| PS/2 Keyboard DATA | GPIO32 | |
| PS/2 Mouse CLK | GPIO26 | (unused) |
| PS/2 Mouse DATA | GPIO27 | (unused) |
| SD Card CS | GPIO13 | HSPI |
| SD Card MOSI | GPIO12 | HSPI |
| SD Card MISO | GPIO2 | HSPI (note: GPIO2 is a strapping pin — must float/high at boot) |
| SD Card SCK | GPIO14 | HSPI |
| Audio DAC | GPIO25 | ESP32 internal DAC1 |

## SD Card Setup

Format the MicroSD as **FAT32** and create the following structure:

### CoCo 2 ROMs

```
/roms/
├── bas13.rom          # Color BASIC 1.3 (required)
├── extbas11.rom       # Extended BASIC 1.1 (required)
└── disk11.rom         # Disk BASIC 1.1 (required for floppy support)
```

| File           | Size | Description                      |
|----------------|------|----------------------------------|
| `bas13.rom`    | 8 KB | Color BASIC 1.3 (primary)        |
| `extbas11.rom` | 8 KB | Extended BASIC 1.1 (primary)     |
| `disk11.rom`   | 8 KB | Disk BASIC 1.1 (primary)         |

### CoCo 3 ROMs

```
/roms/
├── coco3.rom          # Super Extended Color BASIC 2.0 (32 KB, required)
└── disk11.rom         # Disk BASIC (8 KB external cartridge ROM, starts with 'DK')
```

| File         | Size  | Description |
|--------------|-------|-------------|
| `coco3.rom`  | 32 KB | Super Extended Color BASIC (internal ROM, CRC: 0xb4c88d6c) |
| `disk11.rom` | 8 KB  | Disk BASIC cartridge ROM (must start with 'DK' signature at $C000) |

ROM files are validated by CRC-32 on startup.

### Supported Disk Formats

- **`.DSK`** (JVC format) — fully supported
- **`.VDK`** (Virtual Disk with 12-byte header) — fully supported

## Build & Flash

### 1. Install Arduino-CLI (or Arduino IDE 2.x)

This guide uses `arduino-cli`. The same FQBN and library list applies inside Arduino IDE.

### 2. ESP32 Board Support — **must be core 2.0.x**

FabGL 1.0.9 is **not compatible** with Arduino-ESP32 core 3.x (ESP-IDF 5). The board package URL and install command:

```bash
arduino-cli config add board_manager.additional_urls \
    https://espressif.github.io/arduino-esp32/package_esp32_index.json
arduino-cli core update-index
arduino-cli core install esp32:esp32@2.0.17
```

### 3. Install Required Libraries

```bash
arduino-cli lib install FabGL
```

`FabGL` provides VGA, PS/2 keyboard, and DAC drivers in one package. `SD` and `Preferences` ship with the ESP32 core.

### 4. Configure (optional)

Edit `config.h` if you want to change:

- `MACHINE_TYPE` — default compile-time machine (4 = CoCo 3, 3 = CoCo 2). The active machine is also runtime-switchable from the supervisor menu, persisted to NVS.
- Keyboard layout — edit `hal_keyboard.cpp` `hal_keyboard_init()` to pick a different `fabgl::XxxLayout` (US, UK, German, Italian, **Spanish** (default), French, Belgian, Norwegian, Japanese).

### 5. Compile & Upload

From the `coco3/` directory:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32wrover:PartitionScheme=huge_app \
    TTGO-VGA32-COCO/

arduino-cli upload --fqbn esp32:esp32:esp32wrover:PartitionScheme=huge_app \
    -p /dev/ttyACM0 TTGO-VGA32-COCO/

arduino-cli monitor -p /dev/ttyACM0 -c baudrate=115200
```

If `Failed to connect to ESP32: Wrong boot mode detected`, hold the **BOOT** button on the board until the "Connecting..." dots appear, then release.

## Keyboard Controls

| Key | Function |
|-----|----------|
| F1  | Forwarded to CoCo 3 matrix (PA6,PB5) |
| F2  | Forwarded to CoCo 3 matrix (PA6,PB6) |
| F3  | Toggle OSD (supervisor menu) |
| F4  | Machine reset (flushes dirty disks first) |
| F5  | Toggle FPS overlay |
| F6  | Quick-mount last disk |
| ESC | BREAK |
| Backspace | LEFT ARROW (BASIC line edit) |
| Insert / Delete | CLEAR |

PS/2 keyboard layout defaults to **Spanish**. Change at the top of `hal_keyboard_init()` to any of FabGL's built-in layouts (US, UK, German, Italian, French, Belgian, Norwegian, Japanese).

The shifted-symbol row is handled by FabGL's layout (it pre-resolves SHIFT+digit into `VK_HASH`, `VK_DOLLAR`, etc.); the HAL maps each symbol back to the corresponding CoCo SHIFT+N combination so `#`, `$`, `%`, `&`, `(`, `)`, `!`, `"`, `'`, `?`, `<`, `>`, `+` all produce the right CoCo character.

## On-Screen Display (OSD)

Press **F3** to open the supervisor overlay. From here you can:

- **Mount/Eject Disks** — browse the SD card and mount `.DSK`/`.VDK` images to drives 0–3
- **Disk Manager** — view mounted drives and eject disks
- **Machine** — switch between CoCo 2 / CoCo 3 (`esp_restart()` after confirm; persisted to NVS)
- **Reset Machine** — warm or cold reset with confirmation
- **About** — version info and free memory

> The supervisor's OSD was originally written against the TFT_eSPI API. Under this VGA32 port it draws into the FabGL framebuffer through `src/hal/tft_compat.{h,cpp}` — a thin shim that re-implements the subset of TFT_eSPI primitives the supervisor uses (`fillRect`, `drawString`, `setTextFont`, datum-based alignment, etc.) on top of FabGL's `Canvas`. The supervisor code itself is unchanged.

## Architecture

The emulator runs on the ESP32-WROVER's **dual cores**, though under FabGL the video DMA is what consumes Core 1 timing — the emulation lives alongside it.

- **Core 0** — FabGL VGA scan-out timing and PS/2 ULP polling
- **Core 1** — Main emulation loop (CPU, video scanline render, audio, OSD)

### Emulation Loop

```
loop():
  1. hal_process_input()        — drain FabGL VK queue, tick deferred releases
  2. supervisor_update_and_render()  — if OSD active, render and return
  3. machine_run_frame()        — emulate 262 scanlines (14,916 CPU cycles)
  4. hal_render_frame()         — no-op on VGA32; FabGL DMA scans out continuously
```

### CoCo 2 Memory Map

```
$0000–$7FFF   RAM (up to 64 KB with SAM paging)
$8000–$9FFF   Extended BASIC ROM (8 KB)
$A000–$BFFF   Color BASIC ROM (8 KB)
$C000–$FEFF   Cartridge / Disk BASIC ROM (16 KB)
$FF00–$FF3F   PIA registers (keyboard, audio, VDG control)
$FF40–$FF5F   Disk controller (WD1793)
$FFC0–$FFDF   SAM control registers
```

### Architecture Diagrams

- `docs/ESP32_COCO_CORE.png` — CPU, memory map, PIA, VDG, SAM wiring
- `docs/Architecture.jpg` — system block diagram

## Project Structure

```
TTGO-VGA32-COCO.ino  Main Arduino sketch (setup/loop)
config.h                    Hardware and build configuration (BOARD_TYPE switch)
src/
├── core/                   Emulation core (HAL-agnostic)
│   ├── machine.h/cpp         CoCo machine — memory map, chip wiring, interrupts
│   ├── mc6809.h/cpp          MC6809 CPU — full opcode set with cycle-accurate timing
│   ├── mc6809_opcodes.h      Opcode table and cycle counts
│   ├── mc6821.h/cpp          6821 PIA — peripheral I/O
│   ├── mc6847.h/cpp          MC6847 VDG — scanline video rendering (CoCo 2)
│   ├── tcc1014.h/cpp         TCC1014 GIME — MMU, video, interrupts (CoCo 3)
│   └── sam6883.h/cpp         SAM — address multiplexing and clock control
├── hal/                    Hardware Abstraction Layer
│   ├── hal.h/cpp             HAL dispatcher (init, input, render)
│   ├── hal_video.cpp         FabGL VGAController (640×200 @ 70 Hz, 64-color)
│   ├── hal_audio.cpp         Internal DAC1 on GPIO25 via timer ISR
│   ├── hal_keyboard.cpp      FabGL PS/2 VirtualKey → CoCo matrix mapping
│   ├── hal_joystick.cpp      Neutral stub — hardware not wired
│   ├── hal_storage.cpp       SD card access on dedicated HSPI
│   └── tft_compat.h/cpp      TFT_eSPI API shim backed by FabGL Canvas (supervisor OSD)
├── supervisor/             On-Screen Display system (HAL-agnostic via tft_compat)
│   ├── supervisor.h/cpp      OSD lifecycle and state machine
│   ├── sv_menu.h/cpp         Menu definitions and actions
│   ├── sv_disk.h/cpp         WD1793 FDC emulation and PSRAM cache
│   ├── sv_filebrowser.h/cpp  SD card file browser
│   └── sv_render.h/cpp       OSD rendering (green phosphor theme)
├── roms/
│   └── rom_loader.h/cpp      ROM loading with CRC-32 validation
├── tests/
│   └── integration_test.h/cpp  LOADM binary verification
└── utils/
    └── debug.h               Debug output macros
```

## Documentation

All technical documentation is in the `docs/` directory:

| File | Description |
|------|-------------|
| [Architecture.md](docs/Architecture.md) | System architecture — CoCo 2 and CoCo 3 extensions on TTGO VGA32 |
| [core.md](docs/core.md) | MC6809 CPU, MC6821 PIA, MC6847 VDG, SAM6883, GIME machine integration |
| [coco3-gime.md](docs/coco3-gime.md) | CoCo 3 GIME porting guide, register map, MMU |
| [disk-hal.md](docs/disk-hal.md) | WD1793 FDC emulation, HALT/NMI flow, PSRAM disk cache |
| [keyboard-hal.md](docs/keyboard-hal.md) | PS/2 → CoCo matrix mapping (note: older revisions describe the USB HID variant) |
| [supervisor.md](docs/supervisor.md) | OSD state machine, file browser, NVS persistence |
| [video.md](docs/video.md) | Video rendering pipeline, scale modes, palette |
| [audio-hal.md](docs/audio-hal.md) | Audio path, ISR, PIA DAC routing |
| [joystick-hal.md](docs/joystick-hal.md) | Analog joystick emulation (S3 history; current VGA32 build stubs the HAL) |
| [performance.md](docs/performance.md) | FPS analysis, optimization log, methodology |
| [cpu-enhancements.md](docs/cpu-enhancements.md) | MC6809 correctness/performance analysis |
| [runtime-machine-switch.md](docs/runtime-machine-switch.md) | Runtime CoCo 2 / CoCo 3 switching, NVS state |
| [dragon-support.md](docs/dragon-support.md) | Future: Dragon 32 support plan |
| [os9-keys-issue.md](docs/os9-keys-issue.md) | NitrOS-9 keyboard compatibility notes |
| [xroar-analysis.md](docs/xroar-analysis.md) | Original XRoar 0.32.0 port analysis (historical reference) |

## Known Limitations

- VGA mode `640×200 @ 70 Hz` may not sync on some modern LCD monitors — older CRTs and most VGA→HDMI scalers accept it
- DMK disk format is recognized but not mountable
- Max 128 file entries in the SD card browser
- Joystick hardware is not wired on the TTGO VGA32 v1.4 build; HAL stub returns centered, button-released
- Audio frequency does not exactly match original CoCo hardware
- NTSC TV emulation is a stub only
- Supervisor OSD was sized for 320×240; on the 640×200 VGA surface its layout sits in the upper-left region

## Credits

- **Reinaldo Torres / CoCo Byte Club** — ESP32 port and hardware design
- **Ciaran Anscomb** — [XRoar](http://www.6809.org.uk/xroar/) CoCo/Dragon emulator (original source)
- **Claude Code (Anthropic)** — co-development of the ESP32 port
- **Fabrizio Di Vittorio** — [FabGL](http://www.fabgl.com/) VGA / PS/2 / DAC library

## License

This project is licensed under the **MIT License**. See [LICENSE](LICENSE) for details.
