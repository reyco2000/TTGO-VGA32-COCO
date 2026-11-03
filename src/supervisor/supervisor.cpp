/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : supervisor.cpp
 *  Module : OSD supervisor — F3-activated overlay with menu, disk mounting, and machine reset
 * ============================================================
*/

/*
 * supervisor.cpp - On-Screen Display supervisor for CoCo_ESP32
 *
 * Lifecycle: F3 toggles active. While active, emulation paused.
 * Framebuffer snapshot saved on activate, restored on deactivate.
 */

#include "supervisor.h"
#include "sv_menu.h"
#include "sv_filebrowser.h"
#include "sv_disk.h"
#include "sv_debug.h"
#include "sv_keymap.h"
#include "sv_joystick.h"
#include "sv_wifi.h"
#include "sv_fujinet.h"
#include "sv_cart.h"
#include "../core/orch90.h"
#include "../core/ssc/ssc.h"
#include "../core/vhd.h"
#include "../net/dw_bus.h"
#include "sv_render.h"
#include "../hal/hal.h"
#include "../utils/debug.h"
#include "../../config.h"
#include <Preferences.h>

// External TFT instance from hal_video.cpp
extern OSDCanvas* hal_video_get_canvas(void);

// Global supervisor instance
static Supervisor_t sv;

// HID usage codes for disk manager
#define HID_UP    0x52
#define HID_DOWN  0x51
#define HID_ENTER 0x28
#define HID_ESC   0x29
#define HID_LEFT  0x50
#define HID_RIGHT 0x4F

// ============================================================
// Framebuffer snapshot — DISABLED
// ============================================================
// readRect() requires MISO wired (not available on this board)
// and 153KB PSRAM (not available on this N16R8 module).
// Instead, on deactivate we simply let the next emulated frame
// repaint the display naturally.

static void capture_snapshot(void) {
    // No-op: cannot read back from TFT without MISO
}

static void restore_snapshot(void) {
    // Clear the OSD area so no ghost pixels remain in border margins.
    // The emulator repaints its active area on the next frame; the main
    // menu box is wider than the submenu box, so clear that whole rect.
    sv_render_wide_clear();
}

// Wide-box bookkeeping: the main menu, Disk Manager, Settings, Debug screens and About share a frame wider
// than the submenu one, so it is cleared before a submenu draws and the
// screen is fully repainted whenever it is entered from another state.
static bool uses_wide_frame(SV_State st) {
    switch (st) {
        case SV_MAIN_MENU:
        case SV_FILE_BROWSER:
        case SV_SETTINGS:
        case SV_KEYBOARD_MENU:
        case SV_MACHINE_MENU:
        case SV_SOUND_MENU:
        case SV_ABOUT:
        case SV_DEBUG_MENU:
        case SV_DEBUG_DUMP:
        case SV_DEBUG_DUMP_NAME:
        case SV_KEYMAP_LIST:
        case SV_KEYMAP_TEST:
        case SV_WIFI:
        case SV_FUJINET:
        case SV_CART:
            return true;
        default:
            return false;
    }
}

static bool     s_main_box_on_screen = false;
static SV_State s_last_rendered = SV_INACTIVE;

// ============================================================
// About screen
// ============================================================

