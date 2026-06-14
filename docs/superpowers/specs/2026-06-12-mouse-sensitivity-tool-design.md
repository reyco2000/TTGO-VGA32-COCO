# Mouse / Joystick Sensitivity Adjuster — Design

Date: 2026-06-12
Status: Approved (design)

## Summary

Add an interactive **Mouse Sensitivity** tool under Supervisor → Settings. It opens
a full-screen "pad" with a live cursor driven by the PS/2 mouse (CoCo joystick 1).
Arrow keys adjust the mouse sensitivity (Left/Right) and toggle Invert-Y (Up/Down).
The setting is applied live and persisted to NVS so it survives reboot.

## Background

The CoCo "joystick 1" is a PS/2 mouse (`src/hal/hal_joystick.cpp`). Sensitivity is
currently the compile-time macro `JOYSTICK_MOUSE_SCALE` (default 4) in `config.h`:
each raw mouse count is divided by it before being integrated into the 0..63 logical
axis. Lower divisor = more sensitive. Y inversion is the compile-time
`JOYSTICK_MOUSE_INVERT_Y` (default 0). Neither is adjustable at runtime today.

Full-screen interactive supervisor screens already exist (Key Mapper,
`src/supervisor/sv_keymap.cpp`): a dedicated `SV_State`, an `on_key` handler, and a
render function wired into `supervisor.cpp`. The supervisor loop
(`supervisor_update_and_render()`) is event-driven — it only repaints when
`needs_redraw` is set — and `hal_joystick_update()` is **not** called while the
supervisor is active (emulation is paused).

## Goals

- Adjust mouse sensitivity and Y-inversion from the OSD, no rebuild required.
- Show a live cursor that moves with the mouse so the user can feel the change.
- Persist the chosen values across reboots.

## Non-goals

- Joystick 2 (port 1) — remains a neutral stub.
- Analog/ADC joystick hardware (not wired in this build).
- Per-axis independent sensitivity, acceleration curves, or sub-1 gain.

## UI

New Settings row **"Mouse Sensitivity"** opens the screen. `SETTINGS_COUNT` 4 → 5.

```
+------------ Mouse Sensitivity ------------+
|                                           |
|      +--------------------------+         |
|      |            #             |  cursor |
|      |                          |  tracks |
|      |                          |  mouse  |
|      +--------------------------+         |
|                                           |
|        Sensitivity:  [|||||||...]  7      |
|        Invert Y:      OFF                  |
|                                           |
|   < > Sensitivity   ^v Invert   ESC Save  |
+-------------------------------------------+
```

- **Left/Right**: sensitivity level 1..10. Higher level = more sensitive.
- **Up/Down**: toggle Invert-Y.
- **ESC**: save to NVS and return to Settings.
- Mouse buttons are ignored on this screen (no clicks leak into the key matrix).

### Level → divisor mapping

Level is the user-facing 1..10 value; divisor is `JOYSTICK_MOUSE_SCALE`.
Higher level = lower divisor = more sensitive.

| Level | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
|-------|---|---|---|---|---|---|---|---|---|----|
| Divisor | 10 | 9 | 8 | 7 | 6 | 5 | 4 | 3 | 2 | 1 |

So `divisor = 11 - level`, `level = 11 - divisor`. The `config.h` default
divisor 4 maps to level 7. Divisor is clamped to >= 1 to avoid divide-by-zero.

## Architecture / Components

### `hal_joystick.cpp` + `hal.h` — runtime tunables
- Replace direct use of the `JOYSTICK_MOUSE_SCALE` / `JOYSTICK_MOUSE_INVERT_Y`
  macros in `hal_joystick_update()` with runtime variables seeded from those
  macros at startup:
  - `static int  s_scale  = JOYSTICK_MOUSE_SCALE;`
  - `static bool s_invert = JOYSTICK_MOUSE_INVERT_Y;`
- The `#if JOYSTICK_MOUSE_INVERT_Y` compile branch becomes a runtime `if (s_invert)`.
- Public API (declared in `hal.h`):
  - `void hal_joystick_set_sensitivity(uint8_t level);`  // 1..10, clamps
  - `uint8_t hal_joystick_get_sensitivity(void);`        // returns 1..10
  - `void hal_joystick_set_invert_y(bool on);`
  - `bool hal_joystick_get_invert_y(void);`
  - `void hal_joystick_get_pos(uint8_t* x, uint8_t* y);` // live 0..63 axes
- Level<->divisor conversion lives here so the mapping has one owner.

