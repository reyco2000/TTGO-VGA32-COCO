/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : sv_menu.cpp
 *  Module : Supervisor OSD main menu — disk/drive options, reset, and settings
 * ============================================================
*/

/*
 * sv_menu.cpp - Main menu for supervisor OSD
 */

#include "sv_menu.h"
#include "supervisor.h"
#include "sv_filebrowser.h"
#include "sv_render.h"
#include "sv_debug.h"
#include "sv_keymap.h"
#include "sv_joystick.h"
#include "sv_wifi.h"
#include "sv_fujinet.h"
#include "sv_cart.h"
#include "../core/orch90.h"
#include "../core/ssc/ssc.h"
#include "../core/vhd.h"
#include "../core/tcc1014.h"
#include "../core/gime_monitor.h"
#include "../core/gime_artifact.h"
#include "../net/dw_bus.h"
#include "../net/wifi_mgr.h"
#include "../hal/hal.h"
#include "../../config.h"

// External canvas instance from hal_video.cpp
extern OSDCanvas* hal_video_get_canvas(void);

// HID usage codes
#define HID_UP    0x52
#define HID_DOWN  0x51
#define HID_LEFT  0x50
#define HID_RIGHT 0x4F
#define HID_ENTER 0x28
#define HID_ESC   0x29
#define HID_F1    0x3A

// ============================================================
// Main menu — 4x2 grid of icon tiles
// ============================================================
//
// Drawn in the wide green frame (sv_render_wide_frame). 640x200 pixels are
// ~2.4x taller than wide, so tiles and icons are stretched horizontally to
// look square.

#define TILE_W       128
#define TILE_H       58
#define TILE_PITCH_X 136
#define TILE_PITCH_Y 66
#define TILE_COLS    4
#define TILE_X0      (SVW_BOX_X + (SVW_BOX_W - ((TILE_COLS - 1) * TILE_PITCH_X + TILE_W)) / 2)
#define TILE_Y0      (SVW_BOX_Y + 36)
#define ICON_W       64      // 32 units, each 2 pixels wide
#define ICON_H       28

struct SV_Tile {
    const char*   label;
    SV_MenuAction action;
};

static const SV_Tile tiles[] = {
    { "Disks",  SV_ACT_MOUNT_DISK },
    { "Cart",   SV_ACT_LOAD_CART  },
    { "Setup",  SV_ACT_SETTINGS   },
    { "WiFi",   SV_ACT_WIFI       },
    { "Reset",  SV_ACT_RESET      },
    { "Debug",  SV_ACT_DEBUG      },
    { "About",  SV_ACT_ABOUT      },
    { "Resume", SV_ACT_RESUME     },
};

static const int TILE_COUNT = sizeof(tiles) / sizeof(tiles[0]);

// Tile last drawn as selected; -1 = nothing on screen, draw the whole menu.
static int8_t s_tile_drawn = -1;

void sv_menu_init(Supervisor_t* sv) {
    sv->menu_cursor = 0;
    sv->menu_scroll_offset = 0;
    sv->menu_item_count = TILE_COUNT;
}

void sv_menu_invalidate(void) {
    s_tile_drawn = -1;
}

// Machine-select submenu (2 options: CoCo 2 / CoCo 3).
static const char* const MACHINE_LABELS[2] = { MACHINE_NAME_COCO2, MACHINE_NAME_COCO3 };
static const uint8_t      MACHINE_VALUES[2] = { 3, 4 };
// Stashed during the confirm dialog so the callback knows which target to set.
static uint8_t pending_machine_type = 0;
// Saved main-menu cursor/count so ESC from the submenu restores the caller.
static int8_t saved_main_menu_cursor = 0;
static uint8_t saved_main_menu_count = 0;

static void restore_main_menu(Supervisor_t* sv) {
    sv->state = SV_MAIN_MENU;
    sv->menu_cursor = saved_main_menu_cursor;
    sv->menu_item_count = saved_main_menu_count;
    sv->needs_redraw = true;
}

void sv_menu_return(Supervisor_t* sv) {
    restore_main_menu(sv);
}

static void execute_action(Supervisor_t* sv, SV_MenuAction action) {
    switch (action) {
        case SV_ACT_LOAD_CART:
            saved_main_menu_cursor = sv->menu_cursor;
            saved_main_menu_count  = sv->menu_item_count;
            sv->prev_state = sv->state;
            sv_cart_open(sv);
            break;

        case SV_ACT_WIFI:
            saved_main_menu_cursor = sv->menu_cursor;
            saved_main_menu_count  = sv->menu_item_count;
            sv->prev_state = sv->state;
            sv_wifi_open(sv, true);
            break;

        case SV_ACT_MOUNT_DISK:
            sv->target_drive = 0;
            sv->prev_state = sv->state;
            sv->state = SV_FILE_BROWSER;
            sv_filebrowser_open(sv, sv->current_path, sv->target_drive);
            break;

        case SV_ACT_SETTINGS:
            // Save main-menu state so ESC restores it.
            saved_main_menu_cursor = sv->menu_cursor;
            saved_main_menu_count  = sv->menu_item_count;
            sv->prev_state = sv->state;
            sv->state = SV_SETTINGS;
            sv->menu_cursor = 0;
            sv->needs_redraw = true;
            break;

        case SV_ACT_RESET:
            sv->prev_state = sv->state;
            sv->state = SV_CONFIRM_DIALOG;
            sv->confirm_message = "Reset machine?";
            sv->confirm_yes_selected = false;
            sv->confirm_callback = [](bool accepted, void* ctx) {
                Supervisor_t* s = (Supervisor_t*)ctx;
                if (accepted && s->machine) {
                    // Flush dirty disk caches to SD before reset — a reset
                    // wipes FDC state and would otherwise lose pending writes.
                    sv_disk_flush_all(&s->machine->fdc);
                    machine_reset(s->machine);
                }
                // Close through the toggle so the main-menu box is cleared.
                supervisor_toggle();
            };
            sv->confirm_context = sv;
            sv->needs_redraw = true;
            break;

        case SV_ACT_DEBUG:
            // Save main-menu state so ESC from the submenu restores it.
            saved_main_menu_cursor = sv->menu_cursor;
            saved_main_menu_count  = sv->menu_item_count;
            sv->prev_state = sv->state;
            sv->state = SV_DEBUG_MENU;
            sv->menu_cursor = 0;
            sv->menu_item_count = SV_DBG_PAGE_COUNT;
            sv->needs_redraw = true;
            break;

        case SV_ACT_ABOUT:
            sv->prev_state = sv->state;
            sv->state = SV_ABOUT;
            sv->needs_redraw = true;
            break;

        case SV_ACT_RESUME:
            supervisor_toggle();
            break;

        default:
            break;
    }
}

void sv_menu_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;

    int sel = sv->menu_cursor;
    switch (hid_usage) {
        case HID_LEFT:  if (sel % TILE_COLS > 0) sel--; break;
        case HID_RIGHT: if (sel % TILE_COLS < TILE_COLS - 1 && sel < TILE_COUNT - 1) sel++; break;
        case HID_UP:    if (sel >= TILE_COLS) sel -= TILE_COLS; break;
        case HID_DOWN:  if (sel + TILE_COLS < TILE_COUNT) sel += TILE_COLS; break;

        case HID_ENTER:
            DEBUG_PRINTF("menu: SUPERVISOR > %s (open)", tiles[sel].label);
            execute_action(sv, tiles[sel].action);
            return;

        case HID_ESC:
        case HID_F1:
            supervisor_toggle();
            return;
    }
    if (sel != sv->menu_cursor) {
        sv->menu_cursor = sel;
        sv->needs_redraw = true;
    }
}

// One icon rectangle in 32x28 unit coordinates (inclusive corners).
static inline void icon_box(OSDCanvas* tft, int x, int y, int ux1, int uy1,
                            int ux2, int uy2, uint16_t color) {
    tft->fillRect(x + ux1 * 2, y + uy1, (ux2 - ux1 + 1) * 2, uy2 - uy1 + 1, color);
}

