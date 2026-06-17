#pragma GCC optimize("O2")
/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
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

// Phase 1 active-pixel LUT: byte-swapped RGB565 (as emitted by the GIME core,
// OPT-C4) → FabGL raw VGA byte. Collapses the per-pixel
//   bswap16 → rgb565_to_rgb222 → createRawPixel
// conversion chain to a single indexed load in the scanline loop. 64 KB in
// internal DRAM; built once in init_gime_lut().
static uint8_t              s_gime_pixel_raw_lut[65536];

// FPS overlay state
static bool     fps_overlay_enabled = false;
static uint32_t fps_frame_count = 0;
static uint32_t fps_last_time = 0;
static float    fps_value = 0.0f;

// Tick + render the FPS overlay according to FPS_OVERLAY_MODE in config.h.
// Called from hal_video_present() / hal_video_present_gime() once per frame.
static void fps_tick_and_draw(void) {
    if (!fps_overlay_enabled) return;
    fps_frame_count++;
    uint32_t now = millis();
    uint32_t elapsed = now - fps_last_time;
    if (elapsed >= 1000) {
        fps_value = (float)fps_frame_count * 1000.0f / (float)elapsed;
        fps_frame_count = 0;
        fps_last_time = now;
#if (FPS_OVERLAY_MODE & FPS_OVERLAY_SERIAL)
        DEBUG_PRINTF("FPS: %.1f", fps_value);
#endif
    }
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

// VDG (CoCo 2) scanline: 256 px wide @ palette indices, centered in 640x200.
void hal_video_render_scanline(int line, const uint8_t* pixels, int width) {
    if (!display_available || !pixels) return;
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
}

// GIME (CoCo 3) scanline: pre-converted RGB565 from tcc1014 — convert each
// pixel to a raw VGA byte. Width is 320 or 640 (post-OPT-C4).
void hal_video_render_scanline_gime(int line, int total_lines,
                                     uint8_t border_colour,
                                     const uint16_t* pixels,
                                     int width, const uint16_t* palette) {
    (void)palette;
    if (!display_available || !pixels || width <= 0) return;
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

void hal_video_present_gime(bool* dirty) {
    // FabGL scans out continuously. No DMA push needed. FPS only.
    fps_tick_and_draw();
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

