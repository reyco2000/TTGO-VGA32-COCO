# Joystick HAL — Implementation Details (TTGO VGA32)

## Overview

The joystick subsystem emulates two CoCo analog joystick ports. Each CoCo port provides two 6-bit analog axes (X/Y, range 0–63) and one fire button, read via a software successive-approximation ADC that uses the PIA's 6-bit DAC as a comparator threshold.

On the **TTGO VGA32 v1.4** build, the physical board has no joystick wiring, so the HAL drives Joystick 1 (port 0) from a **PS/2 mouse** plugged into the on-board mouse header. Joystick 2 (port 1) is a neutral stub.

**Source files:**
- `src/hal/hal_joystick.cpp` — PS/2 mouse-as-joystick implementation
- `src/core/machine.cpp` — PIA0 PA7 comparator wiring (the "what we're emulating" section below)
- `config.h` — `JOYSTICK_MOUSE_SCALE`, `JOYSTICK_MOUSE_INVERT_Y`
- `docs/superpowers/specs/2026-05-21-mouse-joystick-design.md` — design spec for the mouse-as-joystick feature
- *(removed)* `src/hal/CoCoJoystick.h/.cpp` — historical analog ADC reader for ESP32-S3; removed from the codebase

---

## Hardware Being Emulated (unchanged across boards)

### Real CoCo Joystick Circuit

```
   Joystick pot (0-5V)          PIA1 PA bits 2-7
          │                     (6-bit DAC value)
          │                            │
          v                            v
     ┌─────────┐              ┌──────────────┐
     │ Analog  │              │  6-bit       │
     │ MUX     │              │  R-2R DAC    │
     │         │              │  (0-5V out)  │
     │ SEL1/2  │              └──────┬───────┘
     │ (CA2/   │                     │
     │  CB2)   │                     │
     └────┬────┘                     │
          │                          │
          v                          v
     ┌────────────────────────────────────┐
     │        Analog Comparator           │
     │  Output: 1 if joy >= dac           │
     │          0 if joy <  dac           │
     └──────────────┬─────────────────────┘
                    │
                    v
              PIA0 PA bit 7
```

### MUX Select Lines (Axis/Port Selection)

| SEL2 (PIA0 CB2) | SEL1 (PIA0 CA2) | Joystick Input |
|:---:|:---:|------|
| 0 | 0 | Right joystick (port 0), X axis |
| 0 | 1 | Right joystick (port 0), Y axis |
| 1 | 0 | Left joystick (port 1), X axis |
| 1 | 1 | Left joystick (port 1), Y axis |

These are the **same MUX select lines** used for audio source selection. The CoCo reuses them for both purposes (see `audio-hal.md`).

### BASIC ROM ADC Routine (GETJOY)

The CoCo has no hardware ADC. BASIC performs successive approximation in software:

```
GETJOY:
  1. Disable sound MUX (clear PIA1 CRB bit 3) — mutes DAC speaker output
  2. Set CA2/CB2 for desired port/axis
  3. For bit = 5 downto 0:
     a. Write trial DAC value to PIA1 PA
     b. Read PIA0 PA bit 7 (comparator result)
     c. If joy >= dac: keep bit set; else: clear bit
  4. Re-enable sound MUX (set PIA1 CRB bit 3)
  5. Return 6-bit result (0-63)
```

~32 DAC writes per axis read. Without MUX gating, each would produce audible clicks (see `audio-hal.md`).

### Fire Buttons

Fire buttons are wired in parallel with keyboard matrix rows on PIA0 PA:
- **Right joystick button (port 0)** → PIA0 PA bit 0 (shared with keyboard row 0)
- **Left joystick button (port 1)** → PIA0 PA bit 1 (shared with keyboard row 1)

When pressed the bit reads as 0 (active low), same as a keyboard key press.

**Polarity convention in this codebase:** `hal_joystick_read_button(port)` returns **non-zero = pressed**. The PIA0 PA read site in `machine.cpp` does `if (hal_joystick_read_button(0)) row_data &= ~0x01;` — so a non-zero return forces PA0 low. *(Getting this polarity wrong silently corrupts the keyboard column scan — see "Lessons Learned" below.)*

