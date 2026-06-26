/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : sv_render.cpp
 *  Module : OSD rendering engine — green phosphor UI using OSDCanvas built-in fonts
 * ============================================================
*/

/*
 * sv_render.cpp - OSD rendering engine
 *
 * Green phosphor aesthetic. Uses OSDCanvas built-in font 2 (16px).
 * Coordinates derived from DISPLAY_WIDTH/DISPLAY_HEIGHT in config.h.
 */

#include "sv_render.h"
#include <string.h>

static OSDCanvas* g_tft = nullptr;

void sv_render_init(OSDCanvas* tft) {
    g_tft = tft;
}

void sv_render_frame(const char* title, const char* footer) {
    if (!g_tft) return;

    g_tft->startWrite();

    // Fill background
    g_tft->fillRect(SV_BORDER_X, SV_BORDER_Y, SV_BORDER_W, SV_BORDER_H, SV_COLOR_BG);

    // Border
    g_tft->drawRect(SV_BORDER_X, SV_BORDER_Y, SV_BORDER_W, SV_BORDER_H, SV_COLOR_BORDER);

    // Title bar (inverse)
    g_tft->fillRect(SV_BORDER_X + 1, SV_BORDER_Y + 1, SV_BORDER_W - 2, SV_TITLE_H, SV_COLOR_TITLE_BG);
    g_tft->setTextFont(2);
    g_tft->setTextColor(SV_COLOR_TITLE_TEXT, SV_COLOR_TITLE_BG);
    g_tft->setTextDatum(TC_DATUM);
    g_tft->drawString(title, SV_BORDER_X + SV_BORDER_W / 2, SV_BORDER_Y + 2);

    // Footer (smaller font)
    if (footer) {
        int footer_y = SV_BORDER_Y + SV_BORDER_H - SV_FOOTER_H - 1;
        g_tft->drawFastHLine(SV_BORDER_X + 1, footer_y, SV_BORDER_W - 2, SV_COLOR_DIM);
        g_tft->setTextFont(0);  // compact 6x8 — keeps long hint strings on one line
        g_tft->setTextColor(SV_COLOR_DIM, SV_COLOR_BG);
        g_tft->setTextDatum(TC_DATUM);
        g_tft->drawString(footer, SV_BORDER_X + SV_BORDER_W / 2, footer_y + 4);
    }

    g_tft->setTextDatum(TL_DATUM);
    g_tft->endWrite();
}

void sv_render_menu_item(int index, const char* label, const char* value,
                         bool highlighted) {
    if (!g_tft) return;

    int y = SV_CONTENT_Y + index * SV_ITEM_H;
    int x = SV_CONTENT_X;

    g_tft->startWrite();

    if (highlighted) {
        g_tft->fillRect(SV_BORDER_X + 2, y, SV_BORDER_W - 4, SV_ITEM_H, SV_COLOR_HIGHLIGHT);
        g_tft->setTextColor(SV_COLOR_HL_TEXT, SV_COLOR_HIGHLIGHT);
    } else {
        g_tft->fillRect(SV_BORDER_X + 2, y, SV_BORDER_W - 4, SV_ITEM_H, SV_COLOR_BG);
        g_tft->setTextColor(SV_COLOR_TEXT, SV_COLOR_BG);
    }

    g_tft->setTextFont(2);
    g_tft->setTextDatum(TL_DATUM);

    // Cursor glyph
    if (highlighted) {
        g_tft->drawString(">", x - 2, y + 2);
    }

    // Label
    g_tft->drawString(label, x + 10, y + 2);

    // Value (right-aligned)
    if (value) {
        g_tft->setTextDatum(TR_DATUM);
        g_tft->drawString(value, SV_VALUE_RIGHT, y + 2);
        g_tft->setTextDatum(TL_DATUM);
    }

    g_tft->endWrite();
}

