# Mouse Sensitivity Tool Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an interactive Supervisor → Settings → "Mouse Sensitivity" screen with a live mouse-driven cursor, arrow-key sensitivity/invert adjustment, applied live and persisted to NVS.

**Architecture:** Convert the compile-time `JOYSTICK_MOUSE_SCALE` / `JOYSTICK_MOUSE_INVERT_Y` into runtime HAL variables with a small API. Add a new full-screen supervisor state (`SV_JOY_SENSE`) following the Key Mapper pattern (logic in a new `sv_joystick.cpp`, drawing helper in `sv_render.cpp`). Persist via `Preferences` (NVS namespace `"sv"`), loaded at boot.

**Tech Stack:** Arduino C++ (ESP32, esp32 core 2.0.x), FabGL, `Preferences` (NVS). No unit-test framework — verification is `arduino-cli compile` after every task plus manual hardware testing.

**Build / verify command (run from `coco3/`):**
```bash
arduino-cli compile --fqbn esp32:esp32:esp32wrover:PartitionScheme=huge_app TTGO-VGA32-COCO/
```
Expected: `Used ... bytes` summary, exit code 0. This is the gate after every code task.

**Note:** This is not a git repository, so the "Commit" steps below are written as `git` commands for completeness but may be skipped; the real per-task gate is a clean compile.

---

## File Structure

| File | Responsibility | Change |
|------|----------------|--------|
| `src/hal/hal_joystick.cpp` | Runtime sensitivity/invert vars + level↔divisor mapping + live-pos getter | Modify |
| `src/hal/hal.h` | Declare new joystick API | Modify |
| `src/supervisor/supervisor.h` | `SV_JOY_SENSE` state; NVS proto decls | Modify |
| `src/supervisor/supervisor.cpp` | State dispatch, tick wiring, NVS save/load | Modify |
| `src/supervisor/sv_joystick.h` | Screen public API | Create |
| `src/supervisor/sv_joystick.cpp` | Screen state, key handling, mouse polling | Create |
| `src/supervisor/sv_render.h` | Declare `sv_render_joystick_pad()` | Modify |
| `src/supervisor/sv_render.cpp` | Draw the pad box, cursor, sensitivity bar | Modify |
| `src/supervisor/sv_menu.cpp` | New "Mouse Sensitivity" Settings row | Modify |
| `TTGO-VGA32-COCO.ino` | Call `supervisor_load_joystick()` at boot | Modify |

---

## Task 1: Runtime sensitivity/invert in the joystick HAL

**Files:**
- Modify: `src/hal/hal_joystick.cpp`
- Modify: `src/hal/hal.h:189-210`

- [ ] **Step 1: Add the runtime variables and live-position state in `hal_joystick.cpp`**

Replace the three `static` declarations near the top of `hal_joystick.cpp`:

```cpp
static int16_t s_pos_x    = 32;   // 6-bit logical position, range 0..63
static int16_t s_pos_y    = 32;
static bool    s_btn_left = false;
```

with:

```cpp
static int16_t s_pos_x    = 32;   // 6-bit logical position, range 0..63
static int16_t s_pos_y    = 32;
static bool    s_btn_left = false;

// Runtime-adjustable mouse tunables (Supervisor -> Settings -> Mouse Sensitivity).
// Seeded from the config.h compile-time defaults; overridden at boot from NVS.
//   s_scale  : raw mouse counts are divided by this. Lower = more sensitive.
//   s_invert : true flips the Y axis.
static int  s_scale  = JOYSTICK_MOUSE_SCALE;
static bool s_invert = (JOYSTICK_MOUSE_INVERT_Y != 0);
```

- [ ] **Step 2: Use the runtime vars inside `hal_joystick_update()`**

In `hal_joystick_update()`, change the divisor and the invert branch. Replace:

```cpp
        int dx = (int)d.deltaX / JOYSTICK_MOUSE_SCALE;
        int dy = (int)d.deltaY / JOYSTICK_MOUSE_SCALE;

        s_pos_x += dx;
#if JOYSTICK_MOUSE_INVERT_Y
        s_pos_y += dy;
#else
        s_pos_y -= dy;   // FabGL Y is positive-up; CoCo Y is positive-down
#endif
```

with:

```cpp
        int divisor = (s_scale < 1) ? 1 : s_scale;
        int dx = (int)d.deltaX / divisor;
        int dy = (int)d.deltaY / divisor;

        s_pos_x += dx;
        if (s_invert) {
            s_pos_y += dy;
        } else {
            s_pos_y -= dy;   // FabGL Y is positive-up; CoCo Y is positive-down
        }
```

