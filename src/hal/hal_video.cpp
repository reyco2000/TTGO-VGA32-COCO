#pragma GCC optimize("O2")
/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : hal_video.cpp
 *  Module : Video HAL — FabGL VGAController, 640x200 @ 60 Hz
 * ============================================================
*/

#include "hal.h"
#include "../utils/debug.h"
#include "../core/mc6847.h"
#include "../core/tcc1014.h"


#include "fabgl.h"
#include "osd_canvas.h"

// Direct 64-color VGA controller. (Plan originally specified VGA16Controller,
// but the GIME core emits pre-converted RGB565 pixels — OPT-C4 — not palette
// indices. VGAController accepts raw 6-bit color per pixel, so RGB565→RGB222
// truncation is the only conversion needed and the core stays untouched.)
// Tall GIME screens (225 lines, at most 512 pixels wide: DK, Rick Dangerous)
// switch the output to 512x240. Same signal as 640x480@60 (25.175 MHz, 800
// clocks per line, 262 lines double-scanned): 512 active pixels with 64
// extra blank clocks each side, so pixels keep their 640-mode width.
#define VGA_512x240_60HzD "\"512x240@60HzD\" 25.175 512 592 688 800 240 245 247 262 -HSync -VSync DoubleScan"

// VGAController whose framebuffer is allocated once and kept: a mode switch
// re-points the rows instead of freeing 128 KB and allocating it again
// (which other allocations could fragment in between). Chunks are whole
// 2560-byte units (lcm of 640 and 512), so 200 rows of 640 and 240 rows of
// 512 both fit the same 50 units with no row straddling two chunks.
class CocoVGAController : public fabgl::VGAController {
    static constexpr int UNIT = 2560;
    static constexpr int UNITS = 128000 / UNIT;
    static constexpr int MAX_CHUNKS = 32;
    static constexpr int MAX_ROWS = 240;
    volatile uint8_t** m_rows = nullptr;   // row-pointer table, kept like the pool
    uint8_t* m_chunk[MAX_CHUNKS] = {};
    int      m_chunkUnits[MAX_CHUNKS] = {};
    int      m_chunks = 0;

    void allocatePool() {
        // Same policy as VGABaseController::allocateViewPort(): leave
        // FABGLIB_MINFREELARGESTBLOCK free for the rest of the system.
        int remaining = UNITS;
        while (remaining > 0 && m_chunks < MAX_CHUNKS) {
            const int largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
            if (largest < FABGLIB_MINFREELARGESTBLOCK) break;
            const int n = fabgl::tmax(1, fabgl::tmin(remaining, (largest - FABGLIB_MINFREELARGESTBLOCK) / UNIT));
            uint8_t* p = (uint8_t*)heap_caps_malloc(n * UNIT, MALLOC_CAP_DMA);
            if (!p) break;
            m_chunk[m_chunks] = p;
            m_chunkUnits[m_chunks++] = n;
            remaining -= n;
        }
    }

    void allocateViewPort() override {
        if (m_chunks == 0) allocatePool();
        const int rowlen = m_viewPortWidth;
        int rows = 0;
        const int want = fabgl::tmin((int)m_viewPortHeight, MAX_ROWS);
        if (!m_rows)
            m_rows = (volatile uint8_t**)heap_caps_malloc(sizeof(uint8_t*) * MAX_ROWS,
                                                          MALLOC_CAP_32BIT | MALLOC_CAP_INTERNAL);
        m_viewPort = m_viewPorts[0] = m_rows;
        for (int c = 0; c < m_chunks && rows < want; c++) {
            uint8_t* p = m_chunk[c];
            for (int r = 0; r < m_chunkUnits[c] * UNIT / rowlen && rows < want; r++, p += rowlen)
                m_viewPort[rows++] = p;
        }
        m_viewPortHeight = rows;   // fewer if the pool came up short
        m_viewPortVisible = m_viewPort;
    }

    void freeViewPort() override {
        // Keep the pool and the row-pointer table (not double-buffered, so
        // m_viewPorts[1] is never set).
        m_viewPorts[0] = m_viewPorts[1] = nullptr;
        m_viewPort = m_viewPortVisible = nullptr;
    }
};