// 64x28 icons drawn from rectangles, ellipses and lines; bg/fg are the tile's
// own colours so each icon reads on both the white and the selected tile.
static void draw_icon(OSDCanvas* tft, SV_MenuAction action, int x, int y,
                      uint16_t bg, uint16_t fg) {
    switch (action) {
        case SV_ACT_MOUNT_DISK:   // 5.25" diskette: jacket, label, hub, head slot
            icon_box(tft, x, y, 4, 1, 27, 26, SVW_DKGRAY);
            icon_box(tft, x, y, 7, 3, 20, 7, SVW_WHITE);
            icon_box(tft, x, y, 7, 3, 20, 4, SVW_RED);
            icon_box(tft, x, y, 26, 6, 27, 8, bg);
            tft->fillEllipse(x + 32, y + 14, 22, 10, SVW_GRAY);
            tft->fillEllipse(x + 32, y + 14, 10, 4, SVW_BLACK);
            icon_box(tft, x, y, 15, 20, 16, 24, SVW_BLACK);
            break;

        case SV_ACT_LOAD_CART:    // game cartridge: shell, grip ridges, label, edge connector
            icon_box(tft, x, y, 5, 1, 26, 21, SVW_DKGRAY);
            icon_box(tft, x, y, 8, 3, 23, 3, SVW_GRAY);
            icon_box(tft, x, y, 8, 5, 23, 5, SVW_GRAY);
            icon_box(tft, x, y, 8, 8, 23, 18, SVW_WHITE);
            icon_box(tft, x, y, 8, 8, 23, 10, SVW_RED);
            icon_box(tft, x, y, 10, 13, 21, 13, SVW_DKGRAY);
            icon_box(tft, x, y, 10, 15, 17, 15, SVW_DKGRAY);
            icon_box(tft, x, y, 9, 22, 22, 26, SVW_ORANGE);
            for (int c = 0; c < 4; c++) icon_box(tft, x, y, 11 + c * 3, 23, 11 + c * 3, 26, SVW_BLACK);
            break;

        case SV_ACT_WIFI:         // signal bars, rising left to right
            for (int b = 0; b < 4; b++) {
                icon_box(tft, x, y, 5 + b * 6, 21 - b * 6, 8 + b * 6, 26, fg);
            }
            break;

        case SV_ACT_SETTINGS: {   // three sliders
            static const uint8_t knob[3] = { 7, 19, 12 };
            for (int r = 0; r < 3; r++) {
                int uy = 5 + r * 8;
                icon_box(tft, x, y, 3, uy, 28, uy + 1, fg);
                icon_box(tft, x, y, knob[r], uy - 3, knob[r] + 5, uy + 4, SVW_LEAF);
            }
            break;
        }

        case SV_ACT_RESET:        // power symbol: ring open at the top, bar through the gap
            tft->fillEllipse(x + 32, y + 15, 52, 23, SVW_ORANGE);
            tft->fillEllipse(x + 32, y + 15, 36, 15, bg);
            icon_box(tft, x, y, 11, 1, 20, 11, bg);
            icon_box(tft, x, y, 14, 1, 17, 14, SVW_ORANGE);
            break;

        case SV_ACT_DEBUG: {      // ladybug: legs, antennae, head, red shell with spots
            static const int8_t legs[3][4] = { { 14, 11, 4, 6 }, { 11, 17, 1, 17 }, { 14, 23, 4, 27 } };
            for (int l = 0; l < 3; l++) {
                for (int t = 0; t < 2; t++) {       // two pixels thick
                    tft->drawLine(x + legs[l][0] + t, y + legs[l][1], x + legs[l][2] + t, y + legs[l][3], fg);
                    tft->drawLine(x + 63 - legs[l][0] - t, y + legs[l][1], x + 63 - legs[l][2] - t, y + legs[l][3], fg);
                }
            }
            for (int t = 0; t < 2; t++) {
                tft->drawLine(x + 27 + t, y + 4, x + 21 + t, y, fg);
                tft->drawLine(x + 36 - t, y + 4, x + 42 - t, y, fg);
            }
            tft->fillEllipse(x + 32, y + 6, 20, 9, fg);
            tft->fillRect(x + 27, y + 4, 2, 2, bg);
            tft->fillRect(x + 35, y + 4, 2, 2, bg);
            tft->fillEllipse(x + 32, y + 17, 44, 20, 0xF800);
            tft->fillRect(x + 31, y + 8, 2, 19, SVW_BLACK);
            static const int8_t spots[6][2] = { { 21, 13 }, { 43, 13 }, { 17, 19 }, { 47, 19 }, { 25, 23 }, { 39, 23 } };
            for (int i = 0; i < 6; i++) {
                tft->fillEllipse(x + spots[i][0], y + spots[i][1], 8, 4, SVW_BLACK);
            }
            break;
        }

        case SV_ACT_ABOUT:        // "i" in a disc
            tft->fillEllipse(x + 32, y + 14, 58, 27, SVW_TEAL);
            icon_box(tft, x, y, 15, 5, 16, 7, SVW_WHITE);
            icon_box(tft, x, y, 15, 10, 16, 22, SVW_WHITE);
            break;

        case SV_ACT_RESUME:       // play triangle
            for (int r = -12; r <= 12; r++) {
                int len = (12 - abs(r)) * 20 / 12;
                icon_box(tft, x, y, 7, 14 + r, 7 + len, 14 + r, SVW_PURPLE);
            }
            break;

        default:
            break;
    }
}

static void draw_tile(OSDCanvas* tft, int idx, bool selected) {
    int x = TILE_X0 + (idx % TILE_COLS) * TILE_PITCH_X;
    int y = TILE_Y0 + (idx / TILE_COLS) * TILE_PITCH_Y;
    uint16_t bg = selected ? SVW_DKBLUE : SVW_WHITE;
    uint16_t fg = selected ? SVW_WHITE : SVW_BLACK;
    uint16_t edge = selected ? SVW_BLACK : SVW_DKBLUE;

    tft->fillRect(x, y, TILE_W, TILE_H, bg);
    tft->drawRect(x, y, TILE_W, TILE_H, edge);
    if (selected) tft->drawRect(x + 1, y + 1, TILE_W - 2, TILE_H - 2, edge);

    draw_icon(tft, tiles[idx].action, x + (TILE_W - ICON_W) / 2, y + 6, bg, fg);

    tft->setTextFont(2);
    tft->setTextColor(fg, bg);
    tft->setTextDatum(TC_DATUM);
    tft->drawString(tiles[idx].label, x + TILE_W / 2, y + 39);
    tft->setTextDatum(TL_DATUM);
}

void sv_menu_render(Supervisor_t* sv) {
    OSDCanvas* tft = hal_video_get_canvas();
    if (!tft) return;

    int sel = sv->menu_cursor;
    if (sel < 0 || sel >= TILE_COUNT) sel = sv->menu_cursor = 0;

    // First pass draws everything; afterwards only the two tiles that changed.
    if (s_tile_drawn < 0) {
        sv_render_wide_frame("COCO SUPERVISOR", "Arrows   ENTER Select   ESC/F3 Exit");
        for (int i = 0; i < TILE_COUNT; i++) draw_tile(tft, i, i == sel);
    } else if (s_tile_drawn != sel) {
        draw_tile(tft, s_tile_drawn, false);
        draw_tile(tft, sel, true);
    }
    s_tile_drawn = sel;
    DEBUG_PRINTF("menu: SUPERVISOR > %s", tiles[sel].label);
}

// ============================================================
// Machine select — popup over the Machine submenu
// ============================================================
//
// Two steps in the same window: pick CoCo 2 / CoCo 3, then confirm the
// restart (No / Yes, defaults to No). Accepting persists the choice in NVS
// and restarts; ESC goes back one step.

static bool   s_machine_confirming = false;
// Row last drawn as selected; -1 = draw the whole window.
static int8_t s_machine_drawn = -1;

void sv_machine_select_invalidate(void) {
    s_machine_drawn = -1;
}

// Opened from the Machine submenu's "Machine" row; ESC and a no-op select
// return there.
static void restore_settings(Supervisor_t* sv) {
    sv->state = SV_MACHINE_MENU;
    sv->menu_cursor = SV_MACH_TYPE;
    sv->needs_redraw = true;
}

static void machine_select_open(Supervisor_t* sv) {
    sv->prev_state = sv->state;
    sv->state = SV_MACHINE_SELECT;
    // Seed cursor on the currently-active machine so ENTER on it is a no-op.
    sv->menu_cursor = (g_machine_type == 4) ? 1 : 0;
    s_machine_confirming = false;
    s_machine_drawn = -1;
    sv->needs_redraw = true;
}

void sv_machine_select_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;

    switch (hid_usage) {
        case HID_UP:
            if (sv->menu_cursor > 0) {
                sv->menu_cursor--;
                sv->needs_redraw = true;
            }
            break;

        case HID_DOWN:
            if (sv->menu_cursor < 1) {
                sv->menu_cursor++;
                sv->needs_redraw = true;
            }
            break;

        case HID_ENTER:
            if (s_machine_confirming) {
                if (sv->menu_cursor == 1) {
                    // Persists selection in NVS and calls esp_restart() — no return.
                    supervisor_set_machine_type(pending_machine_type);
                }
                restore_settings(sv);   // No
                break;
            }
            if (MACHINE_VALUES[sv->menu_cursor] == g_machine_type) {
                // Already active — go back to Settings without restarting.
                restore_settings(sv);
                break;
            }
            pending_machine_type = MACHINE_VALUES[sv->menu_cursor];
            s_machine_confirming = true;
            sv->menu_cursor = 0;        // No
            s_machine_drawn = -1;
            sv->needs_redraw = true;
            break;

        case HID_ESC:
            if (s_machine_confirming) {
                // Back to the machine list, on the machine that was picked.
                s_machine_confirming = false;
                sv->menu_cursor = (pending_machine_type == 4) ? 1 : 0;
                s_machine_drawn = -1;
                sv->needs_redraw = true;
            } else {
                restore_settings(sv);
            }
            break;

        case HID_F1:
            supervisor_toggle();
            break;
    }
}

