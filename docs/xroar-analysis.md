# ANALYSIS.md — XRoar 0.32.0 ESP32-S3 Port Analysis

> Target: CoCo 1 and CoCo 2 ONLY
> Excluded machines: CoCo 3, Dragon 32/64, MC-10
> Excluded peripherals: Cassette, Disk, Printer (stub only), Joystick hardware (stub: center 31,31, no button)
> Goal: Stay as close as possible to original XRoar C code

---

## 1. File Inventory

### 1.1 Core Source Files (`src/`)

| File | Purpose | CoCo 1/2 | Platform | Lines | Action |
|------|---------|-----------|----------|------:|--------|
| mc6809.c | MC6809 CPU core — instruction decode and execution | YES | NO | 1251 | PORT |
| mc6809.h | MC6809 CPU struct, interrupt vectors, register access macros | YES | NO | 130 | PORT |
| mc6809_common.c | Shared instruction implementations (addressing modes, ALU ops) | YES | NO | 300 | PORT |
| mc6809_common.h | Shared CPU macros (fetch, read/write, stack push/pull) | YES | NO | 65 | PORT |
| mc6809_trace.c | CPU instruction tracing/disassembly | YES | NO | 1255 | PORT |
| mc6809_trace.h | Trace interface | YES | NO | 16 | PORT |
| mc6847.c | MC6847 VDG — scanline rendering, timing, mode handling | YES | NO | 494 | PORT |
| mc6847.h | VDG timing constants, struct, color enum | YES | NO | 85 | PORT |
| mc6821.c | MC6821 PIA — register read/write, interrupt logic | YES | NO | 181 | PORT |
| mc6821.h | PIA struct (side A/B), convenience macros | YES | NO | 63 | PORT |
| sam.c | SAM6883 — address multiplexer, VDG address counter, CPU speed | YES | NO | 268 | PORT |
| sam.h | SAM interface, divisor constants | YES | NO | 23 | PORT |
| machine.c | Machine init, configure, reset, run, memory bus read/write | YES | NO | 1300 | PORT |
| machine.h | Machine config struct, RAM array, architecture defines | YES | NO | 144 | PORT |
| xroar.c | Main app logic, config parsing, default ROM/CRC lists, xroar_run() | YES | PARTIAL | 2253 | PORT |
| xroar.h | Public API, xroar_cfg struct, signal enums | YES | NO | 220 | PORT |
| events.c | Event queue management (alloc, queue, dequeue) | YES | NO | 83 | PORT |
| events.h | Event struct, event_run_queue(), event_pending() inlines | YES | NO | 52 | PORT |
| keyboard.c | Keyboard matrix emulation, chord modes, key press/release | YES | NO | 267 | PORT |
| keyboard.h | Keyboard interface, chord mode enum | YES | NO | 83 | PORT |
| dkbd.c | Dragon/CoCo keyboard layout definitions | YES | NO | 173 | PORT |
| dkbd.h | Keyboard symbol definitions and mappings | YES | NO | 124 | PORT |
| cart.c | Cartridge abstraction — attach, detach, ROM loading | YES | NO | 320 | PORT |
| cart.h | Cart struct and config | YES | NO | 68 | PORT |
| romlist.c | ROM file search — name lists, file extensions, path walking | YES | NO | 191 | PORT |
| romlist.h | ROM list interface | YES | NO | 21 | PORT |
| crclist.c | CRC-based ROM identification and matching | YES | NO | 176 | PORT |
| crclist.h | CRC list interface | YES | NO | 23 | PORT |
| crc32.c | CRC-32 calculation (used for ROM validation) | YES | NO | 96 | PORT |
| crc32.h | CRC-32 interface | YES | NO | 13 | PORT |
| crc16.c | CRC-16 calculation (used for disk formats) | YES | NO | 27 | PORT |
| crc16.h | CRC-16 interface | YES | NO | 15 | PORT |
| vdg_palette.c | VDG color palette — Y'U'V' to RGB conversion, NTSC/PAL | YES | NO | 171 | PORT |
| vdg_palette.h | Palette struct definitions | YES | NO | 33 | PORT |
| vdg_bitmaps.c | VDG built-in character ROM (font bitmaps) | YES | NO | 202 | PORT |
| vdg_bitmaps.h | Bitmap externs | YES | NO | 8 | PORT |
| xconfig.c | Configuration option parser (command-line and config file) | YES | NO | 299 | PORT |
| xconfig.h | Config option struct definitions | YES | NO | 93 | PORT |
| hexs19.c | Intel HEX / Motorola S-record file loader | YES | NO | 255 | PORT |
| hexs19.h | HEX/S19 interface | YES | NO | 12 | PORT |
| logging.c | Debug logging (hexdump, log levels) | YES | NO | 138 | PORT |
| logging.h | LOG_DEBUG/LOG_WARN/LOG_ERROR macros | YES | NO | 47 | PORT |
| path.c | File path search utilities | YES | NO | 138 | PORT |
| path.h | Path interface | YES | NO | 16 | PORT |
| fs.c | File I/O helpers (load file to buffer) | YES | NO | 64 | PORT |
| fs.h | FS interface | YES | NO | 20 | PORT |
| module.c | Module system — select, init, shutdown from lists | YES | NO | 212 | PORT |
| module.h | Module structs (UI, Video, Sound, Keyboard, FileReq) | YES | NO | 96 | PORT |
| breakpoint.c | Breakpoint/watchpoint system | YES | NO | 224 | PORT |
| breakpoint.h | Breakpoint interface | YES | NO | 107 | PORT |
| snapshot.c | Save/load full machine state | YES | NO | 514 | PORT |
| snapshot.h | Snapshot interface | YES | NO | 12 | PORT |
| sound.c | Sound synthesis — mixing, buffer management | YES | NO | 463 | STUB |
| sound.h | Sound interface, SoundModule | YES | NO | 39 | STUB |
| joystick.c | Joystick input framework — axis/button mapping | YES | PARTIAL | 363 | STUB |
| joystick.h | Joystick interface | YES | NO | 105 | STUB |
| tape.c | Cassette tape framework — motor, data, rewind | YES | NO | 856 | STUB |
| tape.h | Tape interface | YES | NO | 110 | STUB |
| tape_cas.c | .CAS tape format support | YES | NO | 415 | STUB |
| tape_sndfile.c | Audio-file cassette support (libsndfile) | YES | YES | 225 | SKIP |
| printer.c | Printer output emulation | YES | NO | 164 | STUB |
| printer.h | Printer interface | YES | NO | 24 | STUB |
| wd279x.c | WD279x floppy disk controller | YES | NO | 989 | STUB |
| wd279x.h | WD279x interface | YES | NO | 116 | STUB |
| vdisk.c | Virtual disk image management | YES | NO | 916 | STUB |
| vdisk.h | Virtual disk interface | YES | NO | 77 | STUB |
| vdrive.c | Virtual floppy drive emulation | YES | NO | 424 | STUB |
| vdrive.h | Virtual drive interface | YES | NO | 56 | STUB |
| rsdos.c | RS-DOS cartridge | YES | NO | 239 | STUB |
| rsdos.h | RS-DOS interface | YES | NO | 14 | STUB |
| dragondos.c | DragonDOS cartridge (Dragon only) | NO | NO | 213 | SKIP |
| dragondos.h | DragonDOS interface | NO | NO | 14 | SKIP |
| deltados.c | Delta DOS cartridge | PARTIAL | NO | 138 | SKIP |
| deltados.h | Delta DOS interface | PARTIAL | NO | 14 | SKIP |
| orch90.c | Orchestra-90 sound cartridge | PARTIAL | NO | 84 | STUB |
| orch90.h | Orchestra-90 interface | PARTIAL | NO | 14 | STUB |
| becker.c | Becker port (DriveWire) | PARTIAL | YES | 206 | STUB |
| becker.h | Becker interface | PARTIAL | NO | 21 | STUB |
| gdb.c | GDB remote debugging protocol | YES | YES | 819 | SKIP |
| gdb.h | GDB interface | YES | NO | 16 | SKIP |
| hd6309.c | HD6309 CPU variant (not used by CoCo 1/2) | NO | NO | 2196 | SKIP |
| hd6309.h | HD6309 interface | NO | NO | 56 | SKIP |
| hd6309_trace.c | HD6309 tracing | NO | NO | 1346 | SKIP |
| hd6309_trace.h | HD6309 trace interface | NO | NO | 16 | SKIP |
| main_unix.c | Unix main() entry point | NO | YES | 43 | SKIP |
| ui_null.c | Null UI module (headless) | YES | YES | 62 | PORT |
| vo_null.c | Null video output | YES | YES | 39 | PORT |
| ao_null.c | Null audio output (timing-only) | YES | YES | 110 | PORT |
| vo_opengl.c | OpenGL video renderer | NO | YES | 222 | SKIP |
| vo_opengl.h | OpenGL interface | NO | YES | 26 | SKIP |
| vo_generic_ops.c | Generic scanline rendering helpers | NO | YES | 210 | SKIP |
| filereq_cli.c | CLI file-open dialog fallback | YES | YES | 57 | STUB |