### Comparator Emulation (machine.cpp)

The CoCo's analog comparator is emulated digitally in the PIA0 PA read handler:

```cpp
int joy_port  = (pia0.ctrl_b & 0x08) >> 3;     // CB2 = port select
int joy_axis  = (pia0.ctrl_a & 0x08) >> 3;     // CA2 = axis select
int dac_value = ((pia1.data_a & pia1.ddr_a) & 0xFC) + 2;  // 8-bit range
int js_value  = hal_joystick_read_axis(port, axis) * 4 + 2;
if (js_value >= dac_value)
    row_data |= 0x80;   // PA7 = 1 (joy >= threshold)
else
    row_data &= ~0x80;  // PA7 = 0 (joy < threshold)
```

Both values are converted to 8-bit range (2–254) for comparison: DAC is `(PIA1 PA & 0xFC) + 2`, joystick is `axis_6bit * 4 + 2`. Matches XRoar's `joystick_update()` exactly.

---

## TTGO VGA32 Implementation — PS/2 Mouse as Joystick 1

### Pins (fixed by the board)

| Signal | GPIO | Notes |
|---|---|---|
| PS/2 Mouse CLK | 26 | `PIN_PS2_MOUSE_CLK` |
| PS/2 Mouse DATA | 27 | `PIN_PS2_MOUSE_DATA` |

The PS/2 keyboard and mouse share a single `fabgl::PS2Controller`. `hal_keyboard_init()` brings it up with `PS2Preset::KeyboardPort0_MousePort1`, which configures both ports (GPIO33/32 for the keyboard, GPIO26/27 for the mouse) and allocates both `Keyboard` and `Mouse` objects internally. `hal_joystick_init()` just retrieves the mouse pointer via `PS2Controller::mouse()`.

### Position Model: Accumulate + Clamp

The mouse reports relative deltas; the HAL integrates them into a logical 0..63 position per axis and clamps at the edges. Once the mouse stops, the joystick stays where it is — closest to real CoCo joystick behavior, where the stick holds whatever position you left it in (no spring).

```c
static int16_t s_pos_x    = 32;   // 6-bit position, 0..63
static int16_t s_pos_y    = 32;
static bool    s_btn_left = false;
```

`int16_t` so intermediate clamping math can briefly go out of range without overflow.

### Update Path

`hal_joystick_update()` is called from `machine.cpp` every ~16 emulator scanlines (~16 times per frame). It drains all pending mouse delta packets from FabGL's queue and integrates them:

```c
void hal_joystick_update(void) {
    fabgl::Mouse* m = fabgl::PS2Controller::mouse();
    if (!m) return;
    while (m->deltaAvailable()) {
        fabgl::MouseDelta d;
        if (!m->getNextDelta(&d, 0)) break;

        int dx = (int)d.deltaX / JOYSTICK_MOUSE_SCALE;
        int dy = (int)d.deltaY / JOYSTICK_MOUSE_SCALE;

        s_pos_x += dx;
#if JOYSTICK_MOUSE_INVERT_Y
        s_pos_y += dy;
#else
        s_pos_y -= dy;   // FabGL Y is positive-up; CoCo Y is positive-down
#endif

        if (s_pos_x < 0)  s_pos_x = 0;
        if (s_pos_x > 63) s_pos_x = 63;
        if (s_pos_y < 0)  s_pos_y = 0;
        if (s_pos_y > 63) s_pos_y = 63;

        s_btn_left = (d.buttons.left != 0);
    }
}
```

The `Y` polarity flip comes from the convention difference: FabGL's `MouseDelta::deltaY` is **positive-up** (per the FabGL header comments); CoCo joystick Y is **positive-down**. Without the flip, moving the mouse forward would feel like "joystick down" in-game.

### Read Path

```c
uint8_t hal_joystick_read_axis(int port, int axis) {
    if (port != 0) return 32;                          // Joystick 2 stub
    return (uint8_t)((axis == 0) ? s_pos_x : s_pos_y);
}

uint8_t hal_joystick_read_button(int port) {
    if (port != 0) return 0;                           // Joystick 2 stub
    return s_btn_left ? 1 : 0;
}

bool hal_joystick_compare(int port, int axis, uint8_t dac_value) {
    if (port != 0) return false;
    int v = (axis == 0) ? s_pos_x : s_pos_y;
    return (v * 4 + 2) >= (int)dac_value;
}
```