static void draw_machine_row(int y, int row, bool highlighted) {
    const char* label;
    const char* value = NULL;
    if (s_machine_confirming) {
        label = (row == 0) ? "No" : "Yes";
    } else {
        label = MACHINE_LABELS[row];
        if (MACHINE_VALUES[row] == g_machine_type) value = "(current)";
    }
    sv_render_popup_row(y, label, value, highlighted);
    if (highlighted) DEBUG_PRINTF("popup: Machine > %s %s", label, value ? value : "");
}

void sv_machine_select_render(Supervisor_t* sv) {
    // Both steps carry one message line, so the window keeps its size.
    const char* title = s_machine_confirming ? "Restart emulator?" : "Select Machine";
    const char* msg   = !s_machine_confirming ? "Changing machine restarts the emulator."
                      : (pending_machine_type == 4) ? "Switch to CoCo 3 and restart?"
                                                    : "Switch to CoCo 2 and restart?";
    int sel = sv->menu_cursor;
    bool all = (s_machine_drawn < 0);
    int ry = sv_render_popup(title, msg, NULL, 2, all);

    if (all) {
        for (int i = 0; i < 2; i++) draw_machine_row(ry + i * SVP_ROW_H, i, i == sel);
    } else if (s_machine_drawn != sel) {
        draw_machine_row(ry + s_machine_drawn * SVP_ROW_H, s_machine_drawn, false);
        draw_machine_row(ry + sel * SVP_ROW_H, sel, true);
    }
    s_machine_drawn = sel;
}

// ============================================================
// Row icons — shared by the Settings, Keyboard and Debug lists
// ============================================================

// 32x12 row icons on a 16x12 unit grid (inclusive corners, 2-pixel units).
static inline void row_icon_box(OSDCanvas* tft, int x, int y, int ux1, int uy1,
                                int ux2, int uy2, uint16_t color) {
    tft->fillRect(x + ux1 * 2, y + uy1, (ux2 - ux1 + 1) * 2, uy2 - uy1 + 1, color);
}

// bg/fg are the row's own colours so each icon reads on both the green box
// and the accent-colour selection bar.
static void draw_row_icon(OSDCanvas* tft, int icon, int x, int y, uint16_t bg, uint16_t fg) {
    switch (icon) {
        case ROW_ICON_CHECK:        // tick mark
            for (int t = 0; t < 3; t++) {
                tft->drawLine(x + 7 + t, y + 6, x + 12 + t, y + 10, fg);
                tft->drawLine(x + 12 + t, y + 10, x + 23 + t, y + 1, fg);
            }
            break;

        case ROW_ICON_STOP:         // stop square
            row_icon_box(tft, x, y, 5, 1, 10, 10, SVW_RED);
            break;

        case ROW_ICON_CROSS:        // X
            for (int t = 0; t < 3; t++) {
                tft->drawLine(x + 9 + t, y + 1, x + 20 + t, y + 10, SVW_RED);
                tft->drawLine(x + 20 + t, y + 1, x + 9 + t, y + 10, SVW_RED);
            }
            break;

        case ROW_ICON_MACHINE:      // computer: case, keyboard area, red badge
            row_icon_box(tft, x, y, 1, 2, 14, 10, SVW_GRAY);
            row_icon_box(tft, x, y, 3, 5, 12, 8, SVW_DKGRAY);
            row_icon_box(tft, x, y, 3, 3, 5, 3, SVW_RED);
            break;

        case ROW_ICON_SERIAL:       // RS-232: D-sub connector with two rows of pins
            row_icon_box(tft, x, y, 0, 1, 15, 4, SVW_GRAY);
            row_icon_box(tft, x, y, 1, 5, 14, 7, SVW_GRAY);
            row_icon_box(tft, x, y, 2, 8, 13, 10, SVW_GRAY);
            for (int p = 0; p < 5; p++) row_icon_box(tft, x, y, 3 + p * 2, 3, 3 + p * 2, 4, SVW_BLACK);
            for (int p = 0; p < 4; p++) row_icon_box(tft, x, y, 4 + p * 2, 7, 4 + p * 2, 8, SVW_BLACK);
            break;

        case ROW_ICON_KEYBOARD:     // keyboard: case, two rows of keys, space bar
            row_icon_box(tft, x, y, 0, 1, 15, 10, SVW_DKGRAY);
            for (int r = 0; r < 2; r++) {
                for (int k = 0; k < 6; k++) {
                    row_icon_box(tft, x, y, 2 + k * 2, 3 + r * 2, 2 + k * 2, 3 + r * 2, SVW_WHITE);
                }
            }
            row_icon_box(tft, x, y, 4, 8, 11, 8, SVW_WHITE);
            break;

        case ROW_ICON_JOYSTICK:     // joystick: base, stick, red ball, fire button
            row_icon_box(tft, x, y, 2, 8, 13, 11, SVW_DKGRAY);
            row_icon_box(tft, x, y, 7, 3, 8, 8, fg);
            tft->fillEllipse(x + 16, y + 2, 10, 5, SVW_RED);
            row_icon_box(tft, x, y, 11, 7, 12, 7, SVW_RED);
            break;

        case ROW_ICON_WIFI:         // signal bars, rising left to right
            for (int b = 0; b < 4; b++) {
                row_icon_box(tft, x, y, 2 + b * 3, 9 - b * 3, 3 + b * 3, 11, fg);
            }
            break;

        case ROW_ICON_DRIVEWIRE:    // diskette on a wire
            row_icon_box(tft, x, y, 0, 0, 9, 11, SVW_RED);
            row_icon_box(tft, x, y, 2, 1, 7, 3, SVW_WHITE);
            row_icon_box(tft, x, y, 4, 7, 5, 9, SVW_BLACK);
            row_icon_box(tft, x, y, 10, 5, 15, 6, fg);
            break;

        case ROW_ICON_LANGUAGE:     // flag on a pole
            row_icon_box(tft, x, y, 3, 0, 3, 11, fg);
            row_icon_box(tft, x, y, 4, 1, 13, 7, SVW_WHITE);
            row_icon_box(tft, x, y, 4, 3, 13, 5, SVW_RED);
            break;

        case ROW_ICON_KEYCAP:       // single key: skirt and top
            row_icon_box(tft, x, y, 3, 0, 12, 11, SVW_DKGRAY);
            row_icon_box(tft, x, y, 5, 1, 10, 7, SVW_WHITE);
            break;

        case ROW_ICON_CHIP:         // chip with pins top and bottom
            for (int p = 0; p < 4; p++) {
                row_icon_box(tft, x, y, 4 + p * 2, 0, 4 + p * 2, 11, fg);
            }
            row_icon_box(tft, x, y, 2, 2, 13, 9, SVW_DKGRAY);
            row_icon_box(tft, x, y, 3, 4, 3, 7, SVW_WHITE);
            break;

        case ROW_ICON_PAGE:         // page with lines of text
            row_icon_box(tft, x, y, 2, 0, 13, 11, SVW_WHITE);
            for (int l = 0; l < 4; l++) {
                row_icon_box(tft, x, y, 4, 2 + l * 2, (l == 3) ? 8 : 11, 2 + l * 2, SVW_DKGRAY);
            }
            break;

        case ROW_ICON_SD:           // SD card: body with a cut corner and contacts
            row_icon_box(tft, x, y, 3, 0, 12, 11, SVW_ORANGE);
            row_icon_box(tft, x, y, 11, 0, 12, 1, bg);
            for (int c = 0; c < 3; c++) row_icon_box(tft, x, y, 4 + c * 2, 1, 4 + c * 2, 4, SVW_WHITE);
            break;

        case ROW_ICON_CART:         // cartridge: shell, label, edge connector
            row_icon_box(tft, x, y, 2, 0, 13, 8, SVW_DKGRAY);
            row_icon_box(tft, x, y, 4, 2, 11, 6, SVW_WHITE);
            row_icon_box(tft, x, y, 4, 2, 11, 3, SVW_RED);
            row_icon_box(tft, x, y, 4, 9, 11, 11, SVW_ORANGE);
            break;

        case ROW_ICON_MULTIPAK:     // Multi-Pak: base unit with four cartridges in its slots
            row_icon_box(tft, x, y, 0, 6, 15, 11, SVW_DKGRAY);
            row_icon_box(tft, x, y, 1, 9, 14, 9, SVW_GRAY);
            for (int s = 0; s < 4; s++) {
                row_icon_box(tft, x, y, 1 + s * 4, 1, 2 + s * 4, 5, SVW_GRAY);
                row_icon_box(tft, x, y, 1 + s * 4, 1, 2 + s * 4, 2, SVW_RED);
            }
            break;

        case ROW_ICON_HARDDISK:     // hard disk: case, platter with spindle, head arm, LED
            row_icon_box(tft, x, y, 0, 0, 15, 11, SVW_DKGRAY);
            row_icon_box(tft, x, y, 2, 2, 9, 9, SVW_GRAY);
            row_icon_box(tft, x, y, 1, 4, 10, 7, SVW_GRAY);
            row_icon_box(tft, x, y, 5, 5, 6, 6, SVW_BLACK);
            row_icon_box(tft, x, y, 12, 1, 13, 2, SVW_WHITE);
            row_icon_box(tft, x, y, 10, 2, 12, 3, SVW_WHITE);
            row_icon_box(tft, x, y, 8, 3, 10, 4, SVW_WHITE);
            row_icon_box(tft, x, y, 13, 9, 14, 10, SVW_GREEN);
            break;

        case ROW_ICON_ARROWS:       // arrow keys (inverted T) with a red fire dot
            row_icon_box(tft, x, y, 5, 0, 8, 4, SVW_WHITE);
            for (int k = 0; k < 3; k++) row_icon_box(tft, x, y, 1 + k * 4, 6, 4 + k * 4, 10, SVW_WHITE);
            row_icon_box(tft, x, y, 6, 2, 7, 2, SVW_DKGRAY);
            row_icon_box(tft, x, y, 2, 8, 2, 8, SVW_DKGRAY);
            row_icon_box(tft, x, y, 6, 9, 7, 9, SVW_DKGRAY);
            row_icon_box(tft, x, y, 11, 8, 11, 8, SVW_DKGRAY);
            row_icon_box(tft, x, y, 13, 1, 15, 3, SVW_RED);
            break;

        case ROW_ICON_FOLDER:       // folder: tab and body
            row_icon_box(tft, x, y, 1, 1, 6, 2, SVW_ORANGE);
            row_icon_box(tft, x, y, 1, 3, 14, 10, SVW_ORANGE);
            row_icon_box(tft, x, y, 1, 4, 14, 4, SVW_BLACK);
            break;

        case ROW_ICON_SPEECH:       // speech bubble: body, two lines of text, tail
            row_icon_box(tft, x, y, 1, 0, 14, 7, SVW_WHITE);
            row_icon_box(tft, x, y, 3, 2, 12, 2, SVW_BLACK);
            row_icon_box(tft, x, y, 3, 5, 9, 5, SVW_BLACK);
            row_icon_box(tft, x, y, 3, 8, 5, 8, SVW_WHITE);
            row_icon_box(tft, x, y, 3, 9, 4, 9, SVW_WHITE);
            row_icon_box(tft, x, y, 3, 10, 3, 10, SVW_WHITE);
            break;

        case ROW_ICON_NOTE:         // beamed pair of eighth notes
            row_icon_box(tft, x, y, 5, 1, 12, 2, fg);
            row_icon_box(tft, x, y, 5, 1, 5, 9, fg);
            row_icon_box(tft, x, y, 12, 1, 12, 8, fg);
            row_icon_box(tft, x, y, 3, 8, 5, 10, fg);
            row_icon_box(tft, x, y, 10, 7, 12, 9, fg);
            break;

        case ROW_ICON_SPIDER: {     // spider: four legs each side, body, head, red eyes
            static const int8_t legs[4][4] = { { 6, 5, 1, 2 }, { 5, 6, 0, 6 }, { 5, 8, 0, 9 }, { 6, 9, 2, 11 } };
            for (int l = 0; l < 4; l++) {
                int lx1 = x + legs[l][0] * 2, lx2 = x + legs[l][2] * 2;
                int rx1 = x + 31 - legs[l][0] * 2, rx2 = x + 31 - legs[l][2] * 2;
                for (int t = 0; t < 2; t++) {       // two pixels thick
                    tft->drawLine(lx1 + t, y + legs[l][1], lx2 + t, y + legs[l][3], fg);
                    tft->drawLine(rx1 - t, y + legs[l][1], rx2 - t, y + legs[l][3], fg);
                }
            }
            row_icon_box(tft, x, y, 5, 4, 10, 10, fg);
            row_icon_box(tft, x, y, 6, 1, 9, 3, fg);
            row_icon_box(tft, x, y, 6, 2, 6, 2, SVW_RED);
            row_icon_box(tft, x, y, 9, 2, 9, 2, SVW_RED);
            break;
        }
    }
}