static CocoVGAController    s_vga;
static fabgl::Canvas        s_canvas(&s_vga);
static bool                 s_tall = false;      // 512x240 output active
static int                  s_tall_votes = 0;    // frames the other layout was wanted
static uint32_t             s_switches = 0;
static uint32_t             s_switch_us = 0;
static void set_tall(bool tall);

// OSDCanvas shim instance the supervisor draws into via hal_video_get_canvas().
static OSDCanvas             s_tft_shim;
static bool                 display_available = false;

// 64-entry GIME palette → FabGL raw-pixel byte (with HSYNC/VSYNC bits set).
static uint8_t              s_gime_raw_lut[64];
static void border_rows_invalidate();

// Phase 1 active-pixel LUT: byte-swapped RGB565 (as emitted by the GIME core,
// OPT-C4) → FabGL raw VGA byte. Collapses the per-pixel
//   bswap16 → rgb565_to_rgb222 → createRawPixel
// conversion chain to a single indexed load in the scanline loop. 64 KB in
// internal DRAM; built once in init_gime_lut().
// 64KB GIME pixel LUT lives in PSRAM (allocated in init_gime_lut) so the WiFi
// driver has enough internal DMA-capable DRAM for its RX/TX buffers. CoCo 3's
// on-screen palette is tiny, so the touched LUT entries stay cache-resident.
static uint8_t*             s_gime_pixel_raw_lut = nullptr;

// FPS overlay state
static bool     fps_overlay_enabled = false;
static uint32_t fps_frame_count = 0;
static uint32_t fps_last_time = 0;
static float    fps_value = 0.0f;

// Tick + render the FPS overlay according to FPS_OVERLAY_MODE in config.h.
// Called from hal_video_present() / hal_video_present_gime() once per frame.
// Frames are counted even with the overlay off so /api/status can report FPS.
static void fps_tick_and_draw(void) {
    fps_frame_count++;
    uint32_t now = millis();
    uint32_t elapsed = now - fps_last_time;
    if (elapsed >= 1000) {
        fps_value = (float)fps_frame_count * 1000.0f / (float)elapsed;
        fps_frame_count = 0;
        fps_last_time = now;
#if (FPS_OVERLAY_MODE & FPS_OVERLAY_SERIAL)
        if (fps_overlay_enabled) DEBUG_PRINTF("FPS: %.1f", fps_value);
#endif
    }
    if (!fps_overlay_enabled) return;
#if (FPS_OVERLAY_MODE & FPS_OVERLAY_SCREEN)
    // Draw the current fps_value into the top-left of the framebuffer.
    // Drawn every frame because the scanline render path overwrites the
    // overlay area on the next frame's render — re-drawing here keeps it
    // visible. FabGL drawText cost is well under 1 ms.
    char buf[16];
    snprintf(buf, sizeof(buf), "%.1f fps", fps_value);
    s_canvas.setPenColor(fabgl::RGB888(255, 255, 255));
    s_canvas.setBrushColor(fabgl::RGB888(0, 0, 0));
    s_canvas.setGlyphOptions(fabgl::GlyphOptions().FillBackground(true));
    s_canvas.drawText(&fabgl::FONT_8x14, 2, 2, buf);
    s_canvas.setGlyphOptions(fabgl::GlyphOptions());
#endif
}

static inline fabgl::RGB222 gime_idx_to_rgb222(int i) {
    // GIME palette format (interleaved R1 G1 B1 R0 G0 B0)
    uint8_t r = (((i >> 4) & 2) | ((i >> 2) & 1));  // 0..3
    uint8_t g = (((i >> 3) & 2) | ((i >> 1) & 1));
    uint8_t b = (((i >> 2) & 2) | ((i >> 0) & 1));
    return fabgl::RGB222(r, g, b);
}

