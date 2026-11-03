/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : audio_capture.h
 *  Module : One sound sample per scanline, the level averaged over the line
 * ============================================================
*/

// A point sample would move every edge of a tone to a line boundary: up to
// 64 us of jitter that repeats and folds high harmonics down as inharmonic
// tones. The average keeps where in the line the edge fell. Only lines with an
// edge in them do the division, so a silent or steady machine costs nothing.
//
// Pure logic with no hardware in it; tools/audio_test/ runs it on the host.

#ifndef AUDIO_CAPTURE_H
#define AUDIO_CAPTURE_H

#include <stdint.h>

struct AudioLineAverage {
    uint8_t  level;     // level now
    uint32_t sum;       // level x cycles, from the start of the line up to 'at'
    uint32_t at;        // line position 'sum' covers; 0 = no edge this line
};

// The level becomes 'level' at 'cycle' cycles into the line. A cycle at or
// before 'at' adds no time. That happens only outside the CPU's run, where the
// counter still holds the previous line's total (a reset, for example), and it
// costs at most one line's accuracy.
static inline void audio_line_note(AudioLineAverage* a, uint8_t level, uint32_t cycle) {
    if (level == a->level) return;
    if (cycle > a->at) {
        a->sum += (uint32_t)a->level * (cycle - a->at);
        a->at = cycle;
    }
    a->level = level;
}

// The line ended after 'line_cycles' cycles: its sample, rounded. Starts the next line.
static inline uint8_t audio_line_take(AudioLineAverage* a, uint32_t line_cycles) {
    if (a->at == 0) return a->level;
    uint32_t end = line_cycles > a->at ? line_cycles : a->at;
    uint32_t sum = a->sum + (uint32_t)a->level * (end - a->at);
    a->sum = 0;
    a->at = 0;
    return (uint8_t)((sum + end / 2) / end);
}

#endif // AUDIO_CAPTURE_H
