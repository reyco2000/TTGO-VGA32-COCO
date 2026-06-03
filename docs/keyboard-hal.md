# Keyboard HAL — TTGO VGA32

## Overview

Translates a **PS/2 keyboard** plugged into the TTGO VGA32 v1.4 keyboard header into the CoCo 7×8 keyboard matrix that the emulated PIA0 scans. Keyboard input is driven by **FabGL's PS/2 driver** (ULP-based scancode shifter + scancode-to-VirtualKey converter) — there is no USB or HID involvement on this build.

The PS/2 keyboard shares a single `fabgl::PS2Controller` with the PS/2 mouse used by `hal_joystick.cpp` (see `joystick-hal.md`). One PS2 init call brings up both devices.

**Files:**

| File | Purpose |
|---|---|
| `hal_keyboard.cpp` | FabGL PS/2 init, VK → CoCo matrix mapping, hotkey intercept, deferred release |
| `hal.cpp` | Calls `hal_keyboard_tick()` each frame |
| `hal.h` | Public API declarations |

The historical ESP32-S3 USB HID implementation (`usb_kbd_host.cpp/h`) has been removed from the codebase — the VGA32 PS/2 path is the only keyboard backend.

---

## Architecture

```
PS/2 keyboard ──GPIO33 (CLK) ──┐
              ──GPIO32 (DATA)──┤
                               │
                       ┌───────┴────────┐
                       │   ESP32 ULP    │  ← FabGL ULP scancode shifter
                       │  scancode      │    (always-on, low-power coprocessor)
                       └───────┬────────┘
                               │
                       ┌───────┴────────┐
                       │ SCodeToVK task │  ← FabGL scancode → VirtualKey
                       │ (FreeRTOS)     │    Uses active KeyboardLayout
                       └───────┬────────┘
                               │ VirtualKeyItem { vk, down, ASCII, SHIFT, CTRL, ... }
                               ▼
                    ┌──────────────────────┐
                    │  VK Queue (FabGL)    │
                    └───────────┬──────────┘
                                │ getNextVirtualKey(...)
                                ▼
                       ┌────────┴────────┐
                       │   Core 1        │
                       │ loop()          │
                       │ hal_process_    │  ← hal_keyboard_tick() drains queue
                       │ input()         │    → process_vk(it) for each item
                       └────────┬────────┘
                                │
                    ┌───────────┴───────────┐
                    │ Layer 1: Supervisor / │ F3 toggle, F4 reset,
                    │          Hotkey gate  │ F5 FPS, F6 quick-mount
                    │ Layer 2: VK_MAP[]     │ F1/F2 → CoCo 3 matrix
                    │          lookup       │ Letters/digits/symbols
                    │ Layer 3: set_key /    │
                    │          defer_release│
                    └───────────────────────┘
```

The Mouse path on the same `PS2Controller` (GPIO26 CLK / GPIO27 DATA) is documented in `joystick-hal.md`.

---

## Initialization

`hal_keyboard_init()`:

```c
s_ps2.begin(fabgl::PS2Preset::KeyboardPort0_MousePort1,
            fabgl::KbdMode::CreateVirtualKeysQueue);

fabgl::Keyboard* kbd = fabgl::PS2Controller::keyboard();
if (kbd) kbd->setLayout(&fabgl::SpanishLayout);
```

**Why the preset and not the explicit-pin overload:** the explicit-pin `PS2Controller::begin(clk, data, ...)` only configures the GPIOs and starts the ULP scancode shifter — it does **not** allocate the `Keyboard` (or `Mouse`) object. Without that allocation, the VirtualKey queue is never populated and the keyboard silently does nothing. Use the **preset variants** (`KeyboardPort0`, `KeyboardPort0_MousePort1`, ...) which allocate the device objects internally and call their `begin()` for you.

