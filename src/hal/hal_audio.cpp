/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : hal_audio.cpp
 *  Module : Audio HAL — ESP32 internal DAC1 (GPIO25) via I2S0 DMA
 * ============================================================
*/

// I2S0 drives the built-in DAC by DMA, one sample per emulated scanline. There
// is no interrupt: the DMA loops over its buffers for good and a task on core 0
// polls where it has got to, refilling each buffer once the DMA has fetched it,
// never the one being fetched. If the task is ever late the DMA replays old
// samples; a buffer is fetched again 3 buffers, about 12 ms, after it is done,
// so that is the margin (late_refills).
//
// This file must not include no_psram_memw.h: the ring is shared across cores
// (see audio_feed.h). Nothing here is reached by an interrupt, so it can all
// stay in flash.

#include "hal.h"
#include "audio_capture.h"
#include "audio_feed.h"
#include "../utils/debug.h"

#include "driver/dac.h"
#include "driver/periph_ctrl.h"
#include "rom/lldesc.h"
#include "soc/i2s_struct.h"

// A sample every (N + b/a) * M cycles of the 80 MHz clock, in this I2S mode:
// 262 samples per 16666 us frame, 15720.6 Hz. The pacing timer and this clock
// come off the same crystal, so the 0.8 ppm left over stays what it is.
#define AUDIO_DIV_N  181
#define AUDIO_DIV_B  35
#define AUDIO_DIV_A  47
#define AUDIO_DIV_M  28
static_assert(AUDIO_FRAME_US == 1000000 / TARGET_FPS, "audio rate must follow the frame period");
static_assert(AUDIO_FRAME_SAMPLES == 262, "audio rate is worked out for 262 lines");

#define AUDIO_DMA_BUFS   4
#define AUDIO_DMA_WORDS  64      // each about 4.1 ms
// Only measured with 4 buffers, where 2 ahead and 2 behind are the same buffer:
// see audio_feed.h. Another count needs it measured again.
static_assert(AUDIO_DMA_BUFS == 4, "out_link_dscr lead measured only with 4 buffers");

#define AUDIO_TASK_CORE      0
#define AUDIO_TASK_STACK     2048
#define AUDIO_TASK_PRIORITY  6   // over ssc (3) and dbg_srv / dw_client (1)
#define AUDIO_POLL_TICKS     pdMS_TO_TICKS(2)
// Longer than this between polls, and a buffer may have been fetched again
// before it was refilled. (N + b/a) * M / 80 us per sample.
#define AUDIO_LATE_US \
    ((int64_t)((AUDIO_DMA_BUFS - 1) * AUDIO_DMA_WORDS * \
               (AUDIO_DIV_N + (double)AUDIO_DIV_B / AUDIO_DIV_A) * AUDIO_DIV_M / 80.0))

// In .bss, which is DMA-capable internal RAM.
static AudioFeed audio_feed;
static uint16_t  audio_words[AUDIO_DMA_BUFS][AUDIO_DMA_WORDS] __attribute__((aligned(4)));
static lldesc_t  audio_descs[AUDIO_DMA_BUFS];
static TaskHandle_t audio_task_handle = nullptr;

// The level over the current scanline, and where the line position comes from:
// the CPU's cycle counter, which restarts at each run and so at each line.
static AudioLineAverage     audio_line = {};
static const unsigned long  audio_no_cycles = 0;
static const unsigned long* audio_cycles = &audio_no_cycles;

// Diagnostics (see HalAudioStats in hal.h). The task writes its own; the rest
// are written on core 1. All are read by the debug server on core 0.
static volatile HalAudioStats audio_stats;
static int64_t stat_last_commit_us = 0;

// ============================================================================
//  I2S0 DMA into DAC1 (GPIO25)
// ============================================================================

