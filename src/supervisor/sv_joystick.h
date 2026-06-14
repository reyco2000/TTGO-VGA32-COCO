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
