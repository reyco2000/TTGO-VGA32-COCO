/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
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
#include "../hal/hal.h"
#include "../../config.h"

// HID usage codes
#define HID_UP    0x52
#define HID_DOWN  0x51
#define HID_ENTER 0x28
#define HID_ESC   0x29
#define HID_F1    0x3A

static SV_MenuItem menu_items[] = {
    { "Disk Manager",       SV_ACT_MOUNT_DISK,     NULL },
    { "Machine:",           SV_ACT_MACHINE_SELECT, MACHINE_NAME },
    { "Settings",           SV_ACT_SETTINGS,       NULL },
    { "Reset Machine",      SV_ACT_RESET,          NULL },
    { "Debug",              SV_ACT_DEBUG,          NULL },
    { "About",              SV_ACT_ABOUT,          NULL },
};

static const int MENU_COUNT = sizeof(menu_items) / sizeof(menu_items[0]);

void sv_menu_init(Supervisor_t* sv) {
    sv->menu_cursor = 0;
    sv->menu_scroll_offset = 0;
    sv->menu_item_count = MENU_COUNT;
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

void sv_menu_update_values(Supervisor_t* sv) {
    (void)sv;
    // Reflect the runtime-active machine, not the compile-time default.
    // Index 1 is the "Machine:" item (index 2 is "Settings").
    menu_items[1].value = (g_machine_type == 4) ? MACHINE_NAME_COCO3
                                                : MACHINE_NAME_COCO2;
}

static void execute_action(Supervisor_t* sv, SV_MenuAction action) {
    switch (action) {
        case SV_ACT_MOUNT_DISK:
            sv->target_drive = 0;
            sv->prev_state = sv->state;
            sv->state = SV_FILE_BROWSER;
            sv_filebrowser_open(sv, sv->current_path, sv->target_drive);
            break;

        case SV_ACT_MACHINE_SELECT:
            // Save main-menu state so ESC / no-op select can restore it.
            saved_main_menu_cursor = sv->menu_cursor;
            saved_main_menu_count  = sv->menu_item_count;
            sv->prev_state = sv->state;
            sv->state = SV_MACHINE_SELECT;
            // Seed cursor on the currently-active machine so ENTER on it is a no-op.
            sv->menu_cursor = (g_machine_type == 4) ? 1 : 0;
            sv->menu_item_count = 2;
            sv->needs_redraw = true;
            break;

        case SV_ACT_SETTINGS:
            // Save main-menu state so ESC restores it.
            saved_main_menu_cursor = sv->menu_cursor;
            saved_main_menu_count  = sv->menu_item_count;
            sv->prev_state = sv->state;
            sv->state = SV_SETTINGS;
            sv->menu_cursor = 0;
            sv->menu_item_count = 2;   // Debug Log, RS-232 Pak
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
                s->state = SV_INACTIVE;
                s->needs_redraw = true;
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

    switch (hid_usage) {
        case HID_UP:
            if (sv->menu_cursor > 0) {
                sv->menu_cursor--;
                sv->needs_redraw = true;
            }
            break;

        case HID_DOWN:
            if (sv->menu_cursor < sv->menu_item_count - 1) {
                sv->menu_cursor++;
                sv->needs_redraw = true;
            }
            break;

        case HID_ENTER:
            execute_action(sv, menu_items[sv->menu_cursor].action);
            break;

        case HID_ESC:
        case HID_F1:
            supervisor_toggle();
            break;
    }
}

void sv_menu_render(Supervisor_t* sv) {
    sv_render_frame("*CoCo ESP32 SUPERVISOR*", "Up/Dn -  ENTER - F1/ESC");

    // Vertically center: content area fits ~8 rows, offset to center items
    int content_rows = (SV_BORDER_H - SV_TITLE_H - SV_FOOTER_H - 8) / SV_ITEM_H;
    int offset = (content_rows - sv->menu_item_count) / 2;
    if (offset < 0) offset = 0;

    for (int i = 0; i < sv->menu_item_count; i++) {
        sv_render_menu_item(i + offset, menu_items[i].label, menu_items[i].value,
                           i == sv->menu_cursor);
    }
}

// ============================================================
// Machine-select submenu — coco2and3.md Step 6
// ============================================================

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

        case HID_ENTER: {
            uint8_t selected = MACHINE_VALUES[sv->menu_cursor];
            if (selected == g_machine_type) {
                // Already active — go back to main menu without restarting.
                restore_main_menu(sv);
                break;
            }
            pending_machine_type = selected;
            sv->prev_state = SV_MACHINE_SELECT;
            sv->state = SV_CONFIRM_DIALOG;
            sv->confirm_message = (selected == 4) ? "Switch to CoCo 3\nand restart?"
                                                  : "Switch to CoCo 2\nand restart?";
            sv->confirm_yes_selected = false;
            sv->confirm_callback = [](bool accepted, void* ctx) {
                Supervisor_t* s = (Supervisor_t*)ctx;
                if (accepted) {
                    // Persists selection in NVS and calls esp_restart() — no return.
                    supervisor_set_machine_type(pending_machine_type);
                }
                // Cancel: return to the machine-select submenu.
                s->state = SV_MACHINE_SELECT;
                s->needs_redraw = true;
            };
            sv->confirm_context = sv;
            sv->needs_redraw = true;
            break;
        }

        case HID_ESC:
            restore_main_menu(sv);
            break;

        case HID_F1:
            supervisor_toggle();
            break;
    }
}