static void audio_dma_start(void) {
    for (int i = 0; i < AUDIO_DMA_BUFS; i++) {
        lldesc_t& d = audio_descs[i];
        d.size = d.length = AUDIO_DMA_WORDS * sizeof(uint16_t);
        d.buf = reinterpret_cast<uint8_t*>(audio_words[i]);
        d.offset = 0;
        d.sosf = 0;
        d.eof = 0;      // no out_eof: nothing is listening
        d.owner = 1;
        d.qe.stqe_next = &audio_descs[(i + 1) % AUDIO_DMA_BUFS];
    }

    periph_module_enable(PERIPH_I2S0_MODULE);
    I2S0.int_ena.val = 0;
    I2S0.int_clr.val = 0xFFFFFFFF;

    I2S0.conf.tx_reset = 1;
    I2S0.conf.tx_reset = 0;
    I2S0.lc_conf.out_rst = 1;
    I2S0.lc_conf.out_rst = 0;
    I2S0.conf.tx_fifo_reset = 1;
    I2S0.conf.tx_fifo_reset = 0;

    I2S0.conf_chan.tx_chan_mod = 3;             // GPIO25's DAC
    I2S0.fifo_conf.tx_fifo_mod_force_en = 1;
    I2S0.fifo_conf.tx_fifo_mod = 1;             // 16 bits, single channel
    I2S0.fifo_conf.dscr_en = 1;
    I2S0.conf.tx_mono = 1;
    I2S0.conf.tx_start = 0;
    I2S0.conf.tx_msb_right = 1;
    I2S0.conf.tx_right_first = 1;
    I2S0.conf.tx_slave_mod = 0;
    I2S0.conf.tx_short_sync = 0;
    I2S0.conf.tx_msb_shift = 0;
    I2S0.conf2.lcd_en = 1;
    I2S0.conf2.camera_en = 0;

    I2S0.clkm_conf.clka_en = 0;                 // PLL_D2, not the APLL VGA has
    I2S0.clkm_conf.clkm_div_a = AUDIO_DIV_A;
    I2S0.clkm_conf.clkm_div_b = AUDIO_DIV_B;
    I2S0.clkm_conf.clkm_div_num = AUDIO_DIV_N;
    I2S0.sample_rate_conf.tx_bck_div_num = AUDIO_DIV_M;
    I2S0.sample_rate_conf.tx_bits_mod = 16;

    I2S0.out_link.addr = (uint32_t)(uintptr_t)&audio_descs[0];
    I2S0.out_link.start = 1;
    I2S0.conf.tx_start = 1;

    dac_i2s_enable();
    dac_output_enable(DAC_CHANNEL_1);
}

// The descriptor out_link_dscr names, or -1 if it is none of ours.
static int audio_dma_dscr(void) {
    const uint32_t off = I2S0.out_link_dscr - (uint32_t)(uintptr_t)&audio_descs[0];
    const uint32_t i = off / sizeof(lldesc_t);
    return i < AUDIO_DMA_BUFS ? (int)i : -1;
}

static void audio_task(void*) {
    const int first = audio_dma_dscr();
    int next = audio_dma_first(audio_dma_fetching(first < 0 ? 0 : first, AUDIO_DMA_BUFS),
                               AUDIO_DMA_BUFS);
    int64_t last_poll = esp_timer_get_time();
    for (;;) {
        const int dscr = audio_dma_dscr();
        if (dscr >= 0) {
            const int64_t now = esp_timer_get_time();
            const uint32_t gap = (uint32_t)(now - last_poll);
            if (gap > audio_stats.poll_gap_us_max) audio_stats.poll_gap_us_max = gap;
            if ((int64_t)gap > AUDIO_LATE_US) audio_stats.late_refills++;
            last_poll = now;
            next = audio_dma_refill(next, dscr, AUDIO_DMA_BUFS, [](int b) {
                audio_feed_fill_words(&audio_feed, audio_words[b], AUDIO_DMA_WORDS);
                audio_stats.refills++;
            });
        }
        vTaskDelay(AUDIO_POLL_TICKS);
    }
}

