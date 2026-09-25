# DriveWire: Becker Port, External Client and Internal Server

The emulator speaks **DriveWire** through an emulated **Becker port**
(`$FF41` status, `$FF42` data), the same virtual port XRoar, MAME and VCC use.
It pairs the port with the HDB-DOS DriveWire ROMs. CoCo software, including
`fujinet-lib` and FujiNet's CONFIG, reaches DriveWire only through HDB-DOS's
`DWRead`/`DWWrite` vectors, so the transport is invisible to it.

| Mode | Back end | Needs |
|------|----------|-------|
| **Off** | none; `disk11.rom` and the WD1793 floppy controller | nothing |
| **External** | TCP client to pyDriveWire, DW4 or FujiNet-PC on port 65504 | WiFi and a server |
| **Internal DW** | built-in disk-only DriveWire server (`src/net/dw_server.*`) | nothing |

The mode is chosen in **F3 → Settings → DriveWire** (or `POST /api/bus`) and is
stored in NVS. Changing it saves and restarts.

## Two ways a CoCo reads a sector

Both paths serve the same Disk Manager drives 0–3 from the same PSRAM copies of
the `.DSK` images. What differs is the hardware the CoCo thinks it is talking
to, and which ESP32 core answers.

### Disk BASIC and the WD1793 (Mode: Off)

![Disk BASIC path: DSKCON drives the WD1793 registers; sv_disk.cpp answers each $FF4B read on core 1](images/disk-path-wd1793.svg)

With `disk11.rom` in the cartridge slot, BASIC drives an emulated WD1793 one
register at a time. Every byte of the sector is a CPU read of `$FF4B`, answered
inline on core 1 by `sv_disk.cpp`, and an INTRQ-driven NMI ends the transfer.
See [disk-hal.md](disk-hal.md) for the HALT/DRQ/NMI details.

### HDB-DOS over DriveWire (Mode: Internal DW)

![DriveWire path: HDB-DOS sends a request through the Becker port and cross-core rings to the dw_server task on core 0, which answers from the same PSRAM images](images/disk-path-drivewire.svg)

With `hdbdw3bc3.rom` (CoCo 3) or `hdbdw3bck.rom` (CoCo 2) in the slot there is
no floppy controller. HDB-DOS sends a short request through the Becker port; a
server task on core 0 answers with the whole sector.

### One READEX, byte by byte

`DIR 1` reading the directory (track 17, sector 3):

| Direction | Bytes | Meaning |
|-----------|-------|---------|
| CoCo → server | `D2 00 00 03 AA` | READEX, DW drive 0, LSN 938. That is past drive 0's 630 sectors, so it maps to Disk Manager drive 1, sector 308. |
| server → CoCo | 256 bytes | Sector data (zeros if the drive is empty, so HDB-DOS still gets 256 bytes). |
| CoCo → server | `hi lo` | 16-bit sum of the bytes it received. |
| server → CoCo | `00` | OK. `F3` = checksum mismatch (HDB-DOS retries with REREADEX), `F6` = drive not ready. |

### Side by side

| | Disk BASIC (WD1793) | HDB-DOS (Internal DW) |
|---|---|---|
| Cartridge ROM | `disk11.rom` | `hdbdw3bc3.rom` / `hdbdw3bck.rom` |
| What the CoCo sees | A floppy controller at `$FF40`–`$FF4B` | A byte pipe at `$FF41`/`$FF42` |
| Who answers | `sv_disk.cpp`, inline on core 1 | `dw_server` task on core 0 |
| One sector costs | Command, 256 register reads, NMI | 5-byte request, 256-byte reply, 2-byte checksum, status |
| Disk images | Disk Manager drives 0–3 | The same drives 0–3 |
| Writes reach SD | On eject, reset (F4 or OSD), restart | Also about 2 s after the last write |
| Select it | F3 → Settings → DriveWire → Mode: Off | F3 → Settings → DriveWire → Mode: Internal DW |

## Using Internal DW

1. Put `hdbdw3bc3.rom` (CoCo 3) and/or `hdbdw3bck.rom` (CoCo 2) in `/roms` on
   the SD card. Optional timeout builds: `hdbdw3bc3t.rom` / `hdbdw3bckt.rom`.
2. **F3 → Settings → DriveWire → Mode: Internal DW → Save & Restart.** The boot
   banner then reads `HDB-DOS 1.4 BECKER COCO 3`. Host and Port are only used
   by External mode.