// RGB565 → RGB222 (truncation: top 2 bits of each channel).
static inline fabgl::RGB222 rgb565_to_rgb222(uint16_t c) {
    uint8_t r = (c >> 14) & 0x03;
    uint8_t g = (c >>  9) & 0x03;
    uint8_t b = (c >>  3) & 0x03;
    return fabgl::RGB222(r, g, b);
}

// Build the GIME idx → raw VGA byte LUT once. Stored with HSYNC/VSYNC bits
// included so writes to the scanline are a single byte store.
static void init_gime_lut(void) {
    for (int i = 0; i < 64; i++) {
        s_gime_raw_lut[i] = s_vga.createRawPixel(gime_idx_to_rgb222(i));
    }
    // Active-pixel LUT: index is the raw byte-swapped RGB565 value the core
    // emits, so render-time lookup is s_gime_pixel_raw_lut[pixels[x]].
    if (!s_gime_pixel_raw_lut) {
        s_gime_pixel_raw_lut = (uint8_t*)ps_malloc(65536);
        if (!s_gime_pixel_raw_lut) {
            DEBUG_PRINT("  Video: FATAL — GIME LUT ps_malloc(65536) failed");
            return;
        }
    }
    for (int v = 0; v < 65536; v++) {
        uint16_t c = __builtin_bswap16((uint16_t)v);
        s_gime_pixel_raw_lut[v] = s_vga.createRawPixel(rgb565_to_rgb222(c));
    }
}

void hal_video_init(void) {
    DEBUG_PRINT("  Video: FabGL VGA init...");
    s_vga.begin((gpio_num_t)PIN_VGA_R1, (gpio_num_t)PIN_VGA_R0,
                (gpio_num_t)PIN_VGA_G1, (gpio_num_t)PIN_VGA_G0,
                (gpio_num_t)PIN_VGA_B1, (gpio_num_t)PIN_VGA_B0,
                (gpio_num_t)PIN_VGA_HSYNC, (gpio_num_t)PIN_VGA_VSYNC);
    s_vga.setResolution(VGA_640x200_60HzD);
    border_rows_invalidate();
    s_canvas.setBrushColor(fabgl::Color::Black);
    s_canvas.clear();
    init_gime_lut();
#if !GIME_VGA_DOWNSCALE
    // OPT-G6: the GIME renderer emits raw VGA bytes through this table
    // (hal_video_render_scanline_gime_raw), bypassing the 64 KB LUT.
    tcc1014_set_raw_lut(s_gime_raw_lut);
#endif
    OSDCanvas::bind_canvas(&s_canvas);
    display_available = true;
    DEBUG_PRINTF("  Video: VGA 640x200 ready, viewport=%dx%d",
                 s_vga.getViewPortWidth(), s_vga.getViewPortHeight());
}

void hal_video_set_mode(uint8_t mode) { (void)mode; }

OSDCanvas* hal_video_get_canvas(void) {
    return display_available ? &s_tft_shim : nullptr;
}

// Convert one of the 12 VDG palette indices to a packed VGA raw byte.
// Computed lazily so we can use s_vga.createRawPixel() after begin().
static uint8_t vdg_raw_byte(uint8_t vdg_color) {
    // Approximate RGB222 values matching the existing RGB565 table.
    static const uint8_t r3[16] = {0,3,0,3,3,0,3,3, 0,0,2,3, 0,0,0,0};
    static const uint8_t g3[16] = {3,3,0,0,3,3,0,1, 0,1,0,2, 0,0,0,0};
    static const uint8_t b3[16] = {0,0,2,0,3,1,3,0, 0,0,0,1, 0,0,0,0};
    static uint8_t cache[16] = {0};
    static bool ready = false;
    if (!ready) {
        for (int i = 0; i < 16; i++)
            cache[i] = s_vga.createRawPixel(fabgl::RGB222(r3[i], g3[i], b3[i]));
        ready = true;
    }
    return cache[vdg_color & 0x0F];
}

// Defined with the screenshot-capture machinery further down.
static inline void capture_scanline_vdg(int line, const uint8_t* pixels, int width);

