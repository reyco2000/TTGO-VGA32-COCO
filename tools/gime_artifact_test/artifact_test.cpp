// Host test of src/core/gime_artifact.h against golden.h (golden.py).
#include <cstdio>
#include <cstring>
#include "../../src/core/gime_artifact.h"
#include "golden.h"

static int failures = 0;
#define CHECK_EQ(actual, expected) do { \
    long a_ = (long)(actual), e_ = (long)(expected); \
    if (a_ != e_) { printf("FAIL %s:%d: %s = %ld, want %ld\n", __FILE__, __LINE__, #actual, a_, e_); failures++; } \
} while (0)

static void check_line(const char* name, uint8_t mode, uint8_t c0, uint8_t c1, bool border,
                       const uint8_t* want) {
    uint8_t colours[16];
    gime_artifact_mix(mode, GIME_MONITOR_RGB, c0, c1, colours);
    uint8_t out[8 * 16 + 1];
    out[8 * 16] = 0xEE;
    uint8_t* end = gime_artifact_line<uint8_t>(k_golden_src, 8, colours, border, out);
    CHECK_EQ(end - out, 8 * 16);
    CHECK_EQ(out[8 * 16], 0xEE);                // nothing written past the line
    for (int i = 0; i < 8 * 16; i++) {
        if (out[i] != want[i]) {
            printf("FAIL %s: pixel %d = %02X, want %02X\n", name, i, out[i], want[i]);
            failures++;
            return;
        }
    }
}

int main() {
    check_line("standard", GIME_ARTIFACT_STANDARD, 0x00, 0x3F, false, k_golden_m1_b0);
    check_line("standard, border c1", GIME_ARTIFACT_STANDARD, 0x00, 0x3F, true, k_golden_m1_b1);
    check_line("reverse", GIME_ARTIFACT_REVERSE, 0x00, 0x3F, false, k_golden_m2_b0);
    check_line("reverse, border c1", GIME_ARTIFACT_REVERSE, 0x00, 0x3F, true, k_golden_m2_b1);
    check_line("green/red", GIME_ARTIFACT_STANDARD, 0x12, 0x24, false, k_golden_gr);

    // Standard: 01 pixels blue, 10 orange, as the old fixed table and the CoCo 2.
    uint8_t colours[16], out[32 * 16];
    gime_artifact_mix(GIME_ARTIFACT_STANDARD, GIME_MONITOR_RGB, 0x00, 0x3F, colours);
    uint8_t blue[32], orange[32];
    memset(blue, 0x55, sizeof blue);
    memset(orange, 0xAA, sizeof orange);
    gime_artifact_line<uint8_t>(blue, 32, colours, false, out);
    CHECK_EQ(out[256], 0x19);                   // RGB 0,2,3: G1 B1 B0
    gime_artifact_line<uint8_t>(orange, 32, colours, false, out);
    CHECK_EQ(out[256], 0x34);                   // RGB 3,2,0: R1 G1 R0

    // Composite: the ends follow the monitor's map.
    gime_artifact_mix(GIME_ARTIFACT_STANDARD, GIME_MONITOR_COMPOSITE, 16, 48, colours);
    CHECK_EQ(colours[0], gime_monitor_colour(GIME_MONITOR_COMPOSITE, 16));
    CHECK_EQ(colours[15], gime_monitor_colour(GIME_MONITOR_COMPOSITE, 48));

    // Works for a 16-bit pixel type too.
    uint16_t c16[16], o16[16];
    for (int i = 0; i < 16; i++) c16[i] = (uint16_t)(0x100 + i);
    uint8_t ones = 0xFF;
    gime_artifact_line<uint16_t>(&ones, 1, c16, true, o16);
    for (int i = 0; i < 16; i++) CHECK_EQ(o16[i], 0x10F);

    if (failures) { printf("artifact_test: %d failed\n", failures); return 1; }
    printf("artifact_test: all passed\n");
    return 0;
}