`PS2Preset::KeyboardPort0_MousePort1` defaults map keyboard to GPIO33/32 and mouse to GPIO26/27 — the wiring the TTGO VGA32 v1.4 board provides on its two PS/2 headers. We also use this preset so `hal_joystick.cpp` can pick up `PS2Controller::mouse()` without duplicating PS/2 setup.

**Layout selection:** `fabgl::SpanishLayout` is set as the default to match a Spanish (Latin American) keyboard. Switch to any of FabGL's built-in layouts (`USLayout`, `UKLayout`, `GermanLayout`, `ItalianLayout`, `FrenchLayout`, `BelgianLayout`, `NorwegianLayout`, `JapaneseLayout`) by changing this one line and reflashing. The layout is what translates raw PS/2 scancodes into FabGL's `VirtualKey` enum values, and resolves SHIFT/AltGr combinations into the appropriate "shifted-symbol" virtual keys (`VK_HASH`, `VK_DOLLAR`, …).

---

## CoCo Keyboard Matrix

ROM-verified, 7 rows × 8 columns, active-LOW:

```
        PB0    PB1    PB2    PB3    PB4    PB5    PB6    PB7
PA0:     @      A      B      C      D      E      F      G
PA1:     H      I      J      K      L      M      N      O
PA2:     P      Q      R      S      T      U      V      W
PA3:     X      Y      Z     UP    DOWN   LEFT  RIGHT   SPACE
PA4:     0      1      2      3      4      5      6      7
PA5:     8      9      :      ;      ,      -      .      /
PA6:   ENTER  CLEAR  BREAK   ALT    CTL    F1     F2    SHIFT
```

On CoCo 2 the PA6 PB3–PB6 cells are unused (writes are harmless no-ops). On CoCo 3 they're scanned by the GIME keyboard interrupt and by OS-9.

`key_matrix[col]` is indexed by PB column (0–7). Each bit represents a PA row (0–6). **bit = 0 means pressed.**

---

## VirtualKey → CoCo Mapping

```cpp
struct VkMap {
    fabgl::VirtualKey vk;
    uint8_t col;             // PB column (0-7)
    uint8_t row;             // PA row (0-6)
    bool    needs_shift;     // Force CoCo SHIFT asserted alongside this key
    bool    suppress_shift;  // PC SHIFT held + this combo → suppress CoCo SHIFT
};
```

`find_vk_mapping(vk)` does a linear scan of `VK_MAP[]`. Returns the first match or `nullptr`.

**`VK_MAP[]` covers:**