// VDG (CoCo 2) scanline: 256 px wide @ palette indices, centered in 640x200.
void hal_video_render_scanline(int line, const uint8_t* pixels, int width) {
    if (!display_available || !pixels) return;
    capture_scanline_vdg(line, pixels, width);
    if (line < 0 || line >= VDG_ACTIVE_HEIGHT) return;
    const int vp_w = s_vga.getViewPortWidth();
    const int vp_h = s_vga.getViewPortHeight();
    const int x_off = (vp_w - VDG_ACTIVE_WIDTH * 2) / 2;  // 2x horizontal scale
    const int y_off = (vp_h - VDG_ACTIVE_HEIGHT) / 2;
    int y = line + y_off;
    if (y < 0 || y >= vp_h) return;
    volatile uint8_t* row = s_vga.getScanline(y);
    int w = (width < VDG_ACTIVE_WIDTH) ? width : VDG_ACTIVE_WIDTH;
    for (int x = 0; x < w; x++) {
        uint8_t b = vdg_raw_byte(pixels[x]);
        int dx0 = x_off + x * 2;
        int dx1 = dx0 + 1;
        row[dx0 ^ 2] = b;
        row[dx1 ^ 2] = b;
    }
}

void hal_video_present(const uint8_t* ram, uint16_t vdg_base, uint8_t vdg_mode) {
    (void)ram; (void)vdg_base; (void)vdg_mode;
    set_tall(false);   // the VDG path draws for 640x200
    // FabGL scans out continuously — present is a no-op apart from FPS.
    fps_tick_and_draw();
}

// --- Debug screenshot capture (PSRAM) ---
#define HAL_CAP_W   640
#define HAL_CAP_H   240
static uint16_t*     s_cap_buf       = nullptr;   // HAL_CAP_W * HAL_CAP_H RGB565
static volatile bool s_cap_armed     = false;
static volatile bool s_cap_ready     = false;
static int           s_cap_w         = 0;
static int           s_cap_h         = 0;
static int           s_cap_h_pending = 0;

void hal_video_capture_arm(void) {
    if (!s_cap_buf) {
        s_cap_buf = (uint16_t*)ps_malloc((size_t)HAL_CAP_W * HAL_CAP_H * sizeof(uint16_t));
        if (!s_cap_buf) { DEBUG_PRINT("capture: ps_malloc failed"); return; }
    }
    s_cap_ready     = false;
    s_cap_h_pending = 0;
    s_cap_armed     = true;
}

bool hal_video_capture_ready(void) { return s_cap_ready; }

bool hal_video_capture_armed(void) { return s_cap_armed; }

const uint16_t* hal_video_capture_frame(int* width, int* height) {
    if (!s_cap_ready) return nullptr;
    if (width)  *width  = s_cap_w;
    if (height) *height = s_cap_h;
    return s_cap_buf;
}

// Framebuffer readback: decode what FabGL is actually scanning out (after the
// HAL's geometry, border and swizzle) into the capture buffer. Unlike the
// armed capture, this sees the display itself — used to verify the raw GIME
// path and line-skip logic. Call with the emulator paused.
bool hal_video_capture_framebuffer(void) {
    if (!display_available) return false;
    if (!s_cap_buf) {
        s_cap_buf = (uint16_t*)ps_malloc((size_t)HAL_CAP_W * HAL_CAP_H * sizeof(uint16_t));
        if (!s_cap_buf) return false;
    }
    // raw byte → RGB565 (byte-swapped, the capture format). Every RGB222
    // colour is some GIME 6-bit value, so s_gime_raw_lut covers the display.
    static uint16_t* rev = nullptr;   // PSRAM: debug-only, keep internal RAM free
    if (!rev) rev = (uint16_t*)ps_malloc(256 * sizeof(uint16_t));
    if (!rev) return false;
    for (int i = 0; i < 256; i++) rev[i] = 0;
    for (int i = 0; i < 64; i++) {
        fabgl::RGB222 c = gime_idx_to_rgb222(i);
        uint16_t v = (uint16_t)(((c.R * 31 / 3) << 11) | ((c.G * 63 / 3) << 5) | (c.B * 31 / 3));
        rev[s_gime_raw_lut[i]] = (uint16_t)((v << 8) | (v >> 8));
    }
    const int vp_w = s_vga.getViewPortWidth();
    const int vp_h = s_vga.getViewPortHeight();
    const int w = (vp_w > HAL_CAP_W) ? HAL_CAP_W : vp_w;
    const int h = (vp_h > HAL_CAP_H) ? HAL_CAP_H : vp_h;
    for (int y = 0; y < h; y++) {
        const volatile uint8_t* row = s_vga.getScanline(y);
        uint16_t* dst = s_cap_buf + (size_t)y * HAL_CAP_W;
        for (int x = 0; x < w; x++) dst[x] = rev[row[x ^ 2]];
    }
    s_cap_w = w;
    s_cap_h = h;
    s_cap_armed = false;
    s_cap_ready = true;
    return true;
}

