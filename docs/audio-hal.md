# Audio HAL — TTGO VGA32

## Overview

The audio subsystem emulates the CoCo's two audio output paths and pushes the resulting sample stream to the **ESP32 internal DAC1 on GPIO25** (the TTGO VGA32 v1.4 board's 3.5 mm jack). A timer ISR firing every 45 µs pulls samples from a ring buffer that the emulation loop fills with one sample per scanline, and writes them via `dac_output_voltage()`.

A rate-controlled sample queue (262 samples per frame) drives the **ESP32 internal DAC1 on GPIO25**. The output stage is a simple `dac_output_voltage()` call from the timer ISR — no PWM or LEDC involved.

**Source files:**
- `src/core/sound.cpp/h` — Sound mixing core: mux source/enable, 6-bit DAC and single-bit levels, XRoar-style gain/offset tables — board-independent
- `src/core/machine.cpp` — PIA write hooks (`sound_pia0_written` / `sound_pia1_written`) that forward PIA state to the sound core
- `src/hal/hal_audio.cpp` — DAC init, ISR, `hal_audio_set_level()` sink
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
| DAC width | 8 bit | `dac_output_voltage(channel, 0..255)` |
| ISR rate | 22 222 Hz | Hardware timer fires every 45 µs |
| Idle level | 0 | Unipolar output like the real hardware; jack is AC-coupled |
| Output route | 3.5 mm jack | On-board on the TTGO VGA32 v1.4 |

The ESP32-WROVER inside the TTGO has a real 8-bit DAC on GPIO25 — no PWM RC-filter trick needed. `dac_output_enable(DAC_CHANNEL_1)` once at boot, then `dac_output_voltage(DAC_CHANNEL_1, sample)` from the ISR. Cheap, jitter-free, no peripheral contention.

> **Driver choice:** the spec called for "I2S in built-in-DAC mode" (full DMA streaming), but the simpler legacy `dac_output_voltage()` path matches the existing scanline-buffer / commit-frame model 1:1 with minimal new surface area. Quality is fine for the CoCo's 6-bit source material. If a future DMA upgrade is wanted, FabGL's SoundGenerator or the `dac_continuous_*` driver are the candidates — both fit behind this HAL surface unchanged.

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

1. **Capture**: after each emulated scanline, `hal_audio_capture_scanline()` pushes `audio_current_level` into a 1024-byte ring buffer. That is 262 samples per frame, 15 720 per second.
2. **Playback**: the timer ISR takes samples from the ring with a Q16 fractional stride and writes them to the DAC. The nominal stride is one frame of samples per frame period: `262 × 65536 × 45 / 16666`.
3. **Rate control**: at each frame end `hal_audio_commit_frame()` looks at how many samples are queued and nudges the stride to hold about 390 (`AUDIO_FILL_TARGET`: one frame plus a margin for the idle part of the frame and for late frames). It is a PI controller: the error is averaged over ~16 frames for the proportional term, and an integral term carries any lasting speed difference so the fill returns to the target. The correction is limited to +2 % / −40 %. At 60 FPS it stays within about 0.03 % on a static screen and 0.2 % on an animated one, so pitch is not audibly bent.
4. **Slow emulation**: when the emulator cannot hold 60 FPS it delivers fewer samples per second, and playback slows down with it instead of breaking up (at 50 FPS the stride settles 17 % low: sound is slower and lower, but continuous). A second underrun within 120 frames of a restart steps the rate down 1 % at once, so the rate is found in a second or two; a single underrun, as after a pause, does not.
5. **Held-off interrupt**: each ISR run measures the time since the previous one with the CPU cycle counter and advances one stride per 45 µs elapsed. If another interrupt or a critical section delayed it, it skips ahead rather than falling behind (`catchup_ticks`).
6. **Empty queue**: when emulation stops (OSD open, debugger pause, long SD access) the ring runs dry. The ISR then leaves the DAC at its last level and waits; playback restarts once a whole frame is queued again.

```
machine_run_frame():
  for each scanline (0-261):
    machine_run_scanline()       → CPU executes ~57 cycles; DAC may change
    hal_audio_capture_scanline() → push audio_current_level into the ring
  hal_audio_commit_frame()       → judge fill level, adjust ISR stride

audio_timer_isr() [IRAM_ATTR, every 45 µs]:
  1. If holding (ring empty): do nothing
  2. Else read the sample at the Q16 read position, advance by the stride
  3. dac_output_voltage(DAC_CHANNEL_1, sample)
```

Sound runs about 20–25 ms behind the emulation because of the queued samples.

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

The measurements were taken with `tools/frequency_meter.py`: connect the board's audio jack to a PC audio input, pick the capture device with `--list-devices`, and read the frequency while the note plays. `tools/frequency_meter_mcp.py` serves the same measurement as MCP tools over HTTP (`measure_pitch`, `measure_level`, `list_inputs`), so an agent on another machine can take the readings itself; its header has the setup.

#### Bandwidth

Scanline-rate sampling gives an effective sample rate of 262 × 60 = 15 720 Hz (Nyquist ≈ 7.8 kHz). Only the last level written during a scanline is kept.

### Timer ISR

`timerBegin(0, 80, true)` produces a 1 MHz timer base (APB / 80). The alarm is set to `AUDIO_ISR_PERIOD_US` = 45, so the ISR fires at 22 222 Hz; the timer counts whole microseconds, so 22 050 Hz cannot be hit exactly. The stride is computed from the real period.

The ISR takes about 6 µs worst case. Its start time varies by roughly ±14 µs around the 45 µs period (other interrupts on the same core), which adds a little noise.

> The 2.x Arduino-ESP32 core timer API used here (`timerAlarmWrite` + `timerAlarmEnable`) is required because FabGL 1.0.9 is incompatible with core 3.x / IDF 5; the build pins to `esp32:esp32@2.0.17`. See `README.md` Build & Flash.

### Diagnostics

`/api/status` reports an `audio` object; `?audio_reset=1` clears the counters after the read.

| Field | Meaning |
|---|---|
| `underruns`, `hold_ticks` | Times the ring ran dry, and ISR ticks spent waiting |
| `catchup_ticks` | ISR periods made up after the ISR was held off |
| `overruns` | Samples dropped because the ring was full |
| `fill`, `fill_min`, `fill_max` | Queued samples at frame end (target 390) |
| `stride_adj` | Current rate correction in Q16 stride units (nominal stride is 46 362) |
| `commit_us_min`, `commit_us_max` | Shortest and longest time between frame ends |
| `isr_us_max`, `isr_gap_us_min`, `isr_gap_us_max` | ISR run time and spacing |

`underruns` and `overruns` should stay at 0 while the emulator is running, apart from one underrun per pause and a few while the rate control finds a new speed.

History: up to v0.16.0 the rate control was proportional only and limited to 1 %, so below 60 FPS the ring ran dry several times a second (this is what made CoCo 2 sound bad in v0.15.3), and an ISR that lost ticks left the ring pinned full. Checked for v0.16.1 with a test build slowed to 50 FPS: 14 underruns while the rate settled at −17 %, then none.

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
| `hal_audio_init()` | Enable DAC1, clear the ring, start the 45 µs timer ISR |
| `hal_audio_set_level(l)` | Set output level 0-255 (computed by `src/core/sound.cpp`) |
| `hal_audio_capture_scanline()` | Push the current level into the ring |
| `hal_audio_commit_frame()` | Frame end: judge the fill level, steer the playback rate |
| `hal_audio_get_stats()` / `hal_audio_reset_stats()` | Queue and ISR diagnostics for `/api/status` |
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