- [ ] **Step 3: Add the new API functions at the end of `hal_joystick.cpp`**

Append after `hal_joystick_compare()`:

```cpp
// ---- Runtime tunables API (Supervisor Mouse Sensitivity screen) ----
//
// Level is the user-facing 1..10 value; divisor is JOYSTICK_MOUSE_SCALE.
// Higher level = lower divisor = more sensitive:  divisor = 11 - level.

void hal_joystick_set_sensitivity(uint8_t level) {
    if (level < 1)  level = 1;
    if (level > 10) level = 10;
    s_scale = 11 - (int)level;          // level 10 -> 1, level 1 -> 10
}

uint8_t hal_joystick_get_sensitivity(void) {
    int level = 11 - s_scale;
    if (level < 1)  level = 1;
    if (level > 10) level = 10;
    return (uint8_t)level;
}

void hal_joystick_set_invert_y(bool on) {
    s_invert = on;
}

bool hal_joystick_get_invert_y(void) {
    return s_invert;
}

void hal_joystick_get_pos(uint8_t* x, uint8_t* y) {
    if (x) *x = (uint8_t)s_pos_x;
    if (y) *y = (uint8_t)s_pos_y;
}
```

- [ ] **Step 4: Declare the new API in `hal.h`**

After the existing `void hal_joystick_update(void);` line (around `hal.h:210`), add:

```cpp
// ---- Runtime mouse tunables (Supervisor Mouse Sensitivity screen) ----
// Sensitivity level is 1..10 (higher = more sensitive). Values are clamped.
void    hal_joystick_set_sensitivity(uint8_t level);
uint8_t hal_joystick_get_sensitivity(void);
void    hal_joystick_set_invert_y(bool on);
bool    hal_joystick_get_invert_y(void);
// Current live logical position, each axis 0..63 (center 32).
void    hal_joystick_get_pos(uint8_t* x, uint8_t* y);
```

- [ ] **Step 5: Compile**

Run the build command. Expected: exit code 0. (Nothing calls the new API yet, but it must compile.)

- [ ] **Step 6: Commit**

```bash
git add src/hal/hal_joystick.cpp src/hal/hal.h
git commit -m "feat(hal): runtime mouse sensitivity/invert API for joystick"
```

---

## Task 2: NVS persistence + boot load

**Files:**
- Modify: `src/supervisor/supervisor.h`
- Modify: `src/supervisor/supervisor.cpp` (add near the other NVS helpers ~line 530)
- Modify: `TTGO-VGA32-COCO.ino:85`

- [ ] **Step 1: Declare the persistence functions in `supervisor.h`**

Find the block of `supervisor_save_*` / `supervisor_load_*` declarations in `supervisor.h` (near `supervisor_save_kbd_layout`). Add:

```cpp
// Mouse sensitivity (level 1..10) + invert-Y, persisted in NVS ("sv").
void supervisor_save_joystick(uint8_t level, bool invert);
void supervisor_load_joystick(void);   // reads NVS, applies to HAL
```

- [ ] **Step 2: Implement them in `supervisor.cpp`**

Add after `supervisor_save_serial_mode()` (around `supervisor.cpp:536`):

```cpp
void supervisor_save_joystick(uint8_t level, bool invert) {
    Preferences prefs;
    prefs.begin("sv", false);
    prefs.putUChar("joyLevel", level);
    prefs.putBool("joyInv", invert);
    prefs.end();
}

void supervisor_load_joystick(void) {
    Preferences prefs;
    prefs.begin("sv", true);
    // Default level 7 == config.h divisor 4 (11 - 7 == 4).
    uint8_t level  = prefs.getUChar("joyLevel", 7);
    bool    invert = prefs.getBool("joyInv", false);
    prefs.end();
    hal_joystick_set_sensitivity(level);   // clamps internally
    hal_joystick_set_invert_y(invert);
}
```

- [ ] **Step 3: Call the loader at boot in `TTGO-VGA32-COCO.ino`**

After `supervisor_load_keymap();` (`.ino:85`), add:

```cpp
    // Apply the saved mouse sensitivity / invert before the joystick HAL is used.
    supervisor_load_joystick();
```

(`hal.h` is already included transitively via supervisor headers; `hal_joystick_set_*` are declared there.)

