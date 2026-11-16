/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : perf_probe.cpp
 *  Module : Performance probe — accumulator state and per-frame report
 * ============================================================
*/

#include "perf_probe.h"
#include <string.h>

#if PERF_PROBE_ENABLED

uint64_t perf_probe_accum_us[PROBE_COUNT] = {0};
uint32_t perf_probe_call_count[PROBE_COUNT] = {0};
uint32_t perf_probe_frame_num = 0;

static const char* const probe_names[PROBE_COUNT] = {
    "frame       ",
    "cpu_run     ",
    "render_scn  ",
    "hal_scn     ",
    "push_sprite ",
    "audio_scn   ",
    "ssc         ",
    "scanline    ",
    "line_sig    ",
    "border      ",
    "becker      ",
    "loop_pre    ",
    "loop_post   "
};

PerfCpuStats perf_cpu;

uint8_t __attribute__((noinline)) perf_slow_read(uint8_t (*rd)(uint16_t), uint16_t addr) {
    const uint32_t t0 = perf_ccount();
    const uint8_t v = rd(addr);
    perf_cpu_slow(addr, false, perf_ccount() - t0);
    return v;
}

void __attribute__((noinline)) perf_slow_write(void (*wr)(uint16_t, uint8_t), uint16_t addr, uint8_t val) {
    const uint32_t t0 = perf_ccount();
    wr(addr, val);
    perf_cpu_slow(addr, true, perf_ccount() - t0);
}

#ifdef ARDUINO
// Indices of the n largest entries of v[len], largest first.
static int top_n(const uint32_t* v, int len, int* out, int n) {
    int k = 0;
    for (int i = 0; i < len; i++) {
        if (!v[i]) continue;
        int j = k < n ? k++ : n;
        if (j == n && v[i] <= v[out[n - 1]]) continue;
        if (j == n) j = n - 1;
        while (j > 0 && v[out[j - 1]] < v[i]) { out[j] = out[j - 1]; j--; }
        out[j] = i;
    }
    return k;
}

static void perf_cpu_report(void) {
    PerfCpuStats* s = &perf_cpu;
    const uint32_t f = s->frames ? s->frames : 1;
    const float cyc = s->cycles ? (float)s->cycles : 1.0f;

    unsigned uni = 0;
    for (int i = 0; i < PERF_RAM_BANKS / 32; i++) uni += __builtin_popcount(s->bank_union[i]);
    Serial.printf("\n=== cpu profile (%u frames) ===\n", (unsigned)s->frames);
    Serial.printf("  insns/f %u  cycles/f %u  cyc/insn %.2f\n",
                  (unsigned)(s->insns / f), (unsigned)(s->cycles / f),
                  s->insns ? cyc / (float)s->insns : 0.0f);
#if PERF_PROBE_MEM
    Serial.printf("  fast rd/f %u  wr/f %u\n",
                  (unsigned)(s->fast_rd / f), (unsigned)(s->fast_wr / f));
#endif
    Serial.printf("  slow rd/f io %u  fe %u  rom %u | wr/f io %u  fe %u  rom %u\n",
                  (unsigned)(s->slow_rd[PERF_SLOW_IO] / f), (unsigned)(s->slow_rd[PERF_SLOW_FE] / f),
                  (unsigned)(s->slow_rd[PERF_SLOW_ROM] / f), (unsigned)(s->slow_wr[PERF_SLOW_IO] / f),
                  (unsigned)(s->slow_wr[PERF_SLOW_FE] / f), (unsigned)(s->slow_wr[PERF_SLOW_ROM] / f));
    {
        uint64_t slow = 0;
        for (int k = 0; k < PERF_SLOW_COUNT; k++) slow += s->slow_rd_cc[k] + s->slow_wr_cc[k];
        Serial.printf("  exec us/f %u  (callbacks %u us/f)  host cc/insn excl. callbacks %u\n",
                      (unsigned)(s->exec_cc / 240 / f), (unsigned)(slow / 240 / f),
                      s->insns ? (unsigned)((s->exec_cc - slow) / s->insns) : 0u);
        static const char* const kn[PERF_SLOW_COUNT] = { "io", "fe", "rom" };
        for (int k = 0; k < PERF_SLOW_COUNT; k++) {
            if (!s->slow_rd[k] && !s->slow_wr[k]) continue;
            Serial.printf("    %-3s rd %u us/f (%u cc each)  wr %u us/f (%u cc each)\n", kn[k],
                          (unsigned)(s->slow_rd_cc[k] / 240 / f),
                          s->slow_rd[k] ? (unsigned)(s->slow_rd_cc[k] / s->slow_rd[k]) : 0u,
                          (unsigned)(s->slow_wr_cc[k] / 240 / f),
                          s->slow_wr[k] ? (unsigned)(s->slow_wr_cc[k] / s->slow_wr[k]) : 0u);
        }
    }
    Serial.printf("  mmu wr/f %.1f  page rebuild/f %.1f\n",
                  (float)s->mmu_wr / f, (float)s->page_rebuild / f);
    Serial.printf("  idle skip cyc/f %u (%.1f%% of cycles)\n", (unsigned)(s->idle_skip_cyc / f),
                  100.0f * (float)s->idle_skip_cyc / (cyc + (float)s->idle_skip_cyc));
#if PERF_PROBE_MEM
    Serial.printf("  8K banks/f %.1f  banks this report %u\n", (float)s->banks_sum / f, uni);
#else
    (void)uni;
#endif

    int top[16];
    int n = top_n(s->pc_cyc, PERF_PC_BUCKETS, top, 16);
    Serial.printf("  top PC (%d-byte buckets, %% of cycles):\n", 1 << PERF_PC_SHIFT);
    for (int i = 0; i < n; i++) {
        unsigned a = (unsigned)top[i] << PERF_PC_SHIFT;
        Serial.printf("    $%04X-$%04X %5.1f%%\n", a, a + (1u << PERF_PC_SHIFT) - 1,
                      100.0f * (float)s->pc_cyc[top[i]] / cyc);
    }

    n = top_n(&s->op_cyc[0][0], 3 * 256, top, 16);
    Serial.printf("  top opcodes (%% of cycles, count/f):\n");
    static const char* const pfx[3] = { "   ", "$10", "$11" };
    for (int i = 0; i < n; i++) {
        int pg = top[i] >> 8, op = top[i] & 0xFF;
        Serial.printf("    %s $%02X %5.1f%%  %6u\n", pfx[pg], op,
                      100.0f * (float)s->op_cyc[pg][op] / cyc,
                      (unsigned)(s->op_cnt[pg][op] / f));
    }
}
#endif