void sv_render_file_entry(int index, const char* name, uint32_t size,
                          bool is_dir, bool is_supported, bool highlighted) {
    if (!g_tft) return;

    int y = SV_CONTENT_Y + index * SV_ITEM_H;
    int x = SV_CONTENT_X;

    g_tft->startWrite();

    if (highlighted) {
        g_tft->fillRect(SV_BORDER_X + 2, y, SV_BORDER_W - 4, SV_ITEM_H, SV_COLOR_HIGHLIGHT);
        g_tft->setTextColor(SV_COLOR_HL_TEXT, SV_COLOR_HIGHLIGHT);
    } else {
        g_tft->fillRect(SV_BORDER_X + 2, y, SV_BORDER_W - 4, SV_ITEM_H, SV_COLOR_BG);
        g_tft->setTextColor(SV_COLOR_TEXT, SV_COLOR_BG);
    }

    g_tft->setTextFont(2);
    g_tft->setTextDatum(TL_DATUM);

    // Cursor
    if (highlighted) {
        g_tft->drawString(">", x - 2, y + 2);
    }

    // Icon character
    uint16_t icon_color;
    const char* icon;
    if (is_dir) {
        icon = "+";
        icon_color = highlighted ? SV_COLOR_HL_TEXT : SV_COLOR_DIR;
    } else if (is_supported) {
        icon = "*";
        icon_color = highlighted ? SV_COLOR_HL_TEXT : SV_COLOR_DISK;
    } else {
        icon = "!";
        icon_color = highlighted ? SV_COLOR_HL_TEXT : SV_COLOR_WARN;
    }

    uint16_t saved_fg = g_tft->textcolor;
    uint16_t saved_bg = g_tft->textbgcolor;
    g_tft->setTextColor(icon_color, highlighted ? SV_COLOR_HIGHLIGHT : SV_COLOR_BG);
    g_tft->drawString(icon, x + 10, y + 2);
    g_tft->setTextColor(saved_fg, saved_bg);

    // Filename
    g_tft->drawString(name, x + 22, y + 2);

    // File size (right-aligned, not for dirs)
    if (!is_dir) {
        char size_str[16];
        if (size >= 1024 * 1024) {
            snprintf(size_str, sizeof(size_str), "%lu MB", size / (1024 * 1024));
        } else {
            snprintf(size_str, sizeof(size_str), "%lu KB", size / 1024);
        }
        g_tft->setTextDatum(TR_DATUM);
        g_tft->drawString(size_str, SV_VALUE_RIGHT, y + 2);
        g_tft->setTextDatum(TL_DATUM);
    }

    g_tft->endWrite();
}

void sv_render_scrollbar(int visible_start, int visible_count, int total_count, int base_row) {
    if (!g_tft || total_count <= visible_count) return;

    int sb_x = SV_BORDER_X + SV_BORDER_W - 4;
    int sb_y = SV_CONTENT_Y + base_row * SV_ITEM_H;
    int sb_h = visible_count * SV_ITEM_H;

    // Background
    g_tft->drawFastVLine(sb_x, sb_y, sb_h, SV_COLOR_BG);

    // Thumb
    int thumb_h = (visible_count * sb_h) / total_count;
    if (thumb_h < 4) thumb_h = 4;
    int thumb_y = sb_y + (visible_start * sb_h) / total_count;

    g_tft->drawFastVLine(sb_x, thumb_y, thumb_h, SV_COLOR_DIM);
}

// Two-row drive status strip for the unified Disk Manager screen.
// Layout: row 0 = D0 (left) / D2 (right); row 1 = D1 (left) / D3 (right).
// The active drive's cell is highlighted; mounted cells use the disk color,
// empty cells are dimmed.
void sv_render_drive_strip(const char* cells[4], const bool mounted[4], int active) {
    if (!g_tft) return;

    const int half = SV_CONTENT_W / 2;

    g_tft->startWrite();
    g_tft->setTextFont(2);
    g_tft->setTextDatum(TL_DATUM);

    // Assumes a full-frame redraw precedes this (sv_render_frame clears the bg);
    // cells are not individually background-cleared to the border margins.
    for (int d = 0; d < 4; d++) {
        int row = d % 2;
        int col = d / 2;
        int x = SV_CONTENT_X + col * half;
        int y = SV_CONTENT_Y + row * SV_ITEM_H;
        bool hl = (d == active);

        uint16_t bg = hl ? SV_COLOR_HIGHLIGHT : SV_COLOR_BG;
        uint16_t fg = hl ? SV_COLOR_HL_TEXT
                         : (mounted[d] ? SV_COLOR_DISK : SV_COLOR_DIM);

        g_tft->fillRect(x, y, half, SV_ITEM_H, bg);
        g_tft->setTextColor(fg, bg);
        g_tft->drawString(cells[d] ? cells[d] : "", x + 4, y + 2);
    }

    // Separator line under the two-row strip.
    int sep_y = SV_CONTENT_Y + 2 * SV_ITEM_H;
    g_tft->drawFastHLine(SV_BORDER_X + 2, sep_y, SV_BORDER_W - 4, SV_COLOR_DIM);

    g_tft->setTextDatum(TL_DATUM);
    g_tft->endWrite();
}