// Capture one scanline of GIME output if a capture is armed. Independent of the
// display output path (runs before the viewport clipping / early returns).
static inline void capture_scanline(int line, int total_lines,
                                    const uint16_t* pixels, int width) {
    if (!s_cap_armed || !s_cap_buf) return;
    if (line == 0) {
        s_cap_h_pending = (total_lines > HAL_CAP_H) ? HAL_CAP_H : total_lines;
        s_cap_w = (width > HAL_CAP_W) ? HAL_CAP_W : width;
    }
    if (s_cap_h_pending == 0 || line < 0 || line >= s_cap_h_pending) return;
    int w = (width > HAL_CAP_W) ? HAL_CAP_W : width;
    memcpy(s_cap_buf + (size_t)line * HAL_CAP_W, pixels, (size_t)w * sizeof(uint16_t));
    if (line == s_cap_h_pending - 1) {
        s_cap_h     = s_cap_h_pending;
        s_cap_ready = true;
        s_cap_armed = false;
    }
}

// VDG (CoCo 2) capture: pixels are 4-bit palette indices, not RGB565, so convert
// each to the same byte-swapped RGB565 the GIME path stores (so png_writer treats
// both identically). The 16-entry palette mirrors vdg_raw_byte()'s RGB222 table.
static uint16_t  s_vdg_cap_lut[16];
static bool      s_vdg_cap_lut_ready = false;

static void build_vdg_cap_lut(void) {
    static const uint8_t r3[16] = {0,3,0,3,3,0,3,3, 0,0,2,3, 0,0,0,0};
    static const uint8_t g3[16] = {3,3,0,0,3,3,0,1, 0,1,0,2, 0,0,0,0};
    static const uint8_t b3[16] = {0,0,2,0,3,1,3,0, 0,0,0,1, 0,0,0,0};
    for (int i = 0; i < 16; i++) {
        uint16_t r5 = (uint16_t)(r3[i] * 31 / 3);
        uint16_t g6 = (uint16_t)(g3[i] * 63 / 3);
        uint16_t b5 = (uint16_t)(b3[i] * 31 / 3);
        uint16_t v  = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        s_vdg_cap_lut[i] = (uint16_t)((v << 8) | (v >> 8));  // byte-swap (GIME format)
    }
    s_vdg_cap_lut_ready = true;
}

static inline void capture_scanline_vdg(int line, const uint8_t* pixels, int width) {
    if (!s_cap_armed || !s_cap_buf) return;
    if (line == 0) {
        if (!s_vdg_cap_lut_ready) build_vdg_cap_lut();
        s_cap_h_pending = (VDG_ACTIVE_HEIGHT > HAL_CAP_H) ? HAL_CAP_H : VDG_ACTIVE_HEIGHT;
        int w = (width > VDG_ACTIVE_WIDTH) ? VDG_ACTIVE_WIDTH : width;
        s_cap_w = (w > HAL_CAP_W) ? HAL_CAP_W : w;
    }
    if (s_cap_h_pending == 0 || line < 0 || line >= s_cap_h_pending) return;
    uint16_t* dst = s_cap_buf + (size_t)line * HAL_CAP_W;
    for (int x = 0; x < s_cap_w; x++) dst[x] = s_vdg_cap_lut[pixels[x] & 0x0F];
    if (line == s_cap_h_pending - 1) {
        s_cap_h     = s_cap_h_pending;
        s_cap_ready = true;
        s_cap_armed = false;
    }
}

