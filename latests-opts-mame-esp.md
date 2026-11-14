# Improvement plan: mame-coco3-ttgovga

Plan for [reyco2000/mame-coco3-ttgovga](https://github.com/reyco2000/mame-coco3-ttgovga),
branch `feat/coco3-emulator`. Written from reading the code and the docs only: nothing here
was built or run on a board. Every gain is a hypothesis to measure.

## Where things stand

- **Everything happens at line boundaries.** The CPU runs a whole line's worth of cycles in
  one go (`runCoco3Cpu(cpu, budget)`, `src/emu/coco3.cpp:473`). After that comes `lineTick`
  (GIME timer, HBORD, VBORD), then PIA0 CA1/CB1, the keyboard, the floppy controller and one
  audio sample. The only interrupts raised in the middle of a line are ones the CPU causes
  itself by writing a register.
- **The GIME timer can only expire at the end of a line** (`src/emu/gime.cpp:157`). With the
  fast source (279 ns), several expiries in one line become a single interrupt edge
  (`timerExpired`, `src/emu/gime.cpp:142`).
- **SYNC and CWAI waits use up the rest of the line** (`src/emu/cpu6809.h:267`). That is
  correct today, but only because no event ever lands mid-line.
- **The emulation is not locked to the VGA beam.** `emuTask` paces itself with `esp_timer`,
  and `video.publish()` can swap frames in the middle of the screen. The VGA interrupt also
  reads video RAM live, which is why a screen being redrawn can tear for a frame (a known
  limit in `docs/gime.md`).
- **Audio** is one sample per line, the level averaged over the line, about 15.7 kHz on
  8 bits. The buffer holds about 280 samples (roughly 18 ms) before output starts, plus
  4 DMA buffers of about 4 ms each.

## Phase 0: Measure first

1. **Build a set of test programs** that cover:
   - sample playback driven by timer FIRQ (digitized speech or music players);
   - tight DAC loops (BASIC `SOUND`/`PLAY`, sound effects);
   - colour bars or palette splits triggered on HSYNC/HBORD;
   - programs that count VBORD;
   - page-flipping games;
   - NitrOS-9 6309 with gshell;
   - a TFM-heavy demo.
2. **Record a baseline for each program** with the `VGA_STATS` build: emulation µs per frame,
   idle, worst frame, late and overruns, audio underruns and drops.
3. **Compare against MAME.** Extend `tools/mame6309/runner.lua` so it logs when each
   IRQ/FIRQ/NMI is entered (frame, line, cycle within the line, vector). Add the same log to
   the host tests, then diff the two timelines.
4. **Add interrupt counters** to `VGA_STATS` and `/api/video` (or a new `/api/irq`): entries
   per second for each source (TMR, HBORD, VBORD, EI1, PIA0, PIA1, CART), the current timer
   period, and cycles spent in SYNC/CWAI.

Steps 1, 3 and 4 are what prove (or disprove) each later change.

## Phase 1: Interrupt timing (largest expected gain)

| # | Change | Why | Cost / risk |
|---|---|---|---|
| 1.1 | **Fire the GIME timer on the exact cycle.** When the timer is running, work out how many cycles remain until it expires, run the CPU only that far, raise TMR, then run the rest of the line. A SYNC/CWAI wait wakes at that point too. | Timer-driven PCM now plays with up to one line (63.6 µs) of jitter, which adds noise and tics. Fixing it should make sampled audio cleaner and tempo steadier. | Moderate. Lines with no timer event run as they do now, so the extra IRAM is small. |
| 1.2 | **One interrupt edge per expiry**, which follows from 1.1. | Very short timer counts currently drop interrupts. | Small. |
| 1.3 | **Raise HBORD/HSYNC (PIA0 CA1) at MAME's position within the line**, and record `recordLine`'s snapshot at the matching point. | Raster effects that change the palette or mode on HSYNC currently land on the wrong line, or one line late. | Moderate. Needs MAME traces to confirm the positions. |
| 1.4 | **Fix TFM's re-fetch at chunk boundaries** (a documented 2.6–5% overhead, `docs/cpu6309.md`). Resume TFM with no extra opcode charge. Do this **before** 1.1, because 1.1 splits lines more often and would make this overhead worse. | Exact 6309 timing and fewer lost cycles. | Small. |
| 1.5 | **Check the smaller timing details against MAME**: FS fall/rise fixed at lines 244 and 248 whatever `$FF99` says (`src/emu/coco3.cpp:17`), the slow timer's 1824/1820 rounding, and the 1986 GIME's +2 count. | Programs that time themselves off VBORD or the timer. | Small, mostly tests. |

## Phase 2: Video and FPS

| # | Change | Why | Cost / risk |
|---|---|---|---|
| 2.1 | **Lock the frame start to the VGA beam.** Have the VGA callback timestamp a fixed scan line, and have `emuTask` start each frame at a fixed phase from it, so `publish()` always lands during VGA vertical blank. | No more frame swaps mid-screen, and no tear seam that slowly crawls as `esp_timer` and the VGA clock drift apart. | Small. The pacing loop is already built for this. |
| 2.2 | **Optional beam racing:** emulate line L shortly before the VGA scans it, in the same frame. | One frame less input latency (about 17 ms) and consistent video RAM reads. Switch back to the current one-frame-behind mode for heavy loads (NitrOS-9 6309 is near 15.3 of 16.7 ms). | Moderate. Needs a fallback and hysteresis so it doesn't flap between modes. |
| 2.3 | **Have core 1 copy each visible line's bytes into internal RAM** as it records the line, so the VGA interrupt never reads PSRAM. Core 1 has 30–45% idle time to pay for the copy. | Fixes the remaining gshell overruns with Network On, and gives every line a consistent snapshot (no tearing). | About 38 KB of internal RAM per buffer, which clashes with WiFi. Possibly only for 80- and 160-byte modes, or only with Network Off. |
| 2.4 | **CPU fast paths**: a cached page pointer for opcode fetches (skips the `< $FE00` test and the table lookup on every byte, `src/emu/cpu6809.h:277`), plus faster direct-page and same-page 16-bit accesses. | More headroom for heavy software, so frames don't run long. | IRAM is tight. Report `.iram0.text` and IRAM free each time, as `CLAUDE.md` requires. |

## Phase 3: Audio

| # | Change | Why | Cost / risk |
|---|---|---|---|
| 3.1 | **Lower audio latency.** Once 2.1/2.2 spread sample production through the frame, cut `kStartFill` from 280 to about 100 (`src/platform/audio_out.cpp:52`). | Sound follows the picture more closely, with a smaller sawtooth in the fill. | Small, but only after Phase 2. |
| 3.2 | **Two samples per line** (about 31.4 kHz), each averaged over half a line. The I2S divider halves. | Pushes aliasing images higher, which helps timer PCM after 1.1. | Small CPU cost, twice the buffer memory. |
| 3.3 | **Trace the 50–105 samples dropped in the first second after boot.** | They're unexplained in `docs/perf.md`. | Small. |
| 3.4 | **Bring Orchestra-90 over from TTGO-VGA32-COCO** (two DACs at `$FF7A`/`$FF7B`), plus the cartridge SND line through mux channel 2. | Feature parity with the old project. | Needs a decision on the slot model (there's no Multi-Pak). |

## Phase 4: Tests and loose ends

- Add host tests for each Phase 1 change: timer FIRQ cycle positions, HSYNC split lines,
  edges per expiry, and TFM under split budgets, checked against the MAME traces.
- Look at OS-9 2.00.01's `/w1` opening as a small window. It isn't diagnosed yet and might
  be a timing or GIME register issue.

## Suggested order

0 (baseline and counters) → 1.4 → 1.1 + 1.2 → 1.5 → 2.1 → 3.1 → 1.3 → 2.3 → 2.4 → 3.2 → 3.4

Keep the repo's rules throughout:

- MAME is the reference.
- `src/emu/` stays free of ESP headers.
- Core 1 never blocks.
- Never drop emulated time.
- Report IRAM after every hot-path change.