// COCOBYTE LOGO — 91x12 pixels, horizontal bitmap (MSB first, 12 bytes/row)
static const unsigned char bitmap_cocobyte[] PROGMEM = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xf0, 0x01, 0x1b, 0xf0, 0x01, 0x1b, 0xfe, 0xf9, 0x7c, 0xff, 0xfd, 0x03,
    0xf8, 0x83, 0x3b, 0xf8, 0x83, 0x3b, 0xfe, 0xfb, 0x7c, 0xff, 0xfd, 0x03,
    0xfc, 0xc7, 0x7b, 0xfc, 0xc7, 0x7b, 0xfe, 0xfb, 0x7c, 0xff, 0xfd, 0x03,
    0xfe, 0xef, 0xfb, 0xfe, 0xef, 0xfb, 0xf0, 0xfb, 0x7f, 0xff, 0x7d, 0x00,
    0xfe, 0xe1, 0xfb, 0xfe, 0xe1, 0xfb, 0xfe, 0xfb, 0x7f, 0x7c, 0xfc, 0x03,
    0x7e, 0xe0, 0xfb, 0x7e, 0xe0, 0xfb, 0xfe, 0xf1, 0x3f, 0x7c, 0xfc, 0x03,
    0xfe, 0xe1, 0xfb, 0xfe, 0xe1, 0xfb, 0xfe, 0xe3, 0x1f, 0x7c, 0xfc, 0x03,
    0xfe, 0xef, 0xfb, 0xfe, 0xef, 0xfb, 0xe0, 0xc3, 0x0f, 0x7c, 0x7c, 0x00,
    0xfc, 0xc7, 0x7b, 0xfc, 0xc7, 0x7b, 0xfe, 0xc3, 0x0f, 0x7c, 0xfc, 0x07,
    0xf8, 0x83, 0x3b, 0xf8, 0x83, 0x3b, 0xfe, 0xc3, 0x0f, 0x7c, 0xfc, 0x07,
    0xf0, 0x01, 0x1b, 0xf0, 0x01, 0x1b, 0xfe, 0xc1, 0x0f, 0x7c, 0xfc, 0x07
};
#define LOGO_W  91
#define LOGO_H  12
#define LOGO_ROW_BYTES  12  // ceil(91/8)

// Draw monochrome XBM bitmap scaled to TFT (set bits = pixel on)
static void draw_mono_bitmap_scaled(OSDCanvas* tft, int x0, int y0,
                                    const unsigned char* bmp, int w, int h,
                                    int scale, uint16_t color) {
    tft->startWrite();
    for (int row = 0; row < h; row++) {
        for (int col = 0; col < w; col++) {
            uint8_t b = pgm_read_byte(&bmp[row * LOGO_ROW_BYTES + (col >> 3)]);
            if (b & (1 << (col & 7))) {  // XBM format: LSB first
                if (scale == 1) {
                    tft->drawPixel(x0 + col, y0 + row, color);
                } else {
                    tft->fillRect(x0 + col * scale, y0 + row * scale,
                                  scale, scale, color);
                }
            }
        }
    }
    tft->endWrite();
}

