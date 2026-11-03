// license:BSD-3-Clause
// copyright-holders:Nathan Woods, reyco
/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 * ============================================================
 *  File   : gime_monitor.h
 *  Module : What each GIME colour looks like on an RGB or a composite monitor
 * ============================================================
 *
 * The composite table is MAME's (coco3.cpp, via the mame-coco3-ttgovga port's
 * src/emu/palette.cpp), under its BSD-3-Clause licence; the rest of this
 * emulator is GPL-3.0-or-later. The VGA output has 2 bits per channel, so a
 * colour is shown as the nearest of those 64, which is itself a GIME colour on
 * an RGB monitor. The whole display path then stays in GIME colour numbers.
 *
 * Pure logic with no hardware in it; tools/gime_monitor_test/ runs it on the host.
 */

#ifndef GIME_MONITOR_H
#define GIME_MONITOR_H

#include <stdint.h>

enum { GIME_MONITOR_RGB = 0, GIME_MONITOR_COMPOSITE = 1, GIME_MONITOR_COUNT };

// 0xRRGGBB of a GIME colour on an RGB monitor: interleaved R1 G1 B1 R0 G0 B0.
static inline uint32_t gime_colour_rgb(int c) {
    return (uint32_t)(((c >> 4) & 2) | ((c >> 2) & 1)) * 0x550000u |
           (uint32_t)(((c >> 3) & 2) | ((c >> 1) & 1)) * 0x005500u |
           (uint32_t)(((c >> 2) & 2) | ((c >> 0) & 1)) * 0x000055u;
}

// 0xRRGGBB of a GIME colour on a composite monitor (MAME).
static inline uint32_t gime_colour_composite(int c) {
    static const uint32_t composite[64] = {
        0x000000, 0x004c00, 0x004300, 0x0a3100, 0x2f1b00, 0x550100, 0x6c0000, 0x770006,
        0x71004b, 0x5c008b, 0x3b00b8, 0x1100ca, 0x001499, 0x002c62, 0x004011, 0x004b00,
        0x2d2d2d, 0x069800, 0x288f00, 0x537d00, 0x786700, 0xa04c00, 0xb63402, 0xc3224c,
        0xbd1693, 0xa814d5, 0x881cfe, 0x5e2cff, 0x105ee9, 0x0076b2, 0x008b60, 0x009618,
        0x747474, 0x41d714, 0x62cf00, 0x8ebd00, 0xb4a700, 0xdd8c01, 0xf5733a, 0xfe6085,
        0xfd53ce, 0xe950ff, 0xc958ff, 0x9e67ff, 0x4e9aff, 0x36b3f7, 0x26c9a3, 0x2bd558,
        0xfdfdfe, 0x88e85a, 0xa1e03f, 0xbed238, 0xd8c342, 0xf1b161, 0xfea08d, 0xfe95bf,
        0xfd8ef1, 0xef8eff, 0xd895ff, 0xb9a1ff, 0x86c4ff, 0x78d4f2, 0x71e2b6, 0xffffff,
    };
    return composite[c & 0x3F];
}

// The GIME colour (as on an RGB monitor) nearest to 0xRRGGBB, 2 bits a channel.
static inline uint8_t gime_nearest(uint32_t rgb) {
    const unsigned r = (((rgb >> 16) & 0xFF) * 3 + 127) / 255;
    const unsigned g = (((rgb >> 8) & 0xFF) * 3 + 127) / 255;
    const unsigned b = ((rgb & 0xFF) * 3 + 127) / 255;
    return (uint8_t)(((r & 2) << 4) | ((g & 2) << 3) | ((b & 2) << 2) |
                     ((r & 1) << 2) | ((g & 1) << 1) | (b & 1));
}

// The GIME colour the display shows for colour c on this monitor.
static inline uint8_t gime_monitor_colour(int monitor, int c) {
    return monitor == GIME_MONITOR_COMPOSITE ? gime_nearest(gime_colour_composite(c))
                                             : (uint8_t)(c & 0x3F);
}

#endif // GIME_MONITOR_H