3. Mount disks in **F3 → Disk Manager**: `←`/`→` pick drive 0–3, `ENTER` on a
   `.DSK` mounts it, `U` ejects, `F` flushes to SD. Mounts are remembered.
4. On the CoCo: `DIR 0`, `DIR 1`, `LOAD`, `SAVE`, `LOADM`/`EXEC` as usual.

The Settings → DriveWire screen shows the link state (`Serving` or `No disks`)
and byte counters.

Without a keyboard, the debug API does the same:

```bash
IP=192.168.8.190
curl -XPOST http://$IP/api/disk -d 'op=mount&drive=0&path=/PRUEBA.DSK'
curl http://$IP/api/disk        # what is mounted, dirty flags
curl http://$IP/api/bus         # server{reads,writes,errors,flushes,last_op,stack_free}
curl -XPOST "http://$IP/api/bus?mode=2"   # switch to Internal DW (reboots)
```

## Internal server details

- **Opcodes:** DWINIT, INIT/TERM, TIME, READ/READEX (+RE-), WRITE (+RE-),
  GETSTAT/SETSTAT, the RESET codes. Virtual-serial, print and named-object
  opcodes are consumed and answered "no data" / "not found".
- **Drive mapping:** HDB-DOS addresses `DRIVE n` as LSN `n × 630` (35 tracks ×
  18 sectors) on DriveWire drive 0. An LSN inside the addressed image is served
  as-is, so NitrOS-9 images and HDB-DOS hard-drive images work unchanged. An
  LSN past the end maps to Disk Manager drive `drive + LSN / 630`, sector
  `LSN % 630` (DW4's "HDB-DOS mode"). The one wrong case: HDB-DOS `DRIVE 1`–`3`
  behind a lower drive that holds an image larger than 35 tracks.
- **Concurrency:** the server copies to and from the PSRAM caches under
  `sv_disk_lock()`, a recursive mutex also taken by mount, eject and flush. Core 0
  holds it for one 256-byte copy or a background flush; core 1 takes it only from
  the OSD, F4 and API paths, never mid-frame. No global SD mutex is needed: the
  core's FATFS is built reentrant (`FF_FS_REENTRANT 1`).
- **Flushing:** each image keeps a one-bit-per-sector dirty map, so a flush
  writes only the changed sectors (768 bytes for a `SAVE` instead of 160 KB). The
  server flushes ~2 s after the last DriveWire write; eject, reset and restart
  flush as before.
- **CoCo reset:** aborts the transaction in progress, drops any reply still
  queued for the CoCo, and keeps the bytes the CoCo wrote after the reset.
- **TIME:** uses SNTP once WiFi is up; time zone is `DW_SERVER_TZ` in
  `config.h` (`UTC0` by default). Without WiFi it reports the unsynced clock.
- **Memory:** the task has a 4 KB internal-RAM stack (measured peak ~1.7 KB,
  including a flush). Each mounted disk also costs ~5 KB of internal RAM for its
  open `File`, in every mode.

## Caveats

- **No timeout in standard HDB-DOS.** `DWRead` spins with interrupts masked
  until a byte arrives. If the back end stops answering mid-command (External
  link lost), the CoCo freezes. The `t`-suffixed ROMs (Settings → HDB-DOS ROM:
  Timeout) give up after 2 s instead.
- **FujiNet-PC read timeout (External mode).** FujiNet-PC's Becker-over-IP
  transport waits only 500 ms per byte; a WiFi retransmit can desync it. A patch
  raising it to 5 s is in `tools/fujinet-pc-boip-timeout.patch`.

## Files

| File | Role |
|------|------|
| `src/core/becker.*` | Becker port, lock-free TX (1 KB) / RX (4 KB) rings in PSRAM |
| `src/net/dw_bus.*` | Mode, NVS settings, ROM selection, back-end start/stop |
| `src/net/dw_client.*` | External back end: TCP client with auto-reconnect |
| `src/net/dw_server.*` | Internal back end: disk-only DriveWire server |
| `src/supervisor/sv_disk.*` | Disk images, PSRAM caches, drive lock, dirty-sector flush |
| `src/supervisor/sv_fujinet.*` | Settings → DriveWire OSD screen |
| `tools/dw_test_server.py` | Minimal Python 3 DriveWire server for testing External mode |
| `tools/dw_proxy.py` | Logging TCP proxy for byte-level traces |
