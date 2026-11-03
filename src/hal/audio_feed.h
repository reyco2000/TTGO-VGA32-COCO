/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : audio_feed.h
 *  Module : Ring between the emulation loop and the DMA buffers
 * ============================================================
*/

// The emulator delivers one sample per scanline, a frame's worth at a time, on
// core 1. The DAC takes one sample per emulated line's real duration from DMA
// buffers that a task on core 0 refills. At full speed those two rates are the
// same, so a sample goes out exactly as it came in, with nothing resampled.
//
// The emulator can also run slow (a screen too heavy to hold 60 FPS), and sound
// follows it down. Only then does the feed step through the ring by less than
// one sample per output sample, interpolating between neighbours.
//
// Pure logic with no hardware in it; tools/audio_test/ runs it on the host.
// Each index has one writer. Sharing relies on `volatile` stores keeping their
// order across cores, which needs the compiler's `memw`: a file using this must
// not include no_psram_memw.h.

#ifndef AUDIO_FEED_H
#define AUDIO_FEED_H

#include <stdint.h>

#define AUDIO_RING_SIZE      2048                    // power of two
#define AUDIO_RING_MASK      (AUDIO_RING_SIZE - 1)
#define AUDIO_FILL_LIMIT     (AUDIO_RING_SIZE - 64)  // producer drops samples above this

#define AUDIO_FRAME_SAMPLES  262
#define AUDIO_FRAME_US       16666                   // same integer division as pace_frame()
#define AUDIO_STRIDE_ONE     65536u                  // Q16: one sample out per sample in

// Fill wanted right after a frame, the ring's fullest point: one frame plus 512
// samples (33 ms). That covers a frame taking two frame periods, and the three
// slow frames it takes to see that the emulator has slowed down (39 FPS).
#define AUDIO_FILL_TARGET    (AUDIO_FRAME_SAMPLES + 512)

// Rate control, judged once per frame.
// Frame ends jitter by milliseconds even at full speed (12-26 ms apart on the
// board), so the emulator counts as slow only once a 32-frame average is 2 %
// over, and full speed again under 1 %. Below that the trim keeps up.
#define AUDIO_SLOW_DT_US     (AUDIO_FRAME_US * 102 / 100)
#define AUDIO_FULL_DT_US     (AUDIO_FRAME_US * 101 / 100)
#define AUDIO_LONG_DT_US     (AUDIO_FRAME_US * 5 / 4)       // one frame this long is a stall
#define AUDIO_FILL_BAND      128    // averaged fill error that starts a correction at full speed
#define AUDIO_FILL_SETTLED   32     // and where it ends
#define AUDIO_TRIM_MAX       ((int32_t)(AUDIO_STRIDE_ONE * 3 / 100))    // +-3 %
#define AUDIO_STRIDE_MIN     ((uint32_t)(AUDIO_STRIDE_ONE * 55 / 100))  // 55 % speed
// Frame intervals outside this range are pauses (OSD, debugger, SD) or noise,
// not the emulation speed, and are left out of the average.
#define AUDIO_DT_MIN_US      8000
#define AUDIO_DT_MAX_US      60000
#define AUDIO_DT_AVG_SHIFT   5      // 32-frame average
#define AUDIO_ERR_AVG_SHIFT  4      // 16-frame average, kept in Q4
// Stride units per sample of fill error, as a fraction.
#define AUDIO_KP_NUM         15
#define AUDIO_KP_DEN         2

struct AudioFeed {
    uint8_t           ring[AUDIO_RING_SIZE];
    volatile uint32_t wr;       // free-running; written by the producer
    volatile uint32_t rd;       // free-running; written by the consumer
    uint32_t          frac;     // Q16 position past rd; consumer only
    volatile uint32_t stride;   // Q16; written by the producer
    volatile bool     playing;  // set by the producer, cleared by the consumer on running dry
    uint8_t           last;     // last sample out; consumer only
    volatile uint32_t underruns;
    volatile uint32_t hold_samples;
    volatile uint32_t overruns;
    // Rate control state, producer only.
    int32_t           dt_avg_us;
    int32_t           err_q4;
    bool              correcting;
    bool              slow;
    uint8_t           long_run;     // long frames in a row
    uint32_t          long_min_us;  // shortest of them
    uint8_t           short_run;    // frames well under the average in a row
    uint32_t          short_max_us; // longest of them
};

