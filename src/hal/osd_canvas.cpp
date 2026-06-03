/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : osd_canvas.cpp
 *  Module : OSD drawing API backed by FabGL Canvas
 * ============================================================
*/

#include "osd_canvas.h"
#include "fabgl.h"

static fabgl::Canvas* s_canvas = nullptr;

// RGB565 → RGB888 with bit replication.
static inline fabgl::RGB888 rgb565_to_888(uint16_t c) {
    uint8_t r5 = (c >> 11) & 0x1F;
    uint8_t g6 = (c >>  5) & 0x3F;
    uint8_t b5 =  c        & 0x1F;
    uint8_t r = (r5 << 3) | (r5 >> 2);
    uint8_t g = (g6 << 2) | (g6 >> 4);
    uint8_t b = (b5 << 3) | (b5 >> 2);
    return fabgl::RGB888(r, g, b);
}

void OSDCanvas::bind_canvas(fabgl::Canvas* canvas) {
    s_canvas = canvas;
}

void OSDCanvas::fillScreen(uint16_t color) {
    if (!s_canvas) return;
    s_canvas->setBrushColor(rgb565_to_888(color));
    s_canvas->clear();
}

void OSDCanvas::drawPixel(int32_t x, int32_t y, uint16_t color) {
    if (!s_canvas) return;
    s_canvas->setPixel(x, y, rgb565_to_888(color));
}

void OSDCanvas::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
    if (!s_canvas || w <= 0 || h <= 0) return;
    s_canvas->setBrushColor(rgb565_to_888(color));
    s_canvas->fillRectangle(x, y, x + w - 1, y + h - 1);
}

void OSDCanvas::drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
    if (!s_canvas || w <= 0 || h <= 0) return;
    s_canvas->setPenColor(rgb565_to_888(color));
    s_canvas->drawRectangle(x, y, x + w - 1, y + h - 1);
}

void OSDCanvas::drawFastHLine(int32_t x, int32_t y, int32_t w, uint16_t color) {
    if (!s_canvas || w <= 0) return;
    s_canvas->setPenColor(rgb565_to_888(color));
    s_canvas->drawLine(x, y, x + w - 1, y);
}

void OSDCanvas::drawFastVLine(int32_t x, int32_t y, int32_t h, uint16_t color) {
    if (!s_canvas || h <= 0) return;
    s_canvas->setPenColor(rgb565_to_888(color));
    s_canvas->drawLine(x, y, x, y + h - 1);
}

void OSDCanvas::setTextFont(uint8_t font) {
    m_textFont = font;
}

void OSDCanvas::setTextColor(uint16_t fg, uint16_t bg) {
    textcolor   = fg;
    textbgcolor = bg;
}

void OSDCanvas::setTextDatum(uint8_t datum) {
    m_textDatum = datum;
}

const fabgl::FontInfo* OSDCanvas::current_font() const {
    // Font 0 = compact 6x8 (used by debug hex dump for density).
    // Font 1 ≈ small (8x8), font 2 ≈ large (8x14).
    if (m_textFont == 0) return &fabgl::FONT_6x8;
    if (m_textFont == 1) return &fabgl::FONT_8x8;
    return &fabgl::FONT_8x14;
}

int16_t OSDCanvas::textWidth(const char* str) {
    if (!str) return 0;
    const fabgl::FontInfo* f = current_font();
    int n = 0;
    while (str[n]) n++;
    return (int16_t)(n * f->width);
}

void OSDCanvas::drawString(const char* str, int32_t x, int32_t y) {
    if (!s_canvas || !str) return;

    const fabgl::FontInfo* f = current_font();
    int n = 0;
    while (str[n]) n++;
    int w = n * f->width;

    int draw_x = x;
    int draw_y = y;
    switch (m_textDatum) {
        case TC_DATUM: draw_x = x - w / 2; break;
        case TR_DATUM: draw_x = x - w;     break;
        case ML_DATUM: draw_y = y - f->height / 2; break;
        case MC_DATUM: draw_x = x - w / 2; draw_y = y - f->height / 2; break;
        case MR_DATUM: draw_x = x - w;     draw_y = y - f->height / 2; break;
        case BL_DATUM: draw_y = y - f->height; break;
        case BC_DATUM: draw_x = x - w / 2; draw_y = y - f->height; break;
        case BR_DATUM: draw_x = x - w;     draw_y = y - f->height; break;
        case TL_DATUM:
        default: break;
    }

    s_canvas->setPenColor(rgb565_to_888(textcolor));
    s_canvas->setBrushColor(rgb565_to_888(textbgcolor));
    s_canvas->setGlyphOptions(fabgl::GlyphOptions().FillBackground(true));
    s_canvas->drawText(f, draw_x, draw_y, str);
    s_canvas->setGlyphOptions(fabgl::GlyphOptions());
}
