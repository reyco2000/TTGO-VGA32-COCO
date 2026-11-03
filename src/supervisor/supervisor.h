/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : supervisor.h
 *  Module : OSD supervisor interface — overlay lifecycle and event dispatch
 * ============================================================
*/

/*
 * supervisor.h - On-Screen Display (OSD) supervisor for CoCo_ESP32
 *
 * F1 toggles the supervisor overlay. While active, emulation is paused.
 * Provides disk mounting, machine reset, and settings via menu UI.
 */

#ifndef SUPERVISOR_H
#define SUPERVISOR_H

#include <stdint.h>
#include <stdbool.h>
#include "../core/machine.h"
#include "../utils/debug.h"   // SerialPortMode
#include "../hal/hal.h"       // KbdLayout
#include "sv_disk.h"

enum SV_State : uint8_t {
    SV_INACTIVE = 0,
    SV_MAIN_MENU,
    SV_FILE_BROWSER,
    SV_MACHINE_SELECT,
    SV_SETTINGS,
    SV_ABOUT,
    SV_DEBUG_MENU,
    SV_DEBUG_DUMP,
    SV_DEBUG_DUMP_NAME,   // Memory-dump filename entry / save / result screen
    SV_CONFIRM_DIALOG,
    SV_KEYMAP_LIST,
    SV_KEYMAP_CAPTURE,
    SV_KEYMAP_TEST,
    SV_JOY_SENSE,        // Mouse Sensitivity adjust screen
    SV_WIFI,             // WiFi / Debug server status + control screen
    SV_FUJINET,          // DriveWire / FujiNet bus settings screen
    SV_KEYBOARD_MENU,    // Settings -> Keyboard submenu (language, Key Mapper)
    SV_CART_POPUP,       // popup over Multi-Pak: cartridge on/off confirm, ROM missing
    SV_CPU_POPUP,        // popup over the Machine submenu: 6809 / 6309 picker
    SV_MACHINE_MENU,     // Settings -> Machine submenu (machine type, CPU, CoCo 3 memory)
    SV_SOUND_MENU,       // Settings -> Multi-Pak submenu (Orchestra-90, Speech/Sound, Hard Disk)
    SV_RAM_SELECT,       // popup over the Machine submenu: CoCo 3 memory size, then confirm
    SV_CART,             // Load Cart screen: pick a .ccc / .rom cartridge, or unload it
};

struct SV_FileEntry;

typedef struct Supervisor_t {
    SV_State state;
    SV_State prev_state;

    Machine* machine;

    // Framebuffer snapshot (PSRAM)
    uint16_t* emu_snapshot;

    // Menu state
    int8_t   menu_cursor;
    int8_t   menu_scroll_offset;
    uint8_t  menu_item_count;

    // File browser state
    char     current_path[256];
    int16_t  file_cursor;
    int16_t  file_scroll_offset;
    int16_t  file_count;
    uint8_t  target_drive;

    // File entries (allocated once)
    SV_FileEntry* file_entries;

    // Confirm dialog
    const char* confirm_message;
    void (*confirm_callback)(bool accepted, void* ctx);
    void* confirm_context;
    bool  confirm_yes_selected;

    // Rendering
    bool     needs_redraw;
    uint32_t last_blink_ms;
    bool     blink_on;

} Supervisor_t;

// Public API
void supervisor_init(Machine* m);
void supervisor_toggle(void);
bool supervisor_is_active(void);
uint8_t supervisor_state(void);   // SV_State of the current OSD screen (0 = closed)

void supervisor_on_key(uint8_t hid_usage, bool pressed);

// Returns true if supervisor consumed the frame (skip emulation)
bool supervisor_update_and_render(void);

void supervisor_quick_mount_last_disk(Machine* m);

void supervisor_save_state(void);
void supervisor_load_state(void);

// Runtime machine-type selection.
// Returns the stored NVS value, or the compile-time MACHINE_TYPE if none.
// Called from the main sketch before machine_init() to seed g_machine_type.
uint8_t supervisor_load_machine_type(void);

// Persist a new machine type in NVS and reboot the device. Saves supervisor
// state (mounted disks, last_dir) first so the new boot auto-mounts them.
// Does not return — calls esp_restart().
void supervisor_set_machine_type(uint8_t machine_type);

// Flush disk caches, save supervisor state and restart. Never returns.
void supervisor_save_and_restart(void);

