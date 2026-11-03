// Host test of src/hal/audio_feed.h: the ring, its rate control and the DMA
// bookkeeping, including a simulation of the emulator at several speeds.
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../../src/hal/audio_feed.h"

static int failures = 0;
#define CHECK_EQ(actual, expected) do { \
    long a_ = (long)(actual), e_ = (long)(expected); \
    if (a_ != e_) { printf("FAIL %s:%d: %s = %ld, want %ld\n", __FILE__, __LINE__, #actual, a_, e_); failures++; } \
} while (0)
#define CHECK(cond, ...) do { \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); failures++; } \
} while (0)

static AudioFeed f;

static uint8_t out(uint16_t* w, int i) { return (uint8_t)(w[i ^ 1] >> 8); }

static void start_with(int samples) {
    audio_feed_init(&f);
    for (int i = 0; i < samples; i++) audio_feed_push(&f, (uint8_t)i);
    f.playing = true;
}

static void test_word_format_swaps_halves_and_uses_high_byte() {
    start_with(4);
    uint16_t w[4];
    audio_feed_fill_words(&f, w, 4);
    CHECK_EQ(w[1], 0x0000);     // sample 0 goes in the other half
    CHECK_EQ(w[0], 0x0100);
    CHECK_EQ(w[3], 0x0200);
    CHECK_EQ(w[2], 0x0300);
}

static void test_stride_one_passes_samples_through_exactly() {
    start_with(64);
    uint16_t w[64];
    audio_feed_fill_words(&f, w, 64);
    for (int i = 0; i < 64; i++) CHECK_EQ(out(w, i), i);
    CHECK_EQ(audio_feed_fill(&f), 0);
    CHECK_EQ(f.underruns, 0);
}

static void test_half_stride_interpolates() {
    audio_feed_init(&f);
    audio_feed_push(&f, 0); audio_feed_push(&f, 100); audio_feed_push(&f, 200);
    f.playing = true;
    f.stride = AUDIO_STRIDE_ONE / 2;
    uint16_t w[4];
    audio_feed_fill_words(&f, w, 4);
    CHECK_EQ(out(w, 0), 0);
    CHECK_EQ(out(w, 1), 50);
    CHECK_EQ(out(w, 2), 100);
    CHECK_EQ(out(w, 3), 150);
}

static void test_interpolation_falling_edge_stays_in_range() {
    audio_feed_init(&f);
    audio_feed_push(&f, 255); audio_feed_push(&f, 0); audio_feed_push(&f, 0);
    f.playing = true;
    f.stride = AUDIO_STRIDE_ONE / 2;
    uint16_t w[2];
    audio_feed_fill_words(&f, w, 2);
    CHECK_EQ(out(w, 0), 255);
    CHECK_EQ(out(w, 1), 128);   // 127.5, rounded
}

static void test_running_dry_holds_last_sample_and_counts_once() {
    start_with(2);
    f.ring[1] = 77;
    uint16_t w[8];
    audio_feed_fill_words(&f, w, 8);
    CHECK_EQ(out(w, 1), 77);
    for (int i = 2; i < 8; i++) CHECK_EQ(out(w, i), 77);
    CHECK_EQ(f.underruns, 1);
    CHECK_EQ(f.hold_samples, 6);
    CHECK(!f.playing, "still playing after running dry");
}

static void test_not_playing_consumes_nothing() {
    audio_feed_init(&f);
    for (int i = 0; i < 100; i++) audio_feed_push(&f, 9);
    uint16_t w[8];
    audio_feed_fill_words(&f, w, 8);
    CHECK_EQ(audio_feed_fill(&f), 100);
    CHECK_EQ(f.underruns, 0);           // waiting to start is not running dry
}

static void test_restart_waits_for_target() {
    audio_feed_init(&f);
    for (int i = 0; i < AUDIO_FILL_TARGET - 1; i++) audio_feed_push(&f, 0);
    audio_feed_steer(&f, AUDIO_FRAME_US);
    CHECK(!f.playing, "started below the target");
    audio_feed_push(&f, 0);
    audio_feed_steer(&f, AUDIO_FRAME_US);
    CHECK(f.playing, "did not start at the target");
}

static void test_push_stops_at_limit() {
    audio_feed_init(&f);
    for (int i = 0; i < AUDIO_FILL_LIMIT + 5; i++) audio_feed_push(&f, 1);
    CHECK_EQ(audio_feed_fill(&f), AUDIO_FILL_LIMIT);
    CHECK_EQ(f.overruns, 5);
}

