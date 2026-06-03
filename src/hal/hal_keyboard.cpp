/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : hal_keyboard.cpp
 *  Module : Keyboard HAL — FabGL PS/2 on GPIO33/32
 * ============================================================
*/

#include "hal.h"
#include "../utils/debug.h"
#include "../supervisor/supervisor.h"
#include "../supervisor/sv_disk.h"

#define COCO_SHIFT_ROW  6
#define COCO_SHIFT_COL  7

// Shared matrix state
static uint8_t key_matrix[8] = {
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF
};

static void set_key(uint8_t col, uint8_t row, bool pressed) {
    if (col >= 8 || row >= 8) return;
    if (pressed) key_matrix[col] &= ~(1 << row);
    else         key_matrix[col] |=  (1 << row);
}

static void apply_shift(bool active) {
    set_key(COCO_SHIFT_COL, COCO_SHIFT_ROW, active);
}

// Deferred releases — keep the key in the matrix for MIN_HOLD_FRAMES so the
// emulated CPU's KEYIN routine sees it through one full debounce pass.
#define MIN_HOLD_FRAMES 4
struct DeferredRelease {
    uint8_t col;
    uint8_t row;
    uint8_t frames_left;
};
#define MAX_DEFERRED 8
static DeferredRelease deferred_releases[MAX_DEFERRED];

static void defer_release(uint8_t col, uint8_t row) {
    for (int i = 0; i < MAX_DEFERRED; i++) {
        if (deferred_releases[i].frames_left > 0 &&
            deferred_releases[i].col == col && deferred_releases[i].row == row) {
            deferred_releases[i].frames_left = MIN_HOLD_FRAMES;
            return;
        }
    }
    for (int i = 0; i < MAX_DEFERRED; i++) {
        if (deferred_releases[i].frames_left == 0) {
            deferred_releases[i].col = col;
            deferred_releases[i].row = row;
            deferred_releases[i].frames_left = MIN_HOLD_FRAMES;
            return;
        }
    }
    set_key(col, row, false);
}

static void tick_deferred_releases() {
    for (int i = 0; i < MAX_DEFERRED; i++) {
        if (deferred_releases[i].frames_left > 0) {
            deferred_releases[i].frames_left--;
            if (deferred_releases[i].frames_left == 0) {
                set_key(deferred_releases[i].col, deferred_releases[i].row, false);
            }
        }
    }
}

static Machine* s_machine_ptr = nullptr;
void hal_keyboard_set_machine(Machine* m) { s_machine_ptr = m; }

// ============================================================================
//  FabGL PS/2 keyboard
// ============================================================================

#include "fabgl.h"

static fabgl::PS2Controller s_ps2;

// Supervisor maps still want HID Usage IDs (it calls supervisor_on_key with
// a usage). Provide a VK→HID-usage map for the keys the supervisor handles.
static uint8_t vk_to_hid_usage(fabgl::VirtualKey vk) {
    switch (vk) {
        case fabgl::VK_RETURN:       return 0x28;
        case fabgl::VK_KP_ENTER:     return 0x28;
        case fabgl::VK_ESCAPE:       return 0x29;
        case fabgl::VK_UP:           return 0x52;
        case fabgl::VK_DOWN:         return 0x51;
        case fabgl::VK_LEFT:         return 0x50;
        case fabgl::VK_RIGHT:        return 0x4F;
        case fabgl::VK_BACKSPACE:    return 0x2A;
        case fabgl::VK_SPACE:        return 0x2C;
        case fabgl::VK_F1:           return 0x3A;
        case fabgl::VK_F2:           return 0x3B;
        case fabgl::VK_F3:           return 0x3C;
        case fabgl::VK_F4:           return 0x3D;
        case fabgl::VK_F5:           return 0x3E;
        case fabgl::VK_F6:           return 0x3F;
        case fabgl::VK_F7:           return 0x40;
        case fabgl::VK_INSERT:       return 0x49;
        case fabgl::VK_DELETE:       return 0x4C;
        case fabgl::VK_HOME:         return 0x4A;
        case fabgl::VK_END:          return 0x4D;
        case fabgl::VK_PAGEUP:       return 0x4B;
        case fabgl::VK_PAGEDOWN:     return 0x4E;
        default: break;
    }
    // Letters / digits derive from ASCII
    return 0;
}

// VK → CoCo (col, row), with `needs_shift` for shifted-printable VKs that
// have to assert CoCo SHIFT to produce the correct glyph.
struct VkMap {
    fabgl::VirtualKey vk;
    uint8_t col;
    uint8_t row;
    bool needs_shift;   // assert CoCo SHIFT alongside this key
    bool suppress_shift;// host SHIFT held + this combo → suppress CoCo SHIFT
};