### 1.2 Platform-Specific Subdirectories (all SKIP)

| Directory | Files | Lines | Purpose |
|-----------|------:|------:|---------|
| gtk2/ | 13 | 3,374 | GTK+ 2 UI, keyboard, drive/tape controls |
| sdl/ | 9 | 1,855 | SDL UI, video, audio, keyboard, joystick |
| alsa/ | 1 | 241 | ALSA audio backend |
| jack/ | 1 | 128 | JACK audio backend |
| oss/ | 1 | 229 | OSS audio backend |
| pulseaudio/ | 1 | 134 | PulseAudio backend |
| macosx/ | 1 | 306 | macOS CoreAudio backend |
| sunos/ | 1 | 139 | Sun audio backend |
| windows32/ | 4 | 724 | Windows UI and audio |
| linux/ | 1 | 265 | Linux joystick (evdev) |

### 1.3 Portable Library (`portalib/`) — all PORT

| File | Purpose | Lines |
|------|---------|------:|
| delegate.c | Delegate/callback function pointer utilities | 28 |
| delegate.h | DELEGATE_T0/T1/T2 macros, DELEGATE_CALL macros | 84 |
| slist.c | Singly-linked list (append, remove, foreach, sort) | 203 |
| slist.h | Linked list interface | 79 |
| xmalloc.c | Safe malloc/calloc/realloc (abort on OOM) | 55 |
| xalloc.h | xmalloc/xcalloc/xrealloc/xzalloc macros | 28 |
| strsep.c | BSD strsep() compatibility | 29 |
| c-ctype.c | C-locale character classification | 29 |
| c-ctype.h | c_isdigit, c_tolower, etc. | 33 |
| c-strcase.h | c_strcasecmp, c_strncasecmp prototypes | 25 |
| c-strcasecmp.c | Case-insensitive strcmp (C locale) | 30 |
| c-strncasecmp.c | Case-insensitive strncmp (C locale) | 32 |
| array.h | ARRAY_N_ELEMENTS macro | 20 |
| pl-endian.h | Endianness detection (HAVE_BIG_ENDIAN) | 38 |
| pl-string.h | Missing string function prototypes (strsep, etc.) | 32 |