static void test_indices_survive_wrapping() {
    start_with(0);
    f.wr = f.rd = 0xFFFFFFF0u;
    for (int i = 0; i < 40; i++) audio_feed_push(&f, (uint8_t)i);
    CHECK_EQ(audio_feed_fill(&f), 40);
    uint16_t w[40];
    audio_feed_fill_words(&f, w, 40);
    for (int i = 0; i < 40; i++) CHECK_EQ(out(w, i), i);
}

static void test_dma_never_refills_the_buffer_being_fetched() {
    for (int dscr = 0; dscr < 4; dscr++) {
        int fetching = audio_dma_fetching(dscr, 4);
        for (int next = 0; next < 4; next++) {
            bool touched[4] = {};
            int end = audio_dma_refill(next, dscr, 4, [&](int b) { touched[b] = true; });
            CHECK(!touched[fetching], "refilled buffer %d being fetched", fetching);
            CHECK_EQ(end, fetching);
        }
    }
    CHECK_EQ(audio_dma_fetching(2, 4), 0);
    CHECK_EQ(audio_dma_fetching(0, 4), 2);
    CHECK_EQ(audio_dma_first(3, 4), 0);
}

// ---------------------------------------------------------------------------
// Simulation: frames end at the times a callback gives, and a 64-sample buffer
// goes out every 64 output sample periods.
// ---------------------------------------------------------------------------
struct Result {
    uint32_t underruns, overruns, frames_off_one, frames;
    uint32_t stride_min, stride_max;
    int fill_min, fill_max;
    uint32_t mismatches;    // output different from input, passthrough runs only
};

template <class NextDt>
static Result simulate(double seconds, NextDt next_dt, bool check_passthrough = false, double settle_s = 2.0) {
    const double sample_us = (double)AUDIO_FRAME_US / AUDIO_FRAME_SAMPLES;
    const double buf_us = 64 * sample_us;
    audio_feed_init(&f);
    Result r = {};
    r.stride_min = 0xFFFFFFFF;
    r.fill_min = 1 << 30;
    double frame_end = 0, buf_end = buf_us;
    uint32_t dt = 0;
    uint8_t in_seq = 0, out_seq = 0;
    bool synced = false;
    while (frame_end < seconds * 1e6) {
        if (buf_end < frame_end) {
            uint16_t w[64];
            bool was_playing = f.playing;
            audio_feed_fill_words(&f, w, 64);
            if (check_passthrough && was_playing) {
                for (int i = 0; i < 64; i++) {
                    uint8_t v = out(w, i);
                    if (synced && v != (uint8_t)(out_seq + 1)) r.mismatches++;
                    out_seq = v;
                    synced = true;
                }
            }
            buf_end += buf_us;
        } else {
            for (int i = 0; i < AUDIO_FRAME_SAMPLES; i++) audio_feed_push(&f, in_seq++);
            audio_feed_steer(&f, dt);
            dt = next_dt();
            frame_end += dt;
            if (frame_end > settle_s * 1e6) {
                r.frames++;
                int fill = (int)audio_feed_fill(&f);
                if (fill < r.fill_min) r.fill_min = fill;
                if (fill > r.fill_max) r.fill_max = fill;
                if (f.stride != AUDIO_STRIDE_ONE) r.frames_off_one++;
                if (f.stride < r.stride_min) r.stride_min = f.stride;
                if (f.stride > r.stride_max) r.stride_max = f.stride;
            }
        }
    }
    r.underruns = f.underruns;
    r.overruns = f.overruns;
    return r;
}

static void test_full_speed_is_exactly_one_to_one() {
    // pace_frame() keeps an absolute schedule: a late frame does not move the next.
    srand(1);
    int prev = 0;
    Result r = simulate(120, [&prev] {
        int j = rand() % 1601 - 800;
        uint32_t dt = (uint32_t)(AUDIO_FRAME_US + j - prev);
        prev = j;
        return dt;
    }, true);
    CHECK_EQ(r.underruns, 0);
    CHECK_EQ(r.overruns, 0);
    CHECK_EQ(r.frames_off_one, 0);
    CHECK_EQ(r.mismatches, 0);
    printf("  full speed: fill %d..%d\n", r.fill_min, r.fill_max);
}

// Frame ends seen on the board at 60 FPS: 12.4 to 25.5 ms apart, on the same
// absolute schedule. Jitter alone must never move the rate off exactly one.
static void test_board_jitter_keeps_stride_one() {
    srand(2);
    int prev = 0;
    Result r = simulate(300, [&prev] {
        int j = rand() % 7001 - 3500;                   // +-3.5 ms
        if (rand() % 200 == 0) j = 8800;                // now and then a 25.5 ms frame
        uint32_t dt = (uint32_t)(AUDIO_FRAME_US + j - prev);
        prev = j;
        return dt;
    }, true);
    CHECK_EQ(r.underruns, 0);
    CHECK_EQ(r.frames_off_one, 0);
    CHECK_EQ(r.mismatches, 0);
    printf("  board jitter: fill %d..%d\n", r.fill_min, r.fill_max);
}

