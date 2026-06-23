# Audio HAL — TTGO VGA32

## Overview

The audio subsystem emulates the CoCo's two audio output paths and pushes the resulting sample stream to the **ESP32 internal DAC1 on GPIO25** (the TTGO VGA32 v1.4 board's 3.5 mm jack). A timer ISR running at 22 050 Hz pulls one sample per fire from a 262-sample, double-buffered scanline buffer and writes it via `dac_output_voltage()`.

A 262-sample-per-frame pitch-corrected double-buffer drives the **ESP32 internal DAC1 on GPIO25**. The output stage is a simple `dac_output_voltage()` call from the timer ISR — no PWM or LEDC involved.

**Source files:**
- `src/hal/hal_audio.cpp` — DAC init, ISR, DAC and single-bit write functions
- `src/hal/hal.h` — Public API declarations
- `src/core/machine.cpp` — Sound MUX gating logic (PIA1 / PIA0 wiring) — board-independent
- `config.h` — `PIN_DAC_OUT`, `AUDIO_SAMPLE_RATE`

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
| Sample rate | 22 050 Hz | Hardware timer ISR fires at this rate |
| Idle level | 128 (midpoint) | Silence = mid-scale |
| Output route | 3.5 mm jack | On-board on the TTGO VGA32 v1.4 |

The ESP32-WROVER inside the TTGO has a real 8-bit DAC on GPIO25 — no PWM RC-filter trick needed. `dac_output_enable(DAC_CHANNEL_1)` once at boot, then `dac_output_voltage(DAC_CHANNEL_1, sample)` from the ISR. Cheap, jitter-free, no peripheral contention.

> **Driver choice:** the spec called for "I2S in built-in-DAC mode" (full DMA streaming), but the simpler legacy `dac_output_voltage()` path matches the existing scanline-buffer / commit-frame model 1:1 with minimal new surface area. Quality is fine for the CoCo's 6-bit source material. If a future DMA upgrade is wanted, FabGL's SoundGenerator or the `dac_continuous_*` driver are the candidates — both fit behind this HAL surface unchanged.

### Audio Paths

**Two independent paths write to a single shared variable (`audio_current_level`):**

1. **6-bit DAC** (`hal_audio_write_dac`):
   - Input: 6-bit value (0-63) from PIA1 PA bits 2-7
   - Scaling: `(dac6 << 2) | (dac6 >> 4)` maps 0-63 to 0-255 with a smooth ramp
   - Used by: SOUND, PLAY, game sound effects

2. **Single-bit audio** (`hal_audio_write_bit`):
   - Input: boolean from PIA1 PB bit 1
   - Output: 255 (high) or 0 (low) — full swing square wave
   - Used by: cassette relay toggle, simple beeps (`CHR$(7)`)

**Last write wins** — both paths write to the same `audio_current_level` byte. Real CoCo hardware has the same behavior (single speaker output).

### Pitch-Corrected Scanline Buffer

#### The Problem

The emulated 6809 executes the per-frame cycle budget (14 916 cycles) faster than real wall-clock time — currently in ~6–7 ms of the 16.67 ms NTSC frame period. If the DAC level were written directly to the output at the moment the CPU writes PIA1, every audio transition would arrive ~2.5× too early in wall time and tones would play roughly that much higher in pitch.

#### The Solution

Sample-and-replay at the correct CoCo rate:

1. **Capture**: after each emulated scanline in `machine_run_frame()`, `hal_audio_capture_scanline()` snapshots `audio_current_level` into a 262-entry buffer (one entry per scanline of the 262-scanline NTSC frame).
2. **Commit**: at frame end, `hal_audio_commit_frame()` flips the double buffer (the ISR will pick up the new buffer on its next fire).
3. **Playback**: the 22 050 Hz timer ISR walks through the buffer using a Q8 fixed-point stride calibrated so 262 samples play back over exactly one CoCo frame period (~16.67 ms).
4. **Looping**: if the ISR reaches the buffer end during the render gap between frames, it wraps to the beginning — seamless for periodic tones like SOUND.

```
machine_run_frame():
  for each scanline (0-261):
    machine_run_scanline()       → CPU executes ~57 cycles; DAC may change
    hal_audio_capture_scanline() → snapshot audio_current_level into buffer
  hal_audio_commit_frame()       → signal ISR to swap to new buffer

audio_timer_isr() [IRAM_ATTR, 22050 Hz]:
  1. If new buffer ready: swap read/write buffers, reset position
  2. Read sample from playback buffer at Q8 index
  3. Advance index by ISR_STRIDE_Q8 (wraps at 262 for looping)
  4. dac_output_voltage(DAC_CHANNEL_1, sample)
```

#### Pitch Fine-Tuning

The base stride is `262 * 256 * 60 / 22050 ≈ 183`. An `AUDIO_PITCH_TRIM` constant (currently `-6`) adjusts for residual scanline-boundary quantization error. Each trim unit changes pitch by ~0.55 %.

| Trim | Stride | Effect |
|------|--------|--------|
| 0  | 183 | Base rate (slightly sharp) |
| -6 | 177 | Current setting (~3.3 % lower) |

