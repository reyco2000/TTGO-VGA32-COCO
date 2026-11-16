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

#ifdef ARDUINO
#include "../../config.h"
#endif
#ifndef PERF_PROBE_ENABLED          // host builds: off unless -DPERF_PROBE_ENABLED=1
#define PERF_PROBE_ENABLED 0
#endif

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
    PROBE_SSC,                 // ssc_scanline_tick (sum), Speech/Sound Cartridge
    // Split of the non-CPU frame time (added for improvements.11.16.md Phase 0).
    // Per-line glue = scanline - cpu_run - render_scn - hal_scn - border.
    PROBE_SCANLINE,            // whole machine_run_scanline_coco3 (sum)
    PROBE_LINE_SIG,            // line_signature() inside render_scn (sum)
    PROBE_BORDER,              // hal_video_render_border_gime (sum)
    PROBE_BECKER,              // becker_scanline_tick (sum)
    PROBE_LOOP_PRE,            // loop(): input, wifi tick, OSD check, debug RPC
    PROBE_LOOP_POST,           // loop(): pace_frame excluded; hal_render_frame, meter, audio dbg
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
#define PERF_CPU(stmt)           do { stmt; } while (0)
// Per-access fast-path counters (fast reads/writes, 8 KB banks touched) cost
// code at every memory access; with the HD6309 build's forced inlining that
// is enough to thrash the flash cache and slow everything ~3x. Off unless
// -DPERF_PROBE_MEM=1 as well; the other counters stay valid without it.
#ifndef PERF_PROBE_MEM
#define PERF_PROBE_MEM 0
#endif
#if PERF_PROBE_MEM
#define PERF_MEM(stmt)           do { stmt; } while (0)
#else
#define PERF_MEM(stmt)           ((void)0)
#endif

void perf_probe_frame_end(void);

// ------------------------------------------------------------
// CPU profile (rick-performance.md Phase A). Counts where the interpreter's
// cycles go: per opcode, per PC region, fast vs callback memory paths, and
// which 8 KB physical RAM banks are touched. Printed every
// PERF_CPU_REPORT_EVERY frames; the counters live in internal RAM (~8 KB).
// ------------------------------------------------------------
#define PERF_CPU_REPORT_EVERY 300   // frames between CPU profile dumps
#define PERF_PC_SHIFT         7     // PC histogram bucket = 128 bytes
#define PERF_PC_BUCKETS       (65536 >> PERF_PC_SHIFT)
#define PERF_RAM_BANKS        256   // 2 MB / 8 KB

enum { PERF_SLOW_IO = 0, PERF_SLOW_FE, PERF_SLOW_ROM, PERF_SLOW_COUNT };

struct PerfCpuStats {
    uint32_t op_cyc[3][256];        // cycles per opcode: page 1, $10 page 2, $11 page 3
    uint32_t op_cnt[3][256];        // executions per opcode
    uint32_t pc_cyc[PERF_PC_BUCKETS];
    uint32_t insns;
    uint32_t cycles;
    uint32_t fast_rd, fast_wr;
    uint32_t slow_rd[PERF_SLOW_COUNT], slow_wr[PERF_SLOW_COUNT];
    uint32_t mmu_wr;                // $FFA0-$FFAF writes
    uint32_t page_rebuild;          // update_fast_pages() calls
    uint32_t idle_skip_cyc;         // cycles advanced by the idle-loop skip
    uint64_t exec_cc;               // host cycles in execute_one (incl. callbacks)
    uint64_t slow_rd_cc[PERF_SLOW_COUNT], slow_wr_cc[PERF_SLOW_COUNT];   // host cycles in callbacks
    uint32_t bank_frame[PERF_RAM_BANKS / 32];   // banks touched this frame
    uint32_t bank_union[PERF_RAM_BANKS / 32];   // banks touched this report
    uint32_t banks_sum;             // sum over frames of banks touched per frame
    uint32_t frames;
    const uint8_t* ram_base;        // emulated RAM, for bank numbers
};

extern PerfCpuStats perf_cpu;

static inline void perf_cpu_fast(const uint8_t* page, bool wr) {
    if (wr) perf_cpu.fast_wr++; else perf_cpu.fast_rd++;
    uint32_t bank = (uint32_t)(page - perf_cpu.ram_base) >> 13;
    if (bank < PERF_RAM_BANKS) perf_cpu.bank_frame[bank >> 5] |= 1u << (bank & 31);
}

// Host CPU cycle counter (240 per microsecond).
static inline uint32_t perf_ccount(void) {
#if defined(ARDUINO)
    uint32_t c;
    __asm__ __volatile__("rsr %0, ccount" : "=a"(c));
    return c;
#else
    return 0;
#endif
}

// One read or write through the machine callback, cc host cycles long.
static inline void perf_cpu_slow(uint16_t addr, bool wr, uint32_t cc) {
    int kind = addr >= 0xFF00 ? PERF_SLOW_IO : addr >= 0xFE00 ? PERF_SLOW_FE : PERF_SLOW_ROM;
    if (wr) { perf_cpu.slow_wr[kind]++; perf_cpu.slow_wr_cc[kind] += cc; }
    else    { perf_cpu.slow_rd[kind]++; perf_cpu.slow_rd_cc[kind] += cc; }
}

// Slow-path accesses through the machine callback, timed (perf_probe.cpp).
// Out of line so the interpreter's inlined memory helpers stay small.
uint8_t perf_slow_read(uint8_t (*rd)(uint16_t), uint16_t addr);
void    perf_slow_write(void (*wr)(uint16_t, uint8_t), uint16_t addr, uint8_t val);

// One executed instruction: page 0/1/2, opcode, start PC, cycles it took.
static inline void perf_cpu_insn(int page, uint8_t op, uint16_t pc, uint32_t cyc) {
    perf_cpu.op_cyc[page][op] += cyc;
    perf_cpu.op_cnt[page][op]++;
    perf_cpu.pc_cyc[pc >> PERF_PC_SHIFT] += cyc;
    perf_cpu.insns++;
    perf_cpu.cycles += cyc;
}

#else   // PERF_PROBE_ENABLED == 0

#define PERF_PROBE_SCOPE(id)    ((void)0)
#define PERF_CPU(stmt)          ((void)0)
#define PERF_MEM(stmt)          ((void)0)
static inline void perf_probe_frame_end(void) {}

#endif

#endif // PERF_PROBE_H
