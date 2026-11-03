// Host stand-in for src/hal/hal.h: what src/core/sound.cpp uses of it.
#pragma once
#include <stdint.h>
extern uint8_t g_test_level;
static inline void hal_audio_set_level(uint8_t level) { g_test_level = level; }