static void about_render(Supervisor_t* s) {
    (void)s;
    OSDCanvas* tft = hal_video_get_canvas();
    if (!tft) return;

    // No title: the logo sits in the title row.
    sv_render_wide_frame("", "ESC Back   F3 Exit");

    const int cx  = SVW_BOX_X + SVW_BOX_W / 2;  // horizontal center
    const int x1  = cx - 152;                    // separator left
    const int x2  = cx + 152;                    // and right

    // --- Row 1: Logo (scaled 2x = 182x24) centered ---
    const int logo_scale = 2;
    const int logo_sw = LOGO_W * logo_scale;  // 182
    const int logo_sh = LOGO_H * logo_scale;  // 24
    const int logo_y = SVW_BOX_Y + 13;      // just under the top strip
    draw_mono_bitmap_scaled(tft, cx - logo_sw / 2, logo_y, bitmap_cocobyte,
                            LOGO_W, LOGO_H, logo_scale, SVW_DKBLUE);

    // --- Row 2: Project subtitle (two lines) ---
    int y = logo_y + logo_sh + 10;
    tft->setTextFont(1);
    tft->setTextColor(SVW_BLACK, SVW_GREEN);
    tft->setTextDatum(TC_DATUM);
    tft->drawString("Tandy Color Computer 2 & 3 Emulator", cx, y);
    y += 10;
    tft->drawString("ESP32 TTGO VGA", cx, y);

    // --- Separator ---
    y += 16;
    tft->drawFastHLine(x1, y, x2 - x1, SVW_DKBLUE);

    // --- Rows 3-5: Copyright and credits ---
    y += 10;
    tft->drawString("(C) 2026 Reinaldo Torres", cx, y);
    y += 12;
    tft->setTextColor(SVW_DKBLUE, SVW_GREEN);
    tft->drawString("Based on XRoar", cx, y);
    y += 12;
    tft->drawString("Co-developed with Claude Code", cx, y);

    // --- Separator ---
    y += 16;
    tft->drawFastHLine(x1, y, x2 - x1, SVW_DKBLUE);

    // --- Row 6: Version badge ---
    y += 9;
    // Built from config.h so the About screen cannot drift out of step with
    // the version the debug API reports (it read "Beta 1.0" until v0.81).
    const char* ver_str = "v" FIRMWARE_VERSION "  Build " FIRMWARE_BUILD_DATE;
    int ver_w = tft->textWidth(ver_str) + 16;
    int ver_x = cx - ver_w / 2;
    tft->fillRect(ver_x, y, ver_w, 13, SVW_WHITE);
    tft->drawRect(ver_x, y, ver_w, 13, SVW_DKBLUE);
    tft->setTextColor(SVW_BLACK, SVW_WHITE);
    tft->drawString(ver_str, cx, y + 3);

    // --- Row 7: System stats ---
    y += 21;
    char stats[64];
    snprintf(stats, sizeof(stats), "Heap:%dK  PSRAM:%dK",
             ESP.getFreeHeap() / 1024, ESP.getFreePsram() / 1024);
    tft->setTextColor(SVW_DKBLUE, SVW_GREEN);
    tft->drawString(stats, cx, y);

    tft->setTextDatum(TL_DATUM);
}

// ============================================================
// Confirm dialog
// ============================================================

static void confirm_on_key(Supervisor_t* s, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;

    switch (hid_usage) {
        case HID_LEFT:
        case HID_RIGHT:
            s->confirm_yes_selected = !s->confirm_yes_selected;
            s->needs_redraw = true;
            break;

        case HID_ENTER:
            if (s->confirm_callback) {
                s->confirm_callback(s->confirm_yes_selected, s->confirm_context);
            }
            break;

        case HID_ESC:
            s->state = s->prev_state;
            s->needs_redraw = true;
            break;
    }
}

// ============================================================
// Public API
// ============================================================

void supervisor_init(Machine* m) {
    memset(&sv, 0, sizeof(Supervisor_t));
    sv.state = SV_INACTIVE;
    sv.machine = m;
    sv.emu_snapshot = nullptr;
    sv.file_entries = nullptr;
    strcpy(sv.current_path, "/");

    // Initialize FDC
    sv_disk_init(&m->fdc);

    // Wire NMI callback
    m->fdc.nmi_callback = [](void* ctx, bool active) {
        Machine* mach = (Machine*)ctx;
        mc6809_nmi(&mach->cpu, active);
    };
    // Wire HALT callback (DRQ synchronization — matches XRoar signal_halt)
    m->fdc.halt_callback = [](void* ctx, bool halted) {
        Machine* mach = (Machine*)ctx;
        mach->cpu.halted = halted;
    };
    m->fdc.callback_context = m;

    // Initialize render engine
    OSDCanvas* tft = hal_video_get_canvas();
    if (tft) {
        sv_render_init(tft);
    }

    sv_menu_init(&sv);
    sv_filebrowser_init(&sv);
    sv_debug_init(&sv);

    DEBUG_PRINT("Supervisor: initialized");
}

void supervisor_toggle(void) {
    if (sv.state == SV_INACTIVE) {
        // Activate
        capture_snapshot();
        vhd_flush_all();   // the user may pull the SD card next
        sv.state = SV_MAIN_MENU;
        sv.menu_cursor = 0;
        sv.needs_redraw = true;
        s_last_rendered = SV_INACTIVE;
        DEBUG_PRINT("Supervisor: activated");
    } else {
        // Deactivate
        restore_snapshot();
        s_main_box_on_screen = false;
        sv.state = SV_INACTIVE;
        hal_video_force_repaint();
        DEBUG_PRINT("Supervisor: deactivated");
    }
}