// Map a GIME active-area line to a framebuffer row, or -1 to drop it.
// Shorter frames (192/199 lines) are centred. The 225-line mode (LPF=3,
// NitrOS-9's 25-row screen) is taller than the 200-line viewport: instead of
// cutting off the bottom 25 lines, drop the last line of every 9-line block —
// with 9-line text rows that is the blank gap under each glyph, so all 25 rows
// stay visible and the text itself is not distorted.
static inline int gime_display_y(int line, int total_lines, int vp_h) {
    if (line < 0) return -1;
    if (total_lines <= 0) total_lines = vp_h;
    if (total_lines <= vp_h) {
        const int y = line + (vp_h - total_lines) / 2;
        return (y < vp_h) ? y : -1;
    }
    const int excess = total_lines - vp_h;
    const int period = total_lines / excess;          // 225 → 9
    int dropped = (line + 1) / period;
    if (dropped <= excess && (line + 1) % period == 0) return -1;
    if (dropped > excess) dropped = excess;
    const int y = line - dropped;
    return (y < vp_h) ? y : -1;
}

// Raw byte last painted by hal_video_render_border_gime() per framebuffer
// row, or -1 when the row holds something else (an active line, the OSD).
static int16_t s_border_row[256];
static void border_rows_invalidate() {
    for (int i = 0; i < 256; i++) s_border_row[i] = -1;
}

void hal_video_render_border_gime(int line, unsigned total_lines, uint8_t border_colour) {
    if (!display_available) return;
    const int vp_w = s_vga.getViewPortWidth();
    const int vp_h = s_vga.getViewPortHeight();
    if ((int)total_lines >= vp_h || vp_h > 256) return;     // no spare rows
    const int y = line + (vp_h - (int)total_lines) / 2;      // as gime_display_y()
    if (y < 0 || y >= vp_h) return;
    const uint8_t b = s_gime_raw_lut[border_colour & 0x3F];
    if (s_border_row[y] == b) return;
    s_border_row[y] = b;
    memset((void*)s_vga.getScanline(y), b, vp_w);           // same byte: no swizzle
}

// GIME (CoCo 3) scanline: pre-converted RGB565 from tcc1014 — convert each
// pixel to a raw VGA byte. Width is 320 or 640 (post-OPT-C4).
void hal_video_render_scanline_gime(int line, int total_lines,
                                     uint8_t border_colour,
                                     const uint16_t* pixels,
                                     int width, const uint16_t* palette) {
    (void)palette;
    if (!display_available || !pixels || width <= 0) return;
    capture_scanline(line, total_lines, pixels, width);
    const int vp_w = s_vga.getViewPortWidth();
    const int vp_h = s_vga.getViewPortHeight();
    const int y = gime_display_y(line, total_lines, vp_h);
    if (y < 0) return;
    s_border_row[y] = -1;

    volatile uint8_t* row = s_vga.getScanline(y);
    uint8_t border_byte = s_gime_raw_lut[border_colour & 0x3F];

    // Output strategy:
    //   width == 640: 1:1
    //   width == 320: pixel-double horizontally to 640
    //   other widths: nearest-neighbor scale to vp_w
    int x_out_start, x_out_end;
    if (width == vp_w) {
        x_out_start = 0;
        x_out_end = vp_w;
#if GIME_VGA_DOWNSCALE
        // Phase 2: render 640-wide source at 320 + pixel-double horizontally.
        // Halves the per-pixel LUT lookups (one lookup feeds two output cols).
        for (int x = 0; x < vp_w; x += 2) {
            uint8_t b = s_gime_pixel_raw_lut[pixels[x]];
            row[x ^ 2]       = b;
            row[(x + 1) ^ 2] = b;
        }
#else
        for (int x = 0; x < vp_w; x++) {
            // pixels[] is byte-swapped RGB565 (OPT-C4) — LUT does unswap+convert
            row[x ^ 2] = s_gime_pixel_raw_lut[pixels[x]];
        }
#endif
    } else if (width * 2 == vp_w) {
        x_out_start = 0;
        x_out_end = vp_w;
        for (int x = 0; x < width; x++) {
            uint8_t b = s_gime_pixel_raw_lut[pixels[x]];
            int dx0 = x * 2;
            row[dx0 ^ 2] = b;
            row[(dx0 + 1) ^ 2] = b;
        }
    } else if (width < vp_w) {
        int x_off = (vp_w - width) / 2;
        x_out_start = x_off;
        x_out_end   = x_off + width;
        // Left border
        for (int x = 0; x < x_off; x++) row[x ^ 2] = border_byte;
        for (int x = 0; x < width; x++) {
            row[(x_off + x) ^ 2] = s_gime_pixel_raw_lut[pixels[x]];
        }
        // Right border
        for (int x = x_off + width; x < vp_w; x++) row[x ^ 2] = border_byte;
        return;
    } else {
        // width > vp_w: downscale
        x_out_start = 0;
        x_out_end = vp_w;
        for (int x = 0; x < vp_w; x++) {
            int sx = x * width / vp_w;
            row[x ^ 2] = s_gime_pixel_raw_lut[pixels[sx]];
        }
    }
    (void)x_out_start; (void)x_out_end;
}

