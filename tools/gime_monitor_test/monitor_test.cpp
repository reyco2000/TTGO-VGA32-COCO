// Host test of src/core/gime_monitor.h.
#include <cstdio>
#include "../../src/core/gime_monitor.h"

static int failures = 0;
#define CHECK_EQ(actual, expected) do { \
    long a_ = (long)(actual), e_ = (long)(expected); \
    if (a_ != e_) { printf("FAIL %s:%d: %s = %ld, want %ld\n", __FILE__, __LINE__, #actual, a_, e_); failures++; } \
} while (0)

int main() {
    // RGB is the identity, and the nearest of an RGB colour is itself.
    for (int c = 0; c < 64; c++) {
        CHECK_EQ(gime_monitor_colour(GIME_MONITOR_RGB, c), c);
        CHECK_EQ(gime_nearest(gime_colour_rgb(c)), c);
    }
    CHECK_EQ(gime_colour_rgb(0x3F), 0xFFFFFF);
    CHECK_EQ(gime_colour_rgb(0x24), 0xFF0000);      // R1 + R0
    CHECK_EQ(gime_colour_rgb(0x12), 0x00FF00);
    // Composite: the four greys, and a colour in each channel.
    CHECK_EQ(gime_monitor_colour(GIME_MONITOR_COMPOSITE, 0),  0x00);   // black
    CHECK_EQ(gime_monitor_colour(GIME_MONITOR_COMPOSITE, 16), 0x07);   // 0x2d2d2d: dark grey
    CHECK_EQ(gime_monitor_colour(GIME_MONITOR_COMPOSITE, 32), 0x07);   // 0x747474: 116 rounds to level 1 too
    CHECK_EQ(gime_monitor_colour(GIME_MONITOR_COMPOSITE, 48), 0x3F);   // 0xfdfdfe: white
    CHECK_EQ(gime_monitor_colour(GIME_MONITOR_COMPOSITE, 63), 0x3F);
    CHECK_EQ(gime_monitor_colour(GIME_MONITOR_COMPOSITE, 6),  0x04);   // 0x6c0000: R 1
    CHECK_EQ(gime_monitor_colour(GIME_MONITOR_COMPOSITE, 27), 0x0F);   // 0x5e2cff: R1 G1 B3
    CHECK_EQ(gime_monitor_colour(GIME_MONITOR_COMPOSITE, 11), 0x08);   // 0x1100ca: B2
    if (failures) { printf("monitor_test: %d failed\n", failures); return 1; }
    printf("monitor_test: all passed\n");
    return 0;
}