// Serial-port ownership persistence (NVS "sv" namespace, key "serial_mode").
// load returns the stored mode, or SERIAL_MODE_FIRST_BOOT_DEFAULT if unset.
// Called from setup() before the boot banner so the banner respects the mode.
SerialPortMode supervisor_load_serial_mode(void);
void       supervisor_save_serial_mode(SerialPortMode mode);

// Mouse sensitivity (level 1..10) + invert-Y, persisted in NVS ("sv").
void supervisor_save_joystick(uint8_t level, bool invert);
void supervisor_load_joystick(void);   // reads NVS, applies to HAL (also Joy-Keyboard mode)
// Joy-Keyboard Emulation mode (JOY_KBD_OFF / RIGHT / LEFT), NVS key "joy_kbd".
void supervisor_save_joy_kbd(uint8_t mode);

// PS/2 keyboard-layout persistence (NVS "sv" namespace, key "kbd_layout").
// load returns the stored layout, or KBD_LAYOUT_FIRST_BOOT_DEFAULT if unset.
// Called from setup() before hal_init() so the keyboard starts in the
// user's chosen layout.
KbdLayout supervisor_load_kbd_layout(void);
void      supervisor_save_kbd_layout(KbdLayout layout);

// Key Mapper persistence (NVS "sv" namespace, key "keymap"): the whole
// hal_keyboard remap table as one blob. load fills the table (all-default
// if NVS has nothing); called from setup() before the emulator runs.
void supervisor_load_keymap(void);
void supervisor_save_keymap(void);

// CPU selection (NVS "sv" namespace, key "cpu_variant"): CPU_VARIANT_MC6809 or
// CPU_VARIANT_HD6309. load returns the stored value, or the compile-time
// CPU_VARIANT if none; called from setup() before machine_init() to seed
// g_cpu_variant. save only persists: the caller restarts to apply it.
uint8_t supervisor_load_cpu_variant(void);
void    supervisor_save_cpu_variant(uint8_t variant);

// CoCo 3 memory size in KB (NVS "sv" namespace, key "coco3_ram"): 128, 512,
// 1024 or 2048. load returns the stored value, or the compile-time
// COCO3_RAM_KB if none or invalid; called from setup() before machine_init()
// to seed g_coco3_ram_kb. save only persists: the caller restarts to apply it.
uint16_t supervisor_load_coco3_ram(void);
void     supervisor_save_coco3_ram(uint16_t kb);

// CoCo 3 PMODE 4 artifact mode (NVS "sv" namespace, key "artifacts"):
// GIME_ARTIFACT_* from core/gime_artifact.h, default off. load is applied in
// setup(); the Machine menu cycles it live with tcc1014_set_artifacts() and saves.
uint8_t supervisor_load_artifacts(void);
// CoCo 3 monitor (NVS "sv" namespace, key "monitor"): GIME_MONITOR_* from
// core/gime_monitor.h; RGB when unset, and an out-of-range value is ignored.
uint8_t supervisor_load_monitor(void);
void    supervisor_save_monitor(uint8_t monitor);
void supervisor_save_artifacts(uint8_t mode);

// Cartridge settings (NVS "sv" namespace): key "orch90" turns the
// Orchestra-90 DAC ports on, key "ssc" the Speech/Sound Cartridge, key
// "vhd_en" the virtual hard disk at $FF80-$FF86 (its images are remembered
// as "hd0_path"/"hd1_path" by supervisor_save_state), key
// "cart_rom" names the ROM (up to 128 KB) to load at $C000 instead of Disk
// BASIC ("" = default): a bare name in ROM_BASE_PATH, or a full SD path
// starting with '/' (Load Cart screen). The Orchestra-90 toggle sets it to
// ROM_ORCH90_FILE. load applies them (g_orch90_enabled, g_ssc_enabled, g_vhd_enabled,
// g_cart_rom_request); called from setup() after dw_bus_load_config() and before machine_init(). The saves
// only persist: the caller restarts the emulator to apply them.
void        supervisor_load_cart_config(void);
void        supervisor_save_orch90(bool enabled);
void        supervisor_save_ssc(bool enabled);
void        supervisor_save_vhd(bool enabled);
void        supervisor_save_cart_rom(const char* name);
// The cartridge this boot asked for, and the one stored for the next restart
// ("" = none). They differ while a Load Cart change is waiting for a restart.
const char* supervisor_cart_rom_boot(void);
const char* supervisor_cart_rom_saved(void);

// Access global supervisor (for disk manager sub-screens)
Supervisor_t* supervisor_get(void);

#endif // SUPERVISOR_H