// OPT-G6: GIME scanline already converted to raw VGA bytes by the core (one
// byte per pixel, logical order). FabGL's DMA wants each 32-bit word's two
// halves swapped (the x^2 addressing above), which is a 16-bit rotate — so
// the copy runs a word at a time instead of a LUT load + byte store per pixel.
void hal_video_render_scanline_gime_raw(int line, int total_lines,
                                        uint8_t border_colour,
                                        const uint8_t* raw, int width) {
    if (!display_available || !raw || width <= 0) return;
    const int vp_w = s_vga.getViewPortWidth();
    const int vp_h = s_vga.getViewPortHeight();
    const int y = gime_display_y(line, total_lines, vp_h);
    if (y < 0) return;
    s_border_row[y] = -1;

    uint8_t* row = (uint8_t*)s_vga.getScanline(y);
    const uint8_t border_byte = s_gime_raw_lut[border_colour & 0x3F];
    const uint32_t* src = (const uint32_t*)raw;   // line_raw is word-aligned
    const int x_off = (width < vp_w) ? (vp_w - width) / 2 : 0;
    const bool words = !((uintptr_t)row & 3) && !(vp_w & 3) && !(x_off & 3) && !(width & 3);

    if (words && width == vp_w) {
        uint32_t* dst = (uint32_t*)row;
        for (int i = 0; i < vp_w / 4; i++) {
            const uint32_t w = src[i];
            dst[i] = (w >> 16) | (w << 16);
        }
    } else if (words && width * 2 == vp_w) {
        // Pixel-double: source bytes a,b → logical a a b b → stored b b a a
        uint32_t* dst = (uint32_t*)row;
        const uint16_t* src16 = (const uint16_t*)raw;
        for (int i = 0; i < width / 2; i++) {
            const uint32_t s = src16[i];
            dst[i] = ((s >> 8) & 0xFF) * 0x0101u | (s & 0xFF) * 0x01010000u;
        }
    } else if (words && width < vp_w) {
        uint32_t* dst = (uint32_t*)row;
        const uint32_t bw = border_byte * 0x01010101u;
        const int wl = x_off / 4, wa = width / 4;
        for (int i = 0; i < wl; i++) dst[i] = bw;
        for (int i = 0; i < wa; i++) {
            const uint32_t w = src[i];
            dst[wl + i] = (w >> 16) | (w << 16);
        }
        for (int i = wl + wa; i < vp_w / 4; i++) dst[i] = bw;
    } else {
        // Unaligned / downscale fallback — byte at a time, same geometry as
        // hal_video_render_scanline_gime().
        if (width == vp_w) {
            for (int x = 0; x < vp_w; x++) row[x ^ 2] = raw[x];
        } else if (width * 2 == vp_w) {
            for (int x = 0; x < width; x++) {
                row[(2 * x) ^ 2] = raw[x];
                row[(2 * x + 1) ^ 2] = raw[x];
            }
        } else if (width < vp_w) {
            for (int x = 0; x < x_off; x++) row[x ^ 2] = border_byte;
            for (int x = 0; x < width; x++) row[(x_off + x) ^ 2] = raw[x];
            for (int x = x_off + width; x < vp_w; x++) row[x ^ 2] = border_byte;
        } else {
            for (int x = 0; x < vp_w; x++) row[x ^ 2] = raw[x * width / vp_w];
        }
    }
}

