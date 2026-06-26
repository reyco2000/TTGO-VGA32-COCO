/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : sv_keymap.cpp
 *  Module : Key Mapper UI — remap physical keys to CoCo characters
 * ============================================================
*/

/*
 * Screens (reached from Settings -> Key Mapper):
 *   SV_KEYMAP_LIST    — "Test Mappings" / "Clear All Mappings" action rows
 *                       followed by one row per remappable CoCo key showing
 *                       its current binding. CoCo 3-only keys (ALT, CTRL,
 *                       CLEAR, F1, F2) are hidden when a CoCo 2 is active.
 *   SV_KEYMAP_CAPTURE — waits for the next raw keypress and binds it
 *                       (DEL clears the binding, ESC cancels).
 *   SV_KEYMAP_TEST    — shows what each pressed key would type (nothing is
 *                       injected into the CoCo).
 *
 * Bindings live in hal_keyboard's remap table and persist via
 * supervisor_save_keymap() (NVS "sv"/"keymap") on every change.
 */

#include "sv_keymap.h"
#include "sv_render.h"
#include "../hal/hal.h"
#include "fabgl.h"

// HID usage codes (list screen comes through the normal supervisor route)
#define HID_UP    0x52
#define HID_DOWN  0x51
#define HID_ENTER 0x28
#define HID_ESC   0x29
#define HID_F1    0x3A

// Rows visible at once in the list (content area fits 8 of SV_ITEM_H).
#define KM_VISIBLE 7

// List rows 0/1 are actions; characters start at row 2.
#define KM_ROW_TEST   0
#define KM_ROW_CLEAR  1
#define KM_ACTION_ROWS 2

static int16_t km_cursor = 0;
static int16_t km_scroll = 0;
static uint8_t km_capture_idx = 0;
// Key awaiting user confirmation, and the dialog text it's shown in.
static int16_t km_pending_vk = -1;
static char    km_confirm_msg[44];
// Last two test-screen results, newest in [1].
static char km_test_line[2][36];

static int km_total_rows(void) {
    int chars = (g_machine_type == 4) ? KM_COUNT : KM_COCO2_COUNT;
    return KM_ACTION_ROWS + chars;
}

// FabGL 1.0.9 bug guard: virtualKeyToString()'s internal VKTOSTR[] table has
// only 222 entries while the VirtualKey enum has 250 — keys added later
// (VK_TILDE_n = 223 for ñ, accented vowels, ...) index past the table and
// dereferencing the garbage pointer crashes with LoadProhibited.
#define FABGL_VK_NAME_COUNT 222

// FabGL key name without the "VK_" prefix, e.g. "HASH", "F7"; keys beyond
// the library's name table render as "KEY <n>".
static const char* vk_short_name(int16_t vk) {
    static char numbuf[12];
    if (vk <= 0 || vk >= FABGL_VK_NAME_COUNT) {
        snprintf(numbuf, sizeof(numbuf), "KEY %d", vk);
        return numbuf;
    }
    const char* n = fabgl::Keyboard::virtualKeyToString((fabgl::VirtualKey)vk);
    return (n && n[0] == 'V' && n[1] == 'K' && n[2] == '_') ? n + 3 : (n ? n : "?");
}

void sv_keymap_open(Supervisor_t* sv) {
    km_cursor = 0;
    km_scroll = 0;
    sv->prev_state = sv->state;
    sv->state = SV_KEYMAP_LIST;
    sv->needs_redraw = true;
}

static void km_back_to_settings(Supervisor_t* sv) {
    sv->state = SV_SETTINGS;
    sv->menu_cursor = 3;        // the "Key Mapper" row
    sv->menu_item_count = 4;
    sv->needs_redraw = true;
}

void sv_keymap_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;

    switch (hid_usage) {
        case HID_UP:
            if (km_cursor > 0) {
                km_cursor--;
                if (km_cursor < km_scroll) km_scroll = km_cursor;
                sv->needs_redraw = true;
            }
            break;

        case HID_DOWN:
            if (km_cursor < km_total_rows() - 1) {
                km_cursor++;
                if (km_cursor >= km_scroll + KM_VISIBLE)
                    km_scroll = km_cursor - KM_VISIBLE + 1;
                sv->needs_redraw = true;
            }
            break;

        case HID_ENTER:
            if (km_cursor == KM_ROW_TEST) {
                km_test_line[0][0] = '\0';
                km_test_line[1][0] = '\0';
                sv->state = SV_KEYMAP_TEST;
                sv->needs_redraw = true;
            } else if (km_cursor == KM_ROW_CLEAR) {
                sv->prev_state = SV_KEYMAP_LIST;
                sv->state = SV_CONFIRM_DIALOG;
                sv->confirm_message = "Clear all custom\nkey mappings?";
                sv->confirm_yes_selected = false;
                sv->confirm_callback = [](bool accepted, void* ctx) {
                    Supervisor_t* s = (Supervisor_t*)ctx;
                    if (accepted) {
                        hal_keyboard_remap_clear_all();
                        supervisor_save_keymap();
                    }
                    s->state = SV_KEYMAP_LIST;
                    s->needs_redraw = true;
                };
                sv->confirm_context = sv;
                sv->needs_redraw = true;
            } else {
                km_capture_idx = (uint8_t)(km_cursor - KM_ACTION_ROWS);
                sv->state = SV_KEYMAP_CAPTURE;
                sv->needs_redraw = true;
            }
            break;

        case HID_ESC:
            km_back_to_settings(sv);
            break;

        case HID_F1:
            supervisor_toggle();
            break;
    }
}

// ============================================================
// Raw-VK path — capture and test screens
// ============================================================

