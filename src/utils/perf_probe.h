/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : perf_probe.h
 *  Module : Lightweight esp_timer-based hot-path profiler
 * ============================================================
*/

#ifndef PERF_PROBE_H
#define PERF_PROBE_H

#include "../../config.h"

#if PERF_PROBE_ENABLED

#include <stdint.h>
#ifdef ARDUINO
#include <Arduino.h>
#include <esp_timer.h>
#else
static inline int64_t esp_timer_get_time(void) { return 0; }
#endif

enum PerfProbeID {
    PROBE_FRAME = 0,           // whole machine_run_frame
    PROBE_CPU_RUN,             // mc6809_run (sum over 262 scanlines)
    PROBE_RENDER_SCANLINE,     // tcc1014_render_scanline (sum over active area)
    PROBE_HAL_SCANLINE,        // hal_video_render_scanline_gime (sum)
    PROBE_PUSH_SPRITE,         // hal_video_present_gime body (pushSprite)
    PROBE_AUDIO_SCANLINE,      // hal_audio_capture_scanline (sum)
    PROBE_COUNT
};

extern uint64_t perf_probe_accum_us[PROBE_COUNT];
extern uint32_t perf_probe_call_count[PROBE_COUNT];
extern uint32_t perf_probe_frame_num;

#define PERF_PROBE_REPORT_EVERY 60   // frames between Serial dumps

// RAII scope guard — cheap: two esp_timer_get_time() calls + one add.
class PerfProbeScope {
public:
    inline PerfProbeScope(PerfProbeID id) : id_(id), t0_(esp_timer_get_time()) {}
    inline ~PerfProbeScope() {
        perf_probe_accum_us[id_] += (uint64_t)(esp_timer_get_time() - t0_);
        perf_probe_call_count[id_]++;
    }
private:
    PerfProbeID id_;
    int64_t t0_;
};

#define PERF_PROBE_CONCAT2(a, b) a##b
#define PERF_PROBE_CONCAT(a, b)  PERF_PROBE_CONCAT2(a, b)
#define PERF_PROBE_SCOPE(id)     PerfProbeScope PERF_PROBE_CONCAT(_pp_scope_, __LINE__)(id)

void perf_probe_frame_end(void);

#else   // PERF_PROBE_ENABLED == 0

#define PERF_PROBE_SCOPE(id)    ((void)0)
static inline void perf_probe_frame_end(void) {}

#endif

#endif // PERF_PROBE_H
