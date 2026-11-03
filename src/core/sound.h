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

// How the mux and single-bit sound reach the output (Settings > Machine >
// Sound Mixer, saved in NVS). Both keep the same DAC, cartridge and SSC paths.
//   SOUND_MIXER_XROAR  XRoar's measured gain/offset tables: SNDEN off routes
//                      the single-bit sound alone, so the output drops to 0 V;
//                      a non-DAC mux channel goes to its own offset.
//   SOUND_MIXER_MAME   As MAME and the mame-coco3-ttgovga port: SNDEN off holds
//                      the mux output where it was; silent channels sit at the
//                      mid level; the single-bit sound adds on top and clips.
//                      Switching makes no click. The default: it sounded
//                      better on the board (A/B by ear, 2026-11-02).
enum { SOUND_MIXER_XROAR = 0, SOUND_MIXER_MAME = 1, SOUND_MIXER_COUNT };
void    sound_set_mixer(uint8_t mixer);
uint8_t sound_get_mixer(void);

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

// External (cartridge) stereo level, 0-255 per channel — Orchestra-90.
// Mixed to mono and added to the output whatever the mux selects, as XRoar's
// sound_set_external_left/right. Not cleared by sound_reset().
void sound_set_external(uint8_t left, uint8_t right);

// Speech/Sound Cartridge output, -127..127 (bipolar). The first call turns on
// a fixed bias so the signal fits the unipolar output; it is added at half
// scale, like the external level. Only called while the SSC is enabled.
void sound_set_ssc(int8_t level);

#endif // SOUND_H