void sv_machine_select_render(Supervisor_t* sv) {
    sv_render_frame("Select Machine", "Up/Dn  ENTER  ESC Cancel");

    int content_rows = (SV_BORDER_H - SV_TITLE_H - SV_FOOTER_H - 8) / SV_ITEM_H;
    int offset = (content_rows - 2) / 2;
    if (offset < 0) offset = 0;

    for (int i = 0; i < 2; i++) {
        const char* marker = (MACHINE_VALUES[i] == g_machine_type) ? "(current)" : NULL;
        sv_render_menu_item(i + offset, MACHINE_LABELS[i], marker,
                            i == sv->menu_cursor);
    }
}

// ============================================================
// Settings submenu — Debug Log / RS-232 Pak (mutually exclusive)
// ============================================================
//
// Both toggles map onto the single serial-port mode (they share UART0):
//   row 0 Debug Log : ON  -> SERIAL_MODE_DEBUG  (forces RS-232 off)
//                     OFF -> SERIAL_MODE_OFF
//   row 1 RS-232 Pak: ON  -> SERIAL_MODE_RS232  (forces Debug off)
//                     OFF -> SERIAL_MODE_OFF

static const char* const SETTINGS_LABELS[2] = { "Debug Log", "RS-232 Pak" };

static void settings_toggle(Supervisor_t* sv, int row) {
    SerialPortMode next;
    if (row == 0) {  // Debug Log
        next = (g_serial_mode == SERIAL_MODE_DEBUG) ? SERIAL_MODE_OFF
                                                     : SERIAL_MODE_DEBUG;
    } else {         // RS-232 Pak
        next = (g_serial_mode == SERIAL_MODE_RS232) ? SERIAL_MODE_OFF
                                                     : SERIAL_MODE_RS232;
    }
    serial_mode_apply(next);
    supervisor_save_serial_mode(next);
    sv->needs_redraw = true;
}

void sv_settings_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed) {
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
            settings_toggle(sv, sv->menu_cursor);
            break;

        case HID_ESC:
            restore_main_menu(sv);
            break;

        case HID_F1:
            supervisor_toggle();
            break;
    }
}

void sv_settings_render(Supervisor_t* sv) {
    sv_render_frame("Settings", "Up/Dn  ENTER Toggle  ESC");

    int content_rows = (SV_BORDER_H - SV_TITLE_H - SV_FOOTER_H - 8) / SV_ITEM_H;
    int offset = (content_rows - 2) / 2;
    if (offset < 0) offset = 0;

    const bool on[2] = {
        g_serial_mode == SERIAL_MODE_DEBUG,
        g_serial_mode == SERIAL_MODE_RS232,
    };

    for (int i = 0; i < 2; i++) {
        sv_render_menu_item(i + offset, SETTINGS_LABELS[i], on[i] ? "ON" : "OFF",
                            i == sv->menu_cursor);
    }
}

// ============================================================
// Debug submenu — page picker (Status / Hex Dump / RS-232 Pak)
// ============================================================

static const char* const DEBUG_PAGE_LABELS[SV_DBG_PAGE_COUNT] = {
    "CPU / GIME Status",
    "Memory Hex Dump",
    "RS-232 Pak",
};

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
            if (sv->menu_cursor < SV_DBG_PAGE_COUNT - 1) {
                sv->menu_cursor++;
                sv->needs_redraw = true;
            }
            break;

        case HID_ENTER:
            sv_debug_set_page((SV_DebugPage)sv->menu_cursor);
            sv->prev_state = sv->state;
            sv->state = SV_DEBUG_DUMP;
            sv->needs_redraw = true;
            break;

        case HID_ESC:
            restore_main_menu(sv);
            break;

        case HID_F1:
            supervisor_toggle();
            break;
    }
}

void sv_debug_menu_render(Supervisor_t* sv) {
    sv_render_frame("Debug", "Up/Dn  ENTER Open  ESC");

    int content_rows = (SV_BORDER_H - SV_TITLE_H - SV_FOOTER_H - 8) / SV_ITEM_H;
    int offset = (content_rows - SV_DBG_PAGE_COUNT) / 2;
    if (offset < 0) offset = 0;

    for (int i = 0; i < SV_DBG_PAGE_COUNT; i++) {
        sv_render_menu_item(i + offset, DEBUG_PAGE_LABELS[i], NULL,
                            i == sv->menu_cursor);
    }
}