bool supervisor_is_active(void) {
    return sv.state != SV_INACTIVE;
}

uint8_t supervisor_state(void) { return (uint8_t)sv.state; }

void supervisor_on_key(uint8_t hid_usage, bool pressed) {
    // F1 within supervisor = close
    if (hid_usage == 0x3A && pressed) {
        supervisor_toggle();
        return;
    }

    switch (sv.state) {
        case SV_MAIN_MENU:
            sv_menu_on_key(&sv, hid_usage, pressed);
            break;

        case SV_FILE_BROWSER:
            sv_filebrowser_on_key(&sv, hid_usage, pressed);
            break;

        case SV_MACHINE_SELECT:
            sv_machine_select_on_key(&sv, hid_usage, pressed);
            break;

        case SV_SETTINGS:
            sv_settings_on_key(&sv, hid_usage, pressed);
            break;

        case SV_ABOUT:
            if (pressed && (hid_usage == 0x29 || hid_usage == 0x28)) {  // ESC or ENTER
                sv.state = SV_MAIN_MENU;
                sv.needs_redraw = true;
            }
            break;

        case SV_DEBUG_MENU:
            sv_debug_menu_on_key(&sv, hid_usage, pressed);
            break;

        case SV_DEBUG_DUMP:
            sv_debug_on_key(&sv, hid_usage, pressed);
            break;

        case SV_DEBUG_DUMP_NAME:
            sv_debug_dump_on_key(&sv, hid_usage, pressed);
            break;

        case SV_CONFIRM_DIALOG:
            confirm_on_key(&sv, hid_usage, pressed);
            break;

        case SV_KEYMAP_LIST:
            sv_keymap_on_key(&sv, hid_usage, pressed);
            break;

        case SV_WIFI:
            sv_wifi_on_key(&sv, hid_usage, pressed);
            break;

        case SV_FUJINET:
            sv_fujinet_on_key(&sv, hid_usage, pressed);
            break;

        case SV_CART:
            sv_cart_on_key(&sv, hid_usage, pressed);
            break;

        case SV_CART_POPUP:
            sv_cart_popup_on_key(&sv, hid_usage, pressed);
            break;

        case SV_SOUND_MENU:
            sv_sound_menu_on_key(&sv, hid_usage, pressed);
            break;

        case SV_CPU_POPUP:
            sv_cpu_popup_on_key(&sv, hid_usage, pressed);
            break;

        case SV_MACHINE_MENU:
            sv_machine_menu_on_key(&sv, hid_usage, pressed);
            break;

        case SV_RAM_SELECT:
            sv_ram_select_on_key(&sv, hid_usage, pressed);
            break;

        case SV_KEYBOARD_MENU:
            sv_keyboard_menu_on_key(&sv, hid_usage, pressed);
            break;

        case SV_JOY_SENSE:
            sv_joystick_on_key(&sv, hid_usage, pressed);
            break;

        // SV_KEYMAP_CAPTURE / SV_KEYMAP_TEST take raw VKs directly via
        // sv_keymap_on_raw_vk() — events never reach this HID route.

        default:
            break;
    }
}

