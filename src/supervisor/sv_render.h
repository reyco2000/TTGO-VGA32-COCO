/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : sv_render.h
 *  Module : OSD rendering engine interface
 * ============================================================
*/

/*
 * sv_render.h - OSD rendering engine for supervisor
 *
 * CoCo green phosphor aesthetic. Renders into the VGA framebuffer via OSDCanvas.
 */

#ifndef SV_RENDER_H
#define SV_RENDER_H

#include <stdint.h>
#include "../../config.h"
#include "../hal/osd_canvas.h"

// Color theme — blue
#define SV_COLOR_BG          0x0000  // black
#define SV_COLOR_TEXT         0xFFE0  //Yellow 
#define SV_COLOR_HIGHLIGHT   0x5DDF  // Bright blue-white bg
#define SV_COLOR_HL_TEXT     0xFFE0  // Yellow
#define SV_COLOR_DIM         0xFFFF  // white
#define SV_COLOR_BORDER      0x5DDF  // Bright blue-white
#define SV_COLOR_TITLE_BG    0x5DDF
#define SV_COLOR_TITLE_TEXT  0xFFFF // whithe 
#define SV_COLOR_DIR         0xFFE0  // Yellow
#define SV_COLOR_DISK        0x07FF  // Cyan
#define SV_COLOR_WARN        0xF800  // Red
#define SV_COLOR_DIALOG_BG   0x0000  // Black

// Layout constants — centered on display, derived from config.h
#include "../../config.h"

// OSD area matches the 256x192 VDG active area, centered in the 640x200 VGA frame.
#define SV_BORDER_W     256
#define SV_BORDER_H     192
#define SV_ITEM_H       18
#define SV_BORDER_X     ((DISPLAY_WIDTH - SV_BORDER_W) / 2)
#define SV_BORDER_Y     ((DISPLAY_HEIGHT - SV_BORDER_H) / 2)
#define SV_TITLE_H      18
#define SV_FOOTER_H     16
#define SV_CONTENT_X    (SV_BORDER_X + 8)
#define SV_CONTENT_Y    (SV_BORDER_Y + SV_TITLE_H + 4)
#define SV_CONTENT_W    (SV_BORDER_W - 16)
#define SV_VALUE_RIGHT  (SV_BORDER_X + SV_BORDER_W - 8)

void sv_render_init(OSDCanvas* canvas);
void sv_render_frame(const char* title, const char* footer);
void sv_render_menu_item(int index, const char* label, const char* value,
                         bool highlighted);
void sv_render_file_entry(int index, const char* name, uint32_t size,
                          bool is_dir, bool is_supported, bool highlighted);
void sv_render_scrollbar(int visible_start, int visible_count, int total_count, int base_row = 0);

// Two-row drive status strip for the unified Disk Manager screen.
// cells[d] = display text for drive d; mounted[d] = whether a disk is mounted;
// active = the drive index currently selected (its cell is highlighted).
void sv_render_drive_strip(const char* cells[4], const bool mounted[4], int active);

void sv_render_status_line(const char* text, uint16_t color);
void sv_render_confirm_dialog(const char* message, bool yes_highlighted);
void sv_render_clear_content(void);
void sv_render_centered_item(int index, const char* text, uint16_t color);

// Mouse Sensitivity screen: draws the pad box with a live cursor at
// (cursor_x, cursor_y) in 0..63 logical space, plus the sensitivity bar
// (level 1..10) and the Invert-Y value. Caller renders the frame first.
void sv_render_joystick_pad(uint8_t cursor_x, uint8_t cursor_y,
                            uint8_t level, bool invert_y);

// Incremental cursor move for the Mouse Sensitivity pad: erases the cursor at
// its old logical position (0..63) and redraws it at the new one, without
// repainting the box/bar/labels (avoids flicker). Pass old coords > 63 to skip
// the erase on the first draw.
void sv_render_joystick_cursor(uint8_t old_x, uint8_t old_y,
                               uint8_t new_x, uint8_t new_y);

#endif // SV_RENDER_H
