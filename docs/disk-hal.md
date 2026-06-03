# TTGO-VGA32-COCO Disk HAL — Implementation Details

## Overview

The disk subsystem emulates a CoCo Disk Controller (WD1793-based FDC) with up to 4 floppy drives. It supports `.DSK` (JVC) and `.VDK` disk image f
ormats loaded from SD card. The implementation matches XRoar's `rsdos.c` for DSKREG bit assignments, HALT/DRQ synchronization, and NMI/INTRQ gatin
g.

**Source files:**
- `src/supervisor/sv_disk.h` — Data structures and public API
- `src/supervisor/sv_disk.cpp` — WD1793 FDC emulation + DSKREG logic
- `src/supervisor/supervisor.cpp` — Wiring of NMI/HALT callbacks to MC6809
- `src/hal/hal_storage.cpp` — SD card access (dedicated HSPI bus)
- `src/core/machine.cpp` — FDC tick integration in scanline loop

---

## Hardware Being Emulated

### Real CoCo Disk Controller

```
                         $FF40 DSKREG (drive select latch)
                              │
        ┌──────────┬──────────┼──────────────────┐
        │          │          │                   │
   Drive Select  Side     HALT Enable          Density
   (bits 0-2)   (bit 6)   (bit 7)             (bit 5)
        │                     │                   │
        v                     v                   v
   ┌─────────┐         DRQ ──┤             NMI gating
   │ WD1793  │──INTRQ───────►├──► CPU NMI
   │   FDC   │──DRQ──────────┤──► CPU HALT
   │         │               │
   │ $FF48-4B│               │
   └─────────┘               │
                         CPU 6809E
```

### Register Map

| Address | R/W | Register | Description |
|---------|-----|----------|-------------|
| $FF40-$FF47 | W | DSKREG | Drive select latch (active-low drive lines) |
| $FF48 | R | Status | WD1793 status (clears INTRQ on read) |
| $FF48 | W | Command | WD1793 command register |
| $FF49 | R/W | Track | Current track number |
| $FF4A | R/W | Sector | Current sector number (1-based!) |
| $FF4B | R/W | Data | Data transfer register |

### DSKREG Bit Assignments ($FF40)

Matches XRoar `rsdos.c` — **NOT** the commonly-seen but incorrect "bit 7 = NMI enable":

| Bit | Function | Notes |
|-----|----------|-------|
| 0 | Drive 0 select | Active high, directly selects drive |
| 1 | Drive 1 select | |
| 2 | Drive 2 select | |
| 3 | Motor on | |
| 4 | Write precomp | (not emulated) |
| 5 | Density | XOR'd internally; gates NMI path |
| 6 | Side select | Selects floppy head (0=side 0, 1=side 1) |
| 7 | **HALT enable** | Connects DRQ to CPU HALT line |

---

## HALT + DRQ Synchronization

This is the "hardware trick" that makes CoCo disk I/O work. Without it, the CPU free-runs through the data transfer loop and NMI timing is unrelia
ble.

### How It Works

1. DSKCON writes DSKREG with bit 7 = 1 → `halt_enable = true`
2. FDC asserts DRQ when a data byte is ready → `signal_halt(false)` → CPU runs
3. CPU reads byte via `LDA $FF4B` → FDC clears DRQ → `signal_halt(true)` → CPU halted
4. FDC prepares next byte → DRQ asserts again → CPU released
5. After 256 bytes: FDC asserts INTRQ → HALT disabled, CPU released, NMI fires

### Implementation

```cpp
// DRQ callback — used by DSKREG write and WRITE TRACK/SECTOR paths
// NOT used during READ SECTOR byte delivery (see note below)
static void set_drq(SV_DiskController* fdc, bool value) {
    fdc->drq = value;
    if (value) {
        signal_halt(fdc, false);     // Data ready → release CPU
    } else {
        if (fdc->halt_enable)
            signal_halt(fdc, true);  // No data → halt CPU
    }
}

// INTRQ callback (matching XRoar rsdos.c set_intrq)
static void set_intrq(SV_DiskController* fdc, bool value) {
    fdc->intrq = value;
    if (value) {
        fdc->halt_enable = false;    // INTRQ disables HALT
        signal_halt(fdc, false);     // Release CPU
    }
    // NMI gating runs unconditionally (matches XRoar exactly)
    if (!fdc->density && fdc->intrq) {
        signal_nmi(fdc, true);       // Fire NMI via density gating
    } else {
        signal_nmi(fdc, false);      // Deassert NMI for edge detection
    }
}
```