### 1.4 Summary Counts

| Category | Files | Lines |
|----------|------:|------:|
| **PORT** (core emulation) | ~50 | ~10,580 |
| **STUB** (minimal implementations) | ~22 | ~4,340 |
| **SKIP** (not needed) | ~40 | ~9,040 |
| **portalib PORT** | 15 | 745 |
| **Total source tree** | ~127 | ~24,700 |
| **Total to port + stub** | ~87 | ~15,665 |

---

## 2. Exact Files Needed for CoCo 1/2

### 2.1 MC6809 CPU Core

| File | Lines | Notes |
|------|------:|-------|
| `src/mc6809.c` | 1,251 | Full instruction decode/execute loop |
| `src/mc6809.h` | 130 | CPU struct (50 fields), register macros, interrupt vectors |
| `src/mc6809_common.c` | 300 | Shared addressing modes, ALU operations |
| `src/mc6809_common.h` | 65 | Fetch/read/write/stack macros used by mc6809.c |

CPU uses function pointers `read_cycle()` and `write_cycle()` set by machine.c.
The `cpu->run(cpu)` method loops until `cpu->running == 0`.

### 2.2 MC6847 VDG (Video Display Generator)

| File | Lines | Notes |
|------|------:|-------|
| `src/mc6847.c` | 494 | Scanline renderer, horizontal/vertical timing, mode switching |
| `src/mc6847.h` | 85 | All timing constants (see Section 5), MC6847 struct |
| `src/vdg_palette.c` | 171 | Y'U'V' → RGB conversion for 12 VDG colors |
| `src/vdg_palette.h` | 33 | Palette struct |
| `src/vdg_bitmaps.c` | 202 | Built-in 6847 character font (64 chars × 12 rows) |
| `src/vdg_bitmaps.h` | 8 | Font externs |

VDG calls `DELEGATE_CALL2(vdg->fetch_bytes, n, buf)` to get VRAM data.
machine.c wires this to `vdg_fetch_handler()` which calls `sam_vdg_bytes()`.

### 2.3 MC6821 PIA (Peripheral Interface Adapter)

| File | Lines | Notes |
|------|---------|-------|
| `src/mc6821.c` | 181 | Register read/write, interrupt edge detection, output update |
| `src/mc6821.h` | 63 | MC6821 struct with two MC6821_side structs |

Two PIAs in CoCo:
- **PIA0** (0xFF00-0xFF03): Keyboard matrix, joystick comparator
- **PIA1** (0xFF20-0xFF23): VDG mode control, cassette, sound mux, printer

### 2.4 SAM6883

| File | Lines | Notes |
|------|------:|-------|
| `src/sam.c` | 268 | Address multiplexer, VDG address counter, CPU speed control |
| `src/sam.h` | 23 | Interface: `sam_run()`, `sam_vdg_bytes()`, `sam_vdg_hsync/fsync()` |

### 2.5 Machine Setup

| File | Lines | Notes |
|------|------:|-------|
| `src/machine.c` | 1,300 | Machine configure/reset/run, memory bus, PIA wiring, ROM load |
| `src/machine.h` | 144 | machine_config struct, IS_COCO macro, machine_ram[64K] |
| `src/xroar.c` | 2,253 | ROM list definitions, CRC lists, main loop, config defaults |
| `src/xroar.h` | 220 | xroar_cfg struct, public API |
| `src/cart.c` | 320 | Cartridge attach/detach, ROM loading |
| `src/cart.h` | 68 | Cart struct |

### 2.6 Memory Mapping (handled in machine.c + sam.c)

The `do_cpu_cycle()` function in `machine.c:923` calls `sam_run()` which returns:
- **S** = chip-select (which device to access)
- **Z** = translated RAM address
- **ncycles** = SAM clock cycles consumed

Then `read_cycle()` / `write_cycle()` dispatch based on S value.