void sv_menu_draw_row_icon(int icon, int x, int y, uint16_t bg, uint16_t fg) {
    OSDCanvas* tft = hal_video_get_canvas();
    if (tft) draw_row_icon(tft, icon, x, y, bg, fg);
}

// ============================================================
// Settings submenu and its Keyboard, Machine and Multi-Pak submenus
// ============================================================
//
// All are icon lists in the wide green frame.
//
// Settings rows (SV_SettingsRow):
//   Machine      opens the Machine submenu
//   Keyboard     opens the Keyboard submenu
//   Joy - Mouse Sensitivity, WiFi / Debug, DriveWire   open their own screens
//   Joy-Keyboard Emulation   cycles OFF -> RIGHT -> LEFT: arrow keys + Space
//                act as that CoCo joystick (and no longer reach the CoCo
//                keyboard); applied live, persisted in NVS
//   Multi-Pak - Insert Cartridges   opens the Multi-Pak submenu
//
// Multi-Pak rows (SV_SoundRow) — set up at boot, so each toggle asks to
// confirm and restarts (CART_TOGGLES, popup refused with the missing file
// names if a ROM is not on the SD card):
//   Orchestra-90  ON  -> DAC ports on, orch90.rom as the cartridge
//                 OFF -> ports off, cartridge back to Disk BASIC
//   Speech/Sound  ON  -> $FF7D/$FF7E decoded beside the FDC (needs
//                        pic-7040-510.bin and sp0256-al2.bin); Disk BASIC stays
//                 OFF -> ports off
//   Hard Disk     picker (hd_popup_*): Off / RS-DOS / RGB-DOS
//                 RS-DOS  -> $FF80-$FF86 decoded, Disk BASIC as the cartridge
//                            (VCC's Multi-Pak setup: DOS boots NitrOS-9 from a
//                            floppy, its emudsk driver finds the VHD)
//                 RGB-DOS -> $FF80-$FF86 decoded, rgbdos.rom as the cartridge
//                 Both turn Orchestra-90 off. Images go on HD0/HD1 in the
//                 Disk Manager.
//   RS-232 Pak    toggle, live (no restart); owns UART0 while on:
//                   ON  -> SERIAL_MODE_RS232  (forces the debug Echo Log off)
//                   OFF -> SERIAL_MODE_OFF
//                 The Echo Log toggle that shares the port is in the Debug submenu.
//
// Machine rows (SV_MachineRow) — the first three are fixed at boot, so each
// asks to confirm and restarts:
//   Choose Machine Type   CoCo 2 / CoCo 3 popup
//   CPU                   6809 / 6309 picker
//   CoCo 3 Memory         128K / 512K / 1024K / 2048K popup; on a CoCo 2 the
//                         row reads "CoCo 2 Memory", fixed at 64K (no popup)
//   PMODE 4 Artifacts     CoCo 3: NTSC artifact colours for PMODE 4, cycling
//                         OFF / STANDARD / REVERSE (MAME's artifacter, phase A
//                         or B); applied live, persisted in NVS. CoCo 2:
//                         always on, no toggle
//   Monitor               CoCo 3: RGB / COMPOSITE colours (gime_monitor.h);
//                         applied live, persisted in NVS. CoCo 2: N/A
//
// Keyboard rows (SV_KeyboardRow):
//   Keyboard Language   cycles the PS/2 layout (US English / Spanish Latam);
//                       applied live via FabGL setLayout(), persisted in NVS
//   Key Mapper          opens the Key Mapper screens (sv_keymap.cpp)

struct SV_ListRow {
    const char* label;
    uint8_t     icon;
};

static const SV_ListRow SETTINGS_ROWS[SV_SET_COUNT] = {
    { "Machine",                 ROW_ICON_MACHINE   },
    { "Keyboard",                ROW_ICON_KEYBOARD  },
    { "Joy - Mouse Sensitivity", ROW_ICON_JOYSTICK  },
    { "Joy-Keyboard Emulation",  ROW_ICON_ARROWS    },
    { "WiFi / Debug",            ROW_ICON_WIFI      },
    { "DriveWire",               ROW_ICON_DRIVEWIRE },
    { "Multi-Pak - Insert Cartridges", ROW_ICON_MULTIPAK },
};

static const SV_ListRow SOUND_ROWS[SV_SND_COUNT] = {
    { "Orchestra-90",  ROW_ICON_NOTE   },
    { "Speech/Sound",  ROW_ICON_SPEECH },
    { "Hard Disk",     ROW_ICON_HARDDISK },
    { "RS-232 Pak",    ROW_ICON_SERIAL   },
};