static const VkMap VK_MAP[] = {
    // Letters — both lower and upper VK forms map to same matrix cell
    { fabgl::VK_a, 1, 0, false, false }, { fabgl::VK_A, 1, 0, false, true },
    { fabgl::VK_b, 2, 0, false, false }, { fabgl::VK_B, 2, 0, false, true },
    { fabgl::VK_c, 3, 0, false, false }, { fabgl::VK_C, 3, 0, false, true },
    { fabgl::VK_d, 4, 0, false, false }, { fabgl::VK_D, 4, 0, false, true },
    { fabgl::VK_e, 5, 0, false, false }, { fabgl::VK_E, 5, 0, false, true },
    { fabgl::VK_f, 6, 0, false, false }, { fabgl::VK_F, 6, 0, false, true },
    { fabgl::VK_g, 7, 0, false, false }, { fabgl::VK_G, 7, 0, false, true },
    { fabgl::VK_h, 0, 1, false, false }, { fabgl::VK_H, 0, 1, false, true },
    { fabgl::VK_i, 1, 1, false, false }, { fabgl::VK_I, 1, 1, false, true },
    { fabgl::VK_j, 2, 1, false, false }, { fabgl::VK_J, 2, 1, false, true },
    { fabgl::VK_k, 3, 1, false, false }, { fabgl::VK_K, 3, 1, false, true },
    { fabgl::VK_l, 4, 1, false, false }, { fabgl::VK_L, 4, 1, false, true },
    { fabgl::VK_m, 5, 1, false, false }, { fabgl::VK_M, 5, 1, false, true },
    { fabgl::VK_n, 6, 1, false, false }, { fabgl::VK_N, 6, 1, false, true },
    { fabgl::VK_o, 7, 1, false, false }, { fabgl::VK_O, 7, 1, false, true },
    { fabgl::VK_p, 0, 2, false, false }, { fabgl::VK_P, 0, 2, false, true },
    { fabgl::VK_q, 1, 2, false, false }, { fabgl::VK_Q, 1, 2, false, true },
    { fabgl::VK_r, 2, 2, false, false }, { fabgl::VK_R, 2, 2, false, true },
    { fabgl::VK_s, 3, 2, false, false }, { fabgl::VK_S, 3, 2, false, true },
    { fabgl::VK_t, 4, 2, false, false }, { fabgl::VK_T, 4, 2, false, true },
    { fabgl::VK_u, 5, 2, false, false }, { fabgl::VK_U, 5, 2, false, true },
    { fabgl::VK_v, 6, 2, false, false }, { fabgl::VK_V, 6, 2, false, true },
    { fabgl::VK_w, 7, 2, false, false }, { fabgl::VK_W, 7, 2, false, true },
    { fabgl::VK_x, 0, 3, false, false }, { fabgl::VK_X, 0, 3, false, true },
    { fabgl::VK_y, 1, 3, false, false }, { fabgl::VK_Y, 1, 3, false, true },
    { fabgl::VK_z, 2, 3, false, false }, { fabgl::VK_Z, 2, 3, false, true },
    // Arrows + space
    { fabgl::VK_UP,    3, 3, false, false },
    { fabgl::VK_DOWN,  4, 3, false, false },
    { fabgl::VK_LEFT,  5, 3, false, false },
    { fabgl::VK_RIGHT, 6, 3, false, false },
    { fabgl::VK_SPACE, 7, 3, false, false },
    // Digits row (unshifted)
    { fabgl::VK_0, 0, 4, false, false },
    { fabgl::VK_1, 1, 4, false, false },
    { fabgl::VK_2, 2, 4, false, false },
    { fabgl::VK_3, 3, 4, false, false },
    { fabgl::VK_4, 4, 4, false, false },
    { fabgl::VK_5, 5, 4, false, false },
    { fabgl::VK_6, 6, 4, false, false },
    { fabgl::VK_7, 7, 4, false, false },
    { fabgl::VK_8, 0, 5, false, false },
    { fabgl::VK_9, 1, 5, false, false },
    // Shifted-digit symbols. FabGL's PS/2 driver pre-resolves SHIFT+N into
    // these "symbol" VKs, so we re-emit the underlying digit + CoCo SHIFT
    // via needs_shift=true. CoCo layout matches US: !,",#,$,%,&,',(,)
    { fabgl::VK_EXCLAIM,    1, 4, true, false }, // SHIFT+1 → !
    { fabgl::VK_QUOTEDBL,   2, 4, true, false }, // SHIFT+2 → "
    { fabgl::VK_HASH,       3, 4, true, false }, // SHIFT+3 → #
    { fabgl::VK_DOLLAR,     4, 4, true, false }, // SHIFT+4 → $
    { fabgl::VK_PERCENT,    5, 4, true, false }, // SHIFT+5 → %
    { fabgl::VK_AMPERSAND,  6, 4, true, false }, // SHIFT+6 → &
    { fabgl::VK_QUOTE,      7, 4, true, false }, // SHIFT+7 → '
    { fabgl::VK_LEFTPAREN,  0, 5, true, false }, // SHIFT+8 → (
    { fabgl::VK_RIGHTPAREN, 1, 5, true, false }, // SHIFT+9 → )
    { fabgl::VK_ASTERISK,   7, 5, true, false }, // SHIFT+/ on some layouts → *  (CoCo: SHIFT+: = *? actually SHIFT+/ is unused on CoCo; map to /=slash anyway)
    // Shifted punctuation that PC pre-resolves
    { fabgl::VK_QUESTION,   7, 5, true, false }, // SHIFT+/ → ?
    { fabgl::VK_GREATER,    6, 5, true, false }, // SHIFT+. → >
    { fabgl::VK_LESS,       4, 5, true, false }, // SHIFT+, → <
    { fabgl::VK_PLUS,       5, 5, true, false }, // SHIFT+- → +
    // Punctuation row
    { fabgl::VK_SEMICOLON, 3, 5, false, false },
    { fabgl::VK_COLON,     2, 5, false, true },  // : on shifted ; → suppress CoCo SHIFT
    { fabgl::VK_COMMA,     4, 5, false, false },
    { fabgl::VK_MINUS,     5, 5, false, false },
    { fabgl::VK_PERIOD,    6, 5, false, false },
    { fabgl::VK_SLASH,     7, 5, false, false },
    { fabgl::VK_AT,        0, 0, false, true },  // @ → suppress CoCo SHIFT
    // PA6 row
    { fabgl::VK_RETURN,    0, 6, false, false },
    { fabgl::VK_KP_ENTER,  0, 6, false, false },
    { fabgl::VK_BACKSPACE, 5, 3, false, false },  // BS → LEFT ARROW
    { fabgl::VK_ESCAPE,    2, 6, false, false },  // ESC → BREAK
    { fabgl::VK_PAUSE,     2, 6, false, false },  // PAUSE → BREAK
    { fabgl::VK_BREAK,     2, 6, false, false },  // BREAK → BREAK (some FabGL layouts emit this)
    { fabgl::VK_INSERT,    1, 6, false, false },  // INS → CLEAR
    { fabgl::VK_DELETE,    1, 6, false, false },  // DEL → CLEAR
    { fabgl::VK_LALT,      3, 6, false, false },
    { fabgl::VK_RALT,      3, 6, false, false },
    { fabgl::VK_LCTRL,     4, 6, false, false },
    { fabgl::VK_RCTRL,     4, 6, false, false },
    { fabgl::VK_F1,        5, 6, false, false },  // forwarded to CoCo F1
    { fabgl::VK_F2,        6, 6, false, false },  // forwarded to CoCo F2
    // SHIFT
    { fabgl::VK_LSHIFT, COCO_SHIFT_COL, COCO_SHIFT_ROW, false, false },
    { fabgl::VK_RSHIFT, COCO_SHIFT_COL, COCO_SHIFT_ROW, false, false },
};
static const size_t VK_MAP_COUNT = sizeof(VK_MAP) / sizeof(VK_MAP[0]);