static inline void audio_feed_init(AudioFeed* f) {
    f->wr = f->rd = 0;
    f->frac = 0;
    f->stride = AUDIO_STRIDE_ONE;
    f->playing = false;
    f->last = 0;
    f->underruns = f->hold_samples = f->overruns = 0;
    f->dt_avg_us = AUDIO_FRAME_US;
    f->err_q4 = 0;
    f->correcting = false;
    f->slow = false;
    f->long_run = 0;
    f->long_min_us = 0;
    f->short_run = 0;
    f->short_max_us = 0;
}

static inline uint32_t audio_feed_fill(const AudioFeed* f) {
    return f->wr - f->rd;
}

// Producer: queue one sample. Above the limit it is dropped and counted.
static inline void audio_feed_push(AudioFeed* f, uint8_t sample) {
    uint32_t wr = f->wr;
    if (wr - f->rd >= AUDIO_FILL_LIMIT) {
        f->overruns = f->overruns + 1;
        return;
    }
    f->ring[wr & AUDIO_RING_MASK] = sample;
    f->wr = wr + 1;
}

// Producer, at the end of a frame: the ring is at its fullest here, so this is
// where its fill is judged. dt_us is the time since the last frame end, or 0
// for the first frame.
static inline void audio_feed_steer(AudioFeed* f, uint32_t dt_us) {
    if (dt_us >= AUDIO_DT_MIN_US && dt_us <= AUDIO_DT_MAX_US) {
        // A long frame or two is jitter or a stall: pace_frame() keeps an
        // absolute schedule, so the next ones come early and pay it back.
        // Three in a row add more lateness than jitter can, so that is a
        // change of speed, and the ring cannot wait out an average then: the
        // rate goes straight to the shortest of the three. Coming back up is
        // the same: three frames well under the average go straight to the
        // longest of them. Otherwise the average moves slowly, and the trim
        // works off what fill is left over.
        if (dt_us > AUDIO_LONG_DT_US) {
            f->short_run = 0;
            f->long_run = f->long_run < 255 ? f->long_run + 1 : 255;
            if (f->long_run == 1 || dt_us < f->long_min_us) f->long_min_us = dt_us;
            if (f->long_run >= 3 && (int32_t)f->long_min_us > f->dt_avg_us)
                f->dt_avg_us = (int32_t)f->long_min_us;
        } else {
            f->long_run = 0;
            if ((int32_t)dt_us < f->dt_avg_us * 3 / 4) {
                f->short_run = f->short_run < 255 ? f->short_run + 1 : 255;
                if (f->short_run == 1 || dt_us > f->short_max_us) f->short_max_us = dt_us;
                if (f->short_run >= 3) {
                    int32_t to = (int32_t)f->short_max_us;
                    if (to < AUDIO_FRAME_US) to = AUDIO_FRAME_US;
                    if (to < f->dt_avg_us) f->dt_avg_us = to;
                }
            } else {
                f->short_run = 0;
            }
            f->dt_avg_us += ((int32_t)dt_us - f->dt_avg_us) >> AUDIO_DT_AVG_SHIFT;
        }
    }

    const int32_t fill = (int32_t)audio_feed_fill(f);

    if (!f->playing) {
        // Output stays on its last sample until the ring is back at its level,
        // then starts from exactly that level. The consumer leaves rd alone
        // until it sees playing set.
        if (fill >= AUDIO_FILL_TARGET) {
            f->rd = f->wr - AUDIO_FILL_TARGET;
            f->err_q4 = 0;
            f->correcting = false;
            f->stride = AUDIO_STRIDE_ONE;
            f->playing = true;
        }
        return;
    }

    f->err_q4 += (((fill - AUDIO_FILL_TARGET) << 4) - f->err_q4) >> AUDIO_ERR_AVG_SHIFT;
    const int32_t err = f->err_q4 >> 4;
    const int32_t size = err < 0 ? -err : err;
    if (f->dt_avg_us > AUDIO_SLOW_DT_US) f->slow = true;
    else if (f->dt_avg_us < AUDIO_FULL_DT_US) f->slow = false;

    if (f->slow || size > AUDIO_FILL_BAND)
        f->correcting = true;
    else if (size <= AUDIO_FILL_SETTLED)
        f->correcting = false;

    if (!f->correcting) {
        f->stride = AUDIO_STRIDE_ONE;
        return;
    }

    // Feed-forward: one frame of samples per measured frame interval, so the
    // rate follows a speed change within a few frames. The trim brings the fill
    // back, gently enough not to be heard as pitch.
    uint32_t ff = AUDIO_STRIDE_ONE;
    if (f->slow)
        ff = (uint32_t)(((uint64_t)AUDIO_STRIDE_ONE * AUDIO_FRAME_US) / (uint32_t)f->dt_avg_us);
    if (ff < AUDIO_STRIDE_MIN) ff = AUDIO_STRIDE_MIN;

    int32_t trim = err * AUDIO_KP_NUM / AUDIO_KP_DEN;
    if (trim > AUDIO_TRIM_MAX) trim = AUDIO_TRIM_MAX;
    if (trim < -AUDIO_TRIM_MAX) trim = -AUDIO_TRIM_MAX;
    f->stride = (uint32_t)((int32_t)ff + trim);
}