bool supervisor_update_and_render(void) {
    if (sv.state == SV_INACTIVE) return false;

    if (sv.state == SV_JOY_SENSE) {
        sv_joystick_tick(&sv);   // polls mouse, sets needs_redraw
    } else if (sv.state == SV_WIFI) {
        sv_wifi_tick(&sv);       // refresh on WiFi state/IP change
    } else if (sv.state == SV_FUJINET) {
        sv_fujinet_tick(&sv);    // refresh on DriveWire link change
    }

    if (!sv.needs_redraw) {
        delay(16);  // ~60 fps cap while in supervisor
        return true;
    }

    sv.needs_redraw = false;

    if (uses_wide_frame(sv.state)) {
        if (s_last_rendered != sv.state) {
            switch (sv.state) {
                case SV_MAIN_MENU:    sv_menu_invalidate();        break;
                case SV_FILE_BROWSER: sv_filebrowser_invalidate(); break;
                case SV_DEBUG_MENU:   sv_debug_menu_invalidate();  break;
                case SV_ABOUT:        break;    // always repaints in full
                case SV_SETTINGS:
                case SV_MACHINE_MENU:
                case SV_SOUND_MENU:
                case SV_KEYBOARD_MENU: sv_settings_invalidate();   break;
                case SV_KEYMAP_LIST:
                case SV_KEYMAP_TEST:   sv_keymap_invalidate();     break;
                case SV_WIFI:          sv_wifi_invalidate();       break;
                case SV_FUJINET:       sv_fujinet_invalidate();    break;
                case SV_CART:          sv_cart_invalidate();       break;
                default:              sv_debug_invalidate();       break;
            }
        }
        s_main_box_on_screen = true;
    } else if (sv.state == SV_MACHINE_SELECT) {
        // Popup over the Settings list: leave the wide frame in place.
        if (s_last_rendered != sv.state) sv_machine_select_invalidate();
    } else if (sv.state == SV_KEYMAP_CAPTURE || sv.state == SV_JOY_SENSE ||
               sv.state == SV_CART_POPUP || sv.state == SV_CPU_POPUP ||
               sv.state == SV_RAM_SELECT) {
        // Popups over the Key Mapper keyboard / the Settings, Machine and
        // Sound Carts lists: leave the wide frame in place.
    } else if (s_main_box_on_screen && sv.state != SV_CONFIRM_DIALOG) {
        // The confirm dialog is left floating over the main menu.
        sv_render_wide_clear();
        s_main_box_on_screen = false;
    }
    s_last_rendered = sv.state;

    switch (sv.state) {
        case SV_MAIN_MENU:
            sv_menu_render(&sv);
            break;

        case SV_FILE_BROWSER:
            sv_filebrowser_render(&sv);
            break;

        case SV_MACHINE_SELECT:
            sv_machine_select_render(&sv);
            break;

        case SV_SETTINGS:
            sv_settings_render(&sv);
            break;

        case SV_ABOUT:
            about_render(&sv);
            break;

        case SV_DEBUG_MENU:
            sv_debug_menu_render(&sv);
            break;

        case SV_DEBUG_DUMP:
            sv_debug_render(&sv);
            break;

        case SV_DEBUG_DUMP_NAME:
            sv_debug_dump_render(&sv);
            break;

        case SV_CONFIRM_DIALOG:
            sv_render_confirm_dialog(sv.confirm_message, sv.confirm_yes_selected);
            break;

        case SV_KEYMAP_LIST:
            sv_keymap_render(&sv);
            break;

        case SV_KEYMAP_CAPTURE:
            sv_keymap_capture_render(&sv);
            break;

        case SV_KEYMAP_TEST:
            sv_keymap_test_render(&sv);
            break;

        case SV_WIFI:
            sv_wifi_render(&sv);
            break;

        case SV_FUJINET:
            sv_fujinet_render(&sv);
            break;

        case SV_CART:
            sv_cart_render(&sv);
            break;

        case SV_CART_POPUP:
            sv_cart_popup_render(&sv);
            break;

        case SV_SOUND_MENU:
            sv_sound_menu_render(&sv);
            break;

        case SV_CPU_POPUP:
            sv_cpu_popup_render(&sv);
            break;

        case SV_MACHINE_MENU:
            sv_machine_menu_render(&sv);
            break;

        case SV_RAM_SELECT:
            sv_ram_select_render(&sv);
            break;

        case SV_KEYBOARD_MENU:
            sv_keyboard_menu_render(&sv);
            break;

        case SV_JOY_SENSE:
            sv_joystick_render(&sv);
            break;

        default:
            break;
    }

    return true;
}

