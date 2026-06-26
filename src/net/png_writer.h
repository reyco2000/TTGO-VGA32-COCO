/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : png_writer.h
 *  Module : Minimal PNG encoder (RGB565 -> 24-bit PNG, deflate "stored")
 * ============================================================
 *
 * Encodes a byte-swapped RGB565 framebuffer (as produced by tcc1014 /
 * hal_video) into a baseline PNG with uncompressed (stored) deflate blocks —
 * no compression CPU cost. Output is allocated with ps_malloc; caller frees.
 */

#ifndef NET_PNG_WRITER_H
#define NET_PNG_WRITER_H

#include <stdint.h>
#include <stddef.h>

// Encode w*h byte-swapped RGB565 pixels (row stride = `stride` pixels) into a
// PNG. Returns a ps_malloc'd buffer (caller frees) and sets *out_len, or NULL
// on allocation failure.
uint8_t* png_encode_rgb565(const uint16_t* src, int w, int h, int stride, size_t* out_len);

#endif // NET_PNG_WRITER_H