### 2.7 ROM Loading

| File | Lines | Notes |
|------|------:|-------|
| `src/romlist.c` | 191 | Find ROM files by name list with extension search |
| `src/romlist.h` | 21 | |
| `src/crclist.c` | 176 | Validate ROMs by CRC-32 |
| `src/crclist.h` | 23 | |
| `src/crc32.c` | 96 | CRC-32 calculation |

### 2.8 Support Files

| File | Lines | Notes |
|------|------:|-------|
| `src/events.c` | 83 | Event queue alloc/queue/dequeue |
| `src/events.h` | 52 | Inline event_run_queue(), event_pending() |
| `src/keyboard.c` | 267 | Keyboard matrix scan, chord handling |
| `src/dkbd.c` | 173 | CoCo keyboard layout tables |
| `src/xconfig.c` | 299 | Config option parser |
| `src/logging.c` | 138 | Debug logging |
| `src/path.c` | 138 | File path searching |
| `src/fs.c` | 64 | File I/O helpers |
| `src/module.c` | 212 | Module system |
| `src/breakpoint.c` | 224 | Breakpoints (optional) |
| `src/snapshot.c` | 514 | State save/load (optional) |

---

## 3. External Dependencies Map

### 3.1 Standard C Library (available on ESP32 via newlib)

| Header | Used In | Functions | ESP32 Strategy |
|--------|---------|-----------|----------------|
| `<stdio.h>` | xroar.c, logging.c, romlist.c, crclist.c, xconfig.c, path.c, fs.c, snapshot.c, hexs19.c | `fopen`, `fclose`, `fread`, `fwrite`, `fseek`, `ftell`, `fprintf`, `printf`, `sprintf`, `sscanf` | Available via SPIFFS/LittleFS/SD. Replace `fopen`/`fread` with ESP32 filesystem API or embed ROMs in flash |
| `<stdlib.h>` | Nearly all files | `malloc`, `calloc`, `realloc`, `free`, `exit`, `strtol`, `strtoll`, `atoi`, `qsort` | Available. Use PSRAM-aware `ps_malloc()` for large allocations |
| `<string.h>` | Nearly all files | `memset`, `memcpy`, `strcmp`, `strcpy`, `strlen`, `strdup`, `strncpy`, `strrchr`, `strsep` | Available |
| `<stdint.h>` | Nearly all files | `uint8_t`, `uint16_t`, `uint32_t`, `int8_t`, `int16_t` | Available |
| `<assert.h>` | machine.c, dkbd.c, cart.c, logging.c, tape.c, vdrive.c, wd279x.c | `assert()` | Available (consider `configASSERT` for FreeRTOS) |
| `<limits.h>` | events.h, mc6809.c, mc6847.c, sound.c | `UINT_MAX`, `INT_MAX` | Available |
| `<ctype.h>` | xroar.c, logging.c | `isdigit`, `tolower` | Available |
| `<errno.h>` | xroar.c, ao_null.c | `errno`, `ETIMEDOUT` | Available |
| `<math.h>` | vdg_palette.c | `pow`, `cos`, `sin` | Available. Only used once at init for palette calc |
| `<stdbool.h>` | — | Uses `_Bool` (C99) instead | Available |

### 3.2 POSIX Dependencies (must be replaced)

| Header | Used In | Functions | ESP32 Replacement |
|--------|---------|-----------|-------------------|
| `<sys/time.h>` | xroar.c, ao_null.c | `gettimeofday()` | `esp_timer_get_time()` returns microseconds |
| `<sys/types.h>` | machine.h | `size_t`, `ssize_t` | Available in ESP-IDF |
| `<unistd.h>` | fs.c, path.c | `access()`, `read()`, `close()` | Replace with `stat()` or ESP-IDF VFS |
| `<dirent.h>` | — | Not used in core files | N/A |
| `<pthread.h>` | xroar.c (GDB only) | `pthread_mutex_lock`, `pthread_cond_timedwait` | Not needed — GDB support excluded |

### 3.3 SDL Dependencies

**SDL is NOT used in any core emulation file.** It is only used in `src/sdl/` backend files (all SKIP). No replacement needed for core.

### 3.4 GTK Dependencies

**GTK is NOT used in any core emulation file.** Only in `src/gtk2/` (all SKIP).

### 3.5 GLib Dependencies

**GLib is NOT used in core files.** The `portalib/` provides replacements:
- `slist.c/h` replaces `GList`/`GSList`
- `xalloc.h` replaces `g_malloc`/`g_free`
- `c-strcase.h` replaces `g_ascii_strcasecmp`

### 3.6 File I/O in Core Files (must adapt for ESP32)