// Switch the output between 640x200 and 512x240. The framebuffer comes back
// black, so every GIME line and border row is redrawn.
static void set_tall(bool tall) {
    if (tall == s_tall || !display_available) return;
    const int64_t t0 = esp_timer_get_time();
    s_vga.setResolution(tall ? VGA_512x240_60HzD : VGA_640x200_60HzD);
    s_switch_us = (uint32_t)(esp_timer_get_time() - t0);
    s_tall = tall;
    s_tall_votes = 0;
    s_switches++;
    tcc1014_invalidate_lines();
    border_rows_invalidate();
    DEBUG_PRINTF("  Video: %s, viewport=%dx%d, %u us", tall ? "512x240" : "640x200",
                 s_vga.getViewPortWidth(), s_vga.getViewPortHeight(), (unsigned)s_switch_us);
}

void hal_video_gime_frame(unsigned total_lines, int width) {
    const bool want = total_lines > 200 && total_lines <= 240 && width > 0 && width <= 512;
    if (want == s_tall) { s_tall_votes = 0; return; }
    // Only after 30 frames in a row, so a brief mode change (a loader, a
    // one-frame glitch) doesn't blank the monitor twice.
    if (++s_tall_votes >= 30) set_tall(want);
}

void hal_video_standard_layout(void) {
    set_tall(false);
}

void hal_video_layout_info(int* w, int* h, uint32_t* switches, uint32_t* switch_us) {
    *w = display_available ? s_vga.getViewPortWidth() : 0;
    *h = display_available ? s_vga.getViewPortHeight() : 0;
    *switches = s_switches;
    *switch_us = s_switch_us;
}

void hal_video_present_gime(bool* dirty) {
    // FabGL scans out continuously. No DMA push needed. FPS only.
    fps_tick_and_draw();
    if (dirty) *dirty = false;
}

// Something other than the GIME renderer drew on the framebuffer (OSD, FPS
// overlay): make the dirty-line skip (OPT-G5) redraw every line.
//
// FabGL canvas primitives are queued and executed in the background, so the
// drawing that prompted this call (e.g. the supervisor's black fill on close)
// may not have reached the framebuffer yet. Drain the queue first; otherwise
// it lands on top of the lines redrawn below and, being unchanged, they are
// never repainted.
void hal_video_force_repaint(void) {
    if (display_available) s_canvas.waitCompletion(false);
    tcc1014_invalidate_lines();
    border_rows_invalidate();
}

float hal_video_get_fps(void) { return fps_value; }

void hal_video_toggle_fps_overlay(void) {
    fps_overlay_enabled = !fps_overlay_enabled;

#if (FPS_OVERLAY_MODE & FPS_OVERLAY_SCREEN)
    // When the overlay is disabled, clear the rectangle the text occupies.
    // The scanline render path only writes the GIME/VDG *active area*, so
    // rows above it (the top border on 192/199-line modes) never get
    // refreshed and the stale FPS text would linger.
    // Width: "NN.N fps" = 8 chars × FONT_8x14 width = 64 px, +2 px slack.
    // Height: 14 px font + 2 px slack.
    if (!fps_overlay_enabled) {
        s_canvas.setBrushColor(fabgl::RGB888(0, 0, 0));
        s_canvas.fillRectangle(0, 0, 80, 18);
    }
#endif

    hal_video_force_repaint();

    fps_frame_count = 0;
    fps_last_time = millis();
}

