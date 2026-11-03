// Host test of src/core/sound.cpp: the XRoar and MAME mixer models.
#include <cstdio>
#include "core/sound.h"

uint8_t g_test_level;
static int failures = 0;
#define CHECK_EQ(actual, expected) do { \
    long a_ = (long)(actual), e_ = (long)(expected); \
    if (a_ != e_) { printf("FAIL %s:%d: %s = %ld, want %ld\n", __FILE__, __LINE__, #actual, a_, e_); failures++; } \
} while (0)

static void start(uint8_t mixer) {
    sound_set_external(0, 0);
    sound_set_mixer(mixer);
    sound_reset();
}

static void test_xroar_is_unchanged() {
    start(SOUND_MIXER_XROAR);
    CHECK_EQ(g_test_level, 0);                  // silence is 0 V
    sound_set_mux_source(SOURCE_DAC);
    sound_set_mux_enabled(true);
    sound_set_dac_level(63);
    CHECK_EQ(g_test_level, 255);                // 244 + 11
    sound_set_mux_enabled(false);
    CHECK_EQ(g_test_level, 0);                  // single-bit path, SBS input
    sound_set_sbs(true, true);
    CHECK_EQ(g_test_level, 212);
    sound_set_mux_enabled(true);
    sound_set_mux_source(SOURCE_TAPE);
    CHECK_EQ(g_test_level, 128);                // tape offset with SBS high
}

static void test_mame_dac_is_dac_times_four() {
    start(SOUND_MIXER_MAME);
    sound_set_mux_source(SOURCE_DAC);
    sound_set_mux_enabled(true);
    sound_set_dac_level(40);
    CHECK_EQ(g_test_level, 160);
    sound_set_dac_level(63);
    CHECK_EQ(g_test_level, 252);
}

static void test_mame_snden_off_holds_the_level() {
    start(SOUND_MIXER_MAME);
    sound_set_mux_source(SOURCE_DAC);
    sound_set_mux_enabled(true);
    sound_set_dac_level(40);
    sound_set_mux_enabled(false);
    CHECK_EQ(g_test_level, 160);
    sound_set_dac_level(10);                    // a JOYSTK ramp with sound off
    CHECK_EQ(g_test_level, 160);
    sound_set_mux_source(SOURCE_CART);
    CHECK_EQ(g_test_level, 160);
    sound_set_mux_enabled(true);
    CHECK_EQ(g_test_level, 0x7E);               // silent channel: mid level
}

static void test_mame_silent_channels_sit_mid() {
    start(SOUND_MIXER_MAME);
    sound_set_mux_enabled(true);
    for (uint8_t src = SOURCE_TAPE; src <= SOURCE_NONE; src++) {
        sound_set_mux_source(src);
        CHECK_EQ(g_test_level, 0x7E);
    }
}

static void test_mame_single_bit_adds_and_clips() {
    start(SOUND_MIXER_MAME);
    sound_set_mux_source(SOURCE_DAC);
    sound_set_mux_enabled(true);
    sound_set_dac_level(0);
    sound_set_sbs(true, true);
    CHECK_EQ(g_test_level, 252);
    sound_set_sbs(true, false);
    CHECK_EQ(g_test_level, 0);
    sound_set_dac_level(32);
    sound_set_sbs(true, true);
    CHECK_EQ(g_test_level, 255);                // 128 + 252, clipped
    sound_set_sbs(false, true);                 // PB1 as input: no single bit
    CHECK_EQ(g_test_level, 128);
}

static void test_mame_power_on_is_mid() {
    start(SOUND_MIXER_MAME);
    CHECK_EQ(g_test_level, 0x7E);
}

static void test_cartridge_adds_in_both() {
    start(SOUND_MIXER_MAME);
    sound_set_external(40, 60);
    CHECK_EQ(g_test_level, 0x7E + 50);
    start(SOUND_MIXER_XROAR);
    sound_set_external(40, 60);
    CHECK_EQ(g_test_level, 50);
    sound_set_external(0, 0);
}

static void test_switching_mixer_applies_at_once() {
    start(SOUND_MIXER_XROAR);
    sound_set_mixer(SOUND_MIXER_MAME);
    CHECK_EQ(sound_get_mixer(), SOUND_MIXER_MAME);
    CHECK_EQ(g_test_level, 0x7E);
    sound_set_mixer(SOUND_MIXER_XROAR);
    CHECK_EQ(g_test_level, 0);
    sound_set_mixer(7);                         // out of range: ignored
    CHECK_EQ(sound_get_mixer(), SOUND_MIXER_XROAR);
}

int main() {
    CHECK_EQ(sound_get_mixer(), SOUND_MIXER_MAME);      // the default, chosen by ear
    test_xroar_is_unchanged();
    test_mame_dac_is_dac_times_four();
    test_mame_snden_off_holds_the_level();
    test_mame_silent_channels_sit_mid();
    test_mame_single_bit_adds_and_clips();
    test_mame_power_on_is_mid();
    test_cartridge_adds_in_both();
    test_switching_mixer_applies_at_once();
    if (failures) { printf("sound_test: %d failed\n", failures); return 1; }
    printf("sound_test: all passed\n");
    return 0;
}