static const SV_ListRow MACHINE_ROWS[SV_MACH_COUNT] = {
    { "Choose Machine Type", ROW_ICON_MACHINE },
    { "CPU",                 ROW_ICON_CHIP    },
    { "CoCo 3 Memory",       ROW_ICON_PAGE    },   // "CoCo 2 Memory" on a CoCo 2
    { "PMODE 4 Artifacts",   ROW_ICON_CHECK   },
    { "Monitor",             ROW_ICON_MACHINE },
};

static const SV_ListRow KEYBOARD_ROWS[SV_KBD_COUNT] = {
    { "Keyboard Language", ROW_ICON_LANGUAGE },
    { "Key Mapper",        ROW_ICON_KEYCAP   },
};

// Row last drawn as selected in whichever of the lists is on screen;
// -1 = nothing on screen, draw the whole menu.
static int8_t s_settings_drawn = -1;

void sv_settings_invalidate(void) {
    s_settings_drawn = -1;
}

// Cartridge toggle: a popup over the Multi-Pak list, wider than the usual
// one for its two-line explanation. Turning a cartridge on needs its ROM
// file(s) on the SD card: without them the popup names what is missing and
// nothing changes. The Hard Disk row has its own picker (hd_popup_*).
#define CART_POPUP_W   480

struct CartToggle {
    const char* name;               // popup titles: "Enable <name>?"
    bool        (*enabled)(void);
    void        (*save)(bool on);   // persists; the popup then restarts
    const char* files[2];           // ROM_BASE_PATH files needed to turn it on
    size_t      sizes[2];           // their exact sizes, 0 = any
    const char* on_msg[2];
    const char* off_msg[2];
};

// Orchestra-90 and the hard disk both want the cartridge ROM slot, so turning
// one on turns the other off.
static void orch90_save(bool on) {
    supervisor_save_orch90(on);
    supervisor_save_cart_rom(on ? ROM_ORCH90_FILE : "");
    if (on) supervisor_save_vhd(false);
}

// One per row before SV_SND_HARDDISK.
static const CartToggle CART_TOGGLES[SV_SND_HARDDISK] = {
    {   "Orchestra 90", orch90_enabled, orch90_save,
        { ROM_ORCH90_FILE, NULL }, { 0, 0 },
        { "CoCo will restart with the Orchestra 90 ROM.",
          "Disable Orchestra 90 to return to Disk BASIC." },
        { "Disabling Orchestra 90 will restart the CoCo",
          "with the Disk BASIC ROM." } },
    {   "Speech/Sound", ssc_enabled, supervisor_save_ssc,
        { ROM_SSC_PIC_FILE, ROM_SSC_SPEECH_FILE }, { 4096, 2048 },
        { "CoCo will restart with the Speech/Sound",
          "Cartridge at $FF7D/$FF7E. Disk BASIC stays." },
        { "Disabling Speech/Sound will restart",
          "the CoCo without the cartridge." } },
};

static const CartToggle* s_cart     = NULL;   // toggle the popup is asking about
static bool   s_cart_target  = false;         // state the toggle is asking for
static char   s_cart_missing[64];             // "" = ROMs present; else the names
static int8_t s_cart_sel     = 0;             // 0 = No, 1 = Yes
static int8_t s_cart_drawn   = -1;            // row drawn selected; -1 = draw the window

static bool rom_file_ok(const char* name, size_t size) {
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", ROM_BASE_PATH, name);
    return size ? hal_storage_file_size(path) == size : hal_storage_file_exists(path);
}

static void cart_toggle_open(Supervisor_t* sv, int row) {
    s_cart = &CART_TOGGLES[row];
    s_cart_target = !s_cart->enabled();
    s_cart_missing[0] = '\0';
    if (s_cart_target) {
        for (int i = 0; i < 2 && s_cart->files[i]; i++) {
            if (rom_file_ok(s_cart->files[i], s_cart->sizes[i])) continue;
            if (s_cart_missing[0]) strncat(s_cart_missing, ", ", sizeof(s_cart_missing) - strlen(s_cart_missing) - 1);
            strncat(s_cart_missing, s_cart->files[i], sizeof(s_cart_missing) - strlen(s_cart_missing) - 1);
        }
    }
    s_cart_sel   = 0;
    s_cart_drawn = -1;
    sv->prev_state = sv->state;
    sv->state = SV_CART_POPUP;
    sv->needs_redraw = true;
}

// Hard Disk picker (defined below), sharing the SV_CART_POPUP state.
static bool s_hd_open = false;
static void hd_popup_on_key(Supervisor_t* sv, uint8_t hid_usage);
static void hd_popup_render(void);

void sv_cart_popup_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;
    if (s_hd_open) { hd_popup_on_key(sv, hid_usage); return; }
    if (!s_cart) return;
    bool no_rom = s_cart_missing[0] != '\0';

    switch (hid_usage) {
        case HID_UP:
        case HID_DOWN:
            if (!no_rom) {
                s_cart_sel = (hid_usage == HID_DOWN) ? 1 : 0;
                sv->needs_redraw = true;
            }
            break;

        case HID_ENTER:
            if (!no_rom && s_cart_sel == 1) {
                s_cart->save(s_cart_target);
                supervisor_save_and_restart();   // never returns
            }
            // fall through: No / OK
        case HID_ESC:
            sv->state = sv->prev_state;
            sv->needs_redraw = true;
            break;

        case HID_F1:
            supervisor_toggle();
            break;
    }
}

void sv_cart_popup_render(Supervisor_t* sv) {
    (void)sv;
    if (s_hd_open) { hd_popup_render(); return; }
    if (!s_cart) return;
    char title[40];
    char line[64];
    const char* msg1;
    const char* msg2;
    bool no_rom = s_cart_missing[0] != '\0';
    if (no_rom) {
        snprintf(title, sizeof(title), "%s ROM missing", s_cart->name);
        snprintf(line, sizeof(line), "Not found: %s", s_cart_missing);
        msg1 = line;
        msg2 = "Copy to " ROM_BASE_PATH "/ on the SD card.";
    } else if (s_cart_target) {
        snprintf(title, sizeof(title), "Enable %s?", s_cart->name);
        msg1 = s_cart->on_msg[0];
        msg2 = s_cart->on_msg[1];
    } else {
        snprintf(title, sizeof(title), "Disable %s?", s_cart->name);
        msg1 = s_cart->off_msg[0];
        msg2 = s_cart->off_msg[1];
    }

    int count = no_rom ? 1 : 2;
    bool all = (s_cart_drawn < 0);
    int ry = sv_render_popup_w(CART_POPUP_W, title, msg1, msg2, count, all);
    if (all) DEBUG_PRINTF("popup: %s: %s %s", title, msg1, msg2);
    for (int i = 0; i < count; i++) {
        if (!all && i != s_cart_sel && i != s_cart_drawn) continue;
        const char* label = no_rom ? "OK" : (i == 0) ? "No" : "Yes";
        sv_render_popup_row_w(CART_POPUP_W, ry + i * SVP_ROW_H, label, NULL, i == s_cart_sel);
    }
    s_cart_drawn = s_cart_sel;
}

// Hard Disk picker: Off / RS-DOS / RGB-DOS, the current mode preselected.
// The $FF80 device works with either ROM, as in VCC, where the hard disk and
// the FD-502 (RS-DOS) sit in different Multi-Pak slots: the CoCo boots RS-DOS
// and DOS starts NitrOS-9 from a floppy whose emudsk driver finds the VHD.
// RGB-DOS instead reaches the RS-DOS virtual disks on the VHD from BASIC.
enum { HD_OFF, HD_RSDOS, HD_RGBDOS, HD_MODE_COUNT };
static const char* const HD_MODE_LABELS[HD_MODE_COUNT] = {
    "Off", "On, boot RS-DOS (NitrOS-9 compatible)", "On, boot RGB-DOS" };
static const char* const HD_MODE_VALUES[HD_MODE_COUNT] = { "OFF", "(RS-DOS) ON", "(RGB-DOS) ON" };

static int8_t      s_hd_sel   = 0;
static int8_t      s_hd_drawn = -1;      // row drawn selected; -1 = draw the window
static const char* s_hd_note  = NULL;    // replaces the second message line

static bool is_rgbdos_name(const char* path) {
    const char* base = strrchr(path, '/');
    return strcasecmp(base ? base + 1 : path, ROM_RGBDOS_FILE) == 0;
}

// Mode this boot is running in.
static int hd_mode_now(void) {
    if (!vhd_enabled()) return HD_OFF;
    return is_rgbdos_name(supervisor_cart_rom_boot()) ? HD_RGBDOS : HD_RSDOS;
}

// RGB-DOS can boot: in ROM_BASE_PATH, or already chosen with Load Cart.
static bool rgbdos_available(void) {
    return rom_file_ok(ROM_RGBDOS_FILE, 0) || is_rgbdos_name(supervisor_cart_rom_saved());
}