void supervisor_quick_mount_last_disk(Machine* m) {
    Preferences prefs;
    prefs.begin("sv", true);
    String path = prefs.getString("d0_path", "");
    prefs.end();

    if (path.length() > 0) {
        sv_disk_mount(&m->fdc, 0, path.c_str());
        DEBUG_PRINTF("Quick mount: %s", path.c_str());
    }
}

void supervisor_save_state(void) {
    Preferences prefs;
    prefs.begin("sv", false);
    prefs.putString("last_dir", sv.current_path);
    for (int i = 0; i < SV_DISK_MAX_DRIVES; i++) {
        char key[8];
        snprintf(key, sizeof(key), "d%d_path", i);
        if (sv.machine && sv_disk_is_mounted(&sv.machine->fdc, i)) {
            prefs.putString(key, sv_disk_get_path(&sv.machine->fdc, i));
        } else {
            prefs.remove(key);
        }
    }
    // Hard disks: forgotten once the hard disk is turned off (vhd_en already
    // saved false for the next boot), so turning it back on starts empty.
    if (!prefs.getBool("vhd_en", false)) {
        for (int i = 0; i < VHD_NUM_DRIVES; i++) {
            char key[10];
            snprintf(key, sizeof(key), "hd%d_path", i);
            prefs.remove(key);
        }
    } else if (vhd_enabled()) {
        for (int i = 0; i < VHD_NUM_DRIVES; i++) {
            char key[10];
            snprintf(key, sizeof(key), "hd%d_path", i);
            if (vhd_is_mounted(i)) prefs.putString(key, vhd_get_path(i));
            else                   prefs.remove(key);
        }
    }
    prefs.end();
}

void supervisor_load_state(void) {
    Preferences prefs;
    prefs.begin("sv", true);

    String last_dir = prefs.getString("last_dir", "/");
    strncpy(sv.current_path, last_dir.c_str(), sizeof(sv.current_path) - 1);

    bool auto_mount = prefs.getBool("auto_mount", true);
    if (auto_mount && sv.machine) {
        for (int i = 0; i < SV_DISK_MAX_DRIVES; i++) {
            char key[8];
            snprintf(key, sizeof(key), "d%d_path", i);
            String path = prefs.getString(key, "");
            if (path.length() > 0) {
                sv_disk_mount(&sv.machine->fdc, i, path.c_str());
            }
        }
        for (int i = 0; vhd_enabled() && i < VHD_NUM_DRIVES; i++) {
            char key[10];
            snprintf(key, sizeof(key), "hd%d_path", i);
            String path = prefs.getString(key, "");
            if (path.length() > 0) vhd_mount(i, path.c_str());
        }
    }

    prefs.end();
}

Supervisor_t* supervisor_get(void) {
    return &sv;
}

uint8_t supervisor_load_machine_type(void) {
    Preferences prefs;
    prefs.begin("sv", true);
    uint8_t mt = prefs.getUChar("machine_type", (uint8_t)MACHINE_TYPE);
    prefs.end();
    return mt;
}

uint8_t supervisor_load_cpu_variant(void) {
    Preferences prefs;
    prefs.begin("sv", true);
    uint8_t v = prefs.getUChar("cpu_variant", (uint8_t)CPU_VARIANT);
    prefs.end();
    return (v == CPU_VARIANT_HD6309) ? CPU_VARIANT_HD6309 : CPU_VARIANT_MC6809;
}

void supervisor_save_cpu_variant(uint8_t variant) {
    Preferences prefs;
    prefs.begin("sv", false);
    prefs.putUChar("cpu_variant", variant);
    prefs.end();
}

// Stored as a byte. Before the modes it was a bool, which Preferences also
// stores as a byte: an old true reads as 1, Standard, and false as Off.
uint8_t supervisor_load_artifacts(void) {
    Preferences prefs;
    prefs.begin("sv", true);
    uint8_t mode = prefs.getUChar("artifacts", 0);
    prefs.end();
    return mode;      // tcc1014_set_artifacts() ignores an out-of-range value
}