- **Letters** — both lowercase (`VK_a`–`VK_z`) and uppercase (`VK_A`–`VK_Z`) forms map to the same matrix cell. The uppercase entries set `suppress_shift = true` so PC-SHIFT+letter doesn't *also* assert CoCo SHIFT (the layout has already produced the uppercase VK).
- **Digits** — `VK_0`–`VK_9` → PA4/PA5 row, no special handling.
- **Arrows + Space** — `VK_UP`/`DOWN`/`LEFT`/`RIGHT`/`SPACE` → PA3 row.
- **Punctuation** — `VK_SEMICOLON`, `VK_COMMA`, `VK_MINUS`, `VK_PERIOD`, `VK_SLASH`, `VK_AT`.
- **Shifted-digit symbols** — `VK_EXCLAIM`, `VK_QUOTEDBL`, `VK_HASH`, `VK_DOLLAR`, `VK_PERCENT`, `VK_AMPERSAND`, `VK_QUOTE`, `VK_LEFTPAREN`, `VK_RIGHTPAREN`. Each maps to the underlying digit's matrix position **and** sets `needs_shift = true` so the CoCo SHIFT key is asserted while the digit is held — this matches the CoCo's SHIFT+digit symbol set, which is the same as US-PC.
- **Shifted-punctuation** — `VK_QUESTION` (SHIFT+/), `VK_GREATER` (SHIFT+.), `VK_LESS` (SHIFT+,), `VK_PLUS` (SHIFT+-), `VK_COLON` (SHIFT+;).
- **PA6 control row** — `VK_RETURN`/`VK_KP_ENTER` (ENTER), `VK_BACKSPACE` (mapped to LEFT-ARROW PA3,PB5 to match BASIC's line-edit convention), `VK_ESCAPE`/`VK_PAUSE`/`VK_BREAK` (BREAK), `VK_INSERT`/`VK_DELETE` (CLEAR), `VK_LALT`/`VK_RALT` (CoCo 3 ALT), `VK_LCTRL`/`VK_RCTRL` (CoCo 3 CTL), `VK_F1`/`VK_F2` (forwarded to the CoCo 3 matrix).
- **Modifiers** — `VK_LSHIFT`/`VK_RSHIFT` map directly to PA6,PB7 (CoCo SHIFT).

**Why shifted-symbol entries are needed:** FabGL's layout pre-resolves SHIFT-modified keys. Pressing PC SHIFT+3 on a Spanish layout emits `VK_HASH` (not `VK_3` with a SHIFT modifier flag in `VirtualKeyItem`). Without explicit `VK_HASH` etc. mapping, the HAL would silently drop these events — which was the bug behind "SHIFT+3 doesn't produce #" during VGA32 bring-up.

---

## Event Dispatch (`process_vk`)

Called once per dequeued `VirtualKeyItem`:

**Layer 1 — Supervisor toggle** (press only, always active):

| VK | Action |
|---|---|
| `VK_F3` | `supervisor_toggle()` — consume; no further dispatch |

**Layer 1b — When supervisor is active:**
All non-F3 keys are forwarded to `supervisor_on_key(usage, pressed)`. Because the supervisor's existing key handler is keyed on USB HID usage IDs (inherited from the S3 build), the HAL provides a small `vk_to_hid_usage()` translation: VK_RETURN → 0x28, VK_ESCAPE → 0x29, VK_UP/DOWN/LEFT/RIGHT → 0x52/0x51/0x50/0x4F, F1–F7 → 0x3A–0x40, INSERT/DELETE/HOME/END/PAGEUP/PAGEDOWN → 0x49/0x4C/0x4A/0x4D/0x4B/0x4E. Letters and digits are reconstructed from the `VirtualKeyItem::ASCII` byte (letter → 0x04 + offset, digit 0–9 → 0x27/0x1E–0x26). CoCo matrix sees nothing while supervisor is active.

**Layer 1c — Emulation hotkeys** (press only, supervisor must be inactive):

| VK | Action |
|---|---|
| `VK_F4` | Flush dirty disks + `machine_reset()` |
| `VK_F5` | `hal_video_toggle_fps_overlay()` |
| `VK_F6` | `supervisor_quick_mount_last_disk()` |

> **F1/F2 are not host shortcuts.** They are forwarded to the CoCo 3 matrix at PA6,PB5 / PA6,PB6 via the normal VK_MAP path.

**Layer 2 — CoCo matrix injection:**
`find_vk_mapping(vk)` returns the (col, row) plus `needs_shift` / `suppress_shift` flags. If no entry matches, a `[KBD] vk=N UNMAPPED` line is printed on key-down (handy for finding keys to add to the map). On press:

```c
bool want_shift = (item.SHIFT && !k->suppress_shift) || k->needs_shift;
apply_shift(want_shift);
set_key(k->col, k->row, true);
```

On release, `defer_release(k->col, k->row)` is called — see below.

### SHIFT Handling

CoCo SHIFT is a matrix position (PA6, PB7), not a modifier flag. The HAL composes the right SHIFT state per event:

- **PC SHIFT held + plain key** (e.g. SHIFT+A → `VK_A` with `item.SHIFT=1`): `want_shift = true` → CoCo SHIFT asserted alongside `A`.
- **`suppress_shift`** entries (uppercase letters, `@`, `:` etc.): PC SHIFT was needed to produce the VK, but the resulting CoCo character does *not* require CoCo SHIFT — so we suppress.
- **`needs_shift`** entries (shifted-digit symbols like `#`, `$`): the VK alone tells us the user wants a shifted CoCo character — force CoCo SHIFT regardless of `item.SHIFT`.
- **`VK_LSHIFT`/`VK_RSHIFT`** directly toggle the CoCo SHIFT cell. Releases go through `defer_release()` to keep SHIFT held long enough for BASIC's KEYIN debounce.

---

## Deferred Key Release

**Problem:** CoCo BASIC's KEYIN routine scans the matrix over multiple frames with debouncing. A fast PS/2 tap (press+release inside one emulator frame) would otherwise be invisible.

**Solution:** every release schedules a delayed clear in `MIN_HOLD_FRAMES = 4` frames (≈67 ms at 60 fps).

```cpp
struct DeferredRelease {
    uint8_t col, row;
    uint8_t frames_left;   // 0 = slot free
};
```

- `defer_release(col, row)` — queues a release. If the same cell is already scheduled, refresh the counter. Up to `MAX_DEFERRED = 8` slots; overflow falls back to immediate release.
- `tick_deferred_releases()` — called once per frame from `hal_keyboard_tick()`. Decrements each non-zero counter; clears the matrix bit when it hits zero.
- Re-pressing a key while its release is pending just resets the counter to `MIN_HOLD_FRAMES`.

---

## Queue Drain (`hal_keyboard_tick`)

Called once per frame from `hal_process_input()`:

```c
void hal_keyboard_tick(void) {
    tick_deferred_releases();
    fabgl::Keyboard* kbd = fabgl::PS2Controller::keyboard();
    if (!kbd) return;
    while (kbd->virtualKeyAvailable() > 0) {
        fabgl::VirtualKeyItem it;
        if (!kbd->getNextVirtualKey(&it, 0)) break;
        process_vk(it);
    }
}
```

`getNextVirtualKey(&it, 0)` is non-blocking (0 ms timeout). All pending VKs are drained each frame.

---

## PIA Integration

The emulated PIA0 calls `hal_keyboard_scan(column)` during port-A reads:

```c
uint8_t hal_keyboard_scan(uint8_t column) {
    return (column < 8) ? key_matrix[column] : 0xFF;
}
```

The CoCo CPU writes the column-select pattern to PIA0 PB, then reads PA. BASIC's KEYIN computes `key_code = PA_row * 8 + PB_column` after scanning.

`machine.cpp` (PIA0 PA read site) combines the keyboard scan with the joystick button bits via `&=`. **Caveat:** the joystick button read must return 0 = not-pressed; non-zero forces PA0/PA1 low and corrupts every column scan. (This was the bug behind the initial VGA32 keyboard symptom — the joystick stub returned 1 = "not pressed" inverted, ghosting `@` and `A` into every column. See `joystick-hal.md` Lessons Learned.)

---

## Public API Summary

| Function | File | Called from | Purpose |
|---|---|---|---|
| `hal_keyboard_init()` | hal_keyboard.cpp | `hal_init()` | Reset matrix, init deferred releases, start FabGL PS/2 (keyboard + mouse), select layout |
| `hal_keyboard_tick()` | hal_keyboard.cpp | `hal_process_input()` | Tick deferred releases + drain VK queue |
| `hal_keyboard_scan(col)` | hal_keyboard.cpp | PIA0 port-A read | Return matrix column (active LOW) |
| `hal_keyboard_set_machine(m)` | hal_keyboard.cpp | `setup()` | Wire Machine pointer for F4/F6 hotkeys |
| `hal_keyboard_press(row,col)` | hal_keyboard.cpp | Test code | Software key injection |
| `hal_keyboard_release(row,col)` | hal_keyboard.cpp | Test code | Software key release |
| `hal_keyboard_release_all()` | hal_keyboard.cpp | Init / reset | Clear entire matrix to 0xFF |

---

## Hardware

| Signal | GPIO | Notes |
|---|---|---|
| PS/2 CLK | 33 | `PIN_PS2_KBD_CLK` |
| PS/2 DATA | 32 | `PIN_PS2_KBD_DATA` |

Power and pull-ups are provided by the board's PS/2 connector. No external wiring needed.

**Library:** FabGL 1.0.9 (`fabgl::PS2Controller`, `fabgl::Keyboard`, `fabgl::KeyboardLayout`).

---

## Troubleshooting

### Keys not registering at all
1. Check serial boot log for the `Keyboard: FabGL PS/2 on CLK=GPIO33 DATA=GPIO32 (Spanish layout)` line — confirms init reached the right path.
2. Drop a temporary debug print in `hal_keyboard_tick()` before `getNextVirtualKey()` and confirm `virtualKeyAvailable() > 0` while you're typing. If it stays at 0, FabGL isn't seeing scancodes — check the PS/2 cable, keyboard power (5V), and that the keyboard supports PS/2 mode (some USB→PS/2 adapter cables only carry data when the keyboard speaks PS/2).
3. Confirm `PS2Controller::keyboard()` is non-null. If it's null, `hal_keyboard_init()` used the explicit-pin overload by mistake — the explicit-pin variant does **not** allocate the Keyboard object. Use the `PS2Preset::KeyboardPort0_MousePort1` preset.

### Specific keys produce no result (e.g. `#` from SHIFT+3)
The layout pre-resolves shifted keys into "symbol" VKs (`VK_HASH`, `VK_DOLLAR`, ...). If the symbol isn't in `VK_MAP[]`, the HAL prints `[KBD] vk=N UNMAPPED` on key-down — grep the log for the VK number, look it up in `~/Arduino/libraries/FabGL/src/fabutils.h`, and add an entry to `VK_MAP[]`.

### Wrong layout (e.g. Spanish keys produce US characters)
Edit the `kbd->setLayout(&fabgl::XxxLayout)` line in `hal_keyboard_init()` and reflash. Layouts available: `USLayout`, `UKLayout`, `GermanLayout`, `ItalianLayout`, `SpanishLayout` (default), `FrenchLayout`, `BelgianLayout`, `NorwegianLayout`, `JapaneseLayout`.

### Every keyboard column shows `@` or `A` permanently pressed
The joystick button polarity is inverted. `hal_joystick_read_button(port)` must return **0 = not pressed**. See `joystick-hal.md` Lessons Learned.

### Keys "stick" briefly after release
`MIN_HOLD_FRAMES = 4` frames is intentional — it gives BASIC's KEYIN routine time to scan and debounce the key. If a game feels laggy, lower it to 2 or 3 and reflash.

### Supervisor eats all keys
F3 toggles supervisor on/off. When active, all non-F3 keys route to `supervisor_on_key()` — the CoCo matrix is frozen. F3 is always processed first, so it can always close the supervisor.

### Adding a new hotkey
1. Pick an F-key VK from FabGL (`VK_F4`–`VK_F12`). **`VK_F1`/`VK_F2` are reserved for the CoCo 3 matrix — never use them as host shortcuts.**
2. Add a `if (vk == fabgl::VK_FN && pressed)` block in `process_vk()` Layer 1c.
3. Use `extern void your_function(void);` to call across modules.
4. `return` after handling so the key doesn't reach the CoCo matrix.

### Adding a new CoCo key mapping
1. Find the FabGL VK constant for the host key in `~/Arduino/libraries/FabGL/src/fabutils.h`.
2. Find the CoCo matrix position (PA row, PB col) from the matrix table above.
3. Add a `{ fabgl::VK_XXX, col, row, needs_shift, suppress_shift }` entry to `VK_MAP[]`.
4. Decide the shift flags: `needs_shift=true` if pressing the host key should force CoCo SHIFT; `suppress_shift=true` if the host SHIFT was needed to produce this VK but the CoCo character does not need CoCo SHIFT.