static const VkMap* find_vk_mapping(fabgl::VirtualKey vk) {
    for (size_t i = 0; i < VK_MAP_COUNT; i++) {
        if (VK_MAP[i].vk == vk) return &VK_MAP[i];
    }
    return nullptr;
}

static void process_vk(const fabgl::VirtualKeyItem& it) {
    bool pressed = (it.down != 0);
    fabgl::VirtualKey vk = it.vk;
    bool shift_held = (it.SHIFT != 0);

    // Supervisor / hotkey gate — F3 always toggles supervisor.
    if (vk == fabgl::VK_F3 && pressed) { supervisor_toggle(); return; }
    if (supervisor_is_active()) {
        // Supervisor takes all input. Translate to HID usage IDs so the
        // supervisor's existing key handler (HID-keyed) keeps working.
        uint8_t usage = vk_to_hid_usage(vk);
        if (usage == 0 && it.ASCII != 0) {
            // Approximate: ASCII letter → HID usage 0x04 + (toupper(ASCII)-'A')
            uint8_t a = it.ASCII;
            if (a >= 'a' && a <= 'z') usage = 0x04 + (a - 'a');
            else if (a >= 'A' && a <= 'Z') usage = 0x04 + (a - 'A');
            else if (a >= '1' && a <= '9') usage = 0x1E + (a - '1');
            else if (a == '0') usage = 0x27;
        }
        if (usage != 0) supervisor_on_key(usage, pressed);
        return;
    }
    if (pressed) {
        if (vk == fabgl::VK_F4) {
            if (s_machine_ptr) {
                sv_disk_flush_all(&s_machine_ptr->fdc);
                machine_reset(s_machine_ptr);
            }
            return;
        }
        if (vk == fabgl::VK_F6) {
            if (s_machine_ptr) supervisor_quick_mount_last_disk(s_machine_ptr);
            return;
        }
        if (vk == fabgl::VK_F5) {
            extern void hal_video_toggle_fps_overlay(void);
            hal_video_toggle_fps_overlay();
            return;
        }
    }

    const VkMap* k = find_vk_mapping(vk);
    if (!k) {
#if KBD_DEBUG_KEYS
        if (pressed) DEBUG_PRINTF("[KBD] vk=%d UNMAPPED", (int)vk);
#endif
        return;
    }

    bool is_shift_key = (k->col == COCO_SHIFT_COL && k->row == COCO_SHIFT_ROW);
    if (is_shift_key) {
        if (pressed) apply_shift(true);
        else         defer_release(COCO_SHIFT_COL, COCO_SHIFT_ROW);
    } else {
        if (pressed) {
            bool want_shift = (shift_held && !k->suppress_shift) || k->needs_shift;
            apply_shift(want_shift);
            set_key(k->col, k->row, true);
        } else {
            defer_release(k->col, k->row);
            if (!shift_held) defer_release(COCO_SHIFT_COL, COCO_SHIFT_ROW);
        }
    }
}