bool sv_keymap_wants_raw_vk(void) {
    SV_State st = supervisor_get()->state;
    return st == SV_KEYMAP_CAPTURE || st == SV_KEYMAP_TEST;
}

// Keys that may not become custom bindings: SHIFT is needed to form CoCo
// combos, and F3-F6 are host hotkeys consumed before the mapping tables.
static bool km_vk_bindable(int16_t vk) {
    switch ((fabgl::VirtualKey)vk) {
        case fabgl::VK_LSHIFT:
        case fabgl::VK_RSHIFT:
        case fabgl::VK_F3:
        case fabgl::VK_F4:
        case fabgl::VK_F5:
        case fabgl::VK_F6:
            return false;
        default:
            return vk > 0;
    }
}

void sv_keymap_on_raw_vk(int16_t vk, bool pressed) {
    Supervisor_t* sv = supervisor_get();
    if (!pressed) return;

    if (sv->state == SV_KEYMAP_CAPTURE) {
        if (vk == (int16_t)fabgl::VK_ESCAPE) {
            sv->state = SV_KEYMAP_LIST;
            sv->needs_redraw = true;
            return;
        }
        if (vk == (int16_t)fabgl::VK_DELETE || vk == (int16_t)fabgl::VK_KP_DELETE) {
            hal_keyboard_remap_set(km_capture_idx, -1);
            supervisor_save_keymap();
            sv->state = SV_KEYMAP_LIST;
            sv->needs_redraw = true;
            return;
        }
        if (!km_vk_bindable(vk)) return;  // ignore SHIFT / host hotkeys
        // Ask for confirmation before applying. The dialog runs through the
        // normal HID key route (wants_raw_vk is false for SV_CONFIRM_DIALOG).
        km_pending_vk = vk;
        snprintf(km_confirm_msg, sizeof(km_confirm_msg), "Map  %s  to key\n%s ?",
                 hal_keyboard_remap_label(km_capture_idx), vk_short_name(vk));
        sv->prev_state = SV_KEYMAP_CAPTURE;
        sv->state = SV_CONFIRM_DIALOG;
        sv->confirm_message = km_confirm_msg;
        sv->confirm_yes_selected = false;
        sv->confirm_callback = [](bool accepted, void* ctx) {
            Supervisor_t* s = (Supervisor_t*)ctx;
            if (accepted) {
                // Applied live — no machine reset needed.
                hal_keyboard_remap_set(km_capture_idx, km_pending_vk);
                supervisor_save_keymap();
            }
            s->state = SV_KEYMAP_LIST;
            s->needs_redraw = true;
        };
        sv->confirm_context = sv;
        sv->needs_redraw = true;
        return;
    }

    // SV_KEYMAP_TEST
    if (vk == (int16_t)fabgl::VK_ESCAPE) {
        sv->state = SV_KEYMAP_LIST;
        sv->needs_redraw = true;
        return;
    }
    char desc[24];
    hal_keyboard_describe_vk(vk, desc, sizeof(desc));
    memcpy(km_test_line[0], km_test_line[1], sizeof(km_test_line[0]));
    snprintf(km_test_line[1], sizeof(km_test_line[1]), "%s -> %s",
             vk_short_name(vk), desc);
    sv->needs_redraw = true;
}

// ============================================================
// Renderers
// ============================================================

void sv_keymap_render(Supervisor_t* sv) {
    (void)sv;
    sv_render_frame("Key Mapper", "Up/Dn  ENTER  ESC Back");

    int total = km_total_rows();
    for (int i = 0; i < KM_VISIBLE && (km_scroll + i) < total; i++) {
        int row = km_scroll + i;
        bool hl = (row == km_cursor);
        if (row == KM_ROW_TEST) {
            sv_render_menu_item(i, "Test Mappings", NULL, hl);
        } else if (row == KM_ROW_CLEAR) {
            sv_render_menu_item(i, "Clear All Mappings", NULL, hl);
        } else {
            uint8_t idx = (uint8_t)(row - KM_ACTION_ROWS);
            int16_t vk = hal_keyboard_remap_table()[idx];
            char val[20];
            if (vk > 0) snprintf(val, sizeof(val), "%s", vk_short_name(vk));
            else        snprintf(val, sizeof(val), "default");
            sv_render_menu_item(i, hal_keyboard_remap_label(idx), val, hl);
        }
    }
    sv_render_scrollbar(km_scroll, KM_VISIBLE, total);
}

void sv_keymap_capture_render(Supervisor_t* sv) {
    (void)sv;
    sv_render_frame("Remap Key", "DEL Clear  ESC Cancel");

    char msg[40];
    snprintf(msg, sizeof(msg), "CoCo key:  %s",
             hal_keyboard_remap_label(km_capture_idx));
    sv_render_centered_item(1, msg, SV_COLOR_TEXT);

    int16_t cur = hal_keyboard_remap_table()[km_capture_idx];
    if (cur > 0) snprintf(msg, sizeof(msg), "Now bound to: %s", vk_short_name(cur));
    else         snprintf(msg, sizeof(msg), "Now: default");
    sv_render_centered_item(3, msg, SV_COLOR_DIM);

    sv_render_centered_item(5, "Press the new key...", SV_COLOR_TEXT);
}

void sv_keymap_test_render(Supervisor_t* sv) {
    (void)sv;
    sv_render_frame("Key Mapper Test", "Press keys  ESC Back");

    sv_render_centered_item(1, "Press any key to test", SV_COLOR_DIM);
    if (km_test_line[0][0])
        sv_render_centered_item(3, km_test_line[0], SV_COLOR_DIM);
    if (km_test_line[1][0])
        sv_render_centered_item(4, km_test_line[1], SV_COLOR_TEXT);
}
