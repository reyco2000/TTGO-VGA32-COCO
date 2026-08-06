/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : sound.h
 *  Module : Sound mixing core — XRoar-style mux/gain/offset model
 * ============================================================
*/

#ifndef SOUND_H
#define SOUND_H

#include <Arduino.h>

// Analog mux sources (SEL2<<1 | SEL1 from PIA0 CB2/CA2).
// SOURCE_SINGLE_BIT is virtual: selected when the mux is disabled.
enum {
    SOURCE_DAC = 0,
    SOURCE_TAPE,
    SOURCE_CART,
    SOURCE_NONE,
    SOURCE_SINGLE_BIT,
    NUM_SOURCES
};

// Reset all sound state to power-on defaults and push the idle level.
void sound_reset(void);

// PIA1 PA bits 2-7 (0-63). Updates output only when the DAC is audible.
void sound_set_dac_level(uint8_t dac6);

// PIA1 PB1: enabled = pin configured as output (DDR bit set),
// level = data bit. Affects the gain/offset of every mux source.
void sound_set_sbs(bool enabled, bool level);

// SNDEN — PIA1 CRB bit 3.
void sound_set_mux_enabled(bool enabled);

// Mux source select (0-3) — PIA0 CRA/CRB bit 3 (SEL1/SEL2).
void sound_set_mux_source(uint8_t source);

// ------------------------------------------------------------
// Master volume (0-100%). Scales only the signal component of the
// output, leaving each source's calibrated DC offset untouched —
// applying it to the raw DAC byte would shift the "silence" level
// along with the volume and clip/distort at low settings.
// ------------------------------------------------------------
void    sound_set_volume(uint8_t percent);   // clamps to 0-100
uint8_t sound_get_volume(void);
void    sound_volume_up(void);                // +10%, clamped at 100
void    sound_volume_down(void);              // -10%, clamped at 0

#endif // SOUND_H