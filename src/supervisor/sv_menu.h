/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : sv_menu.h
 *  Module : Supervisor OSD main menu interface
 * ============================================================
*/

/*
 * sv_menu.h - Main menu for supervisor OSD
 */

#ifndef SV_MENU_H
#define SV_MENU_H

#include <stdint.h>

// Forward declaration
struct Supervisor_t;

enum SV_MenuAction : uint8_t {
    SV_ACT_MOUNT_DISK,
    SV_ACT_MACHINE_SELECT,
    SV_ACT_RESET,
    SV_ACT_SETTINGS,
    SV_ACT_ABOUT,
    SV_ACT_DEBUG,
    SV_ACT_RESUME,
    SV_ACT_RESET_ESP32  // Restart the ESP32 (not the emulated machine)
};

struct SV_MenuItem {
    const char* label;
    SV_MenuAction action;
    const char* value;  // Right-aligned value text, NULL if none
};

void sv_menu_init(Supervisor_t* sv);
void sv_menu_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);
void sv_menu_render(Supervisor_t* sv);
void sv_menu_update_values(Supervisor_t* sv);

// Machine-select submenu (SV_MACHINE_SELECT state). On ENTER with a different
// machine selected, opens a confirm dialog; on accept calls
// supervisor_set_machine_type(), which persists the choice and restarts.
void sv_machine_select_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);
void sv_machine_select_render(Supervisor_t* sv);

// Settings submenu (SV_SETTINGS state). Two mutually-exclusive toggles —
// Debug Log and RS-232 Pak — backed by the single serial-port mode. Enabling
// one disables the other (they share UART0). Changes persist to NVS.
void sv_settings_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);
void sv_settings_render(Supervisor_t* sv);

// Debug submenu (SV_DEBUG_MENU state). Lists the available debug pages
// (Status / Hex Dump / RS-232 Pak). ENTER opens the chosen page, ESC
// returns to the main menu.
void sv_debug_menu_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);
void sv_debug_menu_render(Supervisor_t* sv);

#endif // SV_MENU_H