### `sv_joystick.cpp` / `sv_joystick.h` — screen logic (new)
Mirrors `sv_keymap.cpp`. Holds the working copy of level + invert, handles keys,
polls the mouse, and calls the render helper.
- `void sv_joystick_open(Supervisor_t* sv);`  // loads current HAL values, sets state
- `void sv_joystick_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);`
- `void sv_joystick_render(Supervisor_t* sv);`
- `void sv_joystick_tick(Supervisor_t* sv);`  // poll mouse, force redraw
- Left/Right adjust level and call `hal_joystick_set_sensitivity()` live;
  Up/Down toggle invert and call `hal_joystick_set_invert_y()` live.
- ESC: `supervisor_save_joystick()` then return to `SV_SETTINGS`.

### `sv_render.cpp` / `sv_render.h` — draw helper
`sv_render.cpp` owns the `g_tft` TFT_eSPI shim. Add:
- `void sv_render_joystick_pad(uint8_t cursor_x, uint8_t cursor_y,
                               uint8_t level, bool invert_y);`
Draws the bordered pad box, the cursor square at the mapped position, the
sensitivity bar + numeric level, and the Invert-Y value. Uses `fillRect` /
`drawRect` / `drawString` already used elsewhere in this file. Cursor position
maps 0..63 into the inner pad rectangle.

### `supervisor.cpp` — wiring + persistence
- New `SV_State` value `SV_JOY_SENSE` (in `supervisor.h`).
- Dispatch in `supervisor_on_key()` → `sv_joystick_on_key()`.
- Dispatch in `supervisor_update_and_render()`: when `state == SV_JOY_SENSE`,
  call `sv_joystick_tick()` (which polls the mouse and sets `needs_redraw`)
  before the normal redraw path, so the cursor animates at the ~60fps cap.
- NVS helpers (namespace `"sv"`, keys `"joyLevel"`, `"joyInv"`), following the
  existing `supervisor_save_kbd_layout` / `supervisor_save_serial_mode` pattern:
  - `void supervisor_save_joystick(uint8_t level, bool invert);`
  - `void supervisor_load_joystick(void);`  // reads NVS, applies to HAL
- Call `supervisor_load_joystick()` during supervisor init so the saved value is
  applied at boot.

### `sv_menu.cpp` — Settings row
- `SETTINGS_COUNT` 4 → 5, add label "Mouse Sensitivity".
- In `settings_toggle()`, the new row calls `sv_joystick_open(sv)`.
- `sv_settings_render()` shows the current level as the row value (e.g. "7").

## Data Flow

1. Boot: `supervisor_load_joystick()` reads NVS → `hal_joystick_set_sensitivity()`
   / `hal_joystick_set_invert_y()`. If NVS empty, HAL keeps the `config.h` defaults.
2. User opens the screen: `sv_joystick_open()` reads current HAL values into the
   screen's working copy.
3. Each tick: `sv_joystick_tick()` calls `hal_joystick_update()` (integrates mouse
   deltas into `s_pos_x/y`), reads `hal_joystick_get_pos()`, forces redraw.
4. Arrow key: working copy changes, HAL setter called immediately (live feel).
5. ESC: `supervisor_save_joystick()` writes NVS; state → `SV_SETTINGS`.

## Error Handling / Edge Cases

- **No mouse enumerated**: `hal_joystick_update()` already no-ops; cursor sits at
  center (32,32). Screen still adjusts the stored level/invert.
- **Divide-by-zero**: divisor clamped to >= 1 (level <= 10).
- **Level clamping**: setter clamps to 1..10; out-of-range NVS values clamped on load.
- **Button leakage**: the screen ignores `s_btn_left`; nothing writes the key matrix.
- **NVS read miss**: `Preferences.getX()` default returns the config.h-equivalent
  default level (7) and invert (false).

## Testing

No automated tests in this project. Manual verification:
1. Build with the documented `arduino-cli` command; confirm it compiles.
2. Open Settings → Mouse Sensitivity; confirm the cursor tracks the mouse.
3. Left/Right changes the bar/level and visibly changes cursor speed.
4. Up/Down flips Invert Y; vertical mouse motion reverses.
5. ESC, reboot, reopen — value persisted.
6. In a game/BASIC `JOYSTK`, confirm the chosen sensitivity is in effect.

## Files Touched

- `config.h` — keep macros as the first-boot defaults (no behavior change).
- `src/hal/hal.h`, `src/hal/hal_joystick.cpp` — runtime vars + API.
- `src/supervisor/sv_joystick.cpp` / `.h` — new screen (logic).
- `src/supervisor/sv_render.cpp` / `.h` — `sv_render_joystick_pad()`.
- `src/supervisor/supervisor.cpp` / `supervisor.h` — state, dispatch, NVS, tick.
- `src/supervisor/sv_menu.cpp` — Settings row.
- `docs/` — update `joystick-hal.md` and `supervisor.md` notes.