- [ ] **Step 4: Compile**

Run the build command. Expected: exit code 0.

- [ ] **Step 5: Commit**

```bash
git add src/supervisor/supervisor.h src/supervisor/supervisor.cpp TTGO-VGA32-COCO.ino
git commit -m "feat(supervisor): persist mouse sensitivity/invert in NVS"
```

---

## Task 3: Pad render helper in `sv_render`

**Files:**
- Modify: `src/supervisor/sv_render.h`
- Modify: `src/supervisor/sv_render.cpp`

- [ ] **Step 1: Declare the helper in `sv_render.h`**

After `void sv_render_centered_item(int index, const char* text, uint16_t color);`, add:

```cpp
// Mouse Sensitivity screen: draws the pad box with a live cursor at
// (cursor_x, cursor_y) in 0..63 logical space, plus the sensitivity bar
// (level 1..10) and the Invert-Y value. Caller renders the frame first.
void sv_render_joystick_pad(uint8_t cursor_x, uint8_t cursor_y,
                            uint8_t level, bool invert_y);
```

- [ ] **Step 2: Implement the helper in `sv_render.cpp`**

Append at the end of `sv_render.cpp`. The geometry uses the existing `SV_*` constants and `TC_DATUM`/`TL_DATUM`/`TR_DATUM` already used in this file.

```cpp
void sv_render_joystick_pad(uint8_t cursor_x, uint8_t cursor_y,
                            uint8_t level, bool invert_y) {
    if (!g_tft) return;

    // --- Pad box: centered square below the title bar ---
    const int PAD = 96;                                   // box side, pixels
    int box_x = SV_BORDER_X + (SV_BORDER_W - PAD) / 2;
    int box_y = SV_BORDER_Y + SV_TITLE_H + 10;

    g_tft->startWrite();

    // Clear the pad area + erase last cursor (whole inner box repainted each tick)
    g_tft->fillRect(box_x, box_y, PAD, PAD, SV_COLOR_BG);
    g_tft->drawRect(box_x, box_y, PAD, PAD, SV_COLOR_BORDER);

    // Cursor: map 0..63 into the inner box, draw a small filled square.
    const int CUR = 6;
    int inner = PAD - 2 - CUR;                            // travel range in px
    int cx = box_x + 1 + (cursor_x * inner) / 63;
    int cy = box_y + 1 + (cursor_y * inner) / 63;
    g_tft->fillRect(cx, cy, CUR, CUR, SV_COLOR_TEXT);

    // --- Sensitivity bar + numeric level ---
    int row_y = box_y + PAD + 8;
    g_tft->setTextFont(2);
    g_tft->setTextColor(SV_COLOR_TEXT, SV_COLOR_BG);
    g_tft->setTextDatum(TL_DATUM);
    g_tft->drawString("Sensitivity:", SV_CONTENT_X, row_y);

    // Ten segment bar, filled up to `level`.
    const int SEG_W = 10, SEG_H = 10, GAP = 2;
    int bar_x = SV_CONTENT_X + 96;
    for (int i = 0; i < 10; i++) {
        int x = bar_x + i * (SEG_W + GAP);
        uint16_t c = (i < level) ? SV_COLOR_BORDER : SV_COLOR_BG;
        g_tft->fillRect(x, row_y + 2, SEG_W, SEG_H, c);
        g_tft->drawRect(x, row_y + 2, SEG_W, SEG_H, SV_COLOR_DIM);
    }

    char lvl[8];
    snprintf(lvl, sizeof(lvl), "%u", (unsigned)level);
    g_tft->setTextDatum(TR_DATUM);
    g_tft->drawString(lvl, SV_VALUE_RIGHT, row_y);

    // --- Invert-Y value ---
    int inv_y = row_y + SV_ITEM_H + 2;
    g_tft->setTextDatum(TL_DATUM);
    g_tft->drawString("Invert Y:", SV_CONTENT_X, inv_y);
    g_tft->setTextDatum(TR_DATUM);
    g_tft->drawString(invert_y ? "ON" : "OFF", SV_VALUE_RIGHT, inv_y);

    g_tft->setTextDatum(TL_DATUM);
    g_tft->endWrite();
}
```

- [ ] **Step 3: Compile**

Run the build command. Expected: exit code 0. (Still unused — must compile.)

- [ ] **Step 4: Commit**

```bash
git add src/supervisor/sv_render.h src/supervisor/sv_render.cpp
git commit -m "feat(supervisor): joystick pad render helper"
```

