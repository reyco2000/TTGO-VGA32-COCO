# Plan: Embedded FujiNet (+ generic DriveWire) for the TTGO CoCo 2 / CoCo 3 emulator

## Context
The goal is to let the emulated CoCo 2 and CoCo 3 use **FujiNet**: the CONFIG app, TNFS/HTTP disk images, the `N:` network device and the clock, running **standalone inside the TTGO firmware** (user's choice) with no second device.

**How FujiNet works on real hardware** ([fujinet-hardware/Coco](https://github.com/FujiNetWIFI/fujinet-hardware/tree/master/Coco)):
- An ESP32 cartridge runs `fujinet-firmware`.
- The cartridge ROM is HDB-DOS DriveWire 3.
- The CoCo talks **DriveWire** to the ESP32 through the bit-banger serial port.

**How emulators do it:** XRoar, MAME and VCC use the **Becker port** instead. It is two I/O addresses: $FF41 status (bit 1 = byte ready) and $FF42 data. They pair it with the HDB-DOS Becker ROMs (`hdbdw3bck.rom`, `hdbdw3bc3.rom`).

**CoCo-side software:**
- `fujinet-lib` and CONFIG call HDB-DOS's DWRead/DWWrite vectors (`JSR [$D93F]`).
- Because of that, the physical DriveWire transport (bit-banger, Becker, 6551) is transparent to all CoCo software.

**Our design:**
- The DriveWire **back end** is independent of the transport:
  - External: a TCP client to pyDriveWire, DW4 or FujiNet-PC on port 65504.
  - Internal: a disk-only DriveWire server first, then vendored FujiNet.
- The emulated **Becker port** is the **front end**. It connects to the back end on core 0 through ring buffers.

### Transport decision: Becker only (no bit-banger, no RS-232 Pak)
**Bit-banger emulation is out of scope:**
- DriveWire over the bit-banger is a cycle-counted loop on PIA1 at 57600 baud (CoCo 2) or 115200 (CoCo 3).
- The emulator would have to decode TX bits from PIA write cycle timestamps and present RX bits at exact CPU cycles, including GIME speed changes.
- It gains nothing: HDB-DOS, NitrOS-9 (Becker builds) and fujinet-lib all go through the DWRead/DWWrite vectors.
- XRoar, MAME and VCC skip it for the same reason.

**The RS-232 Pak is supported by HDB-DOS** (verified in toolshed `hdbdos/Makefile`):
- `hdbdw3sy.rom` (CoCo 1/2) and `hdbdw3s3.rom` (CoCo 3) use a 6551 at **$FF68**, the same address as our existing `mc6551` pak.
- **Considered and rejected** as a DriveWire transport:
  - It adds no ability: all CoCo software reaches DriveWire through the same DWRead/DWWrite vectors.
  - It's slower: the emulated 6551 tops out at 19200 baud and paces bytes in emulated cycles, while the Becker port has no speed limit.
  - Its only unique use (a raw serial link to a PC) collides with the debug log on UART0, and External mode over WiFi already covers "server on another machine".
  - `hal_rs232.cpp` is untested. Testing it is a separate task, not part of this work.

### DWRead has no timeout (verified in toolshed `hdbdos/dwread.asm`)
- The standard `BECKER` and `SY6551N` DWRead loops spin with interrupts masked until a byte arrives ("There is no timeout currently on here...").
  - Slow TNFS/HTTP replies therefore **cannot** cause spurious timeouts, so no emulated-time stalling is needed.
  - The real risk is the opposite: **if the back end dies mid-transaction, the emulated CoCo hangs forever.**
- The `BECKERTO` builds (`hdbdw3bckt.rom`, `hdbdw3bc3t.rom`) add a 2 s timeout. It is counted in VSYNC jiffies, which is emulated time.
- See the dead-link handling in Phase 1.

### Code survey findings
Line numbers drift, so re-check every `file:line` before starting a phase and prefer function names.
- **Existing interfaces:**
  - No Becker, DriveWire or bit-banger support exists.
  - An **RS-232 Pak exists** but is untested: `src/core/mc6551.*`, `src/hal/hal_rs232.cpp` (UART0 transport, 256-byte static ring, polled), mapped at $FF68–$FF6B in `machine.cpp` behind `rs232_pak_enabled()`. Its ring and poll pattern is the model for `becker.cpp`.
- **I/O dispatch:**
  - The address range $FF40–$FF5F goes to `sv_disk_read/write` (`src/supervisor/sv_disk.cpp`).
  - `$FF40–$FF47` is treated as the DSKREG latch, so a write to $FF41 or $FF42 currently changes DSKREG.
  - CoCo 3 routes the cartridge I/O area through GIME select `S=6` (`src/core/tcc1014.cpp`, `machine.cpp` read/write dispatch).
- **Cartridge ROM:**
  - It is hard-coded as `ROM_DISK_FILE "disk11.rom"` (`config.h`, around line 201).
  - CoCo 2 loads 8 K. The HDB-DOS DriveWire ROMs are also 8 K, so **no change to the 8 K cap is needed**.
  - CoCo 3 maps it at $C000–$DFFF.
- **SD usage:** `sv_disk` touches SD only at mount, eject and flush (`sv_disk_flush_all` is called from `hal_keyboard.cpp`, on core 1). Sector I/O otherwise goes to the PSRAM cache.
- **Runtime and memory:**
  - WiFi uses the Arduino `WiFi` STA mode on core 0 (`src/net/wifi_mgr.cpp`).
  - The debug server runs as a core-0 task (`debug_server.cpp`, `xTaskCreatePinnedToCore`).
  - Emulation runs on core 1 in `machine_run_frame()` bursts, and nothing may block inside a frame.
  - Internal DRAM is tight: **~54 KB free after init** (measured 2026-09-22 with WiFi up; the earlier ~160 KB figure was before WiFi/FabGL). PSRAM has about 3.5 MB free.
- **Flash budget:**
  - The standalone build uses huge_app (3 MB).
  - The **ESP32_Bootloader build puts the app in `ota_0`, which is capped at 2816 KB** (`BOOTLOADER_OTA0_MAX_BYTES`). `tools/build_firmware.sh` enforces this.
  - The app is ~1.1 MB today.
- **Toolchain and licensing:**
  - esp32 core **2.0.17 (ESP-IDF 4.4)**, which FabGL requires.
  - Current FujiNet builds with **espressif32 6.x (IDF 5)**, but historically it also supported IDF 4.x.
  - Licensing is compatible: both projects are GPLv3.

## Architecture
```
CORE 1  CoCo SW → HDB-DOS DW ROM → front end ──────────────┐
          Becker $FF41/$FF42 (becker.cpp, SPSC rings)       │  non-blocking,
                                                            │  batched notify
CORE 0  dw_service task: back end (one at a time) ◄─────────┘
          ├─ External: TCP client ↔ host:65504 (pyDriveWire / DW4 / FujiNet-PC)
          ├─ Internal-DW: native disk-only server (SD .DSK images)           [Phase 3]
          └─ Internal-FujiNet: vendored FujiNet dispatcher                   [Phase 4+]
               ├─ disk (READEX/WRITE)   ├─ FUJI 0xE2 (slots, mount, wifi info)
               ├─ NET 0xE3 (TCP, HTTP/S, TNFS, JSON)   └─ CLOCK 0xE5 / TIME / DWINIT
```
Bus mode is stored in NVS: **Off / External DriveWire / Internal DriveWire / Internal FujiNet**.

## Status
- **Phase 1 done** (d9b89ee): Becker port, External TCP client, `/api/bus`, `tools/dw_test_server.py`.
- **Phase 2 done**: HDB-DOS ROM selection with disk11 fallback, OSD screen (Settings → DriveWire), link reset on CoCo reset. Verified on hardware: HDB-DOS boots, `DIR` and `SAVE` over DriveWire on CoCo 2 and CoCo 3.

## Phase 0: Feasibility spike (go/no-go for the FujiNet port strategy)
Phase 0 can run in parallel with Phases 1–3, since those phases don't depend on FujiNet code.
- **Pin a fujinet-firmware commit.**
  - First check that `BUILD_COCO` / DriveWire support exists on an IDF-4.x-compatible tag.
  - If DriveWire only exists on IDF 5, the clean-room path is the realistic one.
- **If porting:** vendor the needed subset into `TTGO-VGA32-COCO/src/fujinet/`: `lib/bus/drivewire`, `lib/device/drivewire`, `fujiDevice`, `NDevice`, `network-protocol` (TCP/HTTP/TNFS/UDP only), `TNFSlib`, `fnjson`, `FileSystem`, `config`, `utils`, `compat`, and `hardware/IOChannel`.
- **Write a compat shim** (`src/fujinet/shim/`) in place of FujiNet's own system, WiFi, storage and config layers:

  | FujiNet layer | Shim maps it to |
  |---|---|
  | `fnSystem` | FreeRTOS / `esp_timer` |
  | `fnWiFi` | read-only view of `wifi_mgr` |
  | `fnSDFAT` | the existing SD HAL (`src/hal/hal_storage.cpp`) |
  | `fnFsSPIFFS` | SD `/fujinet/` |
  | `fnConfig` | `/fujinet/fnconfig.ini` on SD |
  | LED manager | no-op |
  | web UI | none (port 80 belongs to the debug server) |

- **Exit criteria:**
  - It compiles under core 2.0.17.
  - **Both** builds pass `tools/build_firmware.sh`. This means the bootloader app stays **under 2816 KB** (about 1.6 MB of headroom today); that is the binding limit, not huge_app's 3 MB.
  - Boot heap stays OK, with WiFi and the debug server running.
- **Fallback if IDF 5 APIs are too entangled:** a clean-room reimplementation of FUJI and NET (TCP/HTTP/TNFS) on top of the Phase 3 native DriveWire server, using the firmware sources as the protocol spec.

## Phase 1: Becker port device + external DriveWire client (de-risks the CPU side)
- **New `src/core/becker.cpp/h`**, modeled on XRoar `becker.c` and the `hal_rs232.cpp` ring pattern:
  - `becker_read_status()` returns 0x02 if the RX ring is non-empty.
  - `becker_read_data()` pops a byte (or returns 0).
  - `becker_write_data()` pushes a byte.
  - Rings are lock-free single-producer/single-consumer, allocated in PSRAM: RX ~4 KB, TX ~1 KB.
- **Cross-core wakeup:**
  - Never notify core 0 per byte.
  - Set a flag when the TX ring goes from empty to non-empty.
  - Core 1 sends one `xTaskNotifyGive` per frame, at the end of `machine_run_frame()`, if the flag is set.
- **Dead-link handling:** because DWRead has no timeout, a lost back end hangs the CoCo with interrupts masked.
  - On disconnect, flush both rings and set `link_down`.
  - While `link_down`, `becker_read_status()` keeps reporting "no data". Recovery happens on reconnect, when the next CoCo transaction resyncs through the DWINIT/RESET opcodes.
  - Surface it in the OSD and `/api/status` so the user knows why the machine froze. Document the `BECKERTO` ROMs (2 s timeout) as the fail-safe option.
- **Address decode:**
  - When bus mode ≠ Off, handle $FF41 (read) and $FF42 (read/write) in `sv_disk_read/write` before the DSKREG branch, following XRoar `rsdos.c`. The DSKREG latch then responds only at $FF40.
  - This covers CoCo 2 and CoCo 3 through the existing dispatch paths.
  - **Verify CoCo 3 GIME MC2/SCS gating of $FF40–$FF5F here**, not later.
- **New `src/net/dw_client.cpp`:** a core-0 task using a non-blocking `WiFiClient`, auto-reconnect, and shuttling bytes between the rings and the socket.
- **WiFi lifetime:**
  - Today STA mode mainly serves the debug server. External and Internal modes need WiFi to come up at boot, independent of the debug server.
  - Decide whether the debug server stays on alongside it (default: yes), and measure the heap with both running.
- **Verify** with pyDriveWire or FujiNet-PC (`./build.sh -p COCO`) on the Pi before any FujiNet code lands.
- **Automated protocol check through MCP:**
  - `inject_code` a small 6809 routine at $4000 that writes OP_DWINIT and OP_TIME to $FF42 and polls $FF41.
  - Then `read_memory` the reply bytes.
  - This is faster and more precise than screenshots.

## Phase 2: Cartridge ROM selection + supervisor OSD
- **ROM selection:**
  - In `machine_load_roms_coco2/3`, when bus mode ≠ Off, load `/roms/hdbdw3bck.rom` (CoCo 2) or `/roms/hdbdw3bc3.rom` (CoCo 3) instead of `disk11.rom`.
  - These are 8 K, so the CoCo 2 loader needs no change.
  - An optional setting selects the `BECKERTO` variants (`hdbdw3bckt.rom` / `hdbdw3bc3t.rom`).
  - If a file is missing, fall back to disk11, show an OSD warning, and report `becker_rom: missing` in `/api/status`. Do not halt boot.
  - Add `ROM_BECKER_*` defines in `config.h`.
- **New `src/supervisor/sv_fujinet.cpp/h`**, cloned from the `sv_wifi.cpp` pattern:
  - rows: Mode (Off/External/Internal DW/Internal FujiNet), Host (IP or mDNS `.local` name), Port (default 65504), ROM variant (standard/timeout), Status (connected/link down, RX/TX bytes)
  - add a `SV_FUJINET` state in `supervisor.h`
  - add a Settings row in `sv_menu.cpp` (`SETTINGS_LABELS`, `settings_toggle`, `values[]`)
  - store keys in the NVS namespace `"sv"`
  - changing the mode triggers the same save-and-restart flow as `supervisor_set_machine_type()`
- **Status in `/api/status`:** add read-only fields in `debug_server.cpp` for bus mode, ROM variant and status, link state and byte counters, so MCP-driven testing can check them.

### Decision gate (after Phase 2)
At this point External mode pointed at **FujiNet-PC on the Pi** already gives the full FujiNet experience (CONFIG, TNFS, `N:`).
- Re-confirm that "no second device" is worth Phases 4–6 before continuing past Phase 3.
- Phase 3 is worth doing either way.

## Phase 3: Native Internal DriveWire server (disk-only, no FujiNet code)
- **New `src/net/dw_server.cpp/h`:** a minimal DriveWire 3/4 server on core 0.
- **Opcodes:** DWINIT, INIT/TERM, TIME, READEX/REREADEX, WRITE/REWRITE, the RESET codes. Serial and virtual-channel ops are acknowledged with no data. NAMEOBJ can come later.
- **Service task** on core 0 (pinned, priority 1, PSRAM-backed stack). It blocks on the frame notify and drains the TX ring.
- **Disks:**
  - DriveWire drives 0–3 are backed by .DSK images on SD.
  - The **supervisor Disk Manager mounts into DriveWire drives when bus mode is Internal DW**. Otherwise, swapping `disk11.rom` for HDB-DOS makes the supervisor's .DSK mounts invisible to the CoCo, which is a user-visible regression.
  - Reuse the `sv_disk` PSRAM cache and flush machinery rather than doing direct SD I/O per sector.
- **SD mutex:** add one in `hal_storage.cpp`.
  - Core 0 (the DriveWire server, and later FujiNet) takes it normally.
  - **Core 1 must never block mid-frame**: `sv_disk` flush and mount use try-lock and retry on the next frame.
- **What it gives:** standalone HDB-DOS and NitrOS-9 DriveWire booting with no Pi. It also proves out the core-0 service, the SD mutex and wakeup latency on simple code before FujiNet arrives.

## Phase 4: FujiNet integration (Internal FujiNet mode)
- **`BeckerChannel : IOChannel`** (port path) or the Phase 3 dispatcher extended (clean-room path):
  - `dataOut` pushes to the RX ring (toward the CoCo).
  - `updateFIFO` drains the TX ring.
  - It is used as `_port` in the drivewire bus instead of UART or BoIP.
- **Disks:** images live under SD `/fujinet/`.
- **Boot:**
  - Mount the CONFIG boot disk (`dist.coco` from [fujinet-config](https://github.com/FujiNetWIFI/fujinet-config)) on drive 0, following `fujiDevice::insert_boot_device()`.
  - Confirm the exact boot filename in Phase 0.

## Phase 5: FUJI device (0xE2)
- Host and device slots, mount/unmount, directory listing over SD and TNFS, new disk, and boot-mode config.
- WiFi commands (scan, set SSID, status) map to `wifi_mgr` so there is a single source of WiFi credentials. CONFIG's WiFi screen and the OSD WiFi screen share NVS.

## Phase 6: NET device (0xE3) + CLOCK (0xE5)
- The `N:` device with the TCP, UDP, HTTP/HTTPS, TNFS protocols and JSON parsing (`fnjson`).
- Leave out SSH, SMB, NFS, cloud services, CP/M, the printer, the modem and the cassette. They are compile-time excluded and can be added later.
- HTTPS runs on mbedTLS from IDF 4.4. Check that SPIRAM malloc keeps TLS buffers out of internal DRAM, and cap concurrent TLS sockets (2 at most).

## Phase 7: Hardening + docs
- **Performance:** confirm there is no frame-time regression (`PERF_PROBE_ENABLED`, FPS overlay) while traffic flows.
- **Memory:** log heap and PSRAM with the service active, WiFi and the debug server both running.
- **Link loss:** kill the back end mid-`DIR`. Check that the OSD and `/api/status` show link down, and that reconnect plus a CoCo reset recovers.
- **Docs:**
  - new `docs/fujinet.md` covering architecture, front end/back end split, SD layout (`/roms/hdbdw3b*.rom`, `/fujinet/…`), modes, ROM variants and the no-timeout caveat
  - update `CLAUDE.md`, `Architecture.md` and the README
- Run `graphify update .`.

## Critical files
- **New:** `src/core/becker.*`, `src/net/dw_client.*`, `src/net/dw_server.*`, `src/fujinet/**` (vendored subset + shim + BeckerChannel), `src/supervisor/sv_fujinet.*`, `docs/fujinet.md`
- **Modified:**
  - `src/supervisor/sv_disk.cpp`: Becker decode, DriveWire drive mounts
  - `src/core/machine.cpp`: ROM select, per-frame notify
  - `config.h`
  - `src/supervisor/sv_menu.cpp`, `supervisor.h/.cpp`
  - `src/hal/hal_storage.cpp`: mutex
  - `src/net/wifi_mgr.cpp`: bring up STA independent of the debug server
  - `TTGO-VGA32-COCO.ino`: start the service task by mode
  - `src/net/debug_server.cpp`: status fields

## Verification (both machines: `set_machine` via MCP, reboot, repeat)
1. **Phase 1:**
   - MCP `inject_code` DWINIT/TIME round-trip returns the expected bytes.
   - Run pyDriveWire or FujiNet-PC on the Pi and set the mode to External with the Pi's IP.
   - HDB-DOS boots and `DRIVE 0` / `DIR` lists the DW image (MCP `screenshot`).
   - `/api/status` byte counters increase.
   - Emulation FPS is unchanged.
2. **Phase 3:**
   - Set the mode to Internal DW and mount a .DSK from the supervisor.
   - HDB-DOS `DIR`, `LOADM`/`RUN` work.
   - Writes survive a reboot.
   - NitrOS-9 (Becker build) boots from an SD image with no Pi.
3. **Phase 4:**
   - Set the mode to Internal FujiNet.
   - On power-up, CONFIG auto-boots (or type `DOS`).
4. **Phase 5:**
   - CONFIG lists the `tnfs.fujinet.online` host, mounts a remote .DSK, and it boots.
5. **Phase 6:**
   - Run a fujinet-lib CoCo sample (for example an HTTP/JSON app) and see correct output.
   - An `N:` TCP echo test against a netcat server on the Pi.
6. **Regression:**
   - Mode Off restores `disk11.rom`.
   - The WD1793 floppy path still works (DIR on a mounted .DSK).
   - The WiFi debug API still responds.
   - The NitrOS-9 boot over External DriveWire works (generic DW case).
   - Both firmware builds pass `tools/build_firmware.sh` size checks.

## Key risks
- IDF 5 → 4.4 API drift in the vendored FujiNet code (Phase 0 decides the port vs. clean-room path).
- Bootloader `ota_0` 2816 KB limit with FujiNet + mbedTLS linked in.
- Internal DRAM exhaustion with WiFi + TLS + FabGL + the debug server.
- **CoCo hang on back-end loss:** standard DWRead has no timeout. Mitigated by link-down reporting and the optional `BECKERTO` ROMs.
- Core 1 blocking on the SD mutex (mitigated by try-lock on core 1).
- CoCo 3 GIME MC2 gating of the $FF40–$FF4F cartridge I/O area (verified in Phase 1).