// Orchestra-90 needs the cartridge slot too, so an On mode turns it off.
static void hd_mode_save(int mode) {
    const char* saved = supervisor_cart_rom_saved();
    supervisor_save_vhd(mode != HD_OFF);
    if (mode != HD_OFF) supervisor_save_orch90(false);
    if (mode == HD_RGBDOS) {
        if (!is_rgbdos_name(saved)) supervisor_save_cart_rom(ROM_RGBDOS_FILE);
    } else if (mode == HD_RSDOS || is_rgbdos_name(saved)) {
        supervisor_save_cart_rom("");        // Disk BASIC
    }
}

static void hd_popup_open(Supervisor_t* sv) {
    s_hd_open  = true;
    s_hd_sel   = hd_mode_now();
    s_hd_drawn = -1;
    s_hd_note  = NULL;
    sv->prev_state = sv->state;
    sv->state = SV_CART_POPUP;
    sv->needs_redraw = true;
}

static void hd_popup_close(Supervisor_t* sv) {
    s_hd_open = false;
    sv->state = sv->prev_state;
    sv->needs_redraw = true;
}

static void hd_popup_on_key(Supervisor_t* sv, uint8_t hid_usage) {
    switch (hid_usage) {
        case HID_UP:
            if (s_hd_sel > 0) { s_hd_sel--; sv->needs_redraw = true; }
            break;
        case HID_DOWN:
            if (s_hd_sel < HD_MODE_COUNT - 1) { s_hd_sel++; sv->needs_redraw = true; }
            break;
        case HID_ENTER:
            if (s_hd_sel == hd_mode_now()) { hd_popup_close(sv); break; }
            if (s_hd_sel == HD_RGBDOS && !rgbdos_available()) {
                s_hd_note  = "Copy " ROM_RGBDOS_FILE " to " ROM_BASE_PATH "/ on the SD card.";
                s_hd_drawn = -1;
                sv->needs_redraw = true;
                break;
            }
            hd_mode_save(s_hd_sel);
            supervisor_save_and_restart();   // never returns
            break;
        case HID_ESC:
            hd_popup_close(sv);
            break;
        case HID_F1:
            s_hd_open = false;
            supervisor_toggle();
            break;
    }
}

static void hd_popup_render(void) {
    const char* title = "Hard Disk at $FF80";
    const char* msg1  = "Choose how the CoCo boots. It restarts.";
    const char* msg2  = s_hd_note ? s_hd_note : "Mount images on HD0/HD1 in Disks.";
    bool all = (s_hd_drawn < 0);
    int now = hd_mode_now();
    int ry = sv_render_popup_w(CART_POPUP_W, title, msg1, msg2, HD_MODE_COUNT, all);
    if (all) DEBUG_PRINTF("popup: %s: %s %s", title, msg1, msg2);
    for (int i = 0; i < HD_MODE_COUNT; i++) {
        if (!all && i != s_hd_sel && i != s_hd_drawn) continue;
        const char* value = (i == now) ? "current"
                          : (i == HD_RGBDOS && !rgbdos_available()) ? "no ROM" : NULL;
        sv_render_popup_row_w(CART_POPUP_W, ry + i * SVP_ROW_H, HD_MODE_LABELS[i], value, i == s_hd_sel);
        if (i == s_hd_sel) DEBUG_PRINTF("popup: %s > %s", title, HD_MODE_LABELS[i]);
    }
    s_hd_drawn = s_hd_sel;
}

// CPU picker: 6809 / 6309, the running CPU preselected and marked "current".
// ENTER on the other one saves it and restarts; on the current one, closes.
#define CPU_POPUP_W   480

static const uint8_t     CPU_VARIANTS[2]   = { CPU_VARIANT_MC6809, CPU_VARIANT_HD6309 };
static const char* const CPU_LABELS[2]     = { "6809  (Motorola MC6809)", "6309  (Hitachi HD6309)" };

static int8_t s_cpu_sel   = 0;             // row: 0 = 6809, 1 = 6309
static int8_t s_cpu_drawn = -1;            // row drawn selected; -1 = draw the window

static int cpu_current_index(void) {
    return (g_cpu_variant == CPU_VARIANT_HD6309) ? 1 : 0;
}

static void cpu_toggle_open(Supervisor_t* sv) {
    s_cpu_sel   = (int8_t)cpu_current_index();
    s_cpu_drawn = -1;
    sv->prev_state = sv->state;
    sv->state = SV_CPU_POPUP;
    sv->needs_redraw = true;
}

void sv_cpu_popup_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;

    switch (hid_usage) {
        case HID_UP:
        case HID_DOWN:
            s_cpu_sel = (hid_usage == HID_DOWN) ? 1 : 0;
            sv->needs_redraw = true;
            break;

        case HID_ENTER:
            if (s_cpu_sel != cpu_current_index()) {
                supervisor_save_cpu_variant(CPU_VARIANTS[s_cpu_sel]);
                supervisor_save_and_restart();   // never returns
            }
            // fall through: current CPU picked, nothing to change
        case HID_ESC:
            sv->state = SV_MACHINE_MENU;
            sv->needs_redraw = true;
            break;

        case HID_F1:
            supervisor_toggle();
            break;
    }
}

void sv_cpu_popup_render(Supervisor_t* sv) {
    (void)sv;
    const char* title = "Choose CPU";
    const char* msg1  = "Changing the CPU restarts the emulator.";
    const char* msg2  = "6309: illegal 6809 opcodes trap, as on the chip.";

    bool all = (s_cpu_drawn < 0);
    int now = cpu_current_index();
    int ry = sv_render_popup_w(CPU_POPUP_W, title, msg1, msg2, 2, all);
    if (all) DEBUG_PRINTF("popup: %s: %s %s", title, msg1, msg2);
    for (int i = 0; i < 2; i++) {
        if (!all && i != s_cpu_sel && i != s_cpu_drawn) continue;
        sv_render_popup_row_w(CPU_POPUP_W, ry + i * SVP_ROW_H, CPU_LABELS[i],
                              (i == now) ? "current" : NULL, i == s_cpu_sel);
        if (i == s_cpu_sel) DEBUG_PRINTF("popup: %s > %s", title, CPU_LABELS[i]);
    }
    s_cpu_drawn = s_cpu_sel;
}

// CoCo 3 memory: pick one of four sizes, then confirm the restart. Keeps its
// own cursor so the Machine submenu's is untouched.
static const uint16_t    RAM_VALUES[4] = { 128, 512, 1024, 2048 };
static const char* const RAM_LABELS[4] = { "128K", "512K", "1024K (1 MB)", "2048K (2 MB)" };

static int8_t   s_ram_sel        = 0;      // row in the list, or 0 = No / 1 = Yes
static int8_t   s_ram_drawn      = -1;     // row drawn selected; -1 = draw the window
static bool     s_ram_confirming = false;
static uint8_t  s_ram_pending    = 0;      // index into RAM_VALUES being confirmed
// The two steps have different heights, so the list underneath is repainted
// before the next window is drawn.
static bool     s_ram_repaint    = false;

static int ram_current_index(void) {
    for (int i = 0; i < 4; i++) if (RAM_VALUES[i] == g_coco3_ram_kb) return i;
    return 1;
}

static void ram_select_open(Supervisor_t* sv) {
    s_ram_sel        = (int8_t)ram_current_index();
    s_ram_drawn      = -1;
    s_ram_confirming = false;
    s_ram_repaint    = false;
    sv->prev_state = sv->state;
    sv->state = SV_RAM_SELECT;
    sv->needs_redraw = true;
}

void sv_ram_select_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;
    int last = s_ram_confirming ? 1 : 3;

    switch (hid_usage) {
        case HID_UP:
            if (s_ram_sel > 0) { s_ram_sel--; sv->needs_redraw = true; }
            break;

        case HID_DOWN:
            if (s_ram_sel < last) { s_ram_sel++; sv->needs_redraw = true; }
            break;

        case HID_ENTER:
            if (s_ram_confirming) {
                if (s_ram_sel == 1) {
                    supervisor_save_coco3_ram(RAM_VALUES[s_ram_pending]);
                    supervisor_save_and_restart();   // never returns
                }
                sv->state = SV_MACHINE_MENU;         // No
                sv->needs_redraw = true;
                break;
            }
            if (RAM_VALUES[s_ram_sel] == g_coco3_ram_kb) {
                sv->state = SV_MACHINE_MENU;         // already active
                sv->needs_redraw = true;
                break;
            }
            s_ram_pending    = (uint8_t)s_ram_sel;
            s_ram_confirming = true;
            s_ram_sel        = 0;                    // No
            s_ram_drawn      = -1;
            s_ram_repaint    = true;
            sv->needs_redraw = true;
            break;

        case HID_ESC:
            if (s_ram_confirming) {
                // Back to the size list, on the size that was picked.
                s_ram_confirming = false;
                s_ram_sel        = (int8_t)s_ram_pending;
                s_ram_drawn      = -1;
                s_ram_repaint    = true;
                sv->needs_redraw = true;
            } else {
                sv->state = SV_MACHINE_MENU;
                sv->needs_redraw = true;
            }
            break;

        case HID_F1:
            supervisor_toggle();
            break;
    }
}