---

## Task 4: The Mouse Sensitivity screen (`sv_joystick`)

**Files:**
- Modify: `src/supervisor/supervisor.h:31-45` (enum — added here so this task compiles standalone)
- Create: `src/supervisor/sv_joystick.h`
- Create: `src/supervisor/sv_joystick.cpp`

- [ ] **Step 0: Add the `SV_JOY_SENSE` enum value in `supervisor.h`**

In `enum SV_State`, after `SV_KEYMAP_TEST,` add:

```cpp
    SV_JOY_SENSE,        // Mouse Sensitivity adjust screen
```

This is the only piece of state the new translation unit needs to compile; the
dispatch that uses it is wired in Task 5.

- [ ] **Step 1: Create `sv_joystick.h`**

```cpp
/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 * ============================================================
 *  File   : sv_joystick.h
 *  Module : Supervisor "Mouse Sensitivity" screen — live cursor + adjust
 * ============================================================
 */
#ifndef SV_JOYSTICK_H
#define SV_JOYSTICK_H

#include <stdint.h>

typedef struct Supervisor_t Supervisor_t;

// Open the screen (loads current HAL values into the working copy, sets state).
void sv_joystick_open(Supervisor_t* sv);

// HID key handler (Left/Right sensitivity, Up/Down invert, ESC save+exit).
void sv_joystick_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);

// Per-frame: poll the mouse and force a redraw so the cursor animates.
void sv_joystick_tick(Supervisor_t* sv);

// Render the screen.
void sv_joystick_render(Supervisor_t* sv);

#endif // SV_JOYSTICK_H
```

- [ ] **Step 2: Create `sv_joystick.cpp`**

```cpp
/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 * ============================================================
 *  File   : sv_joystick.cpp
 *  Module : Supervisor "Mouse Sensitivity" screen
 * ============================================================
 *
 * Live mouse-driven pad. Left/Right change sensitivity (level 1..10),
 * Up/Down toggle Invert-Y. Both are applied to the HAL immediately for a
 * live feel; ESC persists them to NVS and returns to Settings.
 */
#include "sv_joystick.h"
#include "supervisor.h"
#include "sv_render.h"
#include "../hal/hal.h"

#define HID_UP    0x52
#define HID_DOWN  0x51
#define HID_LEFT  0x50
#define HID_RIGHT 0x4F
#define HID_ENTER 0x28
#define HID_ESC   0x29
#define HID_F1    0x3A

// Working copy mirrors the HAL while the screen is open.
static uint8_t s_level  = 7;
static bool    s_invert = false;

void sv_joystick_open(Supervisor_t* sv) {
    s_level  = hal_joystick_get_sensitivity();
    s_invert = hal_joystick_get_invert_y();
    sv->state = SV_JOY_SENSE;
    sv->needs_redraw = true;
}

void sv_joystick_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;

    switch (hid_usage) {
        case HID_RIGHT:
            if (s_level < 10) {
                s_level++;
                hal_joystick_set_sensitivity(s_level);
                sv->needs_redraw = true;
            }
            break;

        case HID_LEFT:
            if (s_level > 1) {
                s_level--;
                hal_joystick_set_sensitivity(s_level);
                sv->needs_redraw = true;
            }
            break;

        case HID_UP:
        case HID_DOWN:
            s_invert = !s_invert;
            hal_joystick_set_invert_y(s_invert);
            sv->needs_redraw = true;
            break;

        case HID_ESC:
            supervisor_save_joystick(s_level, s_invert);
            sv->state = SV_SETTINGS;
            sv->menu_cursor = 0;       // Settings re-entered at top
            sv->needs_redraw = true;
            break;

        case HID_F1:
            supervisor_save_joystick(s_level, s_invert);
            supervisor_toggle();
            break;
    }
}

void sv_joystick_tick(Supervisor_t* sv) {
    // Emulation is paused while the supervisor is active, so the mouse is not
    // being integrated elsewhere — do it here, then force a repaint so the
    // cursor tracks the mouse at the supervisor's ~60fps cap.
    hal_joystick_update();
    sv->needs_redraw = true;
}

void sv_joystick_render(Supervisor_t* sv) {
    (void)sv;
    sv_render_frame("Mouse Sensitivity",
                    "< > Sens   ^v Invert   ESC Save");
    uint8_t x = 32, y = 32;
    hal_joystick_get_pos(&x, &y);
    sv_render_joystick_pad(x, y, s_level, s_invert);
}
```