**Note on READ SECTOR:** During byte-by-byte reads, `fdc->drq` is set directly (not via `set_drq()`) to avoid engaging HALT. Our non-cycle-accurat
e emulation cannot model the propagation delay between DRQ clearing and HALT engaging — calling `set_drq(false)` would halt the CPU before STA sto
res the byte. The CPU free-runs through the read loop, and the byte-257 mechanism handles INTRQ timing.

The HALT callback is wired to `cpu.halted` in `supervisor.cpp`. When `mc6809_run()` sees `halted = true`, it burns remaining cycle budget, effectively pausing the CPU.

### HALT Deadlock Fix (April 2026)

**Bug:** `fdc_write_drive_select()` called `signal_halt(fdc, halt_enable && !drq)` unconditionally. The standard DSKCON sequence writes DSKREG (enabling HALT) *before* issuing the FDC command. On real hardware, the CPU completes the next instruction (the command write to $FF48) before HALT takes effect. In our scanline-based model, `cpu->halted = true` takes effect at the next loop iteration — the command instruction never executes, DRQ never goes true, and the CPU is permanently deadlocked.

**Fix:** Added `&& fdc->busy` to the halt condition:

```cpp
signal_halt(fdc, fdc->halt_enable && !fdc->drq && fdc->busy);
```

HALT now only engages when an FDC command is actually in progress. Since our FDC completes commands instantly (entire sector available immediately), DRQ is always true during active transfers, so HALT effectively never engages — which is correct for an instant-completion FDC model.

---

## NMI / INTRQ Mechanism