| File | Function | What It Does | ESP32 Strategy |
|------|----------|-------------|----------------|
| `romlist.c` | `fopen()` via `find_in_path()` | Search paths for ROM files | Embed ROMs in flash as `const uint8_t[]` arrays, skip file search |
| `machine.c:1276` | `fopen()`, `fread()` in `machine_load_rom()` | Load ROM file into buffer | Copy from flash-embedded array |
| `fs.c` | `fopen()`, `fread()`, `fseek()`, `ftell()` in `fs_load_file()` | Load arbitrary file | Adapt for SPIFFS or skip |
| `path.c` | `access()` in `find_in_path()` | Check if file exists | Unnecessary if ROMs embedded |
| `snapshot.c` | `fopen()`, `fread()`, `fwrite()` | Save/load state | Use SPIFFS/LittleFS or skip |
| `xconfig.c` | `fopen()`, `fgets()` | Read config file | Hardcode CoCo config, skip file |
| `hexs19.c` | `fopen()`, `fgets()` | Load HEX/S19 binaries | Adapt or skip |
| `crclist.c` | `strtoll()` | Parse CRC hex strings | Available in stdlib |

### 3.7 Dynamic Memory Allocation in Core

| File | Allocations | Purpose | ESP32 Note |
|------|-------------|---------|------------|
| `mc6809.c` | 1× `xmalloc(sizeof(MC6809))` | CPU state (~90 bytes) | One-time, small |
| `mc6821.c` | 2× `xmalloc(sizeof(MC6821))` | PIA state (~80 bytes each) | One-time, small |
| `mc6847.c` | 1× `xmalloc(~2KB)` | VDG internal state | One-time |
| `machine.c` | Static `machine_ram[0x10000]` | 64KB RAM | Static array — place in PSRAM |
| `machine.c` | Static `rom0[0x4000]`, `rom1[0x4000]` | 16KB each ROM bank | Static arrays |
| `cart.c` | `xmalloc` per cartridge config | ~200 bytes per cart | One-time |
| `romlist.c` | `slist_append()`, `strdup()` | ROM list entries | Init-time only |
| `crclist.c` | `slist_append()`, `strdup()` | CRC list entries | Init-time only |
| `xconfig.c` | `strdup()` for string options | Config strings | Init-time only |
| `events.c` | `xmalloc(sizeof(event))` | Event structs (~32 bytes each) | ~10 events total |
| `sound.c` | `xmalloc()` for audio buffer | Audio buffer (~4KB) | Stub or PSRAM |
| `slist.c` | `xmalloc()` per list node | Linked list nodes (16 bytes each) | ~100 nodes max at init |

**Total dynamic allocation estimate: ~2KB at runtime, ~5KB at init-time** (excluding the static 64KB RAM and 32KB ROM arrays).

### 3.8 Threading

Only used for GDB support (`#ifdef WANT_GDB_TARGET` in xroar.c). **Not needed for ESP32 port.** Define `WANT_GDB_TARGET` as undefined.

---

## 4. ROM Loading Path

### 4.1 ROM List Definitions (xroar.c:301-336)

CoCo-relevant ROM lists:

```
romlist coco=bas13,bas12,bas11,bas10
romlist coco_ext=extbas11,extbas10
romlist coco1=bas10,@coco
romlist coco1e=bas11,@coco
romlist coco1e_ext=extbas10,@coco_ext
romlist coco2=bas12,@coco
romlist coco2_ext=extbas11,@coco_ext
romlist coco2b=bas13,@coco
romlist rsdos=disk11,disk10
```

### 4.2 ROM Filenames Searched

For each name in the list, `romlist_find()` tries these extensions (romlist.c:43):
```c
"", ".rom", ".ROM", ".dgn", ".DGN"
```

So for `bas13`, it tries: `bas13`, `bas13.rom`, `bas13.ROM`, `bas13.dgn`, `bas13.DGN`

Search paths (set in xroar.c): `~/.xroar/roms/`, `$datadir/xroar/roms/`, `./roms/`

### 4.3 CRC Validation (xroar.c:352-361)

| ROM Name | CRC-32 | Description |
|----------|--------|-------------|
| `bas10` | `0x00b50aaa` | CoCo 1 Color BASIC 1.0 |
| `bas11` | `0x6270955a` | CoCo 1e Color BASIC 1.1 |
| `bas12` | `0x54368805` | CoCo 2 Color BASIC 1.2 |
| `bas13` | `0xd8f4d15e` | CoCo 2b Color BASIC 1.3 |
| `extbas10` | `0xe031d076` or `0x6111a086` | Extended Color BASIC 1.0 |
| `extbas11` | `0xa82a6254` | Extended Color BASIC 1.1 |

### 4.4 ROM Loading into Memory (machine.c:596-673)

```
rom0[0x0000..0x1FFF] = Extended BASIC ROM (8KB)    ← loaded first
rom0[0x2000..0x3FFF] = Color BASIC ROM (8KB)       ← loaded second, offset 0x2000
rom1[0x0000..0x3FFF] = Alt BASIC (Dragon 64 only, not used for CoCo)
ext_charset[0x0000..0x0FFF] = External charset (4KB, optional)
```

If Extended BASIC ROM is > 8KB, it's treated as a combined ROM (both ExtBAS + BAS in one file).

**machine_rom pointer** starts pointing at `rom0` and is used for all ROM reads.

### 4.5 ROM Address Mapping at Runtime

When the CPU reads address A:

```c
// machine.c:966-968
case 1:   // S=1: 0x8000-0x9FFF (Extended BASIC)
case 2:   // S=2: 0xA000-0xBFFF (Color BASIC)
    read_D = machine_rom[A & 0x3FFF];
```

| CPU Address | S | machine_rom Index | Content |
|------------|---|-------------------|---------|
| `0x8000-0x9FFF` | 1 | `0x0000-0x1FFF` | Extended Color BASIC |
| `0xA000-0xBFFF` | 2 | `0x2000-0x3FFF` | Color BASIC |

Interrupt vectors at `0xFFF0-0xFFFF` are in the BASIC ROM region (S=2, mapped to `rom0[0x3FF0..0x3FFF]`).

### 4.6 ESP32 ROM Strategy

Embed ROMs directly in flash as const arrays:

```c
// In a header file:
const uint8_t rom_extbas11[] PROGMEM = { /* 8192 bytes */ };
const uint8_t rom_bas12[]    PROGMEM = { /* 8192 bytes */ };
```

Replace `machine_load_rom()` to `memcpy()` from the const array. Skip `romlist_find()`, `find_in_path()`, and all filesystem-based ROM search.

---

## 5. Main Emulation Loop

### 5.1 Entry Point and Flow

```
main()                          // main_unix.c:27
  └→ xroar_init(argc, argv)    // xroar.c — parse config, init machine
  └→ ui_module->run()           // or loop calling xroar_run()
       └→ xroar_run()           // xroar.c:840
            ├→ machine_run(VDG_LINE_DURATION * 32)  // xroar.c:863
            │    ├→ cycles += ncycles               // machine.c:831
            │    ├→ CPU0->running = 1               // machine.c:832
            │    └→ CPU0->run(CPU0)                 // machine.c:833
            │         └→ [executes instructions until cycles <= 0]
            │              └→ each instruction calls read_cycle()/write_cycle()
            │                   └→ do_cpu_cycle()   // machine.c:923
            │                        ├→ sam_run()   // address decode
            │                        ├→ cycles -= ncycles
            │                        ├→ event_run_queue(&MACHINE_EVENT_LIST)
            │                        └→ MC6809_IRQ_SET / MC6809_FIRQ_SET
            └→ event_run_queue(&UI_EVENT_LIST)      // xroar.c:880
```

### 5.2 Frame Timing

```
xroar_run() executes: VDG_LINE_DURATION * 32 = 456 * 32 = 14,592 SAM cycles per call
```

One SAM cycle = 1/OSCILLATOR_RATE seconds = 1/14,318,180 ≈ 69.8 ns

One call to xroar_run() = 14,592 × 69.8 ns ≈ **1.019 ms** (about 32 scanlines worth)

A full NTSC frame = 262 scanlines × 456 = 119,472 SAM cycles ≈ **8.34 ms** (≈ 120 fps worth, but vsync limits to 60 fps)

The UI typically calls `xroar_run()` ~8-9 times per frame.

### 5.3 CPU Clock Speed

| Mode | SAM Divisor | Effective Rate |
|------|-------------|----------------|
| Slow (normal) | 16 SAM cycles/CPU cycle | 14,318,180 / 16 = **894,886 Hz** ≈ 0.895 MHz |
| Fast (poke-through) | 8 SAM cycles/CPU cycle | 14,318,180 / 8 = **1,789,773 Hz** ≈ 1.79 MHz |

### 5.4 VDG Scanline Rendering Sequence

Horizontal timing (in pixels = half-VDG-clocks):

```
|←─ FP ─→|←── HSync ──→|←── BP ──→|←─ LBorder ─→|←──── Active ────→|←─ RBorder ─→|
    17          32            35          60              256               56
|←──────── HBlank = 84 ──────────→|←──────────── AVB = 372 ─────────────────────────→|
                                   |←───────── Line Duration = 456 ──────────────────→|
```

Vertical timing (in scanlines):

```
Line    0       VDG_VBLANK_START — FS (field sync) fires PIA1.b FIRQ
Line   13       VDG_TOP_BORDER_START
Line   38       VDG_ACTIVE_AREA_START — first rendered line
Line  229       VDG_ACTIVE_AREA_END (38+192-1)
Line  230       VDG_ACTIVE_AREA_END — bottom border starts
Line  255       VDG_BOTTOM_BORDER_END
Line  261       VDG_VRETRACE_END
Line  262       VDG_FRAME_DURATION — total lines
```

Active display: **192 scanlines × 256 pixels**

### 5.5 Interrupt Timing

**HSYNC** (every scanline):
- mc6847.c `do_hs_fall()` fires at the falling edge of HS
- Calls `VDG0->signal_hs` delegate → `vdg_hs()` in machine.c
- `vdg_hs()` calls `sam_vdg_hsync()` and `mc6821_set_cx1(&PIA0->a)` → triggers PIA0 side A interrupt
- PIA0's interrupts feed into CPU **IRQ** line

