/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
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
    SV_ACT_LOAD_CART,
    SV_ACT_WIFI,
};

void sv_menu_init(Supervisor_t* sv);
void sv_menu_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);
void sv_menu_render(Supervisor_t* sv);

// Force the next render to repaint the whole main menu; otherwise only the
// two tiles whose selection changed are redrawn.
void sv_menu_invalidate(void);

// Go back to the main menu with the cursor on the tile that opened the
// current screen (Load Cart, and WiFi when opened from its tile).
void sv_menu_return(Supervisor_t* sv);

// Machine select (SV_MACHINE_SELECT state): a popup over the Machine submenu,
// opened from its "Machine" row. Picking the other machine asks to confirm
// the restart in the same window (defaults to No); on Yes it calls
// supervisor_set_machine_type(), which persists the choice and restarts.
void sv_machine_select_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);
void sv_machine_select_render(Supervisor_t* sv);
// Force the next render to repaint the whole popup window.
void sv_machine_select_invalidate(void);

// Settings submenu (SV_SETTINGS state) and its Keyboard submenu
// (SV_KEYBOARD_MENU state): icon lists in the wide green frame. Sub-screens
// use these row numbers to put the cursor back on their own row.
enum SV_SettingsRow : uint8_t {
    SV_SET_MACHINE,     // opens the Machine submenu
    SV_SET_KEYBOARD,    // opens the Keyboard submenu
    SV_SET_JOYSTICK,    // Joy - Mouse Sensitivity screen
    SV_SET_JOYKBD,      // Joy-Keyboard Emulation: OFF / RIGHT / LEFT, live
    SV_SET_WIFI,        // WiFi / Debug screen
    SV_SET_DRIVEWIRE,   // DriveWire screen
    SV_SET_SOUND,       // opens the Multi-Pak submenu
    SV_SET_COUNT
};
// Multi-Pak submenu (SV_SOUND_MENU state): the I/O cartridges. Orchestra-90,
// Speech/Sound and Hard Disk ask to confirm and restart the emulator; the
// RS-232 Pak toggles live.
enum SV_SoundRow : uint8_t {
    SV_SND_ORCH90,      // Orchestra-90 (also swaps the cartridge ROM)
    SV_SND_SSC,         // Speech/Sound Cartridge
    SV_SND_HARDDISK,    // VCC virtual hard disk at $FF80-$FF86: Off / RS-DOS / RGB-DOS picker
    SV_SND_RS232,       // RS-232 Pak toggle, live (shares UART0 with the Echo Log)
    SV_SND_COUNT
};
// Machine submenu (SV_MACHINE_MENU state). Everything here is fixed at boot,
// so each row asks to confirm and restarts the emulator.
enum SV_MachineRow : uint8_t {
    SV_MACH_TYPE,       // CoCo 2 / CoCo 3 (machine-select popup)
    SV_MACH_CPU,        // MC6809 / HD6309
    SV_MACH_RAM,        // CoCo 3 memory: 128K / 512K / 1024K / 2048K (CoCo 2: fixed 64K)
    SV_MACH_ARTIFACTS,  // CoCo 3 PMODE 4 artifact colours ON / OFF (CoCo 2: always on)
    SV_MACH_SOUND_MIX,  // Sound mixer model: XRoar / MAME (both machines)
    SV_MACH_COUNT
};
enum SV_KeyboardRow : uint8_t {
    SV_KBD_LANGUAGE,    // cycles the PS/2 keyboard layout
    SV_KBD_MAPPER,      // opens the Key Mapper screens
    SV_KBD_COUNT
};
void sv_settings_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);
void sv_settings_render(Supervisor_t* sv);
void sv_keyboard_menu_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);
void sv_keyboard_menu_render(Supervisor_t* sv);
void sv_machine_menu_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);
void sv_machine_menu_render(Supervisor_t* sv);
void sv_sound_menu_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);
void sv_sound_menu_render(Supervisor_t* sv);
// Force the next render to repaint the whole Settings / Keyboard list.
void sv_settings_invalidate(void);

// SV_CART_POPUP state: popup over the Cartridges list, opened from one of
// its rows. Asks to confirm the restart (No / Yes, defaults to No), or, when
// turning a cartridge on without its ROM file(s) on the SD card, names the
// missing file(s) and changes nothing. From the Hard Disk row it is a picker
// instead: Off / On with RS-DOS / On with RGB-DOS, restarting on a change.
void sv_cart_popup_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);
void sv_cart_popup_render(Supervisor_t* sv);

// SV_CPU_POPUP state: popup over the Machine submenu, opened from its CPU row.
// Picker: 6809 / 6309, the running CPU preselected and marked "current";
// ENTER on the other one persists it and restarts.
void sv_cpu_popup_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);
void sv_cpu_popup_render(Supervisor_t* sv);

// SV_RAM_SELECT state: popup over the Machine submenu, opened from its
// "CoCo 3 Memory" row. Lists the four sizes ("(current)" on the active one);
// picking another asks to confirm the restart (No / Yes, defaults to No), then
// persists it with supervisor_save_coco3_ram() and restarts.
void sv_ram_select_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);
void sv_ram_select_render(Supervisor_t* sv);

// 32x12 row icons for lists in the wide frame, drawn in code. bg/fg are the
// row's own colours so an icon reads on the green box and on the selection bar.
enum SV_RowIcon : uint8_t {
    ROW_ICON_MACHINE, ROW_ICON_SERIAL, ROW_ICON_KEYBOARD, ROW_ICON_JOYSTICK,
    ROW_ICON_WIFI, ROW_ICON_DRIVEWIRE, ROW_ICON_LANGUAGE, ROW_ICON_KEYCAP,
    ROW_ICON_CHIP, ROW_ICON_PAGE, ROW_ICON_SD, ROW_ICON_SPIDER,
    ROW_ICON_CHECK, ROW_ICON_STOP, ROW_ICON_CROSS, ROW_ICON_NOTE,
    ROW_ICON_CART, ROW_ICON_FOLDER, ROW_ICON_SPEECH, ROW_ICON_MULTIPAK,
    ROW_ICON_HARDDISK, ROW_ICON_ARROWS,
};
void sv_menu_draw_row_icon(int icon, int x, int y, uint16_t bg, uint16_t fg);

// Debug submenu (SV_DEBUG_MENU state), an icon list in the wide green frame:
// the debug pages (Status / Hex Dump / RS-232 Pak), "Dump RAM to SD", and the
// Echo Log toggle (debug output on the serial port; forces the RS-232 Pak
// off, they share UART0). ESC returns to the main menu.
void sv_debug_menu_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);
void sv_debug_menu_render(Supervisor_t* sv);
// Force the next render to repaint the whole Debug submenu.
void sv_debug_menu_invalidate(void);

#endif // SV_MENU_H
