// license:BSD-3-Clause
// copyright-holders:Nathan Woods, reyco
/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 * ============================================================
 *  File   : gime_artifact.h
 *  Module : PMODE 4 NTSC artifact colours, as MAME's artifacter
 * ============================================================
 *
 * Tables and logic are MAME's (mc6847.h artifacter::update_colors() and
 * artifact_correction, by Nathan Woods), by way of the mame-coco3-ttgovga
 * port's src/emu/render.cpp, under their BSD-3-Clause licence; the rest of this
 * emulator is GPL-3.0-or-later.
 *
 * A PMODE 4 line has two colours, c0 and c1. On an NTSC set a pixel pattern
 * shows as one of 14 mixes of the two, picked from a window of 6 pixels around
 * each output pair. Phase A (Standard) shows 01 pixels blue and 10 orange;
 * phase B (Reverse) swaps them.
 *
 * Pure logic with no hardware in it; tools/gime_artifact_test/ runs it on the host.
 */

#ifndef GIME_ARTIFACT_H
#define GIME_ARTIFACT_H

#include <stdint.h>
#include "gime_monitor.h"

enum { GIME_ARTIFACT_OFF = 0, GIME_ARTIFACT_STANDARD = 1, GIME_ARTIFACT_REVERSE = 2, GIME_ARTIFACT_COUNT };

// How much of c1 each of the 14 mixed colours takes, per channel R, G, B, in 1/1000.
static const uint16_t k_artifact_factors[14 * 3] = {
     157,    0,  157,
       0,  157,    0,
    1000,  824, 1000,
     824, 1000,  824,
     706,  236,  118,
       0,  197,  471,
    1000,  550,  393,
     275,  785, 1000,
       0,  500, 1000,
    1000,  500,    0,
    1000,  942,  785,
     393,  942, 1000,
     236,    0,    0,
       0,    0,  236,
};

// Index = pixels x-2..x+3 as 6 bits, twice, plus which of the output pixels x
// and x+1. Value = which of the 16 colours: 0 is c0, 15 is c1, 1-14 the mixes.
static const uint8_t k_artifact_correction[128] = {
     0,  0,   0,  0,   0,  6,   0,  2,
     5,  7,   5,  7,   1,  3,   1, 11,
     8,  6,   8, 14,   8,  9,   8,  9,
     4,  4,   4, 15,  12, 12,  12, 15,

     5, 13,   5, 13,  13,  0,  13,  2,
    10, 10,  10, 10,  10, 15,  10, 11,
     3,  1,   3,  1,  15,  9,  15,  9,
    11, 11,  11, 11,  15, 15,  15, 15,

    14,  0,  14,  0,  14,  6,  14,  2,
     0,  7,   0,  7,   1,  3,   1, 11,
     9,  6,   9, 14,   9,  9,   9,  9,
    15,  4,  15, 15,  12, 12,  12, 15,

     2, 13,   2, 13,   2,  0,   2,  2,
    10, 10,  10, 10,  10, 15,  10, 11,
    12,  1,  12,  1,  12,  9,  12,  9,
    15, 11,  15, 11,  15, 15,  15, 15,
};

// The 16 colours for GIME colours c0 and c1 on this monitor, as GIME colours
// (RGB-monitor meaning, what the display shows): the two ends through the
// monitor's map, and the 14 mixes as the nearest the VGA output has. Integer.
static inline void gime_artifact_mix(uint8_t mode, int monitor, uint8_t c0, uint8_t c1,
                                     uint8_t out[16]) {
    const uint32_t a = monitor == GIME_MONITOR_COMPOSITE ? gime_colour_composite(c0) : gime_colour_rgb(c0);
    const uint32_t b = monitor == GIME_MONITOR_COMPOSITE ? gime_colour_composite(c1) : gime_colour_rgb(c1);
    out[0] = gime_monitor_colour(monitor, c0);
    out[15] = gime_monitor_colour(monitor, c1);
    for (unsigned i = 1; i <= 14; i++) {
        const uint16_t* f = &k_artifact_factors[((i - 1) ^ (mode & 1)) * 3];
        uint32_t rgb = 0;
        for (int ch = 0; ch < 3; ch++) {
            const unsigned shift = 16 - 8 * ch;
            const uint32_t ca = (a >> shift) & 0xFF, cb = (b >> shift) & 0xFF;
            const uint32_t v = (ca * (1000u - f[ch]) + cb * f[ch] + 500) / 1000;
            rgb |= v << shift;
        }
        out[i] = gime_nearest(rgb);
    }
}

// One PMODE 4 line: n bytes of 8 pixels, two output pixels per pixel (the
// 512-pixel geometry), so 16 P per byte. colours: the 16 from gime_artifact_mix,
// in the output's pixel type. The window reaches into the border on both sides,
// and a border pixel counts as set if the border is c1.
template <typename P>
static inline P* gime_artifact_line(const uint8_t* src, unsigned n, const P* colours,
                                    bool border_is_c1, P* d) {
    const uint32_t border_bits = border_is_c1 ? 0xFF : 0x00;
    uint32_t prev = border_bits & 3;
    for (unsigned k = 0; k < n; k++) {
        const uint32_t cur = src[k];
        const uint32_t next = (k + 1 < n) ? src[k + 1] : border_bits;
        const uint32_t window = (prev << 16) | (cur << 8) | next;
        for (int shift = 12; shift >= 6; shift -= 2) {
            const unsigned v = ((window >> shift) & 0x3F) * 2;
            const P p0 = colours[k_artifact_correction[v]];
            const P p1 = colours[k_artifact_correction[v + 1]];
            d[0] = p0; d[1] = p0; d[2] = p1; d[3] = p1;
            d += 4;
        }
        prev = cur & 3;
    }
    return d;
}

#endif // GIME_ARTIFACT_H
