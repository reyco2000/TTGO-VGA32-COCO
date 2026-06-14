/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : sv_keymap.h
 *  Module : Key Mapper UI — remap physical keys to CoCo characters
 * ============================================================
*/

#ifndef SV_KEYMAP_H
#define SV_KEYMAP_H

#include <stdint.h>
#include "supervisor.h"

// Enter the Key Mapper list screen (from Settings).
void sv_keymap_open(Supervisor_t* sv);

// List-screen key handler (HID usage codes, like the other menus).
void sv_keymap_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);

// Renderers for the three Key Mapper states.
void sv_keymap_render(Supervisor_t* sv);          // SV_KEYMAP_LIST
void sv_keymap_capture_render(Supervisor_t* sv);  // SV_KEYMAP_CAPTURE
void sv_keymap_test_render(Supervisor_t* sv);     // SV_KEYMAP_TEST

// Raw-VirtualKey path for the capture/test screens. The normal supervisor
// key route translates VKs to HID usages and drops most symbol keys, so
// hal_keyboard's process_vk() calls this instead while these screens are up.
bool sv_keymap_wants_raw_vk(void);
void sv_keymap_on_raw_vk(int16_t vk, bool pressed);

#endif // SV_KEYMAP_H
