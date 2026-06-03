/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : hal_audio.cpp
 *  Module : Audio HAL — ESP32 internal DAC1 (GPIO25) via timer ISR
 * ============================================================
*/

#include "hal.h"
#include "../utils/debug.h"

#define AUDIO_ISR_RATE       22050
#define SCANLINES_PER_FRAME  262
#define SAMPLES_PER_FRAME    SCANLINES_PER_FRAME
#define AUDIO_PITCH_TRIM     -6
#define ISR_STRIDE_Q8        (((SAMPLES_PER_FRAME * 256 * 60 + AUDIO_ISR_RATE/2) / AUDIO_ISR_RATE) + AUDIO_PITCH_TRIM)

static uint8_t audio_scanline_buf[2][SAMPLES_PER_FRAME];
static volatile int  audio_write_buf = 0;
static volatile int  audio_read_buf  = 1;
static volatile int  audio_write_pos = 0;
static volatile uint32_t audio_isr_pos_q8 = 0;
static volatile bool audio_buf_ready = false;
static volatile uint8_t audio_current_level = 128;

static hw_timer_t* audio_timer = nullptr;

// ============================================================================
//  ESP32 internal DAC1 (GPIO25)
// ============================================================================

// ESP-IDF 5 still ships the legacy DAC driver (`driver/dac.h`) under a
// deprecation warning. We use it here because it matches the existing
// timer-ISR / sample-by-sample model 1:1 and avoids spinning up the full
// dac_continuous DMA pipeline for a single mono channel.
#include "driver/dac.h"

#define AUDIO_DAC_CHANNEL    DAC_CHANNEL_1   // GPIO25

static void IRAM_ATTR audio_timer_isr() {
    if (audio_buf_ready) {
        audio_read_buf = audio_write_buf;
        audio_write_buf = 1 - audio_write_buf;
        audio_isr_pos_q8 = 0;
        audio_buf_ready = false;
    }
    const uint8_t* buf = audio_scanline_buf[audio_read_buf];
    uint32_t idx = audio_isr_pos_q8 >> 8;
    if (idx >= SAMPLES_PER_FRAME) {
        idx %= SAMPLES_PER_FRAME;
        audio_isr_pos_q8 = idx << 8;
    }
    uint8_t sample = buf[idx];
    audio_isr_pos_q8 += ISR_STRIDE_Q8;
    dac_output_voltage(AUDIO_DAC_CHANNEL, sample);
}

void hal_audio_init(void) {
    dac_output_enable(AUDIO_DAC_CHANNEL);
    dac_output_voltage(AUDIO_DAC_CHANNEL, 128);
    memset(audio_scanline_buf[0], 128, SAMPLES_PER_FRAME);
    memset(audio_scanline_buf[1], 128, SAMPLES_PER_FRAME);
    // ESP32-Arduino 2.x timer API: timerBegin(timer_num, divider, countUp).
    // APB is 80 MHz; divider 80 → 1 MHz timer tick (1 µs per tick).
    audio_timer = timerBegin(0, 80, true);
    timerAttachInterrupt(audio_timer, audio_timer_isr, true);
    timerAlarmWrite(audio_timer, 1000000 / AUDIO_ISR_RATE, true);
    timerAlarmEnable(audio_timer);
    audio_current_level = 128;
    DEBUG_PRINTF("  Audio: DAC1 (GPIO25), %d Hz ISR, stride Q8=%d",
                 AUDIO_ISR_RATE, ISR_STRIDE_Q8);
}

// ============================================================================
//  Backend-independent surface
// ============================================================================

void hal_audio_write_sample(int16_t left, int16_t right) {
    (void)left; (void)right;
}

void hal_audio_set_volume(uint8_t volume) {
    DEBUG_PRINTF("  Audio: volume set to %d", volume);
}

void hal_audio_capture_scanline(void) {
    int pos = audio_write_pos;
    if (pos < SAMPLES_PER_FRAME) {
        audio_scanline_buf[audio_write_buf][pos] = audio_current_level;
        audio_write_pos = pos + 1;
    }
}

void hal_audio_commit_frame(void) {
    uint8_t last = (audio_write_pos > 0)
        ? audio_scanline_buf[audio_write_buf][audio_write_pos - 1]
        : 128;
    while (audio_write_pos < SAMPLES_PER_FRAME) {
        audio_scanline_buf[audio_write_buf][audio_write_pos++] = last;
    }
    audio_buf_ready = true;
    audio_write_pos = 0;
}

void hal_audio_write_bit(bool value) {
    audio_current_level = value ? 255 : 0;
}

void hal_audio_debug_tick(void) {}

void hal_audio_write_dac(uint8_t dac6) {
    audio_current_level = (dac6 << 2) | (dac6 >> 4);
}