Tunable in `hal_audio.cpp`. Calibrated so `SOUND 84,20` matches `SOUND 82,20` on desktop XRoar to within ~1 %. The source of the discrepancy is per-frame CPU pace, not the output stage.

#### Bandwidth

Scanline-rate sampling gives an effective sample rate of 262 × 60 = 15 720 Hz (Nyquist ≈ 7.8 kHz). Covers the full CoCo audio range — the SOUND command's maximum frequency is well under 4 kHz.

### Timer ISR

`timerBegin(0, 80, true)` produces a 1 MHz timer base (APB / 80). `timerAlarmWrite(timer, 1000000 / 22050, true)` sets the 22 050 Hz alarm. `timerAttachInterrupt(timer, audio_timer_isr, true)` arms the ISR.

ISR cost is dominated by the `dac_output_voltage()` call — sub-microsecond on the original ESP32. With `IRAM_ATTR`, ISR placement is in IRAM and not affected by flash cache misses.

> The 2.x Arduino-ESP32 core timer API used here (`timerAlarmWrite` + `timerAlarmEnable`) is required because FabGL 1.0.9 is incompatible with core 3.x / IDF 5; the build pins to `esp32:esp32@2.0.17`. See `README.md` Build & Flash.

---

## Sound MUX Gating

Implemented in `machine.cpp` — board-independent. The DAC's audible output is gated by both the MUX enable bit and the MUX source select.

### The Problem

Running `10 PRINT JOYSTK(0): 20 GOTO 10` would otherwise produce a continuous buzz. BASIC's `GETJOY` routine writes ~32 successive-approximation DAC values to PIA1 PA on every read. Without gating, each write would forward to `hal_audio_write_dac()` and produce audible clicks at the joystick polling rate.

Real CoCo `GETJOY` clears PIA1 CRB bit 3 before the ADC loop, disconnecting the DAC from the speaker via the analog MUX. After reading, it restores the bit.

### The Fix

DAC writes are gated on two conditions:

1. **MUX enabled**: PIA1 CRB bit 3 must be set.
2. **MUX source = DAC**: PIA0 CRA bit 3 *and* PIA0 CRB bit 3 must both be 0.

```
machine_write() PIA1 handler:
  On PIA1 DA or CRA write:
    mux_en  = (pia1.ctrl_b & 0x08) != 0
    mux_src = ((pia0.ctrl_b & 0x08) >> 2) | ((pia0.ctrl_a & 0x08) >> 3)
    if (mux_en AND mux_src == 0):
      hal_audio_write_dac(...)   // Output DAC value
    // else: silently ignore — DAC disconnected from speaker

  On PIA1 CRB write (MUX enable may have changed):
    if (mux just re-enabled AND source == DAC):
      hal_audio_write_dac(current PA value)  // Restore audio immediately
```

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

## Public API

| Function | Purpose |
|---|---|
| `hal_audio_init()` | Enable DAC1, init scanline buffers, start 22 050 Hz timer ISR |
| `hal_audio_write_bit(b)` | PIA1 PB bit 1 → 0/255 |
| `hal_audio_write_dac(d6)` | 6-bit DAC value → 8-bit scaled level (MUX-gated by caller) |
| `hal_audio_capture_scanline()` | Snapshot current level into the active buffer |
| `hal_audio_commit_frame()` | Hand the captured buffer to the ISR |
| `hal_audio_set_volume(v)` | No-op placeholder (volume is set by output stage) |
| `hal_audio_write_sample(l, r)` | No-op placeholder (mono only) |
| `hal_audio_debug_tick()` | Reserved for diagnostics |

---

## Lessons Learned

1. **Shared PIA resources are a CoCo design signature.** The CoCo reuses PIAs extensively for cost. Same bits that produce audio also read joysticks. Same MUX select lines that choose audio source also select joystick port/axis. Always check full hardware context before assuming a PIA bit has a single purpose.
2. **XRoar's `sound.c` is the reference** for correct MUX behavior. It implements full source selection with gain tables matching real hardware voltage levels. The simplified version here (gate on/off only) is sufficient because we don't emulate cassette or cartridge audio.
3. **Single-bit audio (PIA1 PB1) is independent of the MUX.** It bypasses the MUX entirely on real hardware and should never be gated by the MUX enable bit.
4. **Pitch correction requires buffered playback, not CPU throttling.** Slowing the CPU to match wall-clock rate would tank emulation FPS. Scanline-rate buffering with ISR playback at the correct CoCo rate fixes pitch without sacrificing emulation speed.
5. **The internal DAC sounds noticeably cleaner than LEDC PWM** — no carrier whistle, no RC-filter quality dependency, 8-bit linearity. The TTGO VGA32 jack is amplifier-driven and produces usable audio straight out of the board.
6. **MUX gating is implemented in `machine.cpp`, not in the HAL.** The HAL is a sink — it doesn't know about the MUX. The emulator core enforces the gate by simply not calling `hal_audio_write_dac()` when the MUX is disconnected. This keeps the HAL board-agnostic and the MUX logic single-sourced.
