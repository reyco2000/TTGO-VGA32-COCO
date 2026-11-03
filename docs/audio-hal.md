# Audio HAL — TTGO VGA32

## Overview

The audio subsystem emulates the CoCo's two audio output paths and pushes the resulting sample stream to the **ESP32 internal DAC1 on GPIO25** (the TTGO VGA32 v1.4 board's 3.5 mm jack). The emulation loop queues one sample per scanline, the level averaged over that line, and I2S0 plays them by DMA at one sample per scanline's real duration (15 720.6 Hz). A task on core 0 refills the DMA buffers; there is no audio interrupt.

The output path follows the [mame-coco3-ttgovga](https://github.com/reyco2000/mame-coco3-ttgovga) port on the same board (its `src/platform/audio_out.cpp`), with one difference: this emulator can run below 60 FPS, and the sound follows it down.

**Source files:**
- `src/core/sound.cpp/h` — Sound mixing core: mux source/enable, 6-bit DAC and single-bit levels, XRoar-style gain/offset tables — board-independent
- `src/core/machine.cpp` — PIA write hooks (`sound_pia0_written` / `sound_pia1_written`) that forward PIA state to the sound core
- `src/hal/hal_audio.cpp` — I2S0 DMA setup, refill task, `hal_audio_set_level()` sink
- `src/hal/audio_capture.h` — per-scanline level average (host-tested)
- `src/hal/audio_feed.h` — sample ring, rate control, DMA buffer bookkeeping (host-tested)
- `tools/audio_test/run.sh` — host tests for the two headers, including a simulation at full speed, with board-like frame jitter, and at 39 FPS
- `src/hal/hal.h` — Public API declarations
- `config.h` — `PIN_DAC_OUT`, `TARGET_FPS`

Design rationale and the gap analysis that led to this structure:
[`audio-improvement-plan.md`](audio-improvement-plan.md).

---

## Hardware Being Emulated (unchanged across boards)

### Real CoCo Audio Architecture

```
  PIA1 Port A bits 2-7           PIA1 Port B bit 1
  (6-bit DAC value)              (single-bit audio)
        │                               │
        v                               │
   ┌──────────┐                         │
   │ 6-bit    │                         │
   │ R-2R DAC │                         │
   └────┬─────┘                         │
        │                               │
        v                               v
   ┌─────────────────────────────────────────┐
   │         Analog MUX (4066 / MC14066)     │
   │                                         │
   │  SEL1 (PIA0 CA2)  SEL2 (PIA0 CB2)       │
   │       │                  │              │
   │  Source select:                         │
   │    00 = 6-bit DAC                       │
   │    01 = Cassette input                  │
   │    10 = Cartridge audio                 │
   │    11 = No source                       │
   │                                         │
   │  MUX Enable: PIA1 CRB bit 3             │
   │    1 = Route selected source to speaker │
   │    0 = Disconnect (mute DAC)            │
   └────────────────┬────────────────────────┘
                    │
                    v
                 Speaker
```

### Register Map

| Register | Address | Bits | Function |
|----------|---------|------|----------|
| PIA1 DA | $FF20 | 2-7 | 6-bit DAC value (0-63) |
| PIA1 DB | $FF22 | 1 | Single-bit audio toggle |
| PIA1 CRB | $FF23 | 3 | Sound MUX enable (1=on, 0=mute) |
| PIA0 CRA | $FF01 | 3 | MUX source select bit 0 (CA2 output) |
| PIA0 CRB | $FF03 | 3 | MUX source select bit 1 (CB2 output) |

### MUX Source Selection

| SEL2 (CB2) | SEL1 (CA2) | Source | Used by |
|:---:|:---:|--------|---------|
| 0 | 0 | **6-bit DAC** | SOUND, PLAY, game audio |
| 0 | 1 | Cassette input | Not emulated |
| 1 | 0 | Cartridge audio | Not emulated |
| 1 | 1 | None (silence) | — |

---

## TTGO VGA32 Implementation

### Hardware Configuration

| Parameter | Value | Notes |
|-----------|-------|-------|
| Output pin | GPIO25 | ESP32 internal DAC1 (`PIN_DAC_OUT = 25`) |
| DAC width | 8 bit | High byte of each 16-bit I2S word |
| Sample rate | 15 720.6 Hz | 80 MHz / ((181 + 35/47) × 28): 262 samples per 16 666 µs frame, 0.8 ppm off |
| Idle level | 0 | Unipolar output like the real hardware; jack is AC-coupled |
| Output route | 3.5 mm jack | On-board on the TTGO VGA32 v1.4 |

The ESP32-WROVER inside the TTGO has a real 8-bit DAC on GPIO25. I2S0 feeds it in built-in-DAC mode, configured by register as FabGL's SoundGenerator does; VGA uses I2S1, so the two do not meet.

### Audio Paths

The output level is computed by the **sound mixing core** (`src/core/sound.cpp`),
modeled on XRoar's `sound.c`. The HAL is a pure sink: the core calls
`hal_audio_set_level(0-255)` whenever the mix changes.

The core tracks five pieces of state, fed from the PIA write handlers in
`machine.cpp`:

| State | Source | Setter |
|---|---|---|
| `dac_level` (0-63) | PIA1 PA bits 2-7 | `sound_set_dac_level()` |
| `sbs_enabled` | PIA1 PB1 DDR bit (pin is output?) | `sound_set_sbs()` |
| `sbs_level` | PIA1 PB1 data bit | `sound_set_sbs()` |
| `mux_enabled` (SNDEN) | PIA1 CRB bit 3 | `sound_set_mux_enabled()` |
| `mux_source` (0-3) | PIA0 CRA/CRB bit 3 (SEL1/SEL2) | `sound_set_mux_source()` |

`sound_update()` computes:

```
sindex = sbs_enabled ? (sbs_level ? 2 : 1) : 0
source = mux_enabled ? mux_source : SOURCE_SINGLE_BIT
level  = (source == DAC) ? dac_level : 0
out    = level * source_gain[source][sindex] / 63 + source_offset[source][sindex]
```

The gain/offset tables are XRoar's real-hardware voltage measurements
(full scale 4.7 V) rescaled to the 0-255 DAC domain. Consequences of this
model:

- **DAC and single-bit sound mix correctly** — the single-bit output doesn't
  replace the DAC signal; it changes the gain and DC offset of whichever
  source the mux selects (matching the real analog circuit). A PB1 beep during
  `SOUND` no longer destroys the tone.
- **PB1 as input has no effect** — three SBS states (input / output-low /
  output-high), so software leaving PB1 as an input doesn't drag the output low.
- **Pure single-bit audio works with the mux off** — square wave between
  offset 0 and 212 (0 ↔ 3.9 V), used by many games.
- **Silence is 0, not mid-scale** — output is unipolar like the real hardware;
  the jack's AC coupling removes DC.

### Scanline Sample Queue

#### The Problem

The emulated 6809 runs a frame's cycle budget (14 916 cycles) in a burst and then idles until the next 60 Hz frame slot (`pace_frame()` in the sketch). If the level were written to the DAC at the moment the CPU writes PIA1, every transition would arrive too early in wall time and tones would play too high. How long the burst takes also varies with what is on screen, so the end of a frame is not a steady clock either.

#### The Solution

Sample in emulated time, play back in real time, with a queue in between:

1. **Capture, averaged over the line** (`audio_capture.h`): `hal_audio_set_level()` notes each level change with its position in the scanline, read from the CPU's cycle counter (`cpu.cycles`, which the CPU restarts at each run, and the machine runs it once per scanline; the interpreter is unchanged). At the end of the line `hal_audio_capture_scanline()` queues the level averaged over the line, rounded. A point sample would move every edge of a tone to a line boundary: up to 64 µs of jitter that repeats and folds high harmonics down as inharmonic tones, heard as tics. Only lines with a level change do the division.
2. **Playback by DMA** (`hal_audio.cpp`): I2S0 loops for good over 4 buffers of 64 samples (4.1 ms each). A task on core 0 (`audio`, priority 6, above `ssc` and the debug server) polls every 2 ms where the DMA has got to and refills every buffer it has finished, never the one it is fetching. `out_link_dscr` runs two descriptors ahead of the buffer being fetched; that was measured with 4 buffers in the MAME port, so the count is fixed by a `static_assert`. If the task were ever late, the DMA would replay an old buffer; the margin is 3 buffers, about 12 ms (`late_refills`).
3. **Full speed: nothing resampled** (`audio_feed.h`): at 60 FPS the DAC takes samples at exactly the rate the emulator makes them, so each sample goes out once, as it came in. The ring holds 2048 samples and is aimed at 774 at the frame end (`AUDIO_FILL_TARGET`: one frame plus 512). Fill errors under 128 samples, averaged over 16 frames, are left alone; past that a trim of at most ±3 % brings it back, and stops within 32.
4. **Slow emulation**: when the emulator cannot hold 60 FPS, playback slows with it. A 32-frame average of the frame interval sets the rate (feed-forward), and the trim corrects the fill around it. The emulator counts as slow once the average is 2 % over the frame period, and full speed again under 1 %: frame ends jitter by milliseconds even at 60 FPS (12–26 ms apart measured on the board), and an 8-frame average with a 1 % threshold made the rate wander at full speed. Below full speed the feed steps through the ring by less than one sample per output sample, interpolating linearly between neighbours.
5. **Speed changes**: `pace_frame()` keeps an absolute schedule, so a long frame or two is followed by short ones that pay it back. Three frames in a row over 125 % of the period add more lateness than that, so they are a change of speed: the average jumps to the shortest of the three. Three frames in a row under 75 % of the average jump it down the same way. Without these the ring ran dry stepping down to 39 FPS and overflowed coming back.
6. **Empty queue**: when emulation stops (OSD open, debugger pause, long SD access) the ring runs dry. The output stays on its last sample, and playback restarts at the frame end where the ring is back at its target, from exactly the target.

```
machine_run_frame():
  for each scanline (0-261):
    machine_run_scanline()       → CPU executes ~57 cycles; level changes noted with cpu.cycles
    hal_audio_capture_scanline() → queue the line's average level
  hal_audio_commit_frame()       → judge fill and frame interval, set the stride

audio task [core 0, every 2 ms]:
  for each DMA buffer finished since the last poll:
    fill 64 samples: stride exactly 1 → copy; slower → interpolate
```

Sound runs about 50 ms behind the emulation because of the queued samples.

#### Why not swap a buffer per frame

Until v0.15.2 the HAL swapped two 262-sample buffers at each frame end and the ISR restarted from sample 0 on every swap, with an `AUDIO_PITCH_TRIM` constant slowing playback. With 60 Hz pacing the frames arrived faster than the ISR played them: measured on the board, 6.6 of every 262 samples were cut off, every frame, and late frames replayed stale audio. The ring buffer loses none.

#### Pitch

Playback is locked to emulated time, so the pitch heard is the pitch the emulated machine generates. That is not concert pitch, and it is not supposed to be: BASIC's `PLAY` times its notes with a delay loop, and the 60 Hz interrupt takes time away from that loop.

| Command | Delay-loop period (ROM table) | No interrupts | Measured at the jack |
|---|---|---|---|
| `PLAY "O2A"` | 4 × (26 + 8 × 124) + 3 = 4075 cycles | 219.60 Hz | 217.84 Hz |
| `PLAY "O4A"` | 4 × (25 + 5 × 46) + 3 = 1023 cycles | 874.77 Hz | 867.73 Hz |

Both measurements are 0.80 % below the loop's own frequency, which is 120 cycles per 60 Hz interrupt in each case (CoCo 3, Disk BASIC). Do not add a trim constant to "fix" these readings.

Re-measured on v0.16.1 (PI rate control), 6809:

| Command | CoCo 3 | CoCo 2 |
|---|---|---|
| `PLAY "O2A"` | 217.76 Hz | 217.94 Hz |
| `PLAY "O4A"` | 867.70 Hz | **836.99 Hz** |

The CoCo 3 is unchanged (within 0.04 %). The CoCo 2 uses the same ROM delay tables, and its `O2 A` sits slightly higher because its interrupt handler is shorter. Its `O4 A` is 3.6 % flat for a different reason: that note's delay loop (`DECA` / `BNE`) executes many more instructions per second, the CoCo 2 path has too little speed margin for it, and the frame rate falls to 57.7 FPS. Since v0.16.1 playback follows the emulator's speed, so the note comes out flat instead of breaking up. Running the ROM loop directly on the board shows it: 60.0 FPS and no correction for the `O2 A` loop on both machines and for the `O4 A` loop on the CoCo 3, but 57.7 FPS and a −4.1 % correction for the `O4 A` loop on the CoCo 2. The cure is CoCo 2 frame time, not the audio code.

The measurements were taken with `tools/frequency_meter.py`: connect the board's audio jack to a PC audio input, pick the capture device with `--list-devices`, and read the frequency while the note plays.

#### Bandwidth

Scanline-rate sampling gives an effective sample rate of 262 × 60 = 15 720 Hz (Nyquist ≈ 7.8 kHz). Each sample is the level averaged over its scanline, so where in the line a level changed is kept.

#### History

Up to v0.21.0 a timer ISR on core 1 ran every 45 µs (22 222 Hz) and stepped through the ring with a Q16 nearest-sample stride and a PI trim. Each scanline kept only its last level. That added up to 45 µs of edge jitter from the stride (by arithmetic), about ±14 µs of ISR start jitter (measured), and up to 64 µs from point sampling. Removing the ISR also took three of the four boot-benchmark rows down (NOP/BRA 128 → 120, LDA/SUBA/BMI 207 → 195, LDA,X/STA,Y 229 → 212; LEAX/BNE 177 → 180 host cycles per instruction).

### Diagnostics

`/api/status` reports an `audio` object; `?audio_reset=1` clears the counters after the read.

| Field | Meaning |
|---|---|
| `underruns`, `hold_samples` | Times the ring ran dry, and samples output on the last level while waiting |
| `overruns` | Samples dropped because the ring was full |
| `fill`, `fill_min`, `fill_max` | Queued samples at frame end (target 774) |
| `stride_adj` | Current stride minus one sample per sample, Q16 units; 0 at full speed |
| `commit_us_min`, `commit_us_max` | Shortest and longest time between frame ends |
| `refills`, `late_refills` | DMA buffers refilled, and polls late enough that the DMA may have replayed one |
| `poll_gap_us_max` | Longest time between two polls of the DMA |
| `stack_free` | Audio task stack never used, bytes |

`underruns`, `overruns` and `late_refills` should stay at 0 while the emulator is running, apart from one underrun per pause. Measured on the board with the I2S path: 0 of each over 20 s of tone on CoCo 3 6809, CoCo 3 6309 and CoCo 2 6809, and over the `pm4`, `hs2anim` and `w80` screens of `tools/perf/bench/`; `poll_gap_us_max` 2.9–4.6 ms against the 12 ms margin; `stack_free` 1520 of 2048 bytes. The task, stack and DMA buffers cost about 2.2 KB of internal RAM.

History: up to v0.16.0 the rate control was proportional only and limited to 1 %, so below 60 FPS the ring ran dry several times a second (this is what made CoCo 2 sound bad in v0.15.3), and an ISR that lost ticks left the ring pinned full. Checked for v0.16.1 with a test build slowed to 50 FPS: 14 underruns while the rate settled at −17 %, then none.

With the timer ISR: the PI-only control (+2 % / −40 %, 0.2 Hz) learned the emulator's speed only from the fill level, so a game whose speed changes every few seconds kept it behind: Rick Dangerous on the 6309 in fast mode (speed swinging 0.68–0.97) had, in 30 s, 48 underruns, 1.3 s of held silence and 11 023 dropped samples. With the feed-forward rate, 2048-sample ring and 646 target: 0 underruns, 0 held ticks, 0 dropped samples over 30 s at 0.61–0.70 speed. Pitch still follows the emulator's speed — the emulated CPU makes the sound, so at 0.62 speed it plays 0.62 pitch.

---

## Sound MUX Gating

Implemented in `src/core/sound.cpp` — board-independent. The mux state (enable
+ source select) determines which source's gain/offset row is applied.

### The Problem

Running `10 PRINT JOYSTK(0): 20 GOTO 10` would otherwise produce a continuous buzz. BASIC's `GETJOY` routine writes ~32 successive-approximation DAC values to PIA1 PA on every read. Without gating, each write would reach the speaker and produce audible clicks at the joystick polling rate.

Real CoCo `GETJOY` clears PIA1 CRB bit 3 before the ADC loop, disconnecting the DAC from the speaker via the analog MUX. After reading, it restores the bit.

### The Fix

`sound_set_dac_level()` always stores the new DAC value but only recomputes
the output when the DAC is audible (`mux_enabled && mux_source == SOURCE_DAC`)
— XRoar's conditional-update optimization, which keeps the JOYSTK ADC loop
cheap. When the mux is re-enabled or switched back to the DAC, the stored
level is applied immediately.

Mux source changes arrive via **PIA0** CRA/CRB writes (`sound_pia0_written()`
in `machine.cpp`) and take effect immediately — they no longer wait for the
next PIA1 write. When the mux is disabled, the output is the
`SOURCE_SINGLE_BIT` row: 0, unless PB1 is an enabled-high output (offset 212).

### Interaction Table

| Operation | PIA1 CRB bit 3 | MUX Source | DAC audible? | Single-bit audible? |
|-----------|:---:|:---:|:---:|:---:|
| `SOUND 200,5` | 1 | 0 (DAC) | Yes | Independent |
| `PLAY "CDEFG"` | 1 | 0 (DAC) | Yes | Independent |
| `JOYSTK(0)` ADC loop | 0 | — | **No** | Independent |
| `PRINT CHR$(7)` beep | — | — | — | **Yes** |
| Game audio + joystick | Alternates | 0 (DAC) | During sound only | Independent |

### Shared DAC Resource

The 6-bit DAC (PIA1 PA bits 2-7) serves **dual purposes** simultaneously:
- **Audio output** — feeds the R-2R ladder for analog voltage to the speaker
- **Joystick threshold** — same value compared against the joystick potentiometer

The MUX only controls the speaker path; the joystick comparator always reads the PIA register directly. So both uses coexist without conflict in emulation.

---

## Orchestra-90/CC

Full description in `orchestra90.md`; the audio side in short:

`src/core/orch90.cpp` is a port of XRoar's `orch90.c`: two write-only 8-bit unsigned DAC latches, left at `$FF7A` and right at `$FF7B`. There is no status register, timer or interrupt; the program writes sample values directly.

- **Decode:** `machine_write_coco2()` / `machine_write_coco3()` forward the two addresses to `orch90_write()` only while `orch90_enabled()` (Settings → Orchestra-90, read from NVS at boot). Reads return `$FF`.
- **Mixing:** the board has one free DAC pin (GPIO26 is the PS/2 mouse clock), so the channels are mixed to mono: `sound_set_external()` stores `(left + right) / 2` and `sound_update()` adds it to the level the mux/DAC/single-bit model computed, saturating at 255. As in XRoar the cartridge level is added whatever the mux selects.
- **Levels:** unsigned, no centring: `$00` is silence and an idle `$80` is a DC offset the jack's AC coupling removes. The latches start at 0 and survive a machine reset.
- **Bandwidth:** the level is captured once per scanline like every other source, so sample playback faster than ~7.8 kHz aliases.
- **ROM:** the cartridge's 8 KB ROM is `orch90.rom` in `/roms/`, loaded at `$C000` in place of Disk BASIC and started automatically (see "Cartridge ROM" in `supervisor.md`). The toggle refuses to turn on without it.

---

## Public API

| Function | Purpose |
|---|---|
| `hal_audio_init()` | Clear the ring, start I2S0 DMA into DAC1 and the core-0 refill task |
| `hal_audio_set_cycle_source()` | Give the HAL the CPU cycle counter that places level changes in the line |
| `hal_audio_set_level(l)` | Set output level 0-255 (computed by `src/core/sound.cpp`) |
| `hal_audio_capture_scanline()` | Push the current level into the ring |
| `hal_audio_commit_frame()` | Frame end: judge the fill level, steer the playback rate |
| `hal_audio_get_stats()` / `hal_audio_reset_stats()` | Queue and DMA diagnostics for `/api/status` |
| `hal_audio_set_volume(v)` | No-op placeholder (volume is set by output stage) |
| `hal_audio_write_sample(l, r)` | No-op placeholder (mono only) |
| `hal_audio_debug_tick()` | Reserved for diagnostics |

---

## Lessons Learned

1. **Shared PIA resources are a CoCo design signature.** The CoCo reuses PIAs extensively for cost. Same bits that produce audio also read joysticks. Same MUX select lines that choose audio source also select joystick port/axis. Always check full hardware context before assuming a PIA bit has a single purpose.
2. **XRoar's `sound.c` is the reference** for correct MUX behavior. `src/core/sound.cpp` is an integer port of its model: full source selection with gain/offset tables matching real hardware voltage measurements. TAPE and CART source *levels* are still unemulated (treated as 0), but their DC offsets are correct.
3. **Single-bit audio (PIA1 PB1) is not gated by the MUX — but it isn't independent either.** On real hardware the PB1 pin's voltage interacts with the analog mix: it changes the gain and DC offset of whichever source the MUX selects. XRoar models this with a 3-column table index (pin-is-input / output-low / output-high), and so do we.
4. **Pitch correction requires buffered playback, not CPU throttling.** Samples are taken in emulated time (one per scanline) and played from a queue at a rate steered by the queue's fill level. Do not tune pitch with a constant: when frame pacing changed, the old trim silently started dropping samples.
5. **The internal DAC sounds noticeably cleaner than LEDC PWM** — no carrier whistle, no RC-filter quality dependency, 8-bit linearity. The TTGO VGA32 jack is amplifier-driven and produces usable audio straight out of the board.
6. **Mixing lives in `src/core/sound.cpp`, not in the HAL.** The HAL is a sink — `hal_audio_set_level()` just stores a byte. The sound core owns all MUX/DAC/single-bit state and is shared by the CoCo 2 and CoCo 3 write paths (previously three near-identical inline copies in `machine.cpp`). This keeps the HAL board-agnostic and the mix logic single-sourced.