void sv_render_status_line(const char* text, uint16_t color) {
    if (!g_tft) return;

    int y = SV_CONTENT_Y + 8 * SV_ITEM_H;
    g_tft->startWrite();
    g_tft->fillRect(SV_CONTENT_X, y, SV_CONTENT_W, SV_ITEM_H, SV_COLOR_BG);
    g_tft->setTextFont(2);
    g_tft->setTextColor(color, SV_COLOR_BG);
    g_tft->setTextDatum(TC_DATUM);
    g_tft->drawString(text, SV_BORDER_X + SV_BORDER_W / 2, y + 2);
    g_tft->setTextDatum(TL_DATUM);
    g_tft->endWrite();
}

void sv_render_confirm_dialog(const char* message, bool yes_highlighted) {
    if (!g_tft) return;

    int dw = 240, dh = 80;  // 20% wider than the original 200px
    int dx = (DISPLAY_WIDTH - dw) / 2;
    int dy = (DISPLAY_HEIGHT - dh) / 2;

    g_tft->startWrite();
    g_tft->fillRect(dx, dy, dw, dh, SV_COLOR_DIALOG_BG);
    g_tft->drawRect(dx, dy, dw, dh, SV_COLOR_BORDER);

    g_tft->setTextFont(2);
    g_tft->setTextColor(SV_COLOR_TEXT, SV_COLOR_DIALOG_BG);
    g_tft->setTextDatum(TC_DATUM);

    // Render the message across up to two lines, splitting on '\n'.
    const char* nl = strchr(message, '\n');
    if (nl) {
        char line1[48];
        size_t len = (size_t)(nl - message);
        if (len >= sizeof(line1)) len = sizeof(line1) - 1;
        memcpy(line1, message, len);
        line1[len] = '\0';
        g_tft->drawString(line1, dx + dw / 2, dy + 6);
        g_tft->drawString(nl + 1, dx + dw / 2, dy + 22);
    } else {
        g_tft->drawString(message, dx + dw / 2, dy + 10);
    }

    // Yes button
    int btn_y = dy + 45;
    if (yes_highlighted) {
        g_tft->fillRect(dx + 20, btn_y, 60, 20, SV_COLOR_HIGHLIGHT);
        g_tft->setTextColor(SV_COLOR_HL_TEXT, SV_COLOR_HIGHLIGHT);
    } else {
        g_tft->setTextColor(SV_COLOR_TEXT, SV_COLOR_DIALOG_BG);
    }
    g_tft->drawString("Yes", dx + 50, btn_y + 2);

    // No button
    if (!yes_highlighted) {
        g_tft->fillRect(dx + 120, btn_y, 60, 20, SV_COLOR_HIGHLIGHT);
        g_tft->setTextColor(SV_COLOR_HL_TEXT, SV_COLOR_HIGHLIGHT);
    } else {
        g_tft->setTextColor(SV_COLOR_TEXT, SV_COLOR_DIALOG_BG);
    }
    g_tft->drawString("No", dx + 150, btn_y + 2);

    g_tft->setTextDatum(TL_DATUM);
    g_tft->endWrite();
}

void sv_render_centered_item(int index, const char* text, uint16_t color) {
    if (!g_tft) return;
    int y = SV_CONTENT_Y + index * SV_ITEM_H;
    g_tft->startWrite();
    g_tft->fillRect(SV_BORDER_X + 2, y, SV_BORDER_W - 4, SV_ITEM_H, SV_COLOR_BG);
    g_tft->setTextFont(2);
    g_tft->setTextColor(color, SV_COLOR_BG);
    g_tft->setTextDatum(TC_DATUM);
    g_tft->drawString(text, SV_BORDER_X + SV_BORDER_W / 2, y + 2);
    g_tft->setTextDatum(TL_DATUM);
    g_tft->endWrite();
}

void sv_render_clear_content(void) {
    if (!g_tft) return;
    g_tft->fillRect(SV_BORDER_X + 2, SV_CONTENT_Y,
                    SV_BORDER_W - 4, SV_BORDER_H - SV_TITLE_H - SV_FOOTER_H - 6,
                    SV_COLOR_BG);
}

// ---- Mouse Sensitivity pad geometry (shared by full draw + cursor update) ----
static const int JOYPAD_SIDE = 96;   // pad box side, pixels
static const int JOYPAD_CUR  = 6;    // cursor square side, pixels