NMI is **NOT** controlled by a dedicated "NMI enable" bit. Instead, it fires automatically when INTRQ asserts, gated by the density bit (bit 5 of
DSKREG, XOR'd).

### NMI Gating (matching XRoar rsdos.c)

Both `set_intrq()` and `fdc_write_drive_select()` use the **same** NMI condition, exactly matching XRoar:

```cpp
if (!fdc->density && fdc->intrq) {
    signal_nmi(fdc, true);
} else {
    signal_nmi(fdc, false);
}
```

**Path 1 — INTRQ callback** (normal CoCo case, density bit 5 = 1 in original write):
- DSKREG write `$A9`: original bit 5 = 1 → after XOR → `density = false`
- When INTRQ asserts: `!density && intrq` → true → `signal_nmi()` fires immediately

**Path 2 — DSKREG write** (density bit 5 = 0 in original write):
- Original bit 5 = 0 → after XOR → `density = true`
- In `fdc_write_drive_select`: `!density` is false → NMI not fired from this path
- NMI fires later when `set_intrq(true)` is called with `!density` true

The `else` branch (deassert NMI) is critical for proper edge detection — without it, a stale NMI assertion can persist across operations.

### INTRQ Lifecycle

```
Command written ($FF48)     → set_intrq(false)  — clear any stale INTRQ
Command completes (Type I)  → set_intrq(true)   — immediate NMI
Sector read byte 256        → intrq_defer = 5   — defer INTRQ
CPU reads $FF4B (byte 257)  → set_intrq(true)   — NMI fires (primary path)
sv_disk_tick() countdown    → set_intrq(true)   — safety-net fallback (5 scanlines)
Sector write byte 256       → set_intrq(true)   — immediate NMI (last byte already written)
Write track ~6250 bytes     → set_intrq(true)   — immediate NMI (track format complete)
Status read ($FF48)         → set_intrq(false)  — DSKCON reads result
```

### Deferred INTRQ for Sector Reads (Byte-257 Mechanism)

After the CPU reads byte 256 (last byte of a sector), INTRQ is NOT fired immediately. Instead, `intrq_defer = 5` is set. The DSKCON read loop (`LD
A $FF4B / STA ,X+ / BRA`) naturally loops back to read `$FF4B` again (the "byte 257 attempt"). This triggers `fdc_read_data()` which detects `intr
q_defer > 0` and fires `set_intrq(true)` immediately.

**Why this works:** By the time the CPU reads byte 257, the STA ,X+ for byte 256 has already executed — the last byte is safely stored in RAM. NMI
 then preempts the stale byte 257's STA, which is exactly what the DSKCON NMI handler expects (it manipulates the stack to skip the read loop).

**Why not fire INTRQ immediately on byte 256?** Because INTRQ/NMI would preempt the STA ,X+ for byte 256 — the last byte would be lost.

**Why not defer by just 1 scanline (the old approach)?** With `intrq_defer = 1`, `sv_disk_tick()` could fire at the scanline boundary between the
LDA (byte 256) and STA ,X+, causing the same last-byte corruption. The byte-257 `$FF4B` read fires INTRQ in ~14 CPU cycles, well within a single s
canline.

**Safety net:** `sv_disk_tick()` counts down `intrq_defer` once per scanline. With a defer of 5 (~285 CPU cycles), the byte-257 path always wins t
he race. The tick only fires if the CPU somehow doesn't loop back to `$FF4B`.

**Why not use HALT synchronization for reads?** In real hardware, DRQ→HALT has a propagation delay that allows STA to complete before HALT engages
. Our non-cycle-accurate emulation cannot model this delay — `set_drq(false)` sets `halted=true` instantly, which would prevent STA from executing
. MAME's `update_lines()` uses combinational HALT (`!DRQ && HALT_ENABLE`) but relies on cycle-accurate scheduling. We bypass HALT for reads by set
ting `drq` directly (no `set_drq` callback).

---

## DSKCON Read Flow (Disk BASIC ROM)

```
DSKCON:
  1. Write DSKREG ($FF40): motor + drive, HALT=0
  2. RESTORE ($FF48 = $03): FDC → INTRQ → NMI
  3. Write DSKREG ($FF40): motor + drive + HALT=1
     → INTRQ pending → HALT disabled (INTRQ overrides)
     → NMI fires from set_intrq
  4. NMI handler returns to DSKCON
  5. SEEK ($FF48 = $13): same INTRQ → NMI pattern
  6. Set sector ($FF4A), READ SECTOR ($FF48 = $80)
     → INTRQ cleared, reading=true, read_pending=true
     → Sector data NOT loaded yet (deferred — see below)
  7. Write DSKREG ($FF40): side + HALT=1
     → Side select applied to fdc->side
     → INTRQ=false, DRQ=true → HALT not engaged yet
  8. CPU enters tight loop:
       LDA $FF4B    ; FIRST read triggers deferred sector load
                    ; (side select now correct from step 7)
       STA ,X+
       BRA loop
  9. After byte 256: intrq_defer=5 (deferred — STA not yet done)
 10. CPU executes STA ,X+ (stores byte 256), BRA loop
 11. CPU reads $FF4B (byte 257 attempt) → fdc_read_data fires set_intrq(true)
     → HALT disabled, NMI fires
 12. NMI handler (LEAS 12,S / RTS): returns to DSKCON
 13. DSKCON checks status, prepares next sector or exits
```

### Deferred Sector Load (April 2026)

**Bug:** The CoCo DSKCON (and NitrOS-9 CC3Disk driver) writes the FDC command register ($FF48) BEFORE writing DSKREG ($FF40) with the side select bit. On real hardware, the FDC takes time to seek and find the sector, so the side select wire is set before data arrives. In our instant-completion model, the sector was loaded immediately in `fdc_execute_command()`, using the **stale** side value from the previous DSKREG write.

**Symptom:** Every first sector after a side switch read the wrong side's data. NitrOS-9 360K boot corrupted every module that spanned a side boundary, crashing before Phase 5 (user handoff) with "Çp" garbage on screen.

**Fix:** READ SECTOR now sets `read_pending = true` without loading data. The actual `sector_offset()` calculation and `memcpy()` happen in `fdc_read_data()` on the first `$FF4B` read. By that time, DSKREG has been written with the correct side select.

---

## DSKCON Write Flow (Disk BASIC ROM)

```
DSKCON (WRITE):
  1. Write DSKREG ($FF40): motor + drive, HALT=0
  2. RESTORE / SEEK as needed (same as read)
  3. Set sector ($FF4A), WRITE SECTOR ($FF48 = $A0)
     → INTRQ cleared, writing=true, DRQ=true
  4. Write DSKREG ($FF40): HALT=1
     → HALT engaged (DRQ syncs byte pacing)
  5. CPU enters tight loop: LDA ,X+ / STA $FF4B / BRA loop
     → Each STA $FF4B calls fdc_write_data()
     → DRQ cleared after write, HALT engaged
     → DRQ asserted for next byte, CPU released
  6. After byte 256: set_intrq(true) IMMEDIATELY
     → HALT disabled, NMI fires
     → Safe because CPU has already written the last byte
       (unlike READ, where STA of last byte is still pending)
  7. NMI handler returns to DSKCON
  8. Dirty flag set — cache flushed to SD on eject
```

**Why WRITE SECTOR uses immediate INTRQ (not deferred):**
In READ SECTOR, INTRQ must be deferred because the CPU reads the last byte (LDA $FF4B) but hasn't stored it yet (STA ,X+). In WRITE SECTOR, the CP
U has already written the last byte (STA $FF4B) by the time the FDC processes it — the data is safely in the sector buffer. So INTRQ/NMI can fire
immediately.

**Why WRITE SECTOR uses DRQ/HALT (but READ does not):**
WRITE SECTOR uses `set_drq()` for proper DRQ/HALT synchronization because the HALT→release timing is safe: the CPU writes a byte (STA $FF4B), DRQ
clears (HALT engages), FDC accepts it and asserts DRQ (HALT releases). There's no timing race because the CPU has already completed the STA. For R
EAD SECTOR, `set_drq(false)` would halt the CPU before STA stores the byte, so reads bypass HALT entirely.

---

## WRITE TRACK (Format) — DSKINI Support

DSKINI uses the WD1793 WRITE TRACK command ($F0) to format each track. Unlike READ/WRITE SECTOR which transfer a single 256-byte sector, WRITE TRA
CK accepts a raw byte stream (~6250 bytes) representing the entire track including gaps, address marks, ID fields, and data fields.

### How DSKINI Formats a Track

For each of 35 tracks, DSKINI sends a raw format stream containing 18 sectors:
```
Per sector in the stream:
  GAP bytes ($4E)
  Sync bytes ($00)
  $F5 $F5 $F5    — address mark prefix (FDC converts to $A1 with clock violation)
  $FE             — ID Address Mark
  [track] [side] [sector] [size_code=01]  — 4-byte ID field
  $F7             — CRC (FDC generates 2 CRC bytes)
  GAP bytes
  $F5 $F5 $F5    — address mark prefix
  $FB             — Data Address Mark
  256 × $FF       — sector data (fill byte)
  $F7             — CRC
```

### State Machine Implementation

Since our disk images are sector-based (not raw track data), WRITE TRACK parses the incoming byte stream to extract sector data and writes it to t
he PSRAM cache:

```
States: WT_IDLE → WT_ID_FIELD → WT_WAIT_DAM → WT_DATA_FIELD → WT_IDLE

WT_IDLE:       Count $F5 bytes; on $FE after ≥3 $F5 → WT_ID_FIELD
WT_ID_FIELD:   Read 4 bytes (track, side, sector, size) → WT_WAIT_DAM
WT_WAIT_DAM:   Count $F5 bytes; on $FB after ≥3 $F5 → WT_DATA_FIELD
WT_DATA_FIELD: Buffer 256 bytes → memcpy to PSRAM cache → WT_IDLE
```

After 6250 bytes (one full track revolution), the command completes with deferred INTRQ, same as WRITE SECTOR.

### Key Details

- The `write_track` flag is separate from `writing` (WRITE SECTOR) to keep the two paths independent
- Sector number is captured from byte 2 of the ID field (the sector number DSKINI assigns)
- Each sector's 256 data bytes are written to `disk->cache` via `sector_offset(track, side, sector)`
- `disk->dirty = true` is set so the formatted image flushes to SD on eject
- State is cleared on FORCE INTERRUPT, new command, or reset

---

## PSRAM Disk Cache

Entire `.DSK` images are loaded into PSRAM at mount time. All sector reads/writes during emulation go to the in-memory cache — zero SD card access
.

### Memory Budget
- Standard SS .DSK: 35T × 18S × 256B = **161,280 bytes**
- Double-sided 360K: 40T × 2H × 18S × 256B = **368,640 bytes**
- 4 drives max (DS): ~1.4 MB total
- Available PSRAM: 4 MB on TTGO VGA32 (ESP32-WROVER) / 8 MB on the original S3 target — plenty of headroom either way

### Mount Sequence

```
1. Open .DSK file on SD card (FILE_READ)
2. Detect geometry (JVC header, track count, sector size)
3. Allocate PSRAM buffer: heap_caps_malloc(cache_size, MALLOC_CAP_SPIRAM)
4. Load via bounce buffer (512-byte internal RAM buffer):
     file.read(bounce, 512) → memcpy(psram_cache, bounce, got)
   Bounce buffer avoids ESP32 SPI DMA issues with PSRAM destinations.
5. Close read handle, reopen as "r+" for write-back support
6. Cache is now the sole data source during emulation
```

### Write-Back (Flush/Eject)

When a mounted disk is dirty (sectors written):
```
1. Seek to header_size in SD file
2. Write via bounce buffer: memcpy(bounce, psram, 512) → file.write(bounce, 512)
3. file.flush()
4. Clear dirty flag
```

Flush happens on:
- `sv_disk_eject()` — when user unmounts a disk (U key in Disk Manager); flushes implicitly if dirty
- `sv_disk_flush()` — explicit single-drive flush (F key in Disk Manager)
- `sv_disk_flush_all()` — all dirty drives at once; called automatically before **machine reset** (Reset Machine confirm dialog) and before **machine type change** (`supervisor_set_machine_type()` → `esp_restart()`)

### Why Bounce Buffer?

The ESP32 SPI DMA controller (both classic ESP32 and ESP32-S3) cannot reliably read from or write to PSRAM addresses. The SD library's `file.read()` uses SPI DMA internally. Reading directly into a PSRAM pointer can produce corrupted data. The 512-byte stack-allocated bounce buffer sits in internal RAM (DMA-safe), then `memcpy` moves data to/from PSRAM safely.

---

## SD Card Bus

The SD card uses a **dedicated HSPI (SPI3) bus**.

```cpp
// hal_storage.cpp
static SPIClass sd_spi(HSPI);        // SPI3 peripheral
sd_spi.begin(PIN_SD_SCLK, PIN_SD_MISO, PIN_SD_MOSI, PIN_SD_CS);
SD.begin(PIN_SD_CS, sd_spi, 4000000);
```

**Pin assignments (board-specific):**

| Function | TTGO VGA32 |
|----------|:---:|
| SD SCLK | GPIO14 |
| SD MOSI | GPIO12 |
| SD MISO | GPIO2 *(strapping pin — must float/high at boot)* |
| SD CS | GPIO13 |

There is no SPI display bus on the VGA32, so HSPI is dedicated entirely to the SD card. The dedicated bus is required by the board's fixed wiring.

---

## FDC Command Summary

| Command | Opcode | Behavior |
|---------|--------|----------|
| RESTORE | $00-$0F | Track=0, INTRQ immediate |
| SEEK | $10-$1F | Track=data reg, INTRQ immediate |
| STEP | $20-$3F | Track ± step_direction, INTRQ immediate |
| STEP IN | $40-$5F | Track++, INTRQ immediate |
| STEP OUT | $60-$7F | Track--, INTRQ immediate |
| READ SECTOR | $80-$9F | Deferred 256-byte transfer from cache (loads on first $FF4B read), INTRQ deferred |
| WRITE SECTOR | $A0-$BF | 256-byte transfer to cache, INTRQ immediate (uses DRQ/HALT sync) |
| READ ADDRESS | $C0-$DF | Returns 6-byte ID field (track, side, sector, size, CRC, CRC) |
| READ TRACK | $E0-$EF | Returns immediate success (not fully emulated) |
| WRITE TRACK | $F0-$FF | Format track — parses raw stream, writes sectors to cache, INTRQ immediate |
| FORCE INTERRUPT | $D0-$DF | Abort command, INTRQ if bit 3 set |

### Status Register Bits

| Bit | Type I | Type II/III |
|-----|--------|-------------|
| 7 | NOT READY | NOT READY |
| 6 | WRITE PROTECT | WRITE PROTECT |
| 5 | HEAD LOADED | RECORD TYPE |
| 4 | SEEK ERROR | RNF |
| 3 | CRC ERROR | CRC ERROR |
| 2 | TRACK 0 | LOST DATA |
| 1 | INDEX PULSE | DRQ |
| 0 | BUSY | BUSY |

---

## Disk Image Formats

### .DSK (JVC)
- Header size = `file_size % 256` (0 for standard images)
- Data: raw sector data, 256 bytes per sector, 18 sectors per track
- Double-sided layout: interleaved (T0S0, T0S1, T1S0, T1S1, ...)
- Common sizes: 161,280 (35T SS), 184,320 (40T SS), 368,640 (40T DS), 737,280 (80T DS)

### .VDK
- 12-byte header (always)
- Same sector/track layout as JVC after header

### Geometry Detection
```
data_size = image_size - header_size
linear_tracks = data_size / (sectors_per_track × sector_size)
if linear_tracks > 40 → double-sided (tracks = linear_tracks/2, sides = 2)
else                   → single-sided (tracks = linear_tracks, sides = 1)
```

### Sector Offset Calculation (Double-Sided)
```
linear_track = physical_track × sides + side
offset = linear_track × sectors_per_track × sector_size + (sector - 1) × sector_size
```
For single-sided images, `sides=1` and `side=0`, so this reduces to the simple flat offset.

---

## Integration Points

### machine.cpp — Scanline Loop
```cpp
void machine_run_scanline(Machine* m) {
    sv_disk_tick(&m->fdc);     // Deferred INTRQ countdown
    mc6809_run(&m->cpu, cycles);  // CPU executes (may be halted by FDC)
    // ... VDG rendering, SAM counter ...
}
```

### machine.cpp — Memory Map
```cpp
// $FF40-$FF5F: Disk controller
if (addr < 0xFF60) {
    return sv_disk_read(&m->fdc, addr);   // reads
    sv_disk_write(&m->fdc, addr, val);    // writes
}
```

### supervisor.cpp — Callback Wiring
```cpp
m->fdc.nmi_callback = [](void* ctx, bool active) {
    Machine* mach = (Machine*)ctx;
    mc6809_nmi(&mach->cpu, active);      // NMI edge-triggered → CPU
};
m->fdc.halt_callback = [](void* ctx, bool halted) {
    Machine* mach = (Machine*)ctx;
    mach->cpu.halted = halted;           // HALT → CPU
};
m->fdc.callback_context = m;
```

---

## Key Lessons Learned

1. **Bit 7 of DSKREG is HALT, not NMI.** Many CoCo references get this wrong. XRoar's `rsdos.c` is the authoritative source.

2. **NMI fires via density gating, not a dedicated enable bit.** The XOR on bit 5 creates two NMI paths that cover both density settings.

3. **NMI must be edge-triggered.** The real MC6809 latches NMI on 0→1 transitions only. Without edge detection, a single INTRQ assertion can cause
 repeated NMI re-entry. The `nmi_line` field tracks pin state; `nmi_pending` is set only on rising edges. After servicing, `nmi_line` is cleared t
o require a new edge.

4. **HALT cannot be used for READ SECTOR byte synchronization** in a non-cycle-accurate emulator. In real hardware, DRQ→HALT has a propagation del
ay that allows the CPU to complete STA ,X+ before halting. Our emulation sets `halted=true` instantly via callbacks, which would prevent STA from
executing. Solution: bypass HALT during reads (set `drq` directly), use the byte-257 mechanism for INTRQ.

5. **The byte-257 INTRQ mechanism prevents last-byte corruption.** After byte 256 is read, INTRQ is deferred. When the DSKCON loop reads $FF4B aga
in (byte 257), INTRQ fires — guaranteed to be after STA stored byte 256. A scanline-tick safety net (`intrq_defer=5`) provides a fallback.

6. **`intrq_defer=1` caused intermittent last-byte corruption.** The scanline tick could fire between the LDA (byte 256) and STA ,X+ instructions,
 causing NMI to preempt the store. Increasing to 5 ensures the byte-257 path always fires first (~14 cycles vs ~285 cycles).

7. **ESP32 SPI DMA cannot target PSRAM directly.** Use a bounce buffer in internal RAM for all SD↔PSRAM transfers.

8. **Caching entire disk images in PSRAM eliminates SD reliability issues.** At 161 KB per disk, even 4 drives use less than 1 MB of the 8 MB PSRA
M.

9. **WRITE TRACK is essential for DSKINI.** Disk formatting uses the Type III WRITE TRACK command, not individual WRITE SECTOR calls. The raw byte
 stream must be parsed to extract sector data for sector-based image formats.

10. **READ and WRITE SECTOR have different INTRQ/HALT strategies.** READ SECTOR bypasses HALT (sets `drq` directly) and defers INTRQ via the byte-257 mechanism, because instant HALT would prevent STA from storing the last byte. WRITE SECTOR uses proper DRQ/HALT sync (via `set_drq()`) and fires INTRQ immediately, because the CPU has already completed the STA by the time the FDC processes the last byte.

11. **NMI gating must use the same condition in both paths.** Both `set_intrq()` and `fdc_write_drive_select()` must check `!density && intrq` (matching XRoar). An earlier bug had `fdc_write_drive_select` using `density && intrq` (inverted polarity). Both paths also need the `else { signal_nmi(false); }` branch to properly deassert NMI for edge detection.

12. **Double-sided disk support requires side select (DSKREG bit 6) and correct geometry detection.** A 360K NitrOS-9 image (368,640 bytes) is 40 tracks × 2 sides × 18 sectors, not 80 tracks × 1 side. JVC images use interleaved side layout: T0S0, T0S1, T1S0, T1S1, etc. Offset = `(track * sides + side) * SPT * sector_size + (sector-1) * sector_size`.

13. **READ SECTOR must defer sector load until first data read.** The CoCo DSKCON writes the command register ($FF48) BEFORE writing DSKREG ($FF40) with the side select. An instant-completion FDC model that loads the sector at command time uses the stale side from the previous DSKREG write. Fix: set `read_pending=true` and load the sector in `fdc_read_data()` on the first `$FF4B` read, after DSKREG has been written.

14. **OS-9 requires SAM all-RAM mode.** The OS-9 bootstrap (loaded from Track 34 via the DOS command) writes to $FFDF to set SAM MAP TYPE=1, mapping the full 64KB as RAM. Without all-RAM support in `machine_read()`/`machine_write()`, the bootstrap's writes to $8000+ are silently dropped and reads return ROM data instead of the loaded OS-9 kernel, causing an immediate crash. The I/O space ($FF00–$FFFF) must remain hardware-decoded regardless of MAP TYPE.

---

## OS-9 Boot Sequence (DOS Command)

The Disk BASIC `DOS` command (DOSCOM at $DF00 in disk11.rom) boots OS-9:

1. **SWI3** — no-op hook (vectors through $0100 → RTI)
2. **Read 18 sectors** from Track 34, Drive 0 into $2600–$37FF via DSKCON
3. **Check** first 2 bytes at $2600 for "OS" ($4F $53)
4. **Jump to $2602** if found (LBEQ with 16-bit offset wrapping to $2602)

The OS-9 bootstrap at $2602 then:
- Disables PIA0 CB1 interrupt (CLR $FF03) — stops 60Hz timer
- Enables SAM all-RAM mode (STA $FFDF) — unmaps ROMs
- Configures SAM VDG/memory registers
- Loads the OS-9 kernel from disk into upper RAM
- Transfers control to the kernel

**Key dependency**: SAM all-RAM mode support in `machine_read()`/`machine_write()` (see `core.md` Memory Map section).
