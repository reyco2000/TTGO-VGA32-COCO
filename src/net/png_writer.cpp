/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : png_writer.cpp
 *  Module : Minimal PNG encoder (RGB565 -> 24-bit PNG, deflate "stored")
 * ============================================================
 */

#include "png_writer.h"

#include <Arduino.h>   // ps_malloc
#include <string.h>

// --- CRC32 (PNG / zlib polynomial) ---

// Table-free CRC32 (keeps no static buffer out of internal DRAM .bss).
static uint32_t crc32_update(uint32_t crc, const uint8_t* buf, size_t len) {
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++)
            crc = (crc & 1) ? (0xEDB88320u ^ (crc >> 1)) : (crc >> 1);
    }
    return crc ^ 0xFFFFFFFFu;
}

// --- Adler32 (zlib) ---

static uint32_t adler32(const uint8_t* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

// --- byte writers ---

static void put_be32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}

// Append a PNG chunk (length + type + data + CRC). Returns new write position.
static size_t put_chunk(uint8_t* out, size_t pos, const char* type,
                        const uint8_t* data, size_t len) {
    put_be32(out + pos, (uint32_t)len); pos += 4;
    size_t crc_start = pos;
    memcpy(out + pos, type, 4); pos += 4;
    if (len) { memcpy(out + pos, data, len); pos += len; }
    uint32_t crc = crc32_update(0, out + crc_start, 4 + len);
    put_be32(out + pos, crc); pos += 4;
    return pos;
}

uint8_t* png_encode_rgb565(const uint16_t* src, int w, int h, int stride, size_t* out_len) {
    if (!src || w <= 0 || h <= 0) return nullptr;

    // 1) Build the raw (pre-compression) scanline data: filter byte 0 per row.
    const size_t row_bytes = 1 + (size_t)w * 3;
    const size_t raw_len   = row_bytes * (size_t)h;
    uint8_t* raw = (uint8_t*)ps_malloc(raw_len);
    if (!raw) return nullptr;

    size_t rp = 0;
    for (int y = 0; y < h; y++) {
        raw[rp++] = 0x00;  // filter: none
        const uint16_t* line = src + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            // pixels are byte-swapped RGB565 — unswap first.
            uint16_t v = line[x];
            uint16_t p = (uint16_t)((v << 8) | (v >> 8));
            uint8_t r5 = (p >> 11) & 0x1F;
            uint8_t g6 = (p >> 5)  & 0x3F;
            uint8_t b5 =  p        & 0x1F;
            raw[rp++] = (uint8_t)((r5 << 3) | (r5 >> 2));
            raw[rp++] = (uint8_t)((g6 << 2) | (g6 >> 4));
            raw[rp++] = (uint8_t)((b5 << 3) | (b5 >> 2));
        }
    }

    // 2) Allocate the output PNG. IDAT = zlib(2) + stored blocks(5 each) + raw + adler(4).
    const size_t nblocks = (raw_len + 65534) / 65535;
    const size_t idat_len = 2 + nblocks * 5 + raw_len + 4;
    const size_t out_cap  = 8 /*sig*/ + (12 + 13) /*IHDR*/ + (12 + idat_len) /*IDAT*/ + 12 /*IEND*/ + 16;
    uint8_t* out = (uint8_t*)ps_malloc(out_cap);
    if (!out) { free(raw); return nullptr; }

    size_t pos = 0;

    // PNG signature
    static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    memcpy(out + pos, sig, 8); pos += 8;

    // IHDR
    uint8_t ihdr[13];
    put_be32(ihdr + 0, (uint32_t)w);
    put_be32(ihdr + 4, (uint32_t)h);
    ihdr[8] = 8;    // bit depth
    ihdr[9] = 2;    // colour type: truecolour RGB
    ihdr[10] = 0;   // compression
    ihdr[11] = 0;   // filter
    ihdr[12] = 0;   // interlace
    pos = put_chunk(out, pos, "IHDR", ihdr, sizeof(ihdr));

    // IDAT — assemble the zlib stream directly into the output region.
    put_be32(out + pos, (uint32_t)idat_len); pos += 4;
    size_t crc_start = pos;
    memcpy(out + pos, "IDAT", 4); pos += 4;
    out[pos++] = 0x78;  // zlib CMF
    out[pos++] = 0x01;  // FLG (no preset dict, fastest)

    size_t off = 0, remaining = raw_len;
    while (remaining > 0) {
        size_t block = remaining > 65535 ? 65535 : remaining;
        uint8_t bfinal = (block == remaining) ? 1 : 0;
        out[pos++] = bfinal;                 // BTYPE=00 (stored), BFINAL in bit 0
        out[pos++] = block & 0xFF;           // LEN (LE)
        out[pos++] = (block >> 8) & 0xFF;
        uint16_t nlen = ~(uint16_t)block;
        out[pos++] = nlen & 0xFF;            // NLEN (LE)
        out[pos++] = (nlen >> 8) & 0xFF;
        memcpy(out + pos, raw + off, block); pos += block;
        off += block; remaining -= block;
    }
    put_be32(out + pos, adler32(raw, raw_len)); pos += 4;

    uint32_t idat_crc = crc32_update(0, out + crc_start, 4 + idat_len);
    put_be32(out + pos, idat_crc); pos += 4;

    free(raw);

    // IEND
    pos = put_chunk(out, pos, "IEND", nullptr, 0);

    if (out_len) *out_len = pos;
    return out;
}
