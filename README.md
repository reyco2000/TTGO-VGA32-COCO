# ESP32 CoCo 2 & CoCo 3 Emulator for the ESP BOARD LilyGo TTGO VGA32

![TTGO-VGA32-COCO](TTGOVGACOCO.png)

A full **TRS-80 Color Computer** (CoCo 2 and CoCo 3) emulator running on the ESP32  **[LilyGo TTGO VGA32 v1.4](https://lilygo.cc/en-us/products/fabgl-vga32?_pos=1&_sid=4c095f59b&_ss=r)** board (ESP32-WROVER). Inspired on  [XRoar](http://www.6809.org.uk/xroar/) emulator.

**v0.20.0 — October 24, 2026** (LilyGo TTGO VGA32 port)

## Features

- **One firmware, two CoCos** — CoCo 2 and CoCo 3 live in the same binary. Pick your machine at boot from NVS or flip it live in the supervisor menu.
- **Cycle-accurate to the chip** — full MC6809 CPU emulation with accurate cycle counts, faithful enough to run the software that matters.
- **MC6809 or HD6309** — switch the CPU from Setup → Machine → CPU. The Hitachi 6309 adds its extra registers and instructions, native mode and traps (new in v0.15.0; boots NitrOS-9 for the 6309 since v0.18.0).
- **Program cartridges** — load a `.ccc` or `.rom` cartridge ROM from the SD card with the supervisor's **Cart** menu: up to 16 KB on the CoCo 2, and bank-switched ROMs up to 128 KB on the CoCo 3 (new in v0.16.0).
- **Orchestra-90/CC** — the stereo music cartridge's two DACs, mixed to mono on the audio jack, with its ROM in the cartridge slot.
- **Speech/Sound Cartridge** — Tandy's talking cartridge, fully emulated (TMS7040 firmware, SP0256-AL2 speech, AY-3-8913 sound) on the ESP32's second core, alongside Disk BASIC. Needs two ROM dumps (new in v0.17.0).
- **Authentic video, both eras** — MC6847 VDG for CoCo 2 (text plus every semigraphics and graphics mode) and the TCC1014 GIME for CoCo 3 (128 KB to 2 MB RAM with MMU, 16-color palette, native graphics up to 640 px), output over crisp VGA at 640×200 @ 60 Hz via FabGL in 64-color direct mode.
- **Real-time speed** — the CoCo 3 runs at 1.79 MHz in fast mode (SAM R1), as NitrOS-9 expects, with a built-in speed meter (new in v0.19.0). CoCo 3 text and graphics modes run at a paced 60 FPS, the speed of the real machine (v0.12.0 rewrote the GIME video path; it used to manage ~39 FPS at the BASIC prompt and ~20 FPS in graphics).
- **Real disk drives** — WD1793 floppy controller with `.DSK` and `.VDK` support, and entire disk images cached in PSRAM for zero-latency access.
- **Virtual hard disk** — VCC's hard disk device at `$FF80`: two `.VHD` images of up to 512 MB, read in place from the SD card. Boots NitrOS-9 (EOU) with the `emudsk` driver, and RGB-DOS gives BASIC its virtual drives (new in v0.18.0).
- **Complete hardware soul** — dual 6821 PIAs (keyboard, joystick, audio I/O), SAM6883 multiplexer on CoCo 2, GIME-integrated MMU on CoCo 3.
- **FujiNet support** — through an external DriveWire / FujiNet server over WiFi; see [DriveWire / FujiNet Support](#drivewire--fujinet-support).

### Built for Real Use

- **Multi-language keyboards** — PS/2 input with US English and Spanish Latam layouts, switchable live from Setup → Keyboard with no reboot.
- **Remap on a picture of the keyboard** — the supervisor Key Mapper draws the running machine's keyboard (CoCo 2 or CoCo 3); move over it with the arrow keys and bind a physical key to the CoCo symbol keys, the arrows and the CoCo 3-only ALT, CTRL, CLEAR, F1 and F2.
- **Joystick via PS/2 mouse** — with live, on-screen sensitivity tuning persisted in NVS.
- **Real audio out** — ESP32 internal 8-bit DAC on GPIO25, straight to a 3.5 mm jack.
- **Supervisor OSD** — an icon menu in the CoCo's own green, black and dark blue: mount disks, load cartridges, reset the machine, change settings, and check status without ever leaving your seat.
- **Online debugging via WiFi + MCP (experimental)** — a WiFi debug server exposes a small HTTP/JSON API to inspect and control the running 6809 (registers, memory, pause/resume, code injection, reset, machine switch, screenshot). A host-side **MCP bridge** lets Claude / LLM agents drive it live. Join your network from the supervisor's **WiFi / Debug** screen (pick it from a list and type the password, or load it from `cocowifi.cfg` on the SD card); see [docs/wifi-debug.md](docs/wifi-debug.md) and [`mcp-bridge/`](mcp-bridge/).
- **Experimental RS-232 Pak support** — for the serial tinkerers.

## Hardware Requirements

- **[LilyGo TTGO VGA32 v1.4](https://lilygo.cc/en-us/products/fabgl-vga32?_pos=1&_sid=4c095f59b&_ss=r)** (ESP32-WROVER-E, 4 MB PSRAM, 4 MB flash)
- **VGA monitor** capable of 640×200 @ 60 Hz (most VGA CRTs and adapters; some modern LCDs accept this mode, others won't sync)
- **PS/2 keyboard** plugged into the board's mini-DIN PS/2 jack
- **MicroSD card** (FAT32 formatted) inserted in the on-board socket
- **3.5 mm audio output** (mono) on the board's jack
- **5 V USB-C** for power and serial programming

> The board provides VGA, PS/2, audio jack, and SD socket on-board — no extra wiring is required. **Joystick 1 is emulated via the PS/2 mouse port** (GPIO26/27): plug a PS/2 mouse into the board's mouse header and it drives the CoCo right joystick. Joystick 2 is a neutral stub. Pin assignments: [LilyGo TTGO VGA32 v1.4 Pinout](docs/pinout.md).

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

### DriveWire / FujiNet ROMs (optional)

Only needed if you use the [DriveWire / FujiNet support](#drivewire--fujinet-support)
below. Get them from [toolshed](https://github.com/n6il/toolshed)'s `hdbdos` build:

```
/roms/
├── hdbdw3bck.rom      # HDB-DOS 1.4, Becker port, CoCo 2 (8 KB)
├── hdbdw3bc3.rom      # HDB-DOS 1.4, Becker port, CoCo 3 (8 KB)
├── hdbdw3bckt.rom     # same, with a 2 s DriveWire timeout — CoCo 2 (optional)
└── hdbdw3bc3t.rom     # same, with a 2 s DriveWire timeout — CoCo 3 (optional)
```

These replace `disk11.rom` only while the DriveWire bus is enabled; with the
bus Off, `disk11.rom` loads as usual and floppy support is unaffected.

### Cartridge ROMs (optional)

Program cartridges are `.ccc` or `.rom` files. The supervisor's **Cart** menu
opens in a `/carts` folder if the card has one (the card root otherwise) and
can browse to any other folder:

```
/carts/
├── game.ccc           # up to 16 KB on the CoCo 2
└── biggame.ccc        # up to 128 KB (bank-switched) on the CoCo 3
```

The Speech/Sound Cartridge (Setup → Multi-Pak → Speech/Sound) needs two ROM
dumps in `/roms/`, with the MAME names: `pic-7040-510.bin` (4096 bytes) and
`sp0256-al2.bin` (2048 bytes). See [docs/ssc.md](docs/ssc.md).

The hard disk's RGB-DOS mode (Setup → Multi-Pak → Hard Disk) needs VCC's
`rgbdos.rom` in `/roms/`. Its RS-DOS mode uses `disk11.rom`. See
[docs/vhd.md](docs/vhd.md).

### If the Emulator Does Not Start

With no SD card, or a card that is not formatted FAT, the screen shows a
warning and asks for a FAT-formatted card. With a required ROM missing, it
names the missing files and the `/roms` folder they belong in. Fix the card
and restart.

### Supported Disk Formats

- **`.DSK`** (JVC format) — fully supported
- **`.VDK`** (Virtual Disk with 12-byte header) — fully supported
- **`.VHD`** (raw hard disk image, VCC format) — on HD0/HD1 while the hard disk is on

## Build & Flash

### Quick Flash (Pre-built Firmware)

If you just want to flash the emulator without building from source, use the pre-built firmware binary and the browser-based ESP Web Flasher:

1. Connect your TTGO VGA32 board via USB
2. Open [ESP Web Tool](https://esptool.spacehuhn.com/) in a Chrome or Edge browser
3. Click **Connect** and select the board's serial port
4. Set the flash offset to **0x0000**
5. Choose the file `TTGO-VGA32-CoCo-0.12.2-firmware.bin` from this repository
6. Click **Program** and wait for the flash to complete

> Hold the **BOOT** button on the board while clicking Connect if the browser cannot reach the device.

Once flashed, prepare your SD card with the required ROM files (see [SD Card Setup](#sd-card-setup) above) and power-cycle the board.

---

### Running under ESP32_Bootloader (SD-card menu)

Since **v0.9.0** the emulator also ships as an [ESP32_Bootloader](https://github.com/ESP-WORKS/ESP32_Bootloader) payload, so it can share one board with other TTGO VGA32 emulators and be picked from an on-screen menu instead of being reflashed over USB.

Flash ESP32_Bootloader once (it lives in the `factory` partition), then put the release's `firmware.bin` and `version.txt` in a **`CoCo/`** folder on the card root — the folder name is what the menu lists:

```
/ (SD card root)
├── CoCo/
│   ├── firmware.bin      bare app image (NOT the merged -firmware.bin)
│   └── version.txt       one line; change it to force a reflash
└── roms/
    ├── coco3.rom
    └── disk11.rom        (plus bas13.rom / extbas11.rom for CoCo 2)
```

The ROMs and disk images stay in their usual places on the same card — the bootloader's card is also the emulator's card.

On selecting **CoCo**, the bootloader flashes the image into `ota_0` (only when `version.txt` differs from what it flashed last) and boots it. The emulator blanks `otadata` as the very first thing it does, so the **next power-cycle returns to the menu** rather than booting straight back in.

> **Use the right file.** `firmware.bin` from the SD package is the bare app image; `TTGO-VGA32-CoCo-<version>-firmware.bin` is the merged USB image and will *not* work from the menu. A bare image starts with byte `0xE9` — the build script checks this for you.

Notes and limits of this mode:

- The app must fit `ota_0` — **2816 KB**; v0.9.0 uses roughly 1.1 MB, so there is plenty of room.
- NVS (settings, key mappings, WiFi credentials, mounted disks) is shared with the bootloader and survives normally, but a full flash erase clears it.
- Nothing else changes: the standalone USB build is still the default and is unaffected.

---

### Building from Source

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
- `KBD_LAYOUT_FIRST_BOOT_DEFAULT` — initial PS/2 keyboard layout (0 = US English, 1 = Spanish Latam) used the first time the device boots. After that the layout is runtime-switchable (live, no reboot) from the supervisor Setup → Keyboard menu and persisted to NVS.

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

### 6. Building Release Images

`tools/build_firmware.sh` builds **both** distributable flavours from unmodified sources — they differ only by a command-line define, so there is nothing to edit between them:

```bash
tools/build_firmware.sh                     # both, version read from config.h
tools/build_firmware.sh 0.9.0               # both, explicit version
tools/build_firmware.sh 0.9.0 standalone    # merged USB image only
tools/build_firmware.sh 0.9.0 bootloader    # SD-card package only
```

| Output | What it is | How it's flashed |
|--------|-----------|------------------|
| `TTGO-VGA32-CoCo-<version>-firmware.bin` | Merged image: bootloader + partition table + `boot_app0` + app | USB, at offset **`0x0`** |
| `build/sdcard/CoCo/firmware.bin` | Bare app image (`-DBUILD_TARGET=1`) | By ESP32_Bootloader, into `ota_0` |
| `build/sdcard/CoCo/version.txt` | `<version>` (bare, e.g. `0.12.2`) | Read by the bootloader to decide whether to reflash |

The script verifies what the binaries actually contain before publishing them: the SD image really starts with `0xE9` and fits `ota_0`, and — by disassembling `setup()` in each ELF — that the `otadata` erase is **present** in the bootloader build and **absent** from the standalone one. That last check matters because a define that silently fails to reach the sketch produces a bootloader package that never returns to the menu.

Version strings come from `FIRMWARE_VERSION` in `config.h`, which is also what the About screen and `/api/status` report. `version.txt` additionally folds in `git describe --tags --always --dirty`, so two builds of the same version are never mistaken for each other by the bootloader. Tag before building a release, and don't ship a `-dirty` string.

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

PS/2 keyboard layout defaults to **US English** and can be switched live (no reboot) to **Spanish Latam** from the supervisor **Setup → Keyboard → Keyboard Language** row; the choice is persisted in NVS.

The shifted-symbol row is handled by FabGL's layout (it pre-resolves SHIFT+digit into `VK_HASH`, `VK_DOLLAR`, etc.); the HAL maps each symbol back to the corresponding CoCo SHIFT+N combination so `#`, `$`, `%`, `&`, `(`, `)`, `!`, `"`, `'`, `?`, `<`, `>`, `+` all produce the right CoCo character.

### Custom Key Mapping

The supervisor **Setup → Keyboard → Key Mapper** screen draws the keyboard of the running machine — the CoCo 2 or the CoCo 3 layout — and you move over it with the arrow keys:

- **White** keys can be remapped, **cream** keys are fixed, **orange** keys already have a custom mapping. Two lines under the keyboard show what the selected key carries and its current binding.
- **ENTER** remaps the selected key: press the physical key you want, then confirm. Keys with two remappable characters (`:` / `*`, `;` / `+`, ...) first ask which one. **DEL** clears a mapping.
- **T** opens a test screen that shows what each pressed key would type without sending it to the emulated CoCo; **C** clears every mapping after a confirmation.

The remappable keys are the symbol keys, the arrows and, on a CoCo 3, ALT, CTRL, CLEAR, F1 and F2. Mappings persist in NVS.

## On-Screen Display (OSD)

![CoCo Supervisor main menu](pictures/cocosupervisor1.jpg)

Press **F3** to open the supervisor: a box in the CoCo's green, black and dark blue over the paused emulator picture. The main menu is a grid of eight icon tiles — all four arrows move, **ENTER** opens a tile, **ESC** goes back one level and **F3** closes the menu from anywhere. Questions and confirmations appear as small white popups and always start on **No**.

- **Disks** — Disk Manager. A button per drive (0–3) shows what is mounted, plus HD0 and HD1 while the hard disk is on; the SD card browser sits below. `.VHD` images mount on HD0/HD1. **TAB** switches between drives and files. ENTER on a `.DSK`/`.VDK` image asks which drive takes it (the first empty one is preselected) and confirms before replacing a disk or mounting the same image twice; ENTER on a mounted drive asks to unmount it. **F** flushes cached writes to the SD card.
- **Cart** — Load Cart. Browse the SD card for a `.ccc` or `.rom` cartridge ROM and press ENTER on it; **Unload cartridge** removes it. The cartridge takes effect at a restart, so a popup asks whether to restart now or keep the choice for the next restart. The cartridge takes the place of Disk BASIC, so the disk drives are not usable from BASIC until it is unloaded.
- **Setup**
  - **Machine** — *Choose Machine Type* (CoCo 2 / CoCo 3), *CPU* (pick 6809 or 6309) and *CoCo 3 Memory* (128K–2048K; shows the fixed 64K as *CoCo 2 Memory* on a CoCo 2). Each change restarts; persisted to NVS
  - **Keyboard** — *Keyboard Language* (US English / Spanish Latam, live switch) and the *Key Mapper*, which draws the running machine's keyboard (see [Custom Key Mapping](#custom-key-mapping))
  - **Joy - Mouse Sensitivity** — joystick 1 / PS/2 mouse, with a live cursor pad
  - **Joy-Keyboard Emulation** — OFF / RIGHT / LEFT: the arrow keys glide that CoCo joystick (it stays put on release, like the mouse) and Space fires (live; while on they no longer reach the CoCo keyboard)
  - **WiFi / Debug** — *Config WiFi* scans for networks and takes the password from the keyboard; *Read WiFi from SD Card* loads it from `cocowifi.cfg`; plus connect, disconnect, forget, and the debug server switch (see [docs/wifi-debug.md](docs/wifi-debug.md))
  - **DriveWire** — *External Server* on/off with host, port and HDB-DOS ROM (FujiNet support — see below)
  - **Multi-Pak - Insert Cartridges** — *Orchestra-90* and *Speech/Sound* on/off, *Hard Disk*: Off, On booting RS-DOS (NitrOS-9 compatible), or On booting RGB-DOS (each change restarts), and *RS-232 Pak* on/off (live)
- **WiFi** — the same WiFi / Debug screen as in Setup, one step from the main menu
- **Reset** — reset the machine, with confirmation
- **Debug** — CPU / GIME status, memory hex dump, RS-232 Pak status, Dump RAM to SD, and the **Echo Log** toggle (debug output on the serial port; it shares the port with the RS-232 Pak, so turning one on turns the other off)
- **About** — version info and free memory
- **Resume** — back to the emulator

## Architecture

How the emulator is split across the ESP32's two cores, the main loop, the
memory maps and every module: [docs/Architecture.md](docs/Architecture.md).

## Project Structure

```
TTGO-VGA32-COCO.ino  Main Arduino sketch (setup/loop)
config.h                    Hardware and build configuration (BOARD_TYPE switch)
src/
├── core/                   Emulation core (HAL-agnostic)
│   ├── machine.h/cpp         CoCo machine — memory map, chip wiring, interrupts
│   ├── mc6809.h/cpp          MC6809 CPU — full opcode set with cycle-accurate timing
│   ├── mc6809_core_impl.h    CPU interpreter body, compiled once per CPU (6809 and 6309)
│   ├── mc6809_opcodes.h      Opcode table and cycle counts
│   ├── hd6309.cpp            HD6309 CPU — the same interpreter built with the 6309 additions
│   ├── hd6309_ops.h          HD6309-only instructions, traps and native-mode timing
│   ├── mc6821.h/cpp          6821 PIA — peripheral I/O
│   ├── mc6847.h/cpp          MC6847 VDG — scanline video rendering (CoCo 2)
│   ├── mc6551.h/cpp          MC6551 ACIA — RS-232 Pak chip emulation ($FF68-$FF6B)
│   ├── tcc1014.h/cpp         TCC1014 GIME — MMU, video, interrupts (CoCo 3)
│   ├── sam6883.h/cpp         SAM — address multiplexing and clock control
│   ├── sound.h/cpp           Sound mixing core — mux/DAC/single-bit, XRoar gain model
│   ├── orch90.h/cpp          Orchestra-90/CC cartridge — two DAC latches ($FF7A/$FF7B)
│   ├── ssc/                  Speech/Sound Cartridge ($FF7D/$FF7E) — TMS7040, SP0256-AL2, AY-3-8913, glue; runs on core 0
│   ├── vhd.h/cpp             Virtual hard disk ($FF80-$FF86) — VCC's EmuDsk device, two .VHD drives
│   ├── blockfile.h/cpp       Image files read and written in place, a sector at a time
│   ├── becker.h/cpp          Becker port ($FF41/$FF42) — DriveWire byte channel
│   └── font_gime.h           GIME font ROM — 128 chars × 12 rows (PROGMEM)
├── hal/                    Hardware Abstraction Layer
│   ├── hal.h/cpp             HAL dispatcher (init, input, render)
│   ├── hal_video.cpp         FabGL VGAController (640×200 @ 60 Hz, 64-color)
│   ├── hal_audio.cpp         Internal DAC1 on GPIO25 via timer ISR
│   ├── hal_keyboard.cpp      FabGL PS/2 VirtualKey → CoCo matrix mapping
│   ├── hal_joystick.cpp      PS/2 mouse → CoCo joystick 1 (GPIO26/27); joystick 2 stub
│   ├── hal_rs232.h/cpp       RS-232 HAL — bridges MC6551 ACIA to UART0 (Serial)
│   ├── hal_storage.cpp       SD card access on dedicated HSPI
│   └── osd_canvas.h/cpp      OSD drawing API backed by FabGL Canvas (supervisor OSD)
├── supervisor/             On-Screen Display system (HAL-agnostic via osd_canvas)
│   ├── supervisor.h/cpp      OSD lifecycle and state machine
│   ├── sv_menu.h/cpp         Main menu tiles and icons; Setup, Keyboard, Cartridges and Debug lists; machine, CPU, cartridge and hard disk popups
│   ├── sv_disk.h/cpp         WD1793 FDC emulation and PSRAM cache
│   ├── sv_filebrowser.h/cpp  Disk Manager — drive buttons, SD card browser, mount popups
│   ├── sv_cart.h/cpp         Load Cart — cartridge ROM browser, unload, restart popup
│   ├── sv_render.h/cpp       OSD rendering — green frame, list rows, popups
│   ├── sv_debug.h/cpp        Debug overlay — CPU status, GIME state, memory dump
│   ├── sv_joystick.h/cpp     Joy - Mouse Sensitivity popup — live cursor + adjust
│   ├── sv_keymap.h/cpp       Key mapper UI — CoCo 2 / CoCo 3 keyboard picture, remap physical keys
│   ├── sv_wifi.h/cpp         WiFi / Debug screen — status, network scan, password entry
│   └── sv_fujinet.h/cpp      DriveWire screen — External Server, host, port, ROM
├── net/                    WiFi, debug server and DriveWire client
│   ├── wifi_mgr.h/cpp        WiFi state machine, scan, NVS credentials
│   ├── debug_server.h/cpp    HTTP/JSON debug API (core 0)
│   ├── debug_rpc.h/cpp       Command queue that runs debug requests on the emulator core
│   ├── png_writer.h/cpp      PNG encoder for /api/screenshot.png
│   ├── dw_bus.h/cpp          DriveWire bus mode and configuration
│   └── dw_client.h/cpp       TCP client for an external DriveWire server
├── roms/
│   └── rom_loader.h/cpp      ROM loading with CRC-32 validation
├── tests/
│   └── integration_test.h/cpp  LOADM binary verification
└── utils/
    ├── debug.h               Debug output macros
    ├── no_psram_memw.h       Drops the PSRAM cache-bug workaround from files that only store to internal RAM
    └── perf_probe.h/cpp      Lightweight esp_timer-based hot-path profiler
tools/
├── build_firmware.sh       Release images: single flashable .bin and ESP32_Bootloader package
├── cpu_test/               Host tests for the CPU cores (vectors, 6809 vs 6309, optional XRoar comparison)
├── gime_render_test/       Host test for the GIME scanline renderers
├── gime_ram_test/          Host test for GIME address translation at each CoCo 3 RAM size
├── vhd_test/               Hard disk: test image builders and a port test over the debug API
├── perf/                   Benchmark and measurement scripts
├── dw_test_server.py, dw_proxy.py   DriveWire test server and proxy
└── frequency_meter.py    Audio test: measures the pitch of the board's sound output from a PC audio input
mcp-bridge/                 Host-side MCP server that fronts the WiFi debug API
docs/                       Technical documentation (see below)
```

## Documentation

All technical documentation is in the `docs/` directory:

| File | Description |
|------|-------------|
| [Architecture.md](docs/Architecture.md) | System architecture — cores, main loop, memory maps, CoCo 2 and CoCo 3 modules on TTGO VGA32 |
| [pinout.md](docs/pinout.md) | LilyGo TTGO VGA32 v1.4 pinout — VGA, PS/2, SD card and audio pins |
| [core.md](docs/core.md) | MC6821 PIA, MC6847 VDG, SAM6883, GIME and machine integration |
| [cpu.md](docs/cpu.md) | MC6809 and HD6309 CPUs — registers, interrupts, native mode, traps, host tests |
| [orchestra90.md](docs/orchestra90.md) | Orchestra-90/CC cartridge — DAC ports, audio mix, ROM and autostart |
| [ssc.md](docs/ssc.md) | Speech/Sound Cartridge — ports, status bits, chips, core-0 task, audio mix, host tests |
| [vhd.md](docs/vhd.md) | Virtual hard disk — modes, booting NitrOS-9, RGB-DOS layout, ports, SD performance |
| [coco3-gime.md](docs/coco3-gime.md) | CoCo 3 GIME porting guide, register map, MMU |
| [disk-hal.md](docs/disk-hal.md) | WD1793 FDC emulation, HALT/NMI flow, PSRAM disk cache |
| [drivewire.md](docs/drivewire.md) | DriveWire modes, Becker port, built-in server; diagrams of the floppy vs DriveWire sector path |
| [keyboard-hal.md](docs/keyboard-hal.md) | PS/2 → CoCo matrix mapping (note: older revisions describe the USB HID variant) |
| [supervisor.md](docs/supervisor.md) | OSD state machine, file browser, NVS persistence |
| [video.md](docs/video.md) | Video rendering pipeline, scale modes, palette |
| [audio-hal.md](docs/audio-hal.md) | Audio path, ISR, PIA DAC routing |
| [joystick-hal.md](docs/joystick-hal.md) | Joystick HAL — PS/2 mouse as CoCo joystick 1, comparator emulation, lessons learned |
| [rs232-hal.md](docs/rs232-hal.md) | RS-232 Pak emulation, MC6551 ACIA, HAL bridge |
| [runtime-machine-switch.md](docs/runtime-machine-switch.md) | Runtime CoCo 2 / CoCo 3 switching, NVS state |
| [wifi-debug.md](docs/wifi-debug.md) | WiFi debug server — HTTP/JSON API, WiFi setup, MCP bridge |

## Known Limitations

- Orchestra-90 plays in mono (one DAC pin), and Disk BASIC is unavailable while it is on
- The Speech/Sound Cartridge plays in mono, and its sound holds briefly while core 0 is busy (screenshots, heavy WiFi traffic); PSG music programs are not tested yet
- Disk BASIC is unavailable while a program cartridge is loaded (there is no Multi-Pak)
- Bank-switched cartridges (over 16 KB) are CoCo 3 only; an unbanked 32 KB ROM that sits at `$8000-$FEFF` does not work
- Max 128 file entries in the SD card browser
- Joystick 2 (left port) is a stub — returns centered, button released; only Joystick 1 is active via PS/2 mouse
- NTSC composite simulation covers RG6 (PMODE 4) artifact colour only — there is no
  chroma/luma bleed modelling for the other modes
- **The monitor flickers when the video output changes resolution.** The normal output is
  640×200. CoCo 3 screens taller than 200 lines and at most 512 pixels wide (the 225-line
  mode used by games such as Rick Dangerous and Donkey Kong) switch the VGA output to
  512×240, so that all 225 lines show, with border rows above and below. The switch
  happens about half a second after such a screen appears, and again when it goes away
  or the OSD opens (the OSD always uses 640×200). Each switch restarts the VGA signal, so
  the monitor blanks or flickers briefly. 640-wide 225-line screens (NitrOS-9's 80×25
  text) can't use 512×240: they stay at 640×200, which drops one line in nine (the
  blank gap under each text row)
- Only 192-line screens show part of the top and bottom border (4 lines each) at
  640×200; there is no room for side borders in 320- and 640-wide modes

## DriveWire / FujiNet Support

The emulator can talk **DriveWire** over an emulated **Becker port**
(`$FF41` status / `$FF42` data — the same virtual port XRoar, MAME and VCC
use), paired with the HDB-DOS DriveWire ROMs. Because `fujinet-lib` and
CONFIG only ever go through HDB-DOS's `DWRead`/`DWWrite` vectors, the Becker
port is transparent to CoCo software — the same ROM that boots off a plain
DriveWire server also boots FujiNet's CONFIG app.

**What works today (External mode):**
- A TCP client to an external DriveWire/FujiNet server — [pyDriveWire](https://github.com/n6il/pyDriveWire)
  (Python 2), DW4, or [FujiNet-PC](https://github.com/FujiNetWIFI/fujinet-firmware)
  (`./build.sh -p COCO`) — on port 65504.
- HDB-DOS boots automatically once the bus is enabled, giving `DIR`, `LOADM`,
  `SAVE`, disk images, and — via FujiNet-PC — the CONFIG app, TNFS/HTTP disk
  mounts, the `N:` network device and the clock.
- Reconnects on its own after a CoCo reset or a link drop.

**Turning it on:** F3 → Setup → DriveWire → **External Server**. A popup asks for
the Host, Port (default 65504) and HDB-DOS ROM variant, and refuses to turn on
while the ROM file is missing from `/roms`, naming the file to copy. Then
Save & Restart. The screen warns when no WiFi network has been set up yet.

**Known issue — FujiNet-PC's default read timeout.** FujiNet-PC's Becker-over-IP
transport waits only 500 ms per byte by default. Since TCP never loses a byte
(only delays it), a brief WiFi retransmit can occasionally exceed that and
desync the DriveWire stream (a `Checksum error` in FujiNet-PC's log, followed
by the CoCo hanging). A one-line patch that raises this to 5 s ships at
`tools/fujinet-pc-boip-timeout.patch` — apply it to your `fujinet-firmware`
checkout before `./build.sh -p COCO` if you see this. Using the `t`-suffixed
HDB-DOS ROMs (2 s DriveWire timeout, retries instead of hanging) is a
complementary mitigation on the CoCo side.

See [docs/drivewire.md](docs/drivewire.md) for how a sector travels in each
mode, with diagrams.

## TODO

- **Run NitrOS-9 testing** — boot and exercise NitrOS-9 Level 2, including the 6309 build, on both CPUs and at each CoCo 3 memory size.
- Testing and adjustment of RS-232 Pak support
- **CoCo SDC** emulation.

## Changelog

### v0.20.0 — October 24, 2026

**PMODE 4 artifact colours on the CoCo 3, and timer-driven sound at the right
pitch.**

- **Setup → Machine → PMODE 4 Artifacts** (CoCo 3, off by default). Turns on
  NTSC artifact colours for CoCo 2 software drawn in PMODE 4 (Zaxxon,
  Pitfall II, …): pixel pairs show as black, blue, orange and white, the same
  as our CoCo 2 machine. It takes effect at once and is saved. Only PMODE 4
  lines take a different render path, so other modes run at the same speed.
  On a CoCo 2 the row reads ALWAYS.
- **GIME timer fix.** In TINS=1 mode the timer counted 28 ticks per scanline
  instead of 227.5, so timer-driven sound played 8× slow, three octaves low
  (Rick Dangerous's 6 kHz sample FIRQ fired at about 740 Hz). Reload 596 now
  gives 5,995 Hz.
- **Audio rate control.** The playback rate now follows the measured frame
  interval, with a 2,048-sample ring. Over 30 s of Rick Dangerous: 48 → 0
  underruns, 11,023 → 0 dropped samples.
- **Faster HD6309 build.** Short polling loops are skipped to the end of the
  scanline, interpreter helpers are always inlined, and the run loop does one
  combined interrupt check: 15–25 % fewer host cycles per instruction than
  v0.19.0. The MC6809 build is unchanged.
- **Diagnostics:** CPU profiler (`-DPERF_PROBE_ENABLED=1`), boot-time
  interpreter benchmark (`-DPERF_BENCH=1`), and `gime_timer` in `/api/status`.

### v0.19.0 — October 20, 2026

**The CoCo 3 runs at full speed, and fast typing no longer drops keys.**

- **1.79 MHz in fast mode.** The GIME stored SAM R1 (`$FFD9`) but nothing read
  it, so NitrOS-9 and every fast-mode program ran at 0.895 MHz. The CoCo 3 now
  runs two 14,916-cycle slices per frame while R1 is set; the RS-232 Pak ACIA
  gets half the cycles so its baud rate holds. The CoCo 2 is unchanged.
  Measured on the board: BASIC fast mode 1.790 MHz (ratio 1.000, about 13 ms of
  work per 16.7 ms frame), NitrOS-9 1.0.1 1.79 MHz at the shell; some
  animations still show about 44 FPS.
- **Emulated CPU speed meter.** `/api/status` has a `speed` block (`mhz`,
  `target_mhz`, `ratio`, `rate`, frame work average/max).
  `tools/perf/speedcheck.py` cross-checks it against the guest's 60 Hz counter.
- **Fast typing keeps every key.** Color BASIC's KEYIN decodes one newly
  pressed bit per column and drops a key whose column changes while it
  debounces, so quick re-taps and two keys of one column in the same frame were
  lost. Matrix changes now go through a FIFO, one per frame, with a 4-frame
  minimum hold and a 2-frame gap before a re-press.
- **Bootloader `version.txt` is the bare version** (`0.19.0`), as ESP32_Bootloader
  requires; it used to be `CoCo.<version>-<git describe>`. Bump
  `FIRMWARE_VERSION` for every build that must reflash.

### v0.18.2 — October 19, 2026

**Arrow keys as a joystick, and a working RAM dump.**

- **Setup → Joy-Keyboard Emulation** (OFF / RIGHT / LEFT): the arrow keys
  move the chosen CoCo joystick and Space fires. Holding an arrow glides the
  stick (about 1 s from centre to edge) and it stays put on release, like the
  mouse. On RIGHT the arrows and the PS/2 mouse move the same stick. While
  it's on, the arrows and Space don't reach the CoCo keyboard. Applied at
  once and saved.
- **Debug → Dump RAM to SD:**
  - The name can be changed. It suggests the next free `DUMPnnn`; typing
    replaces it.
  - The Saving screen shows the real RAM size (up to 2048K) and a progress
    bar.
  - The result screen shows the file sizes (it showed 0 KB).

## ⚠️ Vibe Coding Alert

Full transparency: this project was built by an ESP32 hobbyist working with AI coding assistants, not a professional embedded/C++ developer. If you're an experienced embedded engineer, you might look at this codebase and wince. That's okay.

The goal here was to scratch an itch — get a CoCo 2/3 emulator running on cheap VGA32 hardware — and learn along the way. The code works, but it's likely missing patterns, optimizations, or elegance that only years of embedded/C++ experience can provide.

This is where you come in. If you see something that makes you cringe, please consider contributing rather than just closing the tab. This is open source specifically because human expertise is irreplaceable. Whether it's refactoring, better error handling, cycle-accuracy fixes, or architectural guidance — PRs and issues are welcome.

Think of it as a chance to mentor an AI-assisted developer through code review. We all benefit when experienced developers share their knowledge.

## Credits

- **Reinaldo Torres / CoCo Byte Club** — ESP32 port and hardware design — [reyco2000@cocobyte.club](mailto:reyco2000@cocobyte.club)
- **Ciaran Anscomb** — [XRoar](http://www.6809.org.uk/xroar/) CoCo/Dragon emulator (original source)
- **Claude Code (Anthropic)** — co-development of the ESP32 port
- **Fabrizio Di Vittorio** — [FabGL](http://www.fabgl.com/) VGA / PS/2 / DAC library

## License

This project is licensed under the **GPL-3.0**.