- [ ] **Step 3: Add the new source to the build sanity check**

Arduino builds every `.cpp` under the sketch automatically, so no build-file edit is needed. Confirm the file path is `TTGO-VGA32-COCO/src/supervisor/sv_joystick.cpp`.

- [ ] **Step 4: Compile**

Run the build command. Expected: exit code 0. `SV_JOY_SENSE` exists (Step 0) and the screen uses only existing supervisor functions, so this compiles standalone. The dispatch that *calls* these functions is added in Task 5.

- [ ] **Step 5: Commit**

```bash
git add src/supervisor/supervisor.h src/supervisor/sv_joystick.h src/supervisor/sv_joystick.cpp
git commit -m "feat(supervisor): Mouse Sensitivity screen logic"
```

---

## Task 5: Wire the state into the supervisor

**Files:**
- Modify: `src/supervisor/supervisor.cpp` (include, on_key dispatch ~line 345, render dispatch ~line 380, tick)

(The `SV_JOY_SENSE` enum value was already added in Task 4 Step 0.)

- [ ] **Step 1: Include the screen header in `supervisor.cpp`**

Near the other supervisor includes at the top of `supervisor.cpp`, add:

```cpp
#include "sv_joystick.h"
```

- [ ] **Step 2: Add key dispatch in `supervisor_on_key()`**

In the `switch (sv.state)` inside `supervisor_on_key()` (before `default:` ~line 352), add:

```cpp
        case SV_JOY_SENSE:
            sv_joystick_on_key(&sv, hid_usage, pressed);
            break;
```

- [ ] **Step 3: Add the tick + render dispatch in `supervisor_update_and_render()`**

`supervisor_update_and_render()` early-returns when `!needs_redraw`. The Mouse
Sensitivity screen must override that idle so the cursor animates. At the very
start of the function body (right after `if (sv.state == SV_INACTIVE) return false;`),
add the tick:

```cpp
    if (sv.state == SV_JOY_SENSE) {
        sv_joystick_tick(&sv);   // polls mouse, sets needs_redraw
    }
```

Then add the render case in the `switch (sv.state)` (alongside the other cases ~line 380):

```cpp
        case SV_JOY_SENSE:
            sv_joystick_render(&sv);
            break;
```

- [ ] **Step 4: Compile**

Run the build command. Expected: exit code 0.

- [ ] **Step 5: Commit**

```bash
git add src/supervisor/supervisor.cpp
git commit -m "feat(supervisor): wire SV_JOY_SENSE state dispatch + tick"
```

---

## Task 6: Add the Settings menu row

**Files:**
- Modify: `src/supervisor/sv_menu.cpp:283-363`

- [ ] **Step 1: Include the screen header**

Near the top includes of `sv_menu.cpp`, add (if not already present):

```cpp
#include "sv_joystick.h"
```

- [ ] **Step 2: Bump the count and add the label**

Change:

```cpp
#define SETTINGS_COUNT 4
static const char* const SETTINGS_LABELS[SETTINGS_COUNT] = {
    "Debug Log", "RS-232 Pak", "Keyboard", "Key Mapper"
};
```

to:

```cpp
#define SETTINGS_COUNT 5
static const char* const SETTINGS_LABELS[SETTINGS_COUNT] = {
    "Debug Log", "RS-232 Pak", "Keyboard", "Key Mapper", "Mouse Sensitivity"
};
```

- [ ] **Step 3: Handle the new row in `settings_toggle()`**

At the top of `settings_toggle()`, before the `if (row == 3)` check, add:

```cpp
    if (row == 4) {  // Mouse Sensitivity — opens its own live screen
        sv_joystick_open(sv);
        return;
    }
```

- [ ] **Step 4: Show the current level as the row value in `sv_settings_render()`**

In `sv_settings_render()`, the `values[]` array currently has 4 entries ending in `NULL,` (Key Mapper). Extend it to 5 with the live sensitivity level. Replace:

```cpp
    const char* values[SETTINGS_COUNT] = {
        (g_serial_mode == SERIAL_MODE_DEBUG) ? "ON" : "OFF",
        (g_serial_mode == SERIAL_MODE_RS232) ? "ON" : "OFF",
        hal_keyboard_layout_name(g_kbd_layout),
        NULL,
    };
```

with:

```cpp
    char sens_str[8];
    snprintf(sens_str, sizeof(sens_str), "%u", (unsigned)hal_joystick_get_sensitivity());
    const char* values[SETTINGS_COUNT] = {
        (g_serial_mode == SERIAL_MODE_DEBUG) ? "ON" : "OFF",
        (g_serial_mode == SERIAL_MODE_RS232) ? "ON" : "OFF",
        hal_keyboard_layout_name(g_kbd_layout),
        NULL,            // Key Mapper (opens sub-screen)
        sens_str,        // Mouse Sensitivity (current level 1..10)
    };
```

(`hal.h` is already reachable from `sv_menu.cpp`; if the compiler reports
`hal_joystick_get_sensitivity` undeclared, add `#include "../hal/hal.h"` to the
top includes.)

- [ ] **Step 5: Compile**

Run the build command. Expected: exit code 0.

- [ ] **Step 6: Commit**

```bash
git add src/supervisor/sv_menu.cpp
git commit -m "feat(supervisor): add Mouse Sensitivity row to Settings"
```

---

## Task 7: Docs + manual verification

**Files:**
- Modify: `TTGO-VGA32-COCO/docs/joystick-hal.md`
- Modify: `TTGO-VGA32-COCO/docs/supervisor.md`

- [ ] **Step 1: Document the runtime tunables in `joystick-hal.md`**

Add a short section noting that `JOYSTICK_MOUSE_SCALE` / `JOYSTICK_MOUSE_INVERT_Y`
are now first-boot defaults only, that runtime values live in `hal_joystick.cpp`
(`s_scale` / `s_invert`), the level↔divisor mapping (`divisor = 11 - level`,
default level 7), the new `hal_joystick_set/get_sensitivity`,
`hal_joystick_set/get_invert_y`, `hal_joystick_get_pos` API, and NVS keys
`joyLevel` / `joyInv` in namespace `"sv"`.

- [ ] **Step 2: Document the screen in `supervisor.md`**

Add a "Mouse Sensitivity" entry under the Settings submenu: opened from
Settings → Mouse Sensitivity, live cursor pad, Left/Right = sensitivity 1..10,
Up/Down = Invert-Y, ESC saves to NVS. Mentions `SV_JOY_SENSE` and `sv_joystick.cpp`.

- [ ] **Step 3: Final compile**

Run the build command. Expected: exit code 0.

- [ ] **Step 4: Manual hardware verification (record results)**

1. Upload to the TTGO and open Settings — confirm "Mouse Sensitivity" row shows a number (default `7`).
2. Enter it — confirm the cursor square tracks the PS/2 mouse inside the box.
3. Left/Right — bar grows/shrinks, number changes, cursor speed visibly changes.
4. Up/Down — "Invert Y" flips ON/OFF and vertical motion reverses.
5. ESC back to Settings, then power-cycle and reopen — the chosen value persisted.
6. In BASIC `JOYSTK(0)` / a joystick game — confirm the new sensitivity is in effect.

- [ ] **Step 5: Commit**

```bash
git add TTGO-VGA32-COCO/docs/joystick-hal.md TTGO-VGA32-COCO/docs/supervisor.md
git commit -m "docs: mouse sensitivity tool (HAL + supervisor)"
```

---

## Self-Review Notes

- **Spec coverage:** Settings row (T6), graphic live-cursor screen (T3+T4), Left/Right sensitivity 1..10 (T4), Up/Down invert (T4), runtime HAL tunables (T1), NVS persistence + boot load (T2), state wiring + live-tick animation (T5), docs (T7). All spec sections covered.
- **Type consistency:** `hal_joystick_set_sensitivity(uint8_t)` / `hal_joystick_get_sensitivity()→uint8_t`, `hal_joystick_set_invert_y(bool)`/`get`, `hal_joystick_get_pos(uint8_t*,uint8_t*)`, `sv_render_joystick_pad(uint8_t,uint8_t,uint8_t,bool)`, `supervisor_save_joystick(uint8_t,bool)`, `supervisor_load_joystick(void)`, `SV_JOY_SENSE` — names identical across all tasks.
- **Mapping:** `divisor = 11 - level`; default divisor 4 → level 7; level 10 → divisor 1 (clamped ≥1). Consistent between T1 (HAL) and T2 (NVS default).
- **Ordering:** `SV_JOY_SENSE` is added in Task 4 Step 0 (with the screen logic that needs it); Task 5 only adds the dispatch that calls the screen functions. Each task compiles standalone in sequence.