static void draw_ram_row(int y, int row, bool highlighted) {
    const char* label;
    const char* value = NULL;
    if (s_ram_confirming) {
        label = (row == 0) ? "No" : "Yes";
    } else {
        label = RAM_LABELS[row];
        if (RAM_VALUES[row] == g_coco3_ram_kb) value = "(current)";
    }
    sv_render_popup_row(y, label, value, highlighted);
    if (highlighted) DEBUG_PRINTF("popup: CoCo 3 Memory > %s %s", label, value ? value : "");
}

void sv_ram_select_render(Supervisor_t* sv) {
    if (s_ram_repaint) {
        s_ram_repaint = false;
        sv_settings_invalidate();
        sv_machine_menu_render(sv);
    }

    char msg[SVP_MSG_COLS + 1];
    const char* title;
    int count;
    if (s_ram_confirming) {
        title = "Restart emulator?";
        snprintf(msg, sizeof(msg), "Set CoCo 3 memory to %uK and restart?",
                 (unsigned)RAM_VALUES[s_ram_pending]);
        count = 2;
    } else {
        title = "CoCo 3 Memory";
        snprintf(msg, sizeof(msg), "Changing memory restarts the emulator.");
        count = 4;
    }

    bool all = (s_ram_drawn < 0);
    int ry = sv_render_popup(title, msg, NULL, count, all);
    if (all) {
        for (int i = 0; i < count; i++) draw_ram_row(ry + i * SVP_ROW_H, i, i == s_ram_sel);
    } else if (s_ram_drawn != s_ram_sel) {
        draw_ram_row(ry + s_ram_drawn * SVP_ROW_H, s_ram_drawn, false);
        draw_ram_row(ry + s_ram_sel * SVP_ROW_H, s_ram_sel, true);
    }
    s_ram_drawn = s_ram_sel;
}

static void machine_menu_activate(Supervisor_t* sv, int row) {
    switch (row) {
        case SV_MACH_TYPE: machine_select_open(sv); break;
        case SV_MACH_CPU:  cpu_toggle_open(sv);     break;
        case SV_MACH_RAM:
            if (g_machine_type == 4) ram_select_open(sv);   // CoCo 2: fixed 64K
            break;
        case SV_MACH_ARTIFACTS:
            if (g_machine_type == 4) {                       // CoCo 2: always on
                const uint8_t mode = (uint8_t)((tcc1014_get_artifacts() + 1) % GIME_ARTIFACT_COUNT);
                tcc1014_set_artifacts(mode);
                supervisor_save_artifacts(mode);
                sv->needs_redraw = true;
            }
            break;
        case SV_MACH_MONITOR:
            if (g_machine_type == 4) {
                const uint8_t m = (uint8_t)((tcc1014_get_monitor() + 1) % GIME_MONITOR_COUNT);
                tcc1014_set_monitor(m);
                hal_video_gime_colours_changed();
                supervisor_save_monitor(m);
                sv->needs_redraw = true;
            }
            break;
    }
}

static void settings_activate(Supervisor_t* sv, int row) {
    switch (row) {
        case SV_SET_MACHINE:
            sv->prev_state = sv->state;
            sv->state = SV_MACHINE_MENU;
            sv->menu_cursor = 0;
            sv->needs_redraw = true;
            break;

        case SV_SET_KEYBOARD:
            sv->prev_state = sv->state;
            sv->state = SV_KEYBOARD_MENU;
            sv->menu_cursor = 0;
            sv->needs_redraw = true;
            break;

        case SV_SET_JOYSTICK:  sv_joystick_open(sv); break;
        case SV_SET_JOYKBD: {
            uint8_t next = (uint8_t)((hal_joystick_get_kbd_mode() + 1) % 3);   // OFF -> RIGHT -> LEFT
            hal_joystick_set_kbd_mode(next);
            supervisor_save_joy_kbd(next);
            sv->needs_redraw = true;
            break;
        }
        case SV_SET_WIFI:      sv_wifi_open(sv, false); break;
        case SV_SET_DRIVEWIRE: sv_fujinet_open(sv);  break;
        case SV_SET_SOUND:
            sv->prev_state = sv->state;
            sv->state = SV_SOUND_MENU;
            sv->menu_cursor = 0;
            sv->needs_redraw = true;
            break;
    }
}

static void keyboard_activate(Supervisor_t* sv, int row) {
    if (row == SV_KBD_MAPPER) {
        sv_keymap_open(sv);
        return;
    }
    // Keyboard Language — takes effect immediately, no reboot
    KbdLayout next = (g_kbd_layout == KBD_LAYOUT_US) ? KBD_LAYOUT_ES_LATAM
                                                     : KBD_LAYOUT_US;
    hal_keyboard_set_layout(next);
    supervisor_save_kbd_layout(next);
    sv->needs_redraw = true;
}

// Up/Down within a list of `count` rows.
static void list_move_cursor(Supervisor_t* sv, uint8_t hid_usage, int count) {
    if (hid_usage == HID_UP && sv->menu_cursor > 0) {
        sv->menu_cursor--;
        sv->needs_redraw = true;
    } else if (hid_usage == HID_DOWN && sv->menu_cursor < count - 1) {
        sv->menu_cursor++;
        sv->needs_redraw = true;
    }
}

void sv_settings_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;

    switch (hid_usage) {
        case HID_UP:
        case HID_DOWN:  list_move_cursor(sv, hid_usage, SV_SET_COUNT); break;
        case HID_ENTER: settings_activate(sv, sv->menu_cursor); break;
        case HID_ESC:   restore_main_menu(sv); break;
        case HID_F1:    supervisor_toggle(); break;
    }
}

void sv_machine_menu_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;

    switch (hid_usage) {
        case HID_UP:
        case HID_DOWN:  list_move_cursor(sv, hid_usage, SV_MACH_COUNT); break;
        case HID_ENTER: machine_menu_activate(sv, sv->menu_cursor); break;
        case HID_ESC:
            sv->state = SV_SETTINGS;
            sv->menu_cursor = SV_SET_MACHINE;
            sv->needs_redraw = true;
            break;
        case HID_F1:    supervisor_toggle(); break;
    }
}

void sv_sound_menu_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;

    switch (hid_usage) {
        case HID_UP:
        case HID_DOWN:  list_move_cursor(sv, hid_usage, SV_SND_COUNT); break;
        case HID_ENTER:
            if (sv->menu_cursor == SV_SND_HARDDISK) {
                hd_popup_open(sv);
            } else if (sv->menu_cursor == SV_SND_RS232) {
                SerialPortMode next = (g_serial_mode == SERIAL_MODE_RS232) ? SERIAL_MODE_OFF
                                                                            : SERIAL_MODE_RS232;
                serial_mode_apply(next);
                supervisor_save_serial_mode(next);
                sv->needs_redraw = true;
            } else {
                cart_toggle_open(sv, sv->menu_cursor);
            }
            break;
        case HID_ESC:
            sv->state = SV_SETTINGS;
            sv->menu_cursor = SV_SET_SOUND;
            sv->needs_redraw = true;
            break;
        case HID_F1:    supervisor_toggle(); break;
    }
}

void sv_keyboard_menu_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;

    switch (hid_usage) {
        case HID_UP:
        case HID_DOWN:  list_move_cursor(sv, hid_usage, SV_KBD_COUNT); break;
        case HID_ENTER: keyboard_activate(sv, sv->menu_cursor); break;
        case HID_ESC:
            sv->state = SV_SETTINGS;
            sv->menu_cursor = SV_SET_KEYBOARD;
            sv->needs_redraw = true;
            break;
        case HID_F1:    supervisor_toggle(); break;
    }
}

// Current value shown right-aligned on a Settings row, or NULL.
static const char* settings_value(int row, char* buf, size_t size) {
    switch (row) {
        // Runtime-active machine, not the compile-time default.
        case SV_SET_MACHINE:   return (g_machine_type == 4) ? MACHINE_NAME_COCO3 : MACHINE_NAME_COCO2;
        case SV_SET_JOYKBD: {
            static const char* const JOYKBD_VALUES[3] = { "OFF", "RIGHT", "LEFT" };
            return JOYKBD_VALUES[hal_joystick_get_kbd_mode() % 3];
        }
        case SV_SET_JOYSTICK:
            snprintf(buf, size, "%u", (unsigned)hal_joystick_get_sensitivity());
            return buf;
        case SV_SET_WIFI:      return wifi_mgr_state_str();
        case SV_SET_DRIVEWIRE: return dw_bus_mode_str(dw_bus_mode());
        default:               return NULL;
    }
}

