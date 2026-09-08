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


#include "fabgl.h"
#include "osd_canvas.h"

// Direct 64-color VGA controller. (Plan originally specified VGA16Controller,
// but the GIME core emits pre-converted RGB565 pixels — OPT-C4 — not palette
// indices. VGAController accepts raw 6-bit color per pixel, so RGB565→RGB222
// truncation is the only conversion needed and the core stays untouched.)
static fabgl::VGAController s_vga;
static fabgl::Canvas        s_canvas(&s_vga);

// OSDCanvas shim instance the supervisor draws into via hal_video_get_canvas().
static OSDCanvas             s_tft_shim;
static bool                 display_available = false;

// 64-entry GIME palette → FabGL raw-pixel byte (with HSYNC/VSYNC bits set).
static uint8_t              s_gime_raw_lut[64];

// OPT: reverse table for the screenshot path only. The GIME core now emits raw
// VGA bytes straight into line_buffer (no RGB565 intermediate), so the screenshot
// capture — which needs byte-swapped RGB565 for the PNG writer — maps each raw
// byte back through this 256-entry table. Only the 64 raw bytes the GIME palette
// can produce are populated; the rest stay 0. Built once in init_gime_lut().
static uint16_t             s_raw_to_rgb565[256] = {0};

// FPS overlay state
static bool     fps_overlay_enabled = false;

// Serial FPS telemetry is always active; screen overlay remains optional.
#define FPS_SERIAL_TELEMETRY 1
static uint32_t fps_frame_count = 0;
static uint32_t fps_last_time = 0;
static float    fps_value = 0.0f;

// Tick + render the FPS overlay according to FPS_OVERLAY_MODE in config.h.
// Called from hal_video_present() / hal_video_present_gime() once per frame.
static void fps_tick_and_draw(void) {
    // Count every presented frame. The screen overlay can be disabled
    // independently; Serial telemetry must not depend on F5.
    fps_frame_count++;
    uint32_t now = millis();
    uint32_t elapsed = now - fps_last_time;
    if (elapsed >= 1000) {
        fps_value = (float)fps_frame_count * 1000.0f / (float)elapsed;
        fps_frame_count = 0;
        fps_last_time = now;
#if FPS_SERIAL_TELEMETRY
        DEBUG_PRINTF("FPS: %.1f", fps_value);
#endif
    }
#if (FPS_OVERLAY_MODE & FPS_OVERLAY_SCREEN)
    if (!fps_overlay_enabled) return;
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

// ------------------------------------------------------------------
// Volume OSD — top-right text+bar, shown for a short time after F9/F10.
// Same trick as the FPS overlay: draw via s_canvas AFTER the scanline
// render path each frame, so it survives being overwritten by the next
// frame's emulated pixels (present() runs after machine_run_frame()).
// ------------------------------------------------------------------
static bool     vol_osd_active    = false;
static uint32_t vol_osd_expire_ms = 0;
static uint8_t  vol_osd_percent   = 100;
#define VOL_OSD_DURATION_MS 1200

static void vol_osd_clear(void) {
    if (!display_available) return;
    int vp_w = s_vga.getViewPortWidth();
    // "VOL 100%" = 8 chars * 8px = 64px, +4px slack
    s_canvas.setBrushColor(fabgl::RGB888(0, 0, 0));
    s_canvas.fillRectangle(vp_w - 68, 0, vp_w - 1, 17);
}

static void vol_osd_draw(void) {
    if (!display_available) return;
    int vp_w = s_vga.getViewPortWidth();
    char buf[16];
    snprintf(buf, sizeof(buf), "VOL %3d%%", vol_osd_percent);

    s_canvas.setPenColor(fabgl::RGB888(255, 255, 255));
    s_canvas.setBrushColor(fabgl::RGB888(0, 0, 0));
    s_canvas.setGlyphOptions(fabgl::GlyphOptions().FillBackground(true));
    int text_w = (int)strlen(buf) * 8;   // FONT_8x14 is 8px wide per glyph
    s_canvas.drawText(&fabgl::FONT_8x14, vp_w - text_w - 2, 2, buf);
    s_canvas.setGlyphOptions(fabgl::GlyphOptions());
}

static void vol_osd_tick_and_draw(void) {
    if (!vol_osd_active) return;
    if ((int32_t)(millis() - vol_osd_expire_ms) >= 0) {
        vol_osd_active = false;
        vol_osd_clear();
        return;
    }
    vol_osd_draw();
}

// Called from hal_keyboard.cpp on F9/F10. Arms/refreshes the OSD; the
// actual drawing happens in fps_tick_and_draw()'s sibling call below.
void hal_video_show_volume_osd(uint8_t percent) {
    vol_osd_percent   = percent;
    vol_osd_active    = true;
    vol_osd_expire_ms = millis() + VOL_OSD_DURATION_MS;
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
    // Reverse table for screenshots: raw VGA byte -> byte-swapped RGB565. The
    // display is RGB222, so we expand each GIME colour's 2-bit channels to 565.
    // Only the 64 raw bytes the palette can emit get entries; the rest stay 0.
    for (int i = 0; i < 64; i++) {
        uint8_t r = (uint8_t)(((i >> 4) & 2) | ((i >> 2) & 1));  // 0..3
        uint8_t g = (uint8_t)(((i >> 3) & 2) | ((i >> 1) & 1));
        uint8_t b = (uint8_t)(((i >> 2) & 2) | ((i >> 0) & 1));
        uint16_t r5 = (uint16_t)(r * 31 / 3);
        uint16_t g6 = (uint16_t)(g * 63 / 3);
        uint16_t b5 = (uint16_t)(b * 31 / 3);
        uint16_t v  = (uint16_t)((r5 << 11) | (g6 << 5) | b5);
        s_raw_to_rgb565[s_gime_raw_lut[i]] = (uint16_t)((v << 8) | (v >> 8));  // byte-swap
    }
}

// OPT: expose the GIME colour -> raw VGA byte table so the core can emit
// framebuffer bytes directly (see tcc1014_set_raw_lut).
const uint8_t* hal_video_get_gime_raw_lut(void) { return s_gime_raw_lut; }

void hal_video_init(void) {
    DEBUG_PRINT("  Video: FabGL VGA init...");
    s_vga.begin((gpio_num_t)PIN_VGA_R1, (gpio_num_t)PIN_VGA_R0,
                (gpio_num_t)PIN_VGA_G1, (gpio_num_t)PIN_VGA_G0,
                (gpio_num_t)PIN_VGA_B1, (gpio_num_t)PIN_VGA_B0,
                (gpio_num_t)PIN_VGA_HSYNC, (gpio_num_t)PIN_VGA_VSYNC);
    s_vga.setResolution(VGA_640x200_60HzD);
    s_canvas.setBrushColor(fabgl::Color::Black);
    s_canvas.clear();
    init_gime_lut();
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
    // FabGL scans out continuously — present is a no-op apart from FPS.
    fps_tick_and_draw();
    vol_osd_tick_and_draw();
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
        s_cap_buf = (uint16_t*)malloc((size_t)HAL_CAP_W * HAL_CAP_H * sizeof(uint16_t));
        if (!s_cap_buf) { DEBUG_PRINT("capture: malloc failed"); return; }
    }
    s_cap_ready     = false;
    s_cap_h_pending = 0;
    s_cap_armed     = true;
}

