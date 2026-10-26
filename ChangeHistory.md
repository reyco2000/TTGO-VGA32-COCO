# Change History

Releases before the three most recent ones. The latest changes are in the
[Changelog section of the README](README.md#changelog).

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

### v0.18.1 — October 19, 2026

**NitrOS-9 shows its whole screen, and a tidier Setup menu.**

- **All 25 rows of NitrOS-9's 80x25 screen are visible.** The GIME's
  225-line mode didn't fit the 200-line VGA picture, so the last three rows
  (the shell prompt) were cut off. Now the blank line under each row of text
  is dropped instead, so the text keeps its shape.
- **Setup → Cartridges is now Setup → Multi-Pak - Insert Cartridges**, with a
  Multi-Pak icon. **RS-232 Pak** moved there from Setup (still on/off with no
  restart).
- **Hard Disk** has a hard-drive icon. Its options are **On, boot RS-DOS
  (NitrOS-9 compatible)** and **On, boot RGB-DOS**, and the row shows
  **(RS-DOS) ON** / **(RGB-DOS) ON** / **OFF**. Turning the hard disk off also
  forgets the HD0/HD1 images saved in NVS.
- **Setup → Machine:** the first row is **Choose Machine Type**. **CPU** is a
  list of **6809** and **6309** with the running one marked *current*, instead
  of a Yes/No "switch to the other CPU" question. The memory row reads
  **CoCo 3 Memory** on a CoCo 3 and **CoCo 2 Memory (64K, fixed)** on a CoCo 2.

### v0.18.0 — October 18, 2026

**Virtual hard disk.** VCC's hard disk device ("EmuDsk", `HardDisk/cc3vhd.cpp`)
at `$FF80-$FF86`: two `.VHD` images of up to 512 MB, read and written in place
on the SD card. NitrOS-9 from the EOU images boots with its `emudsk` driver.

- **Setup → Cartridges → Hard Disk** picks **Off**, **On, boot RS-DOS** or
  **On, boot RGB-DOS**. RS-DOS is VCC's Multi-Pak setup: Disk BASIC boots, and
  `DOS` starts NitrOS-9 from a floppy that then finds the VHD. RGB-DOS
  (`rgbdos.rom` in `/roms/`) gives BASIC the RS-DOS virtual drives on the VHD;
  type `DRIVE OFF` before `DOS` to boot a floppy from it.
- **Setup → Sound Carts is now Setup → Cartridges**, with the Hard Disk row
  next to Orchestra-90 and Speech/Sound.
- **Disk Manager** shows HD0 and HD1 while the hard disk is on; `.VHD` images
  mount there and come back at every boot.
- **Fast enough for NitrOS-9**: about 0.8 ms per sector in sequence and 4 ms
  with seeks. Arduino's file layer threw away a 4 KB buffer on every seek
  (9.6 ms per sector); hard disk files now use a 512-byte buffer, which also
  cuts the internal RAM an open file costs from about 5 KB to 760 bytes.
- **Debug API and MCP bridge**: `/api/disk` takes `drive=hd0` / `hd1`. The
  bridge gains `list_disks`, `mount_disk`, `eject_disk`, `flush_disk` (floppies
  and hard disks) and `osd_keys`, which drives the supervisor menu.
- Checked on the board: EOU NitrOS-9 for the 6309 boots from `63EMU.DSK` with
  `63SDC.VHD` on HD0, on the emulated HD6309 (the first real software checked
  on that CPU on the board). RGB-DOS's detection and disk routines were checked
  against a disassembly of `rgbdos.rom`.
- Details: [docs/vhd.md](docs/vhd.md).

### v0.17.0 — October 15, 2026

**Speech/Sound Cartridge.** Tandy's 26-3144 talking cartridge, ported from
VCC's SSC module (itself from MAME). All four parts are emulated: the TMS7040
microcontroller running the cartridge's own text-to-speech firmware, the
SP0256-AL2 speech chip, the AY-3-8913 sound generator and the glue with its
2 KB of RAM. Programs write text to `$FF7E` and the cartridge speaks it.

- **Setup → Sound Carts** (Cartridges since v0.18.0) is a new submenu with
  the Orchestra-90 and Speech/Sound toggles (the Setup list was full). Turning the cartridge on
  restarts the emulator; if a ROM is missing the popup names it.
- Needs `pic-7040-510.bin` and `sp0256-al2.bin` in `/roms/`. Works on the
  CoCo 2 and the CoCo 3, alongside Disk BASIC and Orchestra-90.
- **Runs on the second core.** On the emulator's core it cost ~2.6 ms per
  frame and `WIDTH 80` / `HSCREEN 2` fell to ~49 FPS; on core 0 everything
  holds 60 FPS. It uses about 4 KB of internal RAM while on, nothing while
  off.
- `/api/status` has an `ssc` block (busy, commands, allophones, underruns...).
- Host tests in `tools/ssc_test/` compare the port with VCC's original code.
- Checked on the board: the test sentence is spoken on the CoCo 3 and the
  CoCo 2 with the real ROMs, at 59-60 FPS.
- Details: [docs/ssc.md](docs/ssc.md).

### v0.16.1 — October 13, 2026

**Sound no longer breaks up when the emulator runs slow.** Until now the
audio queue needed a steady 60 FPS: below that it ran dry several times a
second, and if the audio interrupt was delayed the queue stayed full and
dropped samples.

- When the emulator cannot hold 60 FPS, playback now slows down with it and
  stays continuous. Sound is slower and lower by the same amount the emulator
  is slow (7 % at 55.6 FPS, about 1.3 semitones).
- The audio interrupt now advances by the time that really passed, so a
  delayed interrupt skips ahead instead of falling behind.
- At 60 FPS nothing changes audibly: the rate correction stays within about
  0.03 % on a static screen and 0.2 % on an animated one.
- `/api/status` adds `catchup_ticks` to the `audio` counters.
- Checked on the board: a test build slowed to 50 FPS settled with no
  dropouts after the first seconds; CoCo 3 and CoCo 2 (6809) at 60 FPS, an
  animated screen and a debugger pause all show none.
- By ear: Orchestra-90 holds 60 FPS on the CoCo 3 and the CoCo 2 with no
  audio issues, and CoCo 2 games that drop to 58 FPS show no audible problem.
- `PLAY` pitch on the CoCo 3 is unchanged (`O2 A` 217.76 Hz, `O4 A`
  867.70 Hz).
- Known issue: on the CoCo 2, high `PLAY` notes come out flat (`O4 A` reads
  836.99 Hz, 3.6 % low). The CoCo 2 drops to about 57.7 FPS while that note
  plays, and sound now follows the emulator's speed. CoCo 2 needs more speed
  margin; the audio code is doing what it should.

### v0.16.0 — October 12, 2026

**Load cartridges from the supervisor.** The main menu grows from six tiles to
eight (a 4x2 grid) and gains a **Cart** tile, drawn as a game cartridge.

- **Load Cart** browses the SD card for `.ccc` and `.rom` cartridge ROMs,
  starting in `/carts`. ENTER on a file stores it as the cartridge; **Unload
  cartridge** removes it. A popup then asks whether to restart now: *No* keeps
  the choice for the next restart, *Yes* restarts, ESC cancels.
- The cartridge loader reads ROMs of up to 16 KB (it was 8 KB) from any folder.
  Program cartridges start on their own, as on the real machine.
- **Bank-switched cartridges on the CoCo 3.** A ROM of more than 16 KB, up to
  128 KB, is split into 16 KB banks selected by writing `$FF40`. Tested on the
  board with a 128 KB cartridge.
- A cartridge takes the place of Disk BASIC, and loading one turns the
  Orchestra-90 setting off.
- A **WiFi** tile opens the WiFi / Debug screen straight from the main menu.

**Clearer start-up errors.** The "no SD card" and "missing ROMs" screens are
redrawn centred, with a red warning sign and an SD card or ROM chip icon. The
first asks for a FAT-formatted card; the second names the missing files and the
`/roms` folder. Both point to the project page.

**Fix.** The Disk Manager showed an empty list when the folder it remembered
was no longer on the SD card. It now goes back to the card root.

### v0.15.4 — October 11, 2026

**CoCo 2 runs at full speed, which fixes its sound.** CoCo 2 mode was not
reaching 60 FPS: 56–58 FPS at the BASIC prompt in v0.15.2 and 48–52 FPS in
v0.15.3. The v0.15.3 audio queue needs 60 FPS, so on the CoCo 2 it ran dry
about six times a second and sounded worse than before.

- CoCo 2 now uses the same direct RAM/ROM access in the CPU core that the
  CoCo 3 has had since v0.12.2, instead of a callback for every memory access.
- Measured on the board: 60 FPS at the BASIC prompt on both CPUs and with an
  animated text screen, with no audio dropouts. One run on the 6809 right
  after switching machines read 56 FPS with dropouts; not explained yet.
- ROM stays read-only and the SAM all-RAM mode works through the new path
  (checked with a test routine on the board).
- Known limit: a program heavy enough to drop below 60 FPS still makes the
  sound break up, on either machine.
- Known issue: in one CoCo 2 session the audio interrupt was held off for up
  to 0.5 ms at a time and lost about 1 % of its ticks; the audio queue then
  stays full and drops about 16 samples a second. The FPS overlay (F5) is the
  suspect, not confirmed.

### v0.15.3 — October 11, 2026

**Cleaner sound.** The audio output was losing part of every frame since the
60 Hz frame pacing of v0.12: measured on the board, 6.6 of each frame's 262
samples were cut off, and late frames replayed stale audio.

- Samples now go through a ring buffer between the emulation loop and the
  audio interrupt, and the playback rate follows the buffer's fill level. No
  samples are dropped or replayed, on static or animated screens.
- When emulation stops (supervisor menu, debugger pause) the output holds its
  level and restarts cleanly, instead of looping the last frame.
- The pitch trim constant is gone. `PLAY` pitch is unchanged: `O2 A` is
  217.84 Hz and `O4 A` is 867.73 Hz, which is the ROM's own timing, not
  concert pitch (see [audio-hal.md](docs/audio-hal.md), "Pitch").
- Sound now runs about 20–25 ms behind the emulation.
- `/api/status` reports audio queue and interrupt timing counters (`audio`).
- New `tools/frequency_meter.py`: measures the pitch of the board's sound
  output from a PC audio input.
- Checked on the board with `SOUND`, `PLAY` and Orchestra-90 on the CoCo 3;
  **not yet checked in CoCo 2 mode**.

### v0.15.2 — October 10, 2026

**`SYNC` and `CWAI` wake-up fixed (MC6809 and HD6309).** Found with the
[6809/6309 functional test suite](https://github.com/reyco2000/6809_6309_functional_tests)
(`6809_interrupt_test`, test `$10`).

- `SYNC` no longer hangs when a masked interrupt line is already active. It
  now ends as soon as IRQ or FIRQ is active, masked or not, and carries on
  with the next instruction if the interrupt is masked.
- `CWAI` no longer ends when a masked interrupt line goes active. It waits
  until an interrupt is taken.
- The CPU run loop now makes this decision; the per-scanline wake-up in
  `machine.cpp` is gone.
- The suite passes all 8 runs on the host, on both CPU builds; **not yet
  checked on the board**.

### v0.15.1 — October 10, 2026

**CoCo 3 memory size.** Setup → Machine → CoCo 3 Memory selects 128K, 512K,
1024K or 2048K; the choice is saved and the emulator restarts with it.

- 128K behaves like an unexpanded CoCo 3. 1024K and 2048K emulate a memory
  expansion board: two extra bits in each MMU register, and the video bank in
  `$FF9B`.
- 2048K uses half of the board's PSRAM; if it cannot be allocated at boot the
  machine starts with 512K.
- The debug API reports the size and can set it (`POST /api/machine` with
  `ram=2048`).
- Checked on the host (`tools/gime_ram_test`); **not yet tested on the board**.

**Machine submenu.** Machine type, CPU and CoCo 3 Memory now share a
**Machine** submenu under Setup, because the Setup list holds seven rows. The
CPU option moved from Setup → CPU to Setup → Machine → CPU.

### v0.15.0 — October 10, 2026

**HD6309 CPU.** Setup → CPU (Setup → Machine → CPU since v0.15.1) switches between the Motorola MC6809 and the
Hitachi HD6309; the choice is saved and the emulator restarts with it.

- The 6309 adds the E, F, W, V and MD registers, all of its extra instructions
  and addressing modes (including TFM block moves and the 16- and 32-bit
  multiply and divide), native mode with its shorter timing and larger
  interrupt frame, and the illegal-instruction and divide-by-zero trap.
- On the 6309, undefined 6809 opcodes trap as they do on the real chip. Stay on
  the MC6809 for software that relies on them.
- The MC6809 is untouched: its compiled interpreter is byte-for-byte the same
  as in v0.14.0.
- **Status:** the 6309 core matches XRoar's over millions of random
  instructions in a host test (`tools/cpu_test`), but it has **not yet been
  tested on the board**.
- The debug API and MCP bridge report the CPU, read and write the 6309
  registers, and can switch the CPU (`POST /api/machine` with `cpu=6309`).

**Orchestra-90/CC sound cartridge.** Setup → Orchestra-90 turns on the
cartridge's two DACs at `$FF7A`/`$FF7B` and loads `orch90.rom` in place of Disk
BASIC, starting it automatically.

- The two channels are mixed to mono, because the board has one DAC pin.
- The toggle needs `orch90.rom` in `/roms/` and restarts the emulator; turning
  it off brings Disk BASIC back.

**Documentation.** New [docs/cpu.md](docs/cpu.md) (MC6809 and HD6309) and
[docs/orchestra90.md](docs/orchestra90.md); `docs/core.md` now covers the
support chips and the machine only.

Older releases (v0.14.0 and earlier) are in [ChangeHistory.md](ChangeHistory.md).

### v0.14.0 — October 6, 2026

**Supervisor redesign.** Every OSD screen now shares one look, taken from the
CoCo boot screen: a green box, black text and a dark blue accent.

- **Main menu** is a 3x2 grid of icon tiles: Disks, Setup, Reset, Debug, About,
  Resume. Icons are drawn in code.
- **Disk Manager** has a button per drive, a denser file list (8 rows, names up
  to 63 characters) and popups for mounting, replacing and unmounting, all
  defaulting to No.
- **Setup** is an icon list. Machine moved here from the main menu and is now a
  popup; Keyboard Language and Key Mapper moved into a new **Keyboard**
  submenu; Mouse Sensitivity is now the **Joy - Mouse Sensitivity** popup.
- **Key Mapper** draws the CoCo 2 or CoCo 3 keyboard and is navigated with the
  arrow keys, replacing the scrolling list.
- **Debug** is an icon list; "Debug Log" moved here from Setup as **Echo Log**.
  The status, hex dump, RS-232 and Dump RAM screens use the new frame.
- **WiFi / Debug** shows a status panel above its five actions, which now fit
  without scrolling.
- **DriveWire** replaces "Mode" with an **External Server** on/off row. Turning
  it on asks for Host, Port and ROM in a popup and checks the HDB-DOS ROM is on
  the SD card. Host, Port and ROM rows show only while it is on, and a warning
  appears when WiFi is not configured.
- Screens repaint only the rows, tiles or keys that changed, so moving the
  selection no longer flickers.
- The TAB key now reaches the supervisor.

**WiFi set up from the keyboard or the SD card.** The `CoCo3-Setup` access
point and its web page are gone; the board no longer opens a network of its
own.

- **Config WiFi** (Setup → WiFi / Debug) scans, lists the networks with their
  signal strength, and takes the password from the PS/2 keyboard.
- **Read WiFi from SD Card** loads the network from `cocowifi.cfg` in the root
  of the SD card:

  ```ini
  [WiFi]
  enabled=1
  SSID=MyNetwork
  passphrase=secret
  ```

  `enabled=0` saves the network but leaves WiFi off. The file is read only when
  you choose the row, and the password in it is plain text.

### v0.12.2 — October 1, 2026

**CoCo 3 frame time roughly halved, so 60 FPS now has real headroom.** v0.12.1
ran 2–3 FPS under 60 in `WIDTH 40`, `WIDTH 80` and `HSCREEN 2`: frames were
taking almost the whole 1/60 s. They now take about half of it.

| CoCo 3 mode | v0.12.1 | v0.12.2 |
|---|---|---|
| BASIC prompt, `PMODE 4`, `HSCREEN 3` | 59–60 FPS | **60** |
| `WIDTH 40` | ~57.5 | **60** |
| `WIDTH 80` | ~56 | **60** |
| `HSCREEN 2` | ~56 | **60** |
| Full-screen `HCLS` loop in `HSCREEN 2` | — | **60** |

- **Chip-bug workaround applied only where needed.** ESP32 chips before
  revision 3 need a compiler workaround for a PSRAM bug, which slows every
  memory store. It now stays off in the CPU, GIME, PIA and audio code, which
  only store to internal RAM, and on everywhere else. This alone removed about
  40% of each frame. Build with `-DPSRAM_MEMW_KEEP` to apply it everywhere again.
- **Faster 6809 memory access.** The CPU core reads and writes ordinary RAM
  directly through a page table that follows the GIME MMU, instead of calling
  out for every byte.
- Tested with a disk game and OS-9 at a steady 60 FPS.

### v0.12.1 — October 1, 2026

- **Fixed: black rectangle left on screen after closing the supervisor menu in
  CoCo 3 mode** (introduced in v0.12.0). The menu's clear-on-close could land
  after the screen had been redrawn, and unchanged lines were then never
  repainted.

### v0.12.0 — October 1, 2026

**CoCo 3 runs at full speed: 60 FPS in text and graphics modes.**

| CoCo 3 mode | v0.11.0 | v0.12.0 |
|---|---|---|
| BASIC prompt (32-column) | ~39 FPS | **60** |
| `WIDTH 40` | ~34 | **60** |
| `WIDTH 80` | ~24 | **~58** |
| `HSCREEN 2` (320×192, 16 colours) | ~20 | **60** |
| `HSCREEN 3` (640×192, 2 colours) | ~25 | **60** |
| `PMODE 4` | ~33 | **60** |

Measured on a TTGO VGA32 v1.4 with the built-in FPS counter. A program that
redraws the whole screen every frame can still dip below 60.

- **Faster GIME video.** Each video mode now has its own tight scanline
  renderer, instead of one routine that re-checked the mode for every four
  pixels. Decoding a 320×192 16-colour frame went from 32 ms to 5 ms.
- **Direct VGA output.** The renderer produces the final VGA bytes and the
  display code copies them 32 bits at a time, replacing a per-pixel lookup in a
  64 KB table (9.5 ms → 1.5 ms per frame).
- **Unchanged lines are skipped.** A scanline whose video memory and video
  settings are the same as in the previous frame is not redrawn.
- **Real-time frame pacing.** The emulator never runs faster than 60 frames per
  second, so cursor blink, `SOUND`/`PLAY` tempo and game speed match a real CoCo.
  The F5 FPS overlay therefore tops out at 60. Build with
  `FRAME_LIMIT_ENABLED 0` in `config.h` to remove the cap.
- **Debug API:** `GET /api/screenshot.png?fb=1` returns the picture actually on
  the VGA output (after scaling and borders).
- **Developer tools:** `tools/gime_render_test/` checks the new renderers
  against the original one on the host; `tools/perf/` holds the benchmark
  programs and measurement scripts.

Also in this release:

- **DriveWire is External-only.** The Internal DriveWire server (v0.11.0) and the
  planned embedded FujiNet were dropped: External mode against pyDriveWire, DW4
  or FujiNet-PC covers the same use. A board saved in Internal DW mode boots with
  the bus Off, so its mounted disks are served by the floppy controller again.
- **More free internal RAM:** mounted disk images no longer hold an open file
  (~5 KB → ~0.6 KB each), smaller debug-server and emulator task stacks, and a
  Debug Server On/Off setting (WiFi / Debug screen) that, when Off, never starts
  the server (+~7.8 KB).
- **Screenshots no longer drop WiFi** when memory is low (sent in 4 KB chunks).
- **Forget Credentials asks for confirmation.**
- **Debug API:** `/api/status` adds `fps`, `int8_*` (usable internal RAM),
  `srv_stack_free`, `loop_stack_free`, `osd_state`; new `POST /api/key` injects
  OSD keys.

### v0.11.0 — September 24, 2026

**Internal DriveWire mode — HDB-DOS with no PC and no WiFi.** See
[DriveWire / FujiNet Support](README.md#drivewire--fujinet-support-experimental) in the README.

- **`src/net/dw_server.*`** — a disk-only DriveWire 3/4 server built into the
  firmware (core 0). It serves the Disk Manager's drives 0–3 (the same `.DSK`
  images the floppy controller uses) as DriveWire drives 0–3; HDB-DOS
  `DRIVE 1`–`3` map to Disk Manager drives 1–3. `TIME` uses SNTP when WiFi is
  up. Survives CoCo resets mid-transaction.
- **Background flush** — DriveWire writes reach the SD card ~2 s after the
  last write. Flushes now write only the sectors that changed (floppy path
  too), instead of the whole image.
- **Debug API** — new `/api/disk` (list / mount / eject / flush), server
  counters in `/api/bus`, internal-RAM figures in `/api/status`.

### v0.10.0 — September 23, 2026

**Experimental DriveWire / FujiNet support (External mode).** See
[DriveWire / FujiNet Support](README.md#drivewire--fujinet-support-experimental) in the README.

- **`src/core/becker.*`** — an emulated Becker port (`$FF41`/`$FF42`) with
  lock-free ring buffers between the CPU core and a core-0 network task;
  flushes and reconnects cleanly across CoCo resets and link drops.
- **`src/net/dw_client.*` / `dw_bus.*`** — a TCP DriveWire client to an
  external server (pyDriveWire, DW4, FujiNet-PC), with settings (mode, host,
  port, HDB-DOS ROM variant) in NVS and a new `/api/bus` debug endpoint.
- **HDB-DOS ROM selection** — `hdbdw3bck.rom`/`hdbdw3bc3.rom` load in place of
  `disk11.rom` while the bus is enabled, with automatic fallback (and an OSD
  warning) if they're missing.
- **New Settings → DriveWire OSD screen** to configure and monitor the link.
- **`tools/dw_test_server.py`**, **`tools/dw_proxy.py`** — a minimal Python 3
  DriveWire test server and a logging TCP proxy, for testing without a full
  FujiNet-PC install.
- Closes the DriveWire socket cleanly before any supervisor-triggered restart,
  so an external server isn't left holding a dead connection.

### v0.9.0 — September 21, 2026

**ESP32_Bootloader support.** The emulator can now be launched from
[ESP32_Bootloader](https://github.com/ESP-WORKS/ESP32_Bootloader)'s SD-card menu,
so one TTGO VGA32 can hold several emulators and switch between them without a
USB cable. See [Running under ESP32_Bootloader](#running-under-esp32_bootloader-sd-card-menu).

- **New `BUILD_TARGET` switch in `config.h`**, defaulting to standalone. The
  bootloader image is selected with `-DBUILD_TARGET=1` on the command line, so
  both flavours build from identical sources and the USB build is untouched.

- **`otadata` is blanked at the top of `setup()`** in bootloader builds, before
  Serial, video or the SD probe. Without it the ESP32 would boot this app on
  every power-up and the menu would become unreachable; with it, the ROM falls
  back to the `factory` partition and the menu returns. The call compiles out
  entirely in standalone builds.

- **No partition-scheme change.** An ESP32 app image carries no partition table
  and both `0x10000` and `ota_0`'s `0x130000` are 64 KB-aligned, so the same app
  runs at either offset. `huge_app` stays the standalone scheme.

- **`tools/build_firmware.sh` now builds both images** and validates them:
  `0xE9` bare-image magic, the 2816 KB `ota_0` size limit, and a disassembly
  check that the `otadata` erase is present in the bootloader build and absent
  from the standalone one — the one failure that is otherwise silent.

### v0.81 — September 2, 2026

**CoCo 2 VDG colour accuracy.** Two bugs in the MC6847 path cancelled each other
out, so artifact-colour games looked correct while true 4-colour games did not.

- **PMODE 4 (RG6) is no longer rendered as PMODE 3 (CG6).** The VDG's GM0–GM2
  mode bits were being taken from the SAM's V0–V2. On real hardware they come
  from PIA1 PB4–PB6; the SAM's V bits only describe fetch geometry, which CG6
  and RG6 share (both `V=110`, 6144 bytes, 32 per row), so the SAM cannot tell
  the two apart. Every PMODE 4 screen was being decoded as 2bpp colour.

- **CG6 with CSS=1 now uses the MC6847's real palette** — buff, cyan, magenta,
  orange — in place of the NTSC artifact quad. Fixes titles such as *Pooyan*,
  which rendered as black/blue/orange/white.

- **NTSC artifact colour is now emulated where it actually occurs:** the 1bpp
  RG6 path, where the pixel clock runs at the colour-subcarrier rate and a
  composite TV decodes adjacent pixel *pairs* as black / blue / orange / white.
  Always on, matching XRoar's default for NTSC machines — games such as
  *Zaxxon* are drawn for it and are near-unreadable without it. The
  lower-resolution RG modes clock at half that rate and stay monochrome.

Verified against XRoar on hardware: the *Pooyan* title screen now matches the
reference pixel-for-pixel (0 of 49152 pixel codes differ), and injected RG6 and
CG6 test patterns produce the correct palette in each mode.