void hal_keyboard_init(void) {
    hal_keyboard_release_all();
    memset(deferred_releases, 0, sizeof(deferred_releases));
    // PS2Preset::KeyboardPort0_MousePort1 defaults to keyboard on GPIO33/32
    // and mouse on GPIO26/27 — both matching the TTGO VGA32 v1.4 headers.
    // It allocates Keyboard *and* Mouse objects; hal_joystick.cpp grabs the
    // Mouse via PS2Controller::mouse(). The explicit-pin overload does NOT
    // allocate either device; the preset variant does.
    s_ps2.begin(fabgl::PS2Preset::KeyboardPort0_MousePort1,
                fabgl::KbdMode::CreateVirtualKeysQueue);

    // Select keyboard layout. FabGL defaults to US; switch to Spanish here
    // to match the physical keys on a Spanish (Latin American) keyboard.
    // Available built-in layouts: USLayout, UKLayout, GermanLayout,
    // ItalianLayout, SpanishLayout, FrenchLayout, BelgianLayout,
    // NorwegianLayout, JapaneseLayout.
    fabgl::Keyboard* kbd = fabgl::PS2Controller::keyboard();
    if (kbd) kbd->setLayout(&fabgl::SpanishLayout);

    DEBUG_PRINTF("  Keyboard: FabGL PS/2 on CLK=GPIO%d DATA=GPIO%d (Spanish layout)",
                 PIN_PS2_KBD_CLK, PIN_PS2_KBD_DATA);
}

void hal_keyboard_tick(void) {

    tick_deferred_releases();
    fabgl::Keyboard* kbd = fabgl::PS2Controller::keyboard();
    if (!kbd) return;
    while (kbd->virtualKeyAvailable() > 0) {
        fabgl::VirtualKeyItem it;
        if (!kbd->getNextVirtualKey(&it, 0)) break;
#if KBD_DEBUG_KEYS
        // Slim probe: prints only on key DOWN events so the serial log is
        // useful for diagnosing unmapped keys without flooding during normal
        // typing. Gated behind KBD_DEBUG_KEYS (config.h) — off by default.
        if (it.down) {
            DEBUG_PRINTF("[KBD] vk=%d ascii=%d shift=%d ctrl=%d",
                         (int)it.vk, it.ASCII, it.SHIFT, it.CTRL);
        }
#endif
        process_vk(it);
    }
}

// ============================================================================
//  Shared public API
// ============================================================================

uint8_t hal_keyboard_scan(uint8_t column) {
    return (column < 8) ? key_matrix[column] : 0xFF;
}

void hal_keyboard_press(uint8_t row, uint8_t col) {
    if (row < 8 && col < 8) key_matrix[col] &= ~(1 << row);
}

void hal_keyboard_release(uint8_t row, uint8_t col) {
    if (row < 8 && col < 8) key_matrix[col] |= (1 << row);
}

void hal_keyboard_release_all(void) {
    for (int i = 0; i < 8; i++) key_matrix[i] = 0xFF;
}
