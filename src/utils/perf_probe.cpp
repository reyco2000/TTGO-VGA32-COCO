/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : perf_probe.cpp
 *  Module : Performance probe — accumulator state and per-frame report
 * ============================================================
*/

#include "perf_probe.h"

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
    "audio_scn   "
};

void perf_probe_frame_end(void) {
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
