// Host test of src/hal/audio_capture.h: the per-line sound average.
#include <cmath>
#include <cstdio>
#include "../../src/hal/audio_capture.h"

static int failures = 0;
#define CHECK_EQ(actual, expected) do { \
    long a_ = (long)(actual), e_ = (long)(expected); \
    if (a_ != e_) { printf("FAIL %s:%d: %s = %ld, want %ld\n", __FILE__, __LINE__, #actual, a_, e_); failures++; } \
} while (0)

static void test_no_edge_is_the_level() {
    AudioLineAverage a = {};
    CHECK_EQ(audio_line_take(&a, 57), 0);
    a.level = 200;
    CHECK_EQ(audio_line_take(&a, 57), 200);
    CHECK_EQ(audio_line_take(&a, 0), 200);      // a line with no cycles run
}

static void test_edge_is_weighted_by_position() {
    AudioLineAverage a = {};
    audio_line_note(&a, 100, 19);               // 0 for 19 cycles, 100 for 38
    CHECK_EQ(audio_line_take(&a, 57), 67);      // 66.67, rounded
    CHECK_EQ(audio_line_take(&a, 57), 100);     // the next line holds the level
}

static void test_edge_at_line_start_takes_the_whole_line() {
    AudioLineAverage a = {};
    audio_line_note(&a, 100, 0);
    CHECK_EQ(audio_line_take(&a, 57), 100);
}

static void test_edge_at_line_end_shows_on_the_next_line() {
    AudioLineAverage a = {};
    audio_line_note(&a, 100, 57);
    CHECK_EQ(audio_line_take(&a, 57), 0);
    CHECK_EQ(audio_line_take(&a, 57), 100);
}

static void test_several_edges() {
    AudioLineAverage a = {};
    audio_line_note(&a, 255, 10);               // 0 x 10
    audio_line_note(&a, 0, 20);                 // 255 x 10
    audio_line_note(&a, 255, 30);               // 0 x 10
    CHECK_EQ(audio_line_take(&a, 40), 128);     // 255 x 10 more: 127.5, rounded
}

static void test_repeated_level_is_not_an_edge() {
    AudioLineAverage a = {};
    a.level = 50;
    audio_line_note(&a, 50, 30);
    CHECK_EQ(a.at, 0);                          // no division to do at the line's end
    CHECK_EQ(audio_line_take(&a, 57), 50);
}

// The counter still holds the last line's total when a level is set between
// lines. The next line must not divide by zero or wrap the subtraction.
static void test_stale_position_stays_in_range() {
    AudioLineAverage a = {};
    a.level = 100;
    audio_line_note(&a, 200, 57);               // between lines, stale counter
    audio_line_note(&a, 50, 3);                 // next line: earlier than 'at'
    CHECK_EQ(audio_line_take(&a, 10), 100);     // 100 x 57 over 57, in range
    CHECK_EQ(audio_line_take(&a, 57), 50);
}

static void test_zero_length_line_after_an_edge() {
    AudioLineAverage a = {};
    audio_line_note(&a, 100, 0);
    CHECK_EQ(audio_line_take(&a, 0), 100);
}

// Fast-mode lines run about 114 cycles; the sum must not overflow on far more.
static void test_full_scale_long_line() {
    AudioLineAverage a = {};
    audio_line_note(&a, 255, 1);
    CHECK_EQ(audio_line_take(&a, 100000), 255);
}

// A square wave whose period is not a whole number of lines. The sum of the
// samples must follow the time the wave spent high. A point sample only gets
// this right on average over a long time; the line average must be within
// rounding after every line.
static void test_square_wave_keeps_its_edges() {
    const int line = 57;
    const double half_period = 123.4;           // cycles
    AudioLineAverage a = {};
    double next_edge = half_period, area = 0, exact = 0;
    int high = 0;
    double worst = 0;
    for (int l = 0; l < 10000; l++) {
        double t0 = (double)l * line;
        double t1 = t0 + line, t = t0;
        while (next_edge < t1) {
            exact += (next_edge - t) * (high ? 255 : 0);
            audio_line_note(&a, high ? 0 : 255, (uint32_t)std::lround(next_edge - t0));
            t = next_edge;
            high = !high;
            next_edge += half_period;
        }
        exact += (t1 - t) * (high ? 255 : 0);
        area += audio_line_take(&a, line) * (double)line;
        double err = std::fabs(area - exact);
        if (err > worst) worst = err;
    }
    // Each line rounds by up to half a level and half a cycle per edge. The
    // errors do not build up past a level per line's worth over the whole run.
    if (worst > 255.0 * line) {
        printf("FAIL square wave: drifted %.1f level-cycles from the exact area\n", worst);
        failures++;
    }
}

int main() {
    test_no_edge_is_the_level();
    test_edge_is_weighted_by_position();
    test_edge_at_line_start_takes_the_whole_line();
    test_edge_at_line_end_shows_on_the_next_line();
    test_several_edges();
    test_repeated_level_is_not_an_edge();
    test_stale_position_stays_in_range();
    test_zero_length_line_after_an_edge();
    test_full_scale_long_line();
    test_square_wave_keeps_its_edges();
    if (failures) {
        printf("capture_test: %d failed\n", failures);
        return 1;
    }
    printf("capture_test: all passed\n");
    return 0;
}