// Current value shown right-aligned on a Machine submenu row.
static const char* machine_value(int row, char* buf, size_t size) {
    switch (row) {
        case SV_MACH_TYPE: return (g_machine_type == 4) ? MACHINE_NAME_COCO3 : MACHINE_NAME_COCO2;
        case SV_MACH_CPU:  return (g_cpu_variant == CPU_VARIANT_HD6309) ? "HD6309" : "MC6809";
        case SV_MACH_RAM:
            if (g_machine_type != 4) return "64K";
            snprintf(buf, size, "%uK", (unsigned)g_coco3_ram_kb);
            return buf;
        case SV_MACH_ARTIFACTS:
            if (g_machine_type != 4) return "ALWAYS";
            switch (tcc1014_get_artifacts()) {
                case GIME_ARTIFACT_STANDARD: return "STANDARD";
                case GIME_ARTIFACT_REVERSE:  return "REVERSE";
                default:                     return "OFF";
            }
        case SV_MACH_MONITOR:
            if (g_machine_type != 4) return "N/A";
            return (tcc1014_get_monitor() == GIME_MONITOR_COMPOSITE) ? "COMPOSITE" : "RGB";
        default:           return NULL;
    }
}

// Which of the icon lists render_icon_list() is drawing.
enum { LIST_SETTINGS, LIST_KEYBOARD, LIST_MACHINE, LIST_SOUND };

// Shared renderer: first pass draws everything; afterwards the row left
// behind and the selected row (whose value may have just been toggled).
static void render_icon_list(Supervisor_t* sv, const char* title, const SV_ListRow* rows,
                             int count, int list) {
    OSDCanvas* tft = hal_video_get_canvas();
    if (!tft) return;

    int sel = sv->menu_cursor;
    if (sel < 0 || sel >= count) sel = sv->menu_cursor = 0;
    int y0 = SVW_LIST_Y + (SVW_LIST_ROWS - count) / 2 * SVW_ROW_H;
    bool all = (s_settings_drawn < 0);

    if (all) sv_render_wide_frame(title, "Up/Dn   ENTER Select   ESC Back   F3 Exit");
    for (int i = 0; i < count; i++) {
        if (!all && i != sel && i != s_settings_drawn) continue;
        bool hl = (i == sel);
        int y = y0 + i * SVW_ROW_H;
        char buf[12];
        const char* value = (list == LIST_SETTINGS) ? settings_value(i, buf, sizeof(buf))
                          : (list == LIST_MACHINE)  ? machine_value(i, buf, sizeof(buf))
                          : (list == LIST_SOUND)    ? (i == SV_SND_HARDDISK ? HD_MODE_VALUES[hd_mode_now()]
                                                       : i == SV_SND_RS232 ? (g_serial_mode == SERIAL_MODE_RS232 ? "ON" : "OFF")
                                                       : CART_TOGGLES[i].enabled() ? "ON" : "OFF")
                          : (i == SV_KBD_LANGUAGE)  ? hal_keyboard_layout_name(g_kbd_layout)
                                                    : NULL;
        const char* label = (list == LIST_MACHINE && i == SV_MACH_RAM && g_machine_type != 4)
                          ? "CoCo 2 Memory" : rows[i].label;
        sv_render_wide_row(y, label, value, hl);
        draw_row_icon(tft, rows[i].icon, SVW_ICON_X, y + 2,
                      hl ? SVW_DKBLUE : SVW_GREEN, hl ? SVW_WHITE : SVW_BLACK);
        if (hl) DEBUG_PRINTF("menu: %s > %s %s", title, rows[i].label, value ? value : "");
    }
    s_settings_drawn = sel;
}

void sv_settings_render(Supervisor_t* sv) {
    render_icon_list(sv, "Settings", SETTINGS_ROWS, SV_SET_COUNT, LIST_SETTINGS);
}

void sv_sound_menu_render(Supervisor_t* sv) {
    render_icon_list(sv, "Multi-Pak", SOUND_ROWS, SV_SND_COUNT, LIST_SOUND);
}

void sv_keyboard_menu_render(Supervisor_t* sv) {
    render_icon_list(sv, "Keyboard", KEYBOARD_ROWS, SV_KBD_COUNT, LIST_KEYBOARD);
}

void sv_machine_menu_render(Supervisor_t* sv) {
    render_icon_list(sv, "Machine", MACHINE_ROWS, SV_MACH_COUNT, LIST_MACHINE);
}

// ============================================================
// Debug submenu — pages, RAM dump, Echo Log toggle
// ============================================================
//
// Drawn in the wide green frame as an icon list. Rows 0..2 open the debug
// pages, the next row opens the "Dump RAM to SD" filename screen, and the
// last toggles the Echo Log: debug output on the serial port
// (SERIAL_MODE_DEBUG). It shares UART0 with the RS-232 Pak in Multi-Pak, so
// turning it on forces the Pak off.

static const char* const DEBUG_PAGE_LABELS[SV_DBG_PAGE_COUNT] = {
    "CPU / GIME Status",
    "Memory Hex Dump",
    "RS-232 Pak",
};

#define DEBUG_ROW_DUMP     SV_DBG_PAGE_COUNT
#define DEBUG_ROW_ECHO     (SV_DBG_PAGE_COUNT + 1)
#define DEBUG_MENU_COUNT   (SV_DBG_PAGE_COUNT + 2)
#define DEBUG_LIST_Y       (SVW_LIST_Y + (SVW_LIST_ROWS - DEBUG_MENU_COUNT) / 2 * SVW_ROW_H)

// Row last drawn as selected; -1 = nothing on screen, draw the whole menu.
static int8_t s_debug_drawn = -1;

void sv_debug_menu_invalidate(void) {
    s_debug_drawn = -1;
}

void sv_debug_menu_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;

    switch (hid_usage) {
        case HID_UP:
            if (sv->menu_cursor > 0) {
                sv->menu_cursor--;
                sv->needs_redraw = true;
            }
            break;

        case HID_DOWN:
            if (sv->menu_cursor < DEBUG_MENU_COUNT - 1) {
                sv->menu_cursor++;
                sv->needs_redraw = true;
            }
            break;

        case HID_ENTER:
            if (sv->menu_cursor < SV_DBG_PAGE_COUNT) {
                sv_debug_set_page((SV_DebugPage)sv->menu_cursor);
                sv->prev_state = sv->state;
                sv->state = SV_DEBUG_DUMP;
                sv->needs_redraw = true;
            } else if (sv->menu_cursor == DEBUG_ROW_DUMP) {
                // Action row — open the "Dump RAM to SD" filename screen.
                sv_debug_begin_dump(sv);
            } else {
                // Echo Log — takes effect immediately, persisted in NVS.
                SerialPortMode next = (g_serial_mode == SERIAL_MODE_DEBUG) ? SERIAL_MODE_OFF
                                                                           : SERIAL_MODE_DEBUG;
                serial_mode_apply(next);
                supervisor_save_serial_mode(next);
                sv->needs_redraw = true;
            }
            break;

        case HID_ESC:
            restore_main_menu(sv);
            break;

        case HID_F1:
            supervisor_toggle();
            break;
    }
}

static void draw_debug_row(OSDCanvas* tft, int row, bool highlighted) {
    int y = DEBUG_LIST_Y + row * SVW_ROW_H;
    const char* label = (row < SV_DBG_PAGE_COUNT) ? DEBUG_PAGE_LABELS[row]
                      : (row == DEBUG_ROW_DUMP)   ? "Dump RAM to SD"
                                                  : "Echo Log";
    const char* value = (row != DEBUG_ROW_ECHO) ? NULL
                      : (g_serial_mode == SERIAL_MODE_DEBUG) ? "ON" : "OFF";

    sv_render_wide_row(y, label, value, highlighted);
    static const uint8_t icons[DEBUG_MENU_COUNT] = {
        ROW_ICON_CHIP, ROW_ICON_PAGE, ROW_ICON_SERIAL, ROW_ICON_SD, ROW_ICON_SPIDER
    };
    draw_row_icon(tft, icons[row], SVW_ICON_X, y + 2,
                    highlighted ? SVW_DKBLUE : SVW_GREEN,
                    highlighted ? SVW_WHITE : SVW_BLACK);
    if (highlighted) DEBUG_PRINTF("menu: Debug > %s %s", label, value ? value : "");
}

void sv_debug_menu_render(Supervisor_t* sv) {
    OSDCanvas* tft = hal_video_get_canvas();
    if (!tft) return;

    int sel = sv->menu_cursor;
    if (sel < 0 || sel >= DEBUG_MENU_COUNT) sel = sv->menu_cursor = 0;

    // First pass draws everything; afterwards the row left behind and the
    // selected row (whose value may have just been toggled).
    if (s_debug_drawn < 0) {
        sv_render_wide_frame("Debug", "Up/Dn   ENTER Select   ESC Back   F3 Exit");
        for (int i = 0; i < DEBUG_MENU_COUNT; i++) draw_debug_row(tft, i, i == sel);
    } else {
        if (s_debug_drawn != sel) draw_debug_row(tft, s_debug_drawn, false);
        draw_debug_row(tft, sel, true);
    }
    s_debug_drawn = sel;
}