uint8_t supervisor_load_monitor(void) {
    Preferences prefs;
    prefs.begin("sv", true);
    uint8_t m = prefs.getUChar("monitor", 0);
    prefs.end();
    return m;
}

void supervisor_save_monitor(uint8_t monitor) {
    Preferences prefs;
    prefs.begin("sv", false);
    prefs.putUChar("monitor", monitor);
    prefs.end();
}

void supervisor_save_artifacts(uint8_t mode) {
    Preferences prefs;
    prefs.begin("sv", false);
    prefs.putUChar("artifacts", mode);
    prefs.end();
}

uint16_t supervisor_load_coco3_ram(void) {
    Preferences prefs;
    prefs.begin("sv", true);
    uint16_t kb = prefs.getUShort("coco3_ram", (uint16_t)COCO3_RAM_KB);
    prefs.end();
    return coco3_ram_kb_valid(kb) ? kb : (uint16_t)COCO3_RAM_KB;
}

void supervisor_save_coco3_ram(uint16_t kb) {
    Preferences prefs;
    prefs.begin("sv", false);
    prefs.putUShort("coco3_ram", kb);
    prefs.end();
}

SerialPortMode supervisor_load_serial_mode(void) {
    Preferences prefs;
    prefs.begin("sv", true);
    uint8_t sm = prefs.getUChar("serial_mode", (uint8_t)SERIAL_MODE_FIRST_BOOT_DEFAULT);
    prefs.end();
    if (sm > SERIAL_MODE_RS232) sm = SERIAL_MODE_FIRST_BOOT_DEFAULT;
    return (SerialPortMode)sm;
}

void supervisor_load_keymap(void) {
    int16_t* table = hal_keyboard_remap_table();
    hal_keyboard_remap_clear_all();
    Preferences prefs;
    prefs.begin("sv", true);
    // Only accept a blob of the exact current size — a layout-table size
    // change across firmware versions falls back to all-default bindings.
    if (prefs.getBytesLength("keymap") == KM_COUNT * sizeof(int16_t)) {
        prefs.getBytes("keymap", table, KM_COUNT * sizeof(int16_t));
    }
    prefs.end();
}

void supervisor_save_keymap(void) {
    Preferences prefs;
    prefs.begin("sv", false);
    prefs.putBytes("keymap", hal_keyboard_remap_table(),
                   KM_COUNT * sizeof(int16_t));
    prefs.end();
}

KbdLayout supervisor_load_kbd_layout(void) {
    Preferences prefs;
    prefs.begin("sv", true);
    uint8_t kl = prefs.getUChar("kbd_layout", (uint8_t)KBD_LAYOUT_FIRST_BOOT_DEFAULT);
    prefs.end();
    if (kl >= KBD_LAYOUT_COUNT) kl = KBD_LAYOUT_FIRST_BOOT_DEFAULT;
    return (KbdLayout)kl;
}

void supervisor_save_kbd_layout(KbdLayout layout) {
    Preferences prefs;
    prefs.begin("sv", false);
    prefs.putUChar("kbd_layout", (uint8_t)layout);
    prefs.end();
}

void supervisor_save_serial_mode(SerialPortMode mode) {
    Preferences prefs;
    prefs.begin("sv", false);
    prefs.putUChar("serial_mode", (uint8_t)mode);
    prefs.end();
}

void supervisor_save_joystick(uint8_t level, bool invert) {
    Preferences prefs;
    prefs.begin("sv", false);
    prefs.putUChar("joyLevel", level);
    prefs.putBool("joyInv", invert);
    prefs.end();
}