**VSYNC / Field Sync** (once per frame):
- mc6847.c triggers `VDG0->signal_fs` delegate → `vdg_fs()` in machine.c
- `vdg_fs()` calls `sam_vdg_fsync()` and `mc6821_set_cx1(&PIA1->b)` → triggers PIA1 side B interrupt
- PIA1's interrupts feed into CPU **FIRQ** line

**Interrupt wiring** (machine.c:934-935):
```c
MC6809_IRQ_SET(CPU0, PIA0->a.irq | PIA0->b.irq);    // PIA0 → IRQ
MC6809_FIRQ_SET(CPU0, PIA1->a.irq | PIA1->b.irq);   // PIA1 → FIRQ
```

### 5.6 Event System

Events are timer callbacks scheduled at specific SAM tick counts:

```c
struct event {
    event_ticks at_tick;        // unsigned, SAM cycle count to fire
    DELEGATE_T0(void) delegate; // callback function pointer
    _Bool queued;
    struct event **list;        // which queue this belongs to
    struct event *next;         // linked list next
};
```

Two event queues:
- `MACHINE_EVENT_LIST` — processed every CPU cycle (inside `do_cpu_cycle`)
- `UI_EVENT_LIST` — processed after each `machine_run()` batch

`event_pending()` uses unsigned arithmetic: `(event_current_tick - event->at_tick) <= UINT_MAX/2`

Key events:
- VDG HS rising/falling edges
- VDG FS rising edge
- VDG render scanline callbacks
- Sound buffer fill events

---

## 6. Memory Requirements

### 6.1 Emulated Machine State

| Component | Size | Notes |
|-----------|-----:|-------|
| `machine_ram[0x10000]` | 65,536 | Emulated CoCo RAM (64KB max) |
| `rom0[0x4000]` | 16,384 | ROM bank 0 (ExtBAS + BAS) |
| `rom1[0x4000]` | 16,384 | ROM bank 1 (not used for CoCo, can omit) |
| `ext_charset[0x1000]` | 4,096 | External character set (optional) |
| **Subtotal emulated memory** | **102,400** | **100 KB** |

### 6.2 Hardware State Structs

| Component | Size (est.) | Notes |
|-----------|------------:|-------|
| `struct MC6809` | 90 | CPU registers, state, function pointers |
| `struct MC6821` × 2 | 160 | Two PIA chips (80 bytes each) |
| `struct MC6847` (internal) | ~2,048 | VDG state + scanline pixel buffer |
| SAM state (static vars in sam.c) | ~80 | Address counter, config, registers |
| Event structs (~10) | ~320 | 32 bytes each |
| **Subtotal hardware state** | **~2,700** | **~2.7 KB** |

### 6.3 Video Output Buffer

The VDG produces 456 bytes per scanline (`pixel_data[VDG_LINE_DURATION]`).

For ESP32 display output, you need a framebuffer:

| Strategy | Size | Notes |
|----------|-----:|-------|
| Single scanline buffer | 456 | Render-as-you-go, send to display per line |
| Active area only (256×192 × 1 byte palette index) | 49,152 | 48 KB — compact, index into 12-color palette |
| Active area only (256×192 × 2 bytes RGB565) | 98,304 | 96 KB — direct to TFT display |
| Full frame with borders (320×240 × 2 bytes RGB565) | 153,600 | 150 KB — full frame with borders |

**Recommended**: 256×192 × 2 bytes RGB565 = **96 KB** in PSRAM, DMA to display.

### 6.4 VDG Font / Bitmap Data

| Data | Size | Notes |
|------|-----:|-------|
| `font_6847[]` | 768 | 64 chars × 12 rows × 1 byte |
| `font_6847t1[]` | 768 | T1 variant font |
| **Subtotal font data** | **~1,536** | In flash (const) |

### 6.5 Sound Buffer (if implemented)

| Component | Size | Notes |
|-----------|-----:|-------|
| Sound sample buffer | ~4,096 | Circular buffer for audio output |
| Sound state | ~200 | Mixer state |
| **Subtotal sound** | **~4,300** | Can skip if sound not implemented |

### 6.6 Init-Time / Config Data

| Component | Size (est.) | Notes |
|-----------|------------:|-------|
| ROM lists (slist nodes + strings) | ~2,000 | Init-time, freed after config |
| CRC lists | ~1,000 | Init-time |
| Machine config structs | ~500 | 1-2 configs |
| Cart config | ~300 | One cart config |
| xconfig option strings | ~500 | Can be hardcoded |
| **Subtotal config** | **~4,300** | Much can be eliminated by hardcoding |

### 6.7 Total Memory Budget

| Category | RAM | Flash | Notes |
|----------|----:|------:|-------|
| Emulated RAM | 65,536 | — | Place in PSRAM |
| ROM data | — | 16,384 | Embed in flash as const |
| Hardware structs | 2,700 | — | Internal SRAM OK |
| Video framebuffer (RGB565) | 98,304 | — | PSRAM |
| Font data | — | 1,536 | Flash const |
| Sound (optional) | 4,300 | — | Internal SRAM or skip |
| Init/config | 4,300 | — | Mostly freed after init |
| Code (.text) | — | ~60,000 | Estimated compiled code size |
| **TOTAL** | **~175 KB RAM** | **~78 KB flash** | |

