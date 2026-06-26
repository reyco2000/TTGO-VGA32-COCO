/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : osd_canvas.h
 *  Module : OSD drawing API backed by FabGL Canvas (used by supervisor)
 * ============================================================
*/

#ifndef OSD_CANVAS_H
#define OSD_CANVAS_H

#include <Arduino.h>
#include <stdint.h>

// Text alignment datum constants
#define TL_DATUM 0
#define TC_DATUM 1
#define TR_DATUM 2
#define ML_DATUM 3
#define MC_DATUM 4
#define MR_DATUM 5
#define BL_DATUM 6
#define BC_DATUM 7
#define BR_DATUM 8

// RGB565 color constants used by the OSD
#define OSD_BLACK   0x0000
#define OSD_WHITE   0xFFFF
#define OSD_RED     0xF800
#define OSD_GREEN   0x07E0
#define OSD_BLUE    0x001F
#define OSD_YELLOW  0xFFE0
#define OSD_CYAN    0x07FF
#define OSD_MAGENTA 0xF81F
#define OSD_ORANGE  0xFD20
#define OSD_NAVY    0x000F

// Forward declarations
namespace fabgl {
    class Canvas;
    struct FontInfo;
}

class OSDCanvas {
public:
    // Bind the canvas to a FabGL Canvas. Called once by hal_video_init().
    // Until set, all draw methods are no-ops.
    static void bind_canvas(fabgl::Canvas* canvas);

    // Public color state read by supervisor render code
    uint16_t textcolor   = 0xFFFF;
    uint16_t textbgcolor = 0x0000;

    // ----- Lifecycle (no-op on continuously-scanned VGA) -----
    void init() {}
    void setRotation(uint8_t) {}
    void invertDisplay(bool) {}
    void fillScreen(uint16_t color);
    void startWrite() {}
    void endWrite() {}

    // ----- Primitives -----
    void drawPixel(int32_t x, int32_t y, uint16_t color);
    void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color);
    void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color);
    void drawFastHLine(int32_t x, int32_t y, int32_t w, uint16_t color);
    void drawFastVLine(int32_t x, int32_t y, int32_t h, uint16_t color);

    // ----- Text -----
    void setTextFont(uint8_t font);
    void setTextColor(uint16_t fg, uint16_t bg);
    void setTextDatum(uint8_t datum);
    void drawString(const char* str, int32_t x, int32_t y);
    void drawString(const String& s, int32_t x, int32_t y) { drawString(s.c_str(), x, y); }
    int16_t textWidth(const char* str);
    int16_t textWidth(const String& s) { return textWidth(s.c_str()); }

private:
    uint8_t  m_textDatum = TL_DATUM;
    uint8_t  m_textFont  = 2;

    const fabgl::FontInfo* current_font() const;
};

#endif // OSD_CANVAS_H