void hal_audio_init(void) {
    audio_feed_init(&audio_feed);
    memset(audio_words, 0, sizeof(audio_words));    // idle: the sound core's silence is 0 V
    audio_line = AudioLineAverage{};
    hal_audio_reset_stats();
    audio_dma_start();
    if (xTaskCreatePinnedToCore(audio_task, "audio", AUDIO_TASK_STACK, nullptr,
                                AUDIO_TASK_PRIORITY, &audio_task_handle,
                                AUDIO_TASK_CORE) != pdPASS) {
        DEBUG_PRINT("  Audio: cannot start task, no sound");
        return;
    }
    DEBUG_PRINTF("  Audio: DAC1 (GPIO25), I2S0 DMA, %d Hz",
                 (int)(80000000.0 / ((AUDIO_DIV_N + (double)AUDIO_DIV_B / AUDIO_DIV_A) * AUDIO_DIV_M)));
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
    audio_feed_push(&audio_feed, audio_line_take(&audio_line, (uint32_t)*audio_cycles));
}

// Frame end: the ring is at its fullest here, so this is where its fill is
// judged and the playback rate set (audio_feed_steer).
void hal_audio_commit_frame(void) {
    const int64_t now = esp_timer_get_time();
    uint32_t dt = 0;
    if (stat_last_commit_us != 0) {
        dt = (uint32_t)(now - stat_last_commit_us);
        if (dt < audio_stats.commit_us_min) audio_stats.commit_us_min = dt;
        if (dt > audio_stats.commit_us_max) audio_stats.commit_us_max = dt;
    }
    stat_last_commit_us = now;
    audio_stats.commits++;

    audio_feed_steer(&audio_feed, dt);
    audio_stats.stride_adj = (int32_t)audio_feed.stride - (int32_t)AUDIO_STRIDE_ONE;

    const uint32_t fill = audio_feed_fill(&audio_feed);
    audio_stats.fill = fill;
    if (fill < audio_stats.fill_min) audio_stats.fill_min = fill;
    if (fill > audio_stats.fill_max) audio_stats.fill_max = fill;
}

void hal_audio_get_stats(HalAudioStats* out) {
    out->commits         = audio_stats.commits;
    out->underruns       = audio_feed.underruns - audio_stats.underruns_base;
    out->hold_samples    = audio_feed.hold_samples - audio_stats.hold_samples_base;
    out->overruns        = audio_feed.overruns - audio_stats.overruns_base;
    out->fill            = audio_stats.fill;
    out->fill_min        = audio_stats.fill_min;
    out->fill_max        = audio_stats.fill_max;
    out->stride_adj      = audio_stats.stride_adj;
    out->commit_us_min   = audio_stats.commit_us_min;
    out->commit_us_max   = audio_stats.commit_us_max;
    out->refills         = audio_stats.refills;
    out->late_refills    = audio_stats.late_refills;
    out->poll_gap_us_max = audio_stats.poll_gap_us_max;
    out->stack_free      = audio_task_handle ? uxTaskGetStackHighWaterMark(audio_task_handle) : 0;
}

// Not locked against the task: a counter may be one event off right after.
void hal_audio_reset_stats(void) {
    audio_stats.commits = 0;
    audio_stats.underruns_base = audio_feed.underruns;
    audio_stats.hold_samples_base = audio_feed.hold_samples;
    audio_stats.overruns_base = audio_feed.overruns;
    audio_stats.fill_min        = UINT32_MAX;
    audio_stats.fill_max        = 0;
    audio_stats.commit_us_min   = UINT32_MAX;
    audio_stats.commit_us_max   = 0;
    audio_stats.refills         = 0;
    audio_stats.late_refills    = 0;
    audio_stats.poll_gap_us_max = 0;
}

void hal_audio_set_cycle_source(const unsigned long* cycles) {
    audio_cycles = cycles;
}

void hal_audio_set_level(uint8_t level) {
    audio_line_note(&audio_line, level, (uint32_t)*audio_cycles);
}

void hal_audio_debug_tick(void) {}
