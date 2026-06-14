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

// Repaint bookkeeping. The static frame/box/bar is drawn only when s_full_redraw
// is set (on open and on level/invert changes); between frames the cursor moves
// incrementally. s_drawn_x/y track the last cursor position painted; 0xFF means
// "not drawn yet" so the first paint skips the erase step.
static bool    s_full_redraw = true;
static uint8_t s_drawn_x = 0xFF;
static uint8_t s_drawn_y = 0xFF;

void sv_joystick_open(Supervisor_t* sv) {
    s_level  = hal_joystick_get_sensitivity();
    s_invert = hal_joystick_get_invert_y();
    s_full_redraw = true;
    s_drawn_x = 0xFF;
    s_drawn_y = 0xFF;
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
                s_full_redraw = true;   // bar/level changed
                sv->needs_redraw = true;
            }
            break;

        case HID_LEFT:
            if (s_level > 1) {
                s_level--;
                hal_joystick_set_sensitivity(s_level);
                s_full_redraw = true;   // bar/level changed
                sv->needs_redraw = true;
            }
            break;

        case HID_UP:
        case HID_DOWN:
            s_invert = !s_invert;
            hal_joystick_set_invert_y(s_invert);
            s_full_redraw = true;       // Invert-Y value changed
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
    // being integrated elsewhere — do it here. Only request a redraw when the
    // cursor actually moved (or a full repaint is pending); a still mouse keeps
    // the screen idle so there is nothing to flicker.
    hal_joystick_update();
    uint8_t x = 32, y = 32;
    hal_joystick_get_pos(&x, &y);
    if (s_full_redraw || x != s_drawn_x || y != s_drawn_y) {
        sv->needs_redraw = true;
    }
}

void sv_joystick_render(Supervisor_t* sv) {
    (void)sv;
    uint8_t x = 32, y = 32;
    hal_joystick_get_pos(&x, &y);

    if (s_full_redraw) {
        // Static parts (frame, box, bar, labels) + cursor — drawn once.
        sv_render_frame("Mouse Sensitivity",
                        "< > Sens   ^v Invert   ESC Save");
        sv_render_joystick_pad(x, y, s_level, s_invert);
        s_full_redraw = false;
    } else {
        // Cheap incremental cursor move — no flicker.
        sv_render_joystick_cursor(s_drawn_x, s_drawn_y, x, y);
    }
    s_drawn_x = x;
    s_drawn_y = y;
}