// Consumer: one DMA buffer. count is even. Each word is for the built-in DAC,
// which plays its high byte, and the I2S FIFO takes the two halves of each
// 32-bit word in the other order. Running dry, the output stays on its last
// sample, and it goes on there until the producer restarts it.
static inline void audio_feed_fill_words(AudioFeed* f, uint16_t* words, int count) {
    const uint32_t stride = f->stride;
    if (stride == AUDIO_STRIDE_ONE) f->frac = 0;     // on the sample grid again

    for (int i = 0; i < count; i++) {
        if (f->playing) {
            const uint32_t rd = f->rd;
            const uint32_t have = f->wr - rd;
            if (stride == AUDIO_STRIDE_ONE) {
                if (have != 0) {
                    f->last = f->ring[rd & AUDIO_RING_MASK];
                    f->rd = rd + 1;
                }
            } else if (have >= 2) {
                const int32_t s0 = f->ring[rd & AUDIO_RING_MASK];
                const int32_t s1 = f->ring[(rd + 1) & AUDIO_RING_MASK];
                f->last = (uint8_t)(s0 + (((s1 - s0) * (int32_t)f->frac + 0x8000) >> 16));
                const uint32_t pos = f->frac + stride;
                f->frac = pos & 0xFFFF;
                f->rd = rd + (pos >> 16);
            }
            if (have == 0 || (stride != AUDIO_STRIDE_ONE && have < 2)) {
                f->playing = false;
                f->underruns = f->underruns + 1;
            }
        }
        if (!f->playing) f->hold_samples = f->hold_samples + 1;
        words[i ^ 1] = (uint16_t)((uint16_t)f->last << 8);
    }
}

// ---------------------------------------------------------------------------
// DMA buffer bookkeeping. The DMA loops over its buffers for good, and the task
// polls I2S0's out_link_dscr to see where it has got to. That register runs two
// descriptors ahead of the one whose data the DMA is fetching. Measured on this
// board in the mame-coco3-ttgovga port (its docs/perf.md): with 4 buffers, 2
// ahead and 2 behind are the same buffer, so another count must be measured.
// ---------------------------------------------------------------------------
#define AUDIO_DMA_DSCR_AHEAD 2

static inline int audio_dma_fetching(int dscr, int bufs) {
    return (dscr - AUDIO_DMA_DSCR_AHEAD + bufs) % bufs;
}

// Next buffer to refill, given the one the DMA is fetching at start.
static inline int audio_dma_first(int fetching, int bufs) {
    return (fetching + 1) % bufs;
}

// Buffers the DMA has finished since 'next', in order; never the one being
// fetched. Calls fill(buffer, ctx) for each and returns the new next.
template <class Fill>
static inline int audio_dma_refill(int next, int dscr, int bufs, Fill fill) {
    const int fetching = audio_dma_fetching(dscr, bufs);
    while (next != fetching) {
        fill(next);
        next = (next + 1) % bufs;
    }
    return next;
}

#endif // AUDIO_FEED_H