### 6.8 ESP32-S3 Fit Analysis

| ESP32-S3 Resource | Available | Used | Margin |
|-------------------|----------:|-----:|-------:|
| Internal SRAM | 512 KB | ~12 KB (structs + stack) | ~500 KB free |
| PSRAM (8 MB) | 8,192 KB | ~164 KB (RAM + framebuffer) | ~8,028 KB free |
| Flash (16 MB typical) | 16,384 KB | ~78 KB (code + ROM + fonts) | ~16,306 KB free |

**The ESP32-S3 with 8 MB PSRAM has more than enough memory.** The emulated 64K RAM and framebuffer fit easily in PSRAM, while the CPU core and hardware structs fit in fast internal SRAM.

### 6.9 Performance Estimate

The CoCo CPU runs at ~895 KHz (slow mode). The ESP32-S3 runs at 240 MHz.

- Budget per emulated CPU cycle: 240 MHz / 895 KHz ≈ **268 ESP32 cycles**
- Each MC6809 instruction is 2-19 CPU cycles
- The `do_cpu_cycle()` path (SAM decode + memory access + event check) is ~50-100 ESP32 instructions
- **Performance should be achievable** with careful optimization, leaving headroom for video output

---

## Appendix A: SAM Address Decode Table

From `sam.c:96-137`:

| CPU Address | S Value | Read | Write | Device |
|-------------|---------|------|-------|--------|
| `0x0000-0x7FFF` | 0 (R) / 7 (W) | RAM | RAM | 32KB/64KB RAM |
| `0x8000-0x9FFF` | 1 | ROM | — | Extended BASIC |
| `0xA000-0xBFFF` | 2 | ROM | — | Color BASIC |
| `0xC000-0xFEFF` | 3 | Cart | Cart | Cartridge space |
| `0xFF00-0xFF1F` | 4 | PIA0 | PIA0 | Keyboard, joystick |
| `0xFF20-0xFF3F` | 5 | PIA1 | PIA1 | VDG ctrl, sound, tape |
| `0xFF40-0xFF5F` | 6 | Cart pg2 | Cart pg2 | Cartridge I/O |
| `0xFF60-0xFFBF` | 7 | — | — | Unused |
| `0xFFC0-0xFFDF` | 7 | — | SAM reg | SAM register writes |
| `0xFFE0-0xFFFF` | 2 | ROM | — | Interrupt vectors |

When `map_type_1` (SAM bit 15 set, 64K mode): all of `0x0000-0xFEFF` maps to RAM for reads.

## Appendix B: SAM Register Bits

16-bit register written via `0xFFC0-0xFFDF` (each pair of addresses = 1 bit):

| Bit | Address | Function |
|-----|---------|----------|
| 0-2 | FFC0-FFC5 | VDG display mode (V0-V2) |
| 3-9 | FFC6-FFD3 | VDG display offset (F0-F6) → base address = bits[9:3] << 9 |
| 10 | FFD4-FFD5 | Page select (P1) for 64K mode |
| 11 | FFD6-FFD7 | MPU rate / address-dependent speed (R0) |
| 12 | FFD8-FFD9 | MPU rate fast (R1) |
| 13-14 | FFDA-FFDD | Memory size (M0-M1): 0=4K, 1=16K, 2/3=64K |
| 15 | FFDE-FFDF | Map type (TY): 0=normal, 1=all-RAM |

## Appendix C: Interrupt Vector Table

| Vector | Address | Source |
|--------|---------|--------|
| RESET | `0xFFFE-0xFFFF` | Power-on / hard reset |
| NMI | `0xFFFC-0xFFFD` | Non-maskable interrupt |
| SWI | `0xFFFA-0xFFFB` | Software interrupt |
| IRQ | `0xFFF8-0xFFF9` | PIA0 (HSYNC → keyboard scan) |
| FIRQ | `0xFFF6-0xFFF7` | PIA1 (VSYNC → frame timing) |
| SWI2 | `0xFFF4-0xFFF5` | Software interrupt 2 |
| SWI3 | `0xFFF2-0xFFF3` | Software interrupt 3 |

## Appendix D: CoCo RAM Organization

From `machine.c:775-786`, the `decode_Z()` function translates SAM addresses based on RAM size:

| RAM Size | Organization | Mask | Address Translation |
|----------|-------------|------|---------------------|
| ≤ 8 KB | 4K chips | `0x3F3F` | Complex MUX: `(Z & 0x3F) \| ((Z & 0x3F00) >> 2) \| ((~Z & 0x8000) >> 3)` |
| ≤ 16 KB | 16K chips | `0xFFFF` | MUX: `(Z & 0x7F) \| ((Z & 0x7F00) >> 1) \| ((~Z & 0x8000) >> 1)` |
| > 16 KB (32K/64K) | 64K chips | `0x7FFF` or `0xFFFF` | Direct: `Z & ram_mask` |

For CoCo 2 with 64K: `ram_organisation = RAM_ORGANISATION_64K`, `ram_mask = 0xFFFF`.