// Per frame: fold the banks touched this frame into the report totals.
static void perf_cpu_frame_end(void) {
    PerfCpuStats* s = &perf_cpu;
    for (int i = 0; i < PERF_RAM_BANKS / 32; i++) {
        s->banks_sum += __builtin_popcount(s->bank_frame[i]);
        s->bank_union[i] |= s->bank_frame[i];
        s->bank_frame[i] = 0;
    }
    if (++s->frames < PERF_CPU_REPORT_EVERY) return;
#ifdef ARDUINO
    perf_cpu_report();
#endif
    const uint8_t* base = s->ram_base;
    memset(s, 0, sizeof(*s));
    s->ram_base = base;
}

void perf_probe_frame_end(void) {
    perf_cpu_frame_end();
    perf_probe_frame_num++;
    if (perf_probe_frame_num < PERF_PROBE_REPORT_EVERY) return;

#ifdef ARDUINO
    uint64_t total_frame = perf_probe_accum_us[PROBE_FRAME];
    uint64_t avg_frame   = total_frame / perf_probe_frame_num;
    Serial.printf("\n--- perf (%u frames, avg frame %llu us, ~%.1f FPS) ---\n",
                  (unsigned)perf_probe_frame_num,
                  (unsigned long long)avg_frame,
                  avg_frame ? 1000000.0f / (float)avg_frame : 0.0f);
    for (int i = 0; i < PROBE_COUNT; i++) {
        uint64_t us = perf_probe_accum_us[i];
        uint64_t per_frame = us / perf_probe_frame_num;
        float pct = total_frame ? (100.0f * (float)us / (float)total_frame) : 0.0f;
        uint32_t calls_per_frame = perf_probe_call_count[i] / perf_probe_frame_num;
        Serial.printf("  %s %8llu us/f  %5.1f%%  %5u calls/f\n",
                      probe_names[i],
                      (unsigned long long)per_frame,
                      pct,
                      calls_per_frame);
    }
#endif

    for (int i = 0; i < PROBE_COUNT; i++) {
        perf_probe_accum_us[i] = 0;
        perf_probe_call_count[i] = 0;
    }
    perf_probe_frame_num = 0;
}

#endif // PERF_PROBE_ENABLED