Port 1 reads are identical to the original VGA32 stub: axis = 32 (center), button = 0 (not pressed), compare = false. Those bits of the matrix and the comparator stay undisturbed.

### Configuration

`JOYSTICK_MOUSE_SCALE` and `JOYSTICK_MOUSE_INVERT_Y` in `config.h` (VGA32 block) are now **first-boot defaults only**:

```c
// First-boot default divisor (see "Runtime Sensitivity" below).
#define JOYSTICK_MOUSE_SCALE     4

// First-boot default for Invert-Y.
#define JOYSTICK_MOUSE_INVERT_Y  0
```

These macros seed the runtime values on a fresh device (no saved NVS settings). Once the user adjusts sensitivity via the Supervisor (Settings -> Mouse Sensitivity), the runtime values are persisted to NVS and override the macros on every subsequent boot.

### Runtime Sensitivity

`hal_joystick.cpp` holds two runtime variables, seeded from the `config.h` macros above and overridden at boot from NVS (see `supervisor_load_joystick()`):

```c
static int  s_scale  = JOYSTICK_MOUSE_SCALE;   // divisor applied to raw mouse deltas
static bool s_invert = JOYSTICK_MOUSE_INVERT_Y;
```

`hal_joystick_update()` uses `s_scale` and `s_invert` in place of the old compile-time macros when scaling `deltaX`/`deltaY` and applying the Y-axis flip.

**Level <-> divisor mapping:**

| Level | Divisor (`s_scale`) | Notes |
|---|---|---|
| 1 | 10 | Least sensitive |
| 7 | 4 | Default (matches old `JOYSTICK_MOUSE_SCALE`) |
| 10 | 1 | Most sensitive (clamped, divisor never goes below 1) |

Formula: `divisor = 11 - level`, clamped so `divisor >= 1`. Higher level = smaller divisor = more sensitive.

**HAL API** (`src/hal/hal.h`):

| Function | Description |
|---|---|
| `void hal_joystick_set_sensitivity(uint8_t level)` | Sets `s_scale` from a 1..10 level using the formula above |
| `uint8_t hal_joystick_get_sensitivity(void)` | Returns the current level (inverse of the formula) |
| `void hal_joystick_set_invert_y(bool invert)` | Sets `s_invert` |
| `bool hal_joystick_get_invert_y(void)` | Returns `s_invert` |
| `void hal_joystick_get_pos(uint8_t* x, uint8_t* y)` | Returns the live `s_pos_x`/`s_pos_y` (0..63), used to draw the live cursor in the Mouse Sensitivity screen |

### NVS Persistence