static inline int joypad_box_x(void) { return SV_BORDER_X + (SV_BORDER_W - JOYPAD_SIDE) / 2; }
static inline int joypad_box_y(void) { return SV_BORDER_Y + SV_TITLE_H + 10; }

// Logical axis (0..63) -> pixel offset of the cursor square inside the box.
static inline int joypad_cur_offset(uint8_t v) {
    int inner = JOYPAD_SIDE - 2 - JOYPAD_CUR;   // travel range in px
    return 1 + (int)v * inner / 63;
}

// Full repaint of the pad: box, cursor at the current position, sensitivity
// bar, and Invert-Y value. Called once on open and whenever level/invert
// change — NOT per frame. Between frames the cursor is moved incrementally by
// sv_render_joystick_cursor() so there is no flicker.
void sv_render_joystick_pad(uint8_t cursor_x, uint8_t cursor_y,
                            uint8_t level, bool invert_y) {
    if (!g_tft) return;

    int box_x = joypad_box_x();
    int box_y = joypad_box_y();

    g_tft->startWrite();

    g_tft->fillRect(box_x, box_y, JOYPAD_SIDE, JOYPAD_SIDE, SV_COLOR_BG);
    g_tft->drawRect(box_x, box_y, JOYPAD_SIDE, JOYPAD_SIDE, SV_COLOR_BORDER);

    // Cursor square at the current position.
    int cx = box_x + joypad_cur_offset(cursor_x);
    int cy = box_y + joypad_cur_offset(cursor_y);
    g_tft->fillRect(cx, cy, JOYPAD_CUR, JOYPAD_CUR, SV_COLOR_TEXT);

    // --- Sensitivity bar + numeric level (compact 8x8 font) ---
    int row_y = box_y + JOYPAD_SIDE + 8;
    g_tft->setTextFont(1);
    g_tft->setTextColor(SV_COLOR_TEXT, SV_COLOR_BG);
    g_tft->setTextDatum(TL_DATUM);
    g_tft->drawString("Sensitivity:", SV_CONTENT_X, row_y);

    // Ten segment bar, filled up to `level`.
    const int SEG_W = 10, SEG_H = 10, GAP = 2;
    int bar_x = SV_CONTENT_X + 96;
    for (int i = 0; i < 10; i++) {
        int x = bar_x + i * (SEG_W + GAP);
        uint16_t c = (i < level) ? SV_COLOR_BORDER : SV_COLOR_BG;
        g_tft->fillRect(x, row_y, SEG_W, SEG_H, c);
        g_tft->drawRect(x, row_y, SEG_W, SEG_H, SV_COLOR_DIM);
    }

    char lvl[8];
    snprintf(lvl, sizeof(lvl), "%u", (unsigned)level);
    g_tft->setTextDatum(TR_DATUM);
    g_tft->drawString(lvl, SV_VALUE_RIGHT, row_y);

    // --- Invert-Y value ---
    int inv_y = row_y + SV_ITEM_H + 2;
    g_tft->setTextDatum(TL_DATUM);
    g_tft->drawString("Invert Y:", SV_CONTENT_X, inv_y);
    g_tft->setTextDatum(TR_DATUM);
    g_tft->drawString(invert_y ? "ON" : "OFF", SV_VALUE_RIGHT, inv_y);

    g_tft->setTextDatum(TL_DATUM);
    g_tft->endWrite();
}

// Incremental cursor move: erase the cursor square at its old logical position
// and redraw it at the new one. The pad box border is never touched (the cursor
// stays >=1px inside), so no full repaint — and thus no flicker — is needed.
// Pass old_x/old_y > 63 (e.g. 0xFF) to skip the erase on the first draw.
void sv_render_joystick_cursor(uint8_t old_x, uint8_t old_y,
                               uint8_t new_x, uint8_t new_y) {
    if (!g_tft) return;
    int box_x = joypad_box_x();
    int box_y = joypad_box_y();

    g_tft->startWrite();
    if (old_x <= 63 && old_y <= 63) {
        int ox = box_x + joypad_cur_offset(old_x);
        int oy = box_y + joypad_cur_offset(old_y);
        g_tft->fillRect(ox, oy, JOYPAD_CUR, JOYPAD_CUR, SV_COLOR_BG);
    }
    int nx = box_x + joypad_cur_offset(new_x);
    int ny = box_y + joypad_cur_offset(new_y);
    g_tft->fillRect(nx, ny, JOYPAD_CUR, JOYPAD_CUR, SV_COLOR_TEXT);
    g_tft->endWrite();
}