static void test_slightly_slow_never_runs_dry() {
    Result r = simulate(600, [] { return (uint32_t)16722; });   // 59.8 FPS
    CHECK_EQ(r.underruns, 0);
    CHECK_EQ(r.overruns, 0);
    printf("  59.8 FPS: fill %d..%d, stride %.4f..%.4f\n", r.fill_min, r.fill_max,
           r.stride_min / 65536.0, r.stride_max / 65536.0);
}

static void test_three_percent_slow_never_runs_dry() {
    Result r = simulate(600, [] { return (uint32_t)17182; });   // 58.2 FPS
    CHECK_EQ(r.underruns, 0);
    CHECK_EQ(r.overruns, 0);
}

static void test_slow_emulator_slows_the_sound() {
    Result r = simulate(120, [] { return (uint32_t)25641; }, false, 10.0);    // 39 FPS
    CHECK_EQ(r.underruns, 0);
    CHECK_EQ(r.overruns, 0);
    CHECK(r.stride_min / 65536.0 > 0.62 && r.stride_max / 65536.0 < 0.68,
          "stride %.4f..%.4f, want about 0.65", r.stride_min / 65536.0, r.stride_max / 65536.0);
}

static void test_step_from_full_speed_to_slow() {
    int n = 0;
    Result r = simulate(60, [&n] { return (uint32_t)(n++ < 600 ? AUDIO_FRAME_US : 25641); });
    printf("  step to 39 FPS: underruns %u, fill %d..%d\n", r.underruns, r.fill_min, r.fill_max);
    CHECK_EQ(r.underruns, 0);
    CHECK_EQ(r.overruns, 0);
}

static void test_step_from_slow_back_to_full_speed() {
    int n = 0;
    Result r = simulate(60, [&n] { return (uint32_t)(n++ < 800 ? 25641 : AUDIO_FRAME_US); });
    CHECK_EQ(r.underruns, 0);
    CHECK_EQ(r.overruns, 0);
    CHECK_EQ(f.stride, AUDIO_STRIDE_ONE);       // back to exact by the end
}

static void test_single_long_frame_does_not_run_dry() {
    int n = 0;
    Result r = simulate(30, [&n] { return (uint32_t)(++n == 600 ? 33332 : AUDIO_FRAME_US); });
    CHECK_EQ(r.underruns, 0);
    CHECK_EQ(f.stride, AUDIO_STRIDE_ONE);
}

static void test_single_long_frame_leaves_the_rate_alone() {
    int n = 0;
    Result r = simulate(30, [&n] { return (uint32_t)(++n == 600 ? 33332 : AUDIO_FRAME_US); });
    CHECK(r.stride_min >= AUDIO_STRIDE_ONE - AUDIO_TRIM_MAX, "dipped to %.4f", r.stride_min / 65536.0);
}

static void test_pause_runs_dry_once_then_recovers() {
    int n = 0;
    Result r = simulate(30, [&n] { return (uint32_t)(++n == 600 ? 2000000 : AUDIO_FRAME_US); });
    CHECK_EQ(r.underruns, 1);
    CHECK(f.playing, "did not restart after the pause");
    CHECK_EQ(f.stride, AUDIO_STRIDE_ONE);
}

int main() {
    test_word_format_swaps_halves_and_uses_high_byte();
    test_stride_one_passes_samples_through_exactly();
    test_half_stride_interpolates();
    test_interpolation_falling_edge_stays_in_range();
    test_running_dry_holds_last_sample_and_counts_once();
    test_not_playing_consumes_nothing();
    test_restart_waits_for_target();
    test_push_stops_at_limit();
    test_indices_survive_wrapping();
    test_dma_never_refills_the_buffer_being_fetched();
    test_full_speed_is_exactly_one_to_one();
    test_board_jitter_keeps_stride_one();
    test_slightly_slow_never_runs_dry();
    test_three_percent_slow_never_runs_dry();
    test_slow_emulator_slows_the_sound();
    test_step_from_full_speed_to_slow();
    test_step_from_slow_back_to_full_speed();
    test_single_long_frame_does_not_run_dry();
    test_single_long_frame_leaves_the_rate_alone();
    test_pause_runs_dry_once_then_recovers();
    if (failures) {
        printf("feed_test: %d failed\n", failures);
        return 1;
    }
    printf("feed_test: all passed\n");
    return 0;
}