Namespace `"sv"` (same as the rest of the supervisor's persisted settings):

| Key | Type | Description |
|---|---|---|
| `joyLevel` | UChar | Sensitivity level 1..10 |
| `joyInv` | Bool | Invert-Y flag |

`supervisor_save_joystick(level, invert)` writes both keys; `supervisor_load_joystick()` reads them back and applies them via the HAL setters above. `supervisor_load_joystick()` is called at boot in `TTGO-VGA32-COCO.ino`, immediately after `supervisor_load_keymap()`. If no saved values exist, the `config.h` macro defaults remain in effect (level 7 / divisor 4, Invert-Y off).

The runtime sensitivity is adjusted interactively from **Supervisor -> Settings -> Mouse Sensitivity** — see `supervisor.md`.

### Button Mapping

| Mouse button | CoCo joystick |
|---|---|
| Left | Joystick 1 fire (PA0 forced low when pressed) |
| Middle / Right | Ignored |

### Error Handling

| Condition | Behavior |
|---|---|
| Mouse not plugged in at boot | `PS2Controller::mouse()` returns `nullptr`; `hal_joystick_update()` short-circuits. Joystick stays at center, button reads 0. Boot log: `Joystick: PS/2 mouse not enumerated (yet)`. |
| Mouse unplugged at runtime | `deltaAvailable()` returns false; accumulator freezes at the last position. |
| Huge fast motions | `int16_t` accumulator holds ±32k; clamped to 0..63 after every event. No overflow. |
| Y-axis feels inverted | Set `JOYSTICK_MOUSE_INVERT_Y = 1` in `config.h` and reflash. |
| Supervisor open (F3) | Mouse keeps accumulating in the background — harmless because the emulator CPU is paused while the supervisor is active. |

---

## Data Flow Summary (VGA32)

```
PS/2 Mouse (GPIO26 CLK / GPIO27 DATA)
  │
  v
ESP32 ULP scancode shifter (FabGL)
  │
  v
Mouse Delta Packet Queue (FabGL)
  │
  v
hal_joystick_update()  ───  scale by JOYSTICK_MOUSE_SCALE
(called 16x/frame)    │
                      v
              s_pos_x / s_pos_y (clamped 0..63)
              s_btn_left
                      │
                      v
hal_joystick_read_axis()   hal_joystick_read_button()   hal_joystick_compare()
                      │             │                            │
                      v             v                            v
            machine.cpp PIA0 PA read handler:
              - PA7 = comparator result
              - PA0 forced low if left mouse held
                      │
                      v
                  CPU reads PIA0 PA
```

---

## Lessons Learned

1. **The FabGL `PS2Controller` explicit-pin `begin()` overload does not allocate the `Keyboard` or `Mouse` objects.** Only the preset variants do (`KeyboardPort0`, `KeyboardPort0_MousePort1`, etc.). We use `KeyboardPort0_MousePort1` so a single `begin()` call brings up both devices. *(See `keyboard-hal.md` "Lessons Learned" for the symptom of getting this wrong: keys are processed by the ULP but never appear in the queue because no `Keyboard` exists to receive them.)*

2. **Joystick button polarity is the most subtle bug in this HAL.** `hal_joystick_read_button()` returns *non-zero = pressed*. The first iteration of the VGA32 stub returned `1` for "not pressed", which made the emulator think both joystick buttons were always pressed — forcing PA0 and PA1 low on every PIA0 read. That made every keyboard column read look like "@" and "A" were also held, and BASIC's KEYIN rejected the apparent ghost combination. **Always verify polarity against the call site at `machine.cpp` before committing a stub.**

3. **FabGL Y is positive-up; CoCo Y is positive-down.** Mouse forward = joystick up on the screen. Without the subtraction in the update path, games feel "Y-inverted". A compile flag (`JOYSTICK_MOUSE_INVERT_Y`) is provided in case a particular game expects the opposite — but for general-purpose use, leave the default (`0`).

4. **The DAC is shared between audio and joystick.** PIA1 PA bits 2-7 serve as both the audio DAC output and the joystick comparator threshold. The sound MUX (PIA1 CRB bit 3) controls whether DAC writes reach the speaker, but the comparator always reads the PIA register directly regardless of MUX state. See `audio-hal.md` for the MUX gating fix that prevents `JOYSTK()` noise.

5. **Fire buttons share keyboard matrix lines.** Right button = PA0, Left button = PA1, same bits as keyboard rows 0 and 1. Code must OR the button state into the keyboard scan result, not replace it. `machine.cpp`'s PIA0 PA read handler does this correctly today.

6. **Update throttling matters.** CoCo programs (especially assembly games) may read PIA0 PA hundreds of times per frame in tight polling loops. Calling `hal_joystick_update()` on every read would devastate performance. The 16-scanline throttle in `machine.cpp` gives ~16 updates/frame, more than enough for smooth mouse-driven control with negligible overhead.

7. **The successive-approximation ADC is a software algorithm, not hardware.** The CoCo CPU performs the binary search by writing DAC values and reading the comparator. The emulator does not replicate that algorithm — it just needs the comparator to return the correct boolean for any given DAC threshold. BASIC's `GETJOY` (or game code) does the rest.

8. **MUX select lines (CA2/CB2) are shared with audio source selection.** The same 2-bit field selects both the joystick port/axis AND the audio source. Software must manage these carefully — BASIC's `GETJOY` routine sets them for joystick reading and restores them for audio afterward.
