/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : hal_joystick.cpp
 *  Module : Joystick HAL — PS/2 mouse → CoCo joystick 1; port 1 is a neutral stub
 * ============================================================
*/

#include "hal.h"
#include "../utils/debug.h"


// Joystick 1 (port 0) is driven by the PS/2 mouse plugged into the TTGO
// VGA32 mouse header. Joystick 2 (port 1) remains a neutral stub.
//
// Design spec: docs/superpowers/specs/2026-05-21-mouse-joystick-design.md
//
// Position model: accumulate + clamp. The mouse reports relative deltas;
// we integrate them into s_pos_x / s_pos_y in the 0..63 logical range and
// clamp at the edges. The joystick stays where you left it once the mouse
// stops — closest to real CoCo joystick behavior.
//
// Polarity: machine.cpp does `if (hal_joystick_read_button(0)) row_data
// &= ~0x01;` so a non-zero return means "pressed". Initial s_btn_left is
// false so the keyboard matrix stays untouched until the user clicks.

#include "fabgl.h"

static int16_t s_pos_x    = 32;   // 6-bit logical position, range 0..63
static int16_t s_pos_y    = 32;
static bool    s_btn_left = false;

// Runtime-adjustable mouse tunables (Supervisor -> Settings -> Mouse Sensitivity).
// Seeded from the config.h compile-time defaults; overridden at boot from NVS.
//   s_scale  : raw mouse counts are divided by this. Lower = more sensitive.
//   s_invert : true flips the Y axis.
static int  s_scale  = JOYSTICK_MOUSE_SCALE;
static bool s_invert = (JOYSTICK_MOUSE_INVERT_Y != 0);

void hal_joystick_init(void) {
    // The PS2Controller itself is brought up in hal_keyboard_init() via
    // PS2Preset::KeyboardPort0_MousePort1. Here we just verify the Mouse
    // got allocated and log the result.
    fabgl::Mouse* m = fabgl::PS2Controller::mouse();
    if (m) {
        DEBUG_PRINTF("  Joystick: PS/2 mouse on port 1 (CLK=GPIO%d DATA=GPIO%d), scale=%d",
                     PIN_PS2_MOUSE_CLK, PIN_PS2_MOUSE_DATA, JOYSTICK_MOUSE_SCALE);
    } else {
        DEBUG_PRINT("  Joystick: PS/2 mouse not enumerated (yet) — port 0 will read center");
    }
}

void hal_joystick_update(void) {
    fabgl::Mouse* m = fabgl::PS2Controller::mouse();
    if (!m) return;
    while (m->deltaAvailable()) {
        fabgl::MouseDelta d;
        if (!m->getNextDelta(&d, 0)) break;

        int divisor = (s_scale < 1) ? 1 : s_scale;
        int dx = (int)d.deltaX / divisor;
        int dy = (int)d.deltaY / divisor;

        s_pos_x += dx;
        if (s_invert) {
            s_pos_y += dy;
        } else {
            s_pos_y -= dy;   // FabGL Y is positive-up; CoCo Y is positive-down
        }

        if (s_pos_x < 0)  s_pos_x = 0;
        if (s_pos_x > 63) s_pos_x = 63;
        if (s_pos_y < 0)  s_pos_y = 0;
        if (s_pos_y > 63) s_pos_y = 63;

        s_btn_left = (d.buttons.left != 0);
    }
}

uint8_t hal_joystick_read_axis(int port, int axis) {
    if (port != 0) return 32;   // Joystick 2 stub
    return (uint8_t)((axis == 0) ? s_pos_x : s_pos_y);
}

uint8_t hal_joystick_read_button(int port) {
    if (port != 0) return 0;    // Joystick 2 stub
    return s_btn_left ? 1 : 0;
}

bool hal_joystick_compare(int port, int axis, uint8_t dac_value) {
    if (port != 0) return false;
    int v = (axis == 0) ? s_pos_x : s_pos_y;
    // 6-bit (0..63) → 8-bit DAC space (2..254), matches the inline math at
    // the PIA0 PA read site in machine.cpp.
    return (v * 4 + 2) >= (int)dac_value;
}

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
