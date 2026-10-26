/*
 * Host test: GIME timer rate. TINS=1 counts the 3.58 MHz clock (228
 * ticks per scanline, as XRoar), TINS=0 counts scanlines. Checks the
 * expiry count over many scanlines, several expiries per scanline for a
 * short reload, that each expiry re-reads the reload, that INIT1 writes
 * which only change the MMU task (Rick Dangerous's sample FIRQ) leave the
 * timer running, and that a change of timer source restarts it.
 *
 * Build and run with tools/gime_timer_test/run.sh.
 */
#define TCC1014_RENDER_TEST 1
#include "../../src/core/tcc1014.cpp"

#include <stdio.h>
#include <stdlib.h>

static uint8_t* ram;
static TCC1014 g;
static int g_fail = 0, g_checks = 0;
static const char* g_name = "";

#define CHECK_EQ(got, want) do { g_checks++; long a_ = (long)(got), b_ = (long)(want); if (a_ != b_) { g_fail++; \
    printf("  FAIL [%s] line %d: %s = %ld, want %ld\n", g_name, __LINE__, #got, a_, b_); } } while (0)

static void setup(const char* name, uint8_t init1, unsigned reload) {
    g_name = name;
    tcc1014_init(&g);
    tcc1014_reset(&g);
    tcc1014_set_ram(&g, ram, 2048 * 1024);
    tcc1014_write_register(&g, 0, 0x40);              // INIT0: MMU on
    tcc1014_write_register(&g, 1, init1);
    tcc1014_write_register(&g, 5, reload & 0xFF);     // LSB first: MSB arms
    tcc1014_write_register(&g, 4, reload >> 8);
    g.timer_fires = 0;
}

// Expiries over n scanlines; init1_each, if >= 0, is written to INIT1 on
// every scanline, toggling the MMU task like Rick's FIRQ handler.
static long run(long n, int toggle_task = 0) {
    for (long i = 0; i < n; i++) {
        if (toggle_task) {
            tcc1014_write_register(&g, 1, 0x21);
            tcc1014_write_register(&g, 1, 0x20);
        }
        tcc1014_tick_scanline(&g);
    }
    return g.timer_fires;
}

int main() {
    ram = (uint8_t*)calloc(1, 2048 * 1024);

    // 10 s of NTSC scanlines (15 734.26 Hz)
    const long LINES = 157343;

    // ---- TINS=1, reload 596 (Rick Dangerous): 3 579 545 / 597 = 5 995.9 Hz ----
    setup("TINS=1 596", 0x20, 596);
    CHECK_EQ(run(LINES), LINES * 456 / (2 * 597));

    // ---- same, with the task switched twice per scanline ----
    setup("TINS=1 596 + task toggles", 0x20, 596);
    CHECK_EQ(run(LINES, 1), LINES * 456 / (2 * 597));

    // ---- TINS=1, reload $FFF: 3 579 545 / 4096 = 874 Hz ----
    setup("TINS=1 4095", 0x20, 0xFFF);
    CHECK_EQ(run(LINES), LINES * 456 / (2 * 4096));

    // ---- TINS=1, reload 10: 11 ticks, several expiries per scanline ----
    setup("TINS=1 10", 0x20, 10);
    CHECK_EQ(run(1100), 1100 * 456 / (2 * 11));

    // ---- a reload written while the timer runs takes effect on expiry ----
    setup("TINS=1 reload change", 0x20, 0xFFF);
    tcc1014_write_register(&g, 5, 10);                 // LSB only: no re-arm
    CHECK_EQ(run(17), 0);                              // 17 * 228 < 4096 ticks
    CHECK_EQ(run(1), 1);                               // next period: $F0A + 1 ticks
    CHECK_EQ(g.timer_left_x2, 2 * 0xF0B - (18 * 456 - 2 * 4096));
    tcc1014_write_register(&g, 4, 0);                  // MSB: re-arm, 11 ticks
    g.timer_fires = 0;
    CHECK_EQ(run(100), 100 * 456 / (2 * 11));

    // ---- a reload of 0 written while running stops it at the next expiry ----
    setup("TINS=1 reload to 0", 0x20, 80);             // 81 ticks, < 1 scanline
    tcc1014_write_register(&g, 5, 0);                  // LSB only: no re-arm
    CHECK_EQ(run(1000), 1);

    // ---- TINS=0, reload 10: one expiry every 11 scanlines ----
    setup("TINS=0 10", 0x00, 10);
    CHECK_EQ(run(1100), 100);

    // ---- reload 0: the timer is stopped ----
    setup("reload 0", 0x20, 0);
    CHECK_EQ(run(1000), 0);

    // ---- a change of TINS restarts the count ----
    setup("TINS change rearms", 0x00, 10);
    run(5);                                            // 5 of 11 scanlines gone
    tcc1014_write_register(&g, 1, 0x20);               // to TINS=1: 11 ticks, < 1 scanline
    CHECK_EQ(g.timer_left_x2, 22);
    CHECK_EQ(run(1), 456 / 22);

    // ---- task switch still remaps the CPU pages ----
    setup("TR remaps", 0x20, 596);
    tcc1014_write_mmu(&g, 8 + 1, 0x80);                // task 1, $2000 -> page $80
    tcc1014_write_register(&g, 1, 0x21);
    CHECK_EQ(g.rd_page[1] - ram, 0x80 * 0x2000);
    tcc1014_write_register(&g, 1, 0x20);
    CHECK_EQ(g.rd_page[1] - ram, 0x39 * 0x2000);

    printf("%d checks, %d failed\n%s\n", g_checks, g_fail, g_fail ? "FAILED" : "PASSED");
    return g_fail ? 1 : 0;
}
