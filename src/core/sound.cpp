/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : sound.cpp
 *  Module : Sound mixing core — XRoar-style mux/gain/offset model
 * ============================================================
*/

#include "sound.h"
#include "../hal/hal.h"

// XRoar's voltage-calibrated tables (sound.c, measured on real hardware,
// full scale 4.7 V) rescaled to the 0-255 DAC output domain (255/4.7 per V).
// Column index (sindex): 0 = SBS pin is input, 1 = SBS output low,
// 2 = SBS output high. The single-bit sound doesn't sum into the output —
// it changes the gain and DC offset of whichever source the mux selects.
static const uint8_t source_gain[NUM_SOURCES][3] = {
    { 244, 154, 184 },   // SOURCE_DAC        (4.5, 2.84, 3.4 V)
    {  27,  22,  27 },   // SOURCE_TAPE       (0.5, 0.4,  0.5 V)
    {   0,   0,   0 },   // SOURCE_CART
    {   0,   0,   0 },   // SOURCE_NONE
    {   0,   0,   0 },   // SOURCE_SINGLE_BIT
};
static const uint8_t source_offset[NUM_SOURCES][3] = {
    {  11,  10,  71 },   // SOURCE_DAC        (0.2, 0.18, 1.3 V)
    { 111,  87, 128 },   // SOURCE_TAPE       (2.05, 1.6, 2.35 V)
    {   0,   0, 212 },   // SOURCE_CART       (0, 0, 3.9 V)
    {   0,   0,   1 },   // SOURCE_NONE       (0, 0, 0.01 V)
    {   0,   0, 212 },   // SOURCE_SINGLE_BIT (0, 0, 3.9 V)
};

static uint8_t dac_level    = 0;      // 0-63
static bool    sbs_enabled  = false;
static bool    sbs_level    = false;
static bool    mux_enabled  = false;
static uint8_t mux_source   = SOURCE_DAC;
static uint8_t ext_level    = 0;      // 0-255, mono mix of the cartridge DACs
static uint8_t ssc_out      = 0;      // SSC_BIAS + level/2 once the SSC is on

// Mid-point of the half-scale SSC signal (-63..63 around it).
#define SSC_BIAS 64

// SOUND_MIXER_MAME (MAME coco12.cpp coco_sound(), mc14529.cpp; the
// mame-coco3-ttgovga port's Coco3::soundLevel()): the DAC reaches the speaker
// through mux channel 0, and channels 1-3 are silent here, which is the idle
// level and not 0, or each change of channel would be a step of half the range.
// SNDEN low inhibits the selector, which then holds its last value. The single
// bit is wired to the speaker directly and clips.
#define MAME_IDLE       0x7E
#define MAME_SINGLE_BIT 0xFC

static uint8_t mixer    = SOUND_MIXER_MAME;   // the default, chosen by ear (2026-11-02)
static uint8_t mame_mux = MAME_IDLE;      // the selector's output, held while SNDEN is low

static unsigned xroar_level(void) {
    unsigned sindex = sbs_enabled ? (sbs_level ? 2u : 1u) : 0u;
    unsigned source = mux_enabled ? mux_source : (unsigned)SOURCE_SINGLE_BIT;
    // TAPE and CART levels are not emulated — treated as 0.
    unsigned level  = (source == SOURCE_DAC) ? dac_level : 0u;
    return (level * source_gain[source][sindex]) / 63u
         + source_offset[source][sindex];
}

static unsigned mame_level(void) {
    if (mux_enabled)
        mame_mux = (mux_source == SOURCE_DAC) ? (uint8_t)(dac_level << 2) : (uint8_t)MAME_IDLE;
    unsigned out = mame_mux + ((sbs_enabled && sbs_level) ? MAME_SINGLE_BIT : 0u);
    return out > 255u ? 255u : out;
}

static void sound_update(void) {
    unsigned out = (mixer == SOUND_MIXER_MAME) ? mame_level() : xroar_level();
    // The cartridge level sums into the output; one DAC, so saturate.
    out += ext_level;
    out += ssc_out;
    if (out > 255u) out = 255u;
    hal_audio_set_level((uint8_t)out);
}

void sound_reset(void) {
    dac_level   = 0;
    sbs_enabled = false;
    sbs_level   = false;
    mux_enabled = false;
    mux_source  = SOURCE_DAC;
    mame_mux    = MAME_IDLE;
    sound_update();
}

void sound_set_mixer(uint8_t m) {
    if (m >= SOUND_MIXER_COUNT || m == mixer) return;
    mixer = m;
    sound_update();
}

uint8_t sound_get_mixer(void) {
    return mixer;
}

void sound_set_dac_level(uint8_t dac6) {
    dac_level = dac6 & 0x3F;
    // Skip the update when inaudible — JOYSTK() ADC loops write ~32 DAC
    // values per read with the mux switched away from the DAC.
    if (mux_enabled && mux_source == SOURCE_DAC)
        sound_update();
}

void sound_set_sbs(bool enabled, bool level) {
    if (sbs_enabled == enabled && sbs_level == level) return;
    sbs_enabled = enabled;
    sbs_level   = level;
    sound_update();
}

void sound_set_mux_enabled(bool enabled) {
    if (mux_enabled == enabled) return;
    mux_enabled = enabled;
    sound_update();
}

void sound_set_mux_source(uint8_t source) {
    source &= 0x03;
    if (mux_source == source) return;
    mux_source = source;
    if (mux_enabled)
        sound_update();
}

void sound_set_external(uint8_t left, uint8_t right) {
    uint8_t level = (uint8_t)(((unsigned)left + right) >> 1);
    if (ext_level == level) return;
    ext_level = level;
    sound_update();
}

void sound_set_ssc(int8_t level) {
    uint8_t v = (uint8_t)(SSC_BIAS + (level >> 1));
    if (ssc_out == v) return;
    ssc_out = v;
    sound_update();
}