void supervisor_load_joystick(void) {
    Preferences prefs;
    prefs.begin("sv", true);
    // Default level 7 == config.h divisor 4 (11 - 7 == 4).
    uint8_t level  = prefs.getUChar("joyLevel", 7);
    bool    invert = prefs.getBool("joyInv", false);
    uint8_t kbd    = prefs.getUChar("joy_kbd", JOY_KBD_OFF);
    prefs.end();
    hal_joystick_set_sensitivity(level);   // clamps internally
    hal_joystick_set_invert_y(invert);
    hal_joystick_set_kbd_mode(kbd);        // out-of-range -> OFF
}

void supervisor_save_joy_kbd(uint8_t mode) {
    Preferences prefs;
    prefs.begin("sv", false);
    prefs.putUChar("joy_kbd", mode);
    prefs.end();
}

static char s_cart_rom[256] = "";         // requested at boot
static char s_cart_rom_saved[256] = "";   // stored in NVS for the next restart

void supervisor_load_cart_config(void) {
    Preferences prefs;
    prefs.begin("sv", true);
    g_orch90_enabled = prefs.getBool("orch90", false);
    g_ssc_enabled    = prefs.getBool("ssc", false);
    g_vhd_enabled    = prefs.getBool("vhd_en", false);
    String rom = prefs.getString("cart_rom", "");
    prefs.end();

    strncpy(s_cart_rom, rom.c_str(), sizeof(s_cart_rom) - 1);
    s_cart_rom[sizeof(s_cart_rom) - 1] = '\0';
    strcpy(s_cart_rom_saved, s_cart_rom);
    if (s_cart_rom[0]) {
        g_cart_rom_request[0] = s_cart_rom;
        g_cart_rom_request[1] = s_cart_rom;
    }
    DEBUG_PRINTF("Cartridge: Orchestra-90 %s, Speech/Sound %s, hard disk %s, cart ROM %s",
                 g_orch90_enabled ? "on" : "off", g_ssc_enabled ? "on" : "off",
                 g_vhd_enabled ? "on" : "off", s_cart_rom[0] ? s_cart_rom : "(default)");
}

void supervisor_save_orch90(bool enabled) {
    Preferences prefs;
    prefs.begin("sv", false);
    prefs.putBool("orch90", enabled);
    prefs.end();
}

void supervisor_save_vhd(bool enabled) {
    Preferences prefs;
    prefs.begin("sv", false);
    prefs.putBool("vhd_en", enabled);
    prefs.end();
}

void supervisor_save_ssc(bool enabled) {
    Preferences prefs;
    prefs.begin("sv", false);
    prefs.putBool("ssc", enabled);
    prefs.end();
}

void supervisor_save_cart_rom(const char* name) {
    Preferences prefs;
    prefs.begin("sv", false);
    if (name && name[0]) prefs.putString("cart_rom", name);
    else                 prefs.remove("cart_rom");
    prefs.end();
    strncpy(s_cart_rom_saved, name ? name : "", sizeof(s_cart_rom_saved) - 1);
    s_cart_rom_saved[sizeof(s_cart_rom_saved) - 1] = '\0';
}

const char* supervisor_cart_rom_boot(void)  { return s_cart_rom; }
const char* supervisor_cart_rom_saved(void) { return s_cart_rom_saved; }

void supervisor_save_and_restart(void) {
    // Flush dirty disk caches to SD before the restart — esp_restart() below
    // never returns, so pending writes would otherwise be lost.
    if (sv.machine) {
        sv_disk_flush_all(&sv.machine->fdc);
    }
    vhd_flush_all();

    // Preserve supervisor state (mounted disks, last_dir) across the restart.
    supervisor_save_state();

    // Close the DriveWire socket so the server can accept our next boot.
    dw_bus_shutdown();

    delay(100);
    esp_restart();
}

void supervisor_set_machine_type(uint8_t machine_type) {
    Preferences prefs;
    prefs.begin("sv", false);
    prefs.putUChar("machine_type", machine_type);
    prefs.end();

    DEBUG_PRINTF("Machine type set to %u — restarting...", machine_type);
    supervisor_save_and_restart();
}