bool hal_video_capture_ready(void) { return s_cap_ready; }

const uint16_t* hal_video_capture_frame(int* width, int* height) {
    if (!s_cap_ready) return nullptr;
    if (width)  *width  = s_cap_w;
    if (height) *height = s_cap_h;
    return s_cap_buf;
}

// Capture one scanline of GIME output if a capture is armed. Independent of the
// display output path (runs before the viewport clipping / early returns).
static inline void capture_scanline(int line, int total_lines,
                                    const uint8_t* pixels, int width) {
    if (!s_cap_armed || !s_cap_buf) return;
    if (line == 0) {
        s_cap_h_pending = (total_lines > HAL_CAP_H) ? HAL_CAP_H : total_lines;
        s_cap_w = (width > HAL_CAP_W) ? HAL_CAP_W : width;
    }
    if (s_cap_h_pending == 0 || line < 0 || line >= s_cap_h_pending) return;
    int w = (width > HAL_CAP_W) ? HAL_CAP_W : width;
    // pixels[] are raw VGA bytes now — reverse-map to byte-swapped RGB565 for the PNG.
    uint16_t* dst = s_cap_buf + (size_t)line * HAL_CAP_W;
    for (int x = 0; x < w; x++) dst[x] = s_raw_to_rgb565[pixels[x]];
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

// GIME (CoCo 3) scanline: pre-converted RGB565 from tcc1014 — convert each
// pixel to a raw VGA byte. Width is 320 or 640 (post-OPT-C4).
void hal_video_render_scanline_gime(int line, int total_lines,
                                     uint8_t border_colour,
                                     const uint8_t* pixels,
                                     int width, const uint16_t* palette) {
    (void)palette;
    if (!display_available || !pixels || width <= 0) return;
    capture_scanline(line, total_lines, pixels, width);
    const int vp_w = s_vga.getViewPortWidth();
    const int vp_h = s_vga.getViewPortHeight();
    if (total_lines <= 0 || total_lines > vp_h) total_lines = vp_h;
    int y_off = (vp_h - total_lines) / 2;
    if (y_off < 0) y_off = 0;
    int y = line + y_off;
    if (y < 0 || y >= vp_h) return;

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
            uint8_t b = pixels[x];
            row[x ^ 2]       = b;
            row[(x + 1) ^ 2] = b;
        }
#else
  #if GIME_FUSED_BLIT
        // OPT: pack 4 source bytes into one aligned 32-bit store. The framebuffer
        // wants pixel p at byte p^2, so within each aligned quad the LE word is
        // p2 | p3<<8 | p0<<16 | p1<<24 (verified against the per-byte loop).
        if ((((uintptr_t)row) & 3) == 0 && (vp_w & 3) == 0) {
            volatile uint32_t* w = (volatile uint32_t*)row;
            for (int x = 0; x < vp_w; x += 4) {
                uint32_t p0 = pixels[x], p1 = pixels[x + 1];
                uint32_t p2 = pixels[x + 2], p3 = pixels[x + 3];
                w[x >> 2] = p2 | (p3 << 8) | (p0 << 16) | (p1 << 24);
            }
        } else
  #endif
        {
            for (int x = 0; x < vp_w; x++) {
                // pixels[] is already the raw VGA byte (GIME core emits it directly)
                row[x ^ 2] = pixels[x];
            }
        }
#endif
    } else if (width * 2 == vp_w) {
        x_out_start = 0;
        x_out_end = vp_w;
#if GIME_FUSED_BLIT
        // OPT: dup=2 packed blit — 4 source pixels -> 8 output cols -> 2 words.
        if ((((uintptr_t)row) & 3) == 0 && (width & 3) == 0) {
            volatile uint32_t* w = (volatile uint32_t*)row;
            for (int x = 0; x < width; x += 4) {
                uint32_t p0 = pixels[x], p1 = pixels[x + 1];
                uint32_t p2 = pixels[x + 2], p3 = pixels[x + 3];
                int wj = (x * 2) >> 2;
                w[wj]     = p1 | (p1 << 8) | (p0 << 16) | (p0 << 24);
                w[wj + 1] = p3 | (p3 << 8) | (p2 << 16) | (p2 << 24);
            }
        } else
#endif
        {
            for (int x = 0; x < width; x++) {
                uint8_t b = pixels[x];
                int dx0 = x * 2;
                row[dx0 ^ 2] = b;
                row[(dx0 + 1) ^ 2] = b;
            }
        }
    } else if (width < vp_w) {
        int x_off = (vp_w - width) / 2;
        x_out_start = x_off;
        x_out_end   = x_off + width;
#if GIME_FUSED_BLIT
        // OPT: centered blit in aligned 32-bit stores. Active span uses the same
        // p2|p3<<8|p0<<16|p1<<24 packing; borders are 4 identical bytes per word.
        // Requires x_off and width to be 4-aligned (true for the common 512@640);
        // otherwise fall through to the per-byte path below.
        if ((((uintptr_t)row) & 3) == 0 && (x_off & 3) == 0 && (width & 3) == 0 && (vp_w & 3) == 0) {
            volatile uint32_t* w = (volatile uint32_t*)row;
            uint32_t bw = (uint32_t)border_byte;
            bw |= (bw << 8) | (bw << 16) | (bw << 24);
            for (int o = 0; o < x_off; o += 4)          w[o >> 2] = bw;              // left border
            for (int x = 0; x < width; x += 4) {                                     // active
                uint32_t p0 = pixels[x], p1 = pixels[x + 1];
                uint32_t p2 = pixels[x + 2], p3 = pixels[x + 3];
                w[(x_off + x) >> 2] = p2 | (p3 << 8) | (p0 << 16) | (p1 << 24);
            }
            for (int o = x_off + width; o < vp_w; o += 4) w[o >> 2] = bw;            // right border
            return;
        }
#endif
        // Left border
        for (int x = 0; x < x_off; x++) row[x ^ 2] = border_byte;
        for (int x = 0; x < width; x++) {
            row[(x_off + x) ^ 2] = pixels[x];
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
            row[x ^ 2] = pixels[sx];
        }
    }
    (void)x_out_start; (void)x_out_end;
}

void hal_video_present_gime(bool* dirty) {
    // FabGL scans out continuously. No DMA push needed. FPS only.
    fps_tick_and_draw();
    vol_osd_tick_and_draw();
    if (dirty) *dirty = false;
}

void hal_video_force_repaint(void) { /* no-op on continuously-scanned VGA */ }

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

    fps_frame_count = 0;
    fps_last_time = millis();
}