/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : sv_fujinet.cpp
 *  Module : Supervisor DriveWire / FujiNet settings screen
 * ============================================================
 */

#include "sv_fujinet.h"
#include "supervisor.h"
#include "sv_render.h"

#include "../core/machine.h"
#include "../net/dw_bus.h"

#define HID_UP        0x52
#define HID_DOWN      0x51
#define HID_ENTER     0x28
#define HID_ESC       0x29
#define HID_F1        0x3A
#define HID_BACKSPACE 0x2A
#define HID_MINUS     0x2D
#define HID_PERIOD    0x37

// Editable rows (navigable). Info rows are drawn above and are not selectable.
enum {
    FACT_MODE = 0,   // cycle supported bus modes
    FACT_HOST,       // External server host (text entry)
    FACT_PORT,       // External server port (digit entry)
    FACT_ROM,        // HDB-DOS ROM variant: Standard / Timeout
    FACT_SAVE,       // Save & Restart
    FACT_COUNT
};

#define FACT_VISIBLE 4
#define HOST_MAX     40
#define PORT_MAX     5

static const char* const FUJI_ACTIONS[FACT_COUNT] = {
    "Mode", "Host", "Port", "HDB-DOS ROM", "Save & Restart",
};

// Pending (unsaved) settings.
static BusMode  p_mode;
static char     p_host[HOST_MAX + 1];
static char     p_port[PORT_MAX + 1];
static bool     p_rom_to;
static int      s_editing = -1;         // FACT_HOST / FACT_PORT while typing
static char     s_edit_backup[HOST_MAX + 1];

// Redraw bookkeeping so tick() only repaints on a real change.
static const char* s_last_link = nullptr;

void sv_fujinet_open(Supervisor_t* sv) {
    sv->state = SV_FUJINET;
    sv->menu_cursor = 0;
    s_editing = -1;

    p_mode = dw_bus_mode();
    snprintf(p_host, sizeof(p_host), "%s", dw_bus_host().c_str());
    snprintf(p_port, sizeof(p_port), "%u", (unsigned)dw_bus_port());
    p_rom_to = dw_bus_rom_timeout();

    s_last_link = dw_bus_link_str();
    sv->needs_redraw = true;
}

static bool pending_differs(void) {
    return p_mode != dw_bus_mode()
        || String(p_host) != dw_bus_host()
        || (uint16_t)atoi(p_port) != dw_bus_port()
        || p_rom_to != dw_bus_rom_timeout();
}

static void cycle_mode(void) {
    int m = (int)p_mode;
    do {
        m = (m + 1) % BUS_MODE_COUNT;
    } while (!dw_bus_mode_supported((BusMode)m));
    p_mode = (BusMode)m;
}

static void begin_edit(int row) {
    s_editing = row;
    snprintf(s_edit_backup, sizeof(s_edit_backup), "%s", row == FACT_HOST ? p_host : p_port);
}

static void cancel_edit(void) {
    if (s_editing == FACT_HOST) snprintf(p_host, sizeof(p_host), "%s", s_edit_backup);
    else if (s_editing == FACT_PORT) snprintf(p_port, sizeof(p_port), "%s", s_edit_backup);
    s_editing = -1;
}

static void end_edit(void) {
    if (s_editing == FACT_PORT) {
        long v = atol(p_port);
        if (v <= 0 || v > 65535) snprintf(p_port, sizeof(p_port), "%u", DW_DEFAULT_PORT);
    }
    s_editing = -1;
}

// The supervisor HID route delivers letters as uppercase usages; host names
// are case-insensitive, so they are stored lowercase.
static char usage_to_char(uint8_t u, bool digits_only) {
    if (u >= 0x1E && u <= 0x26) return '1' + (u - 0x1E);
    if (u == 0x27) return '0';
    if (digits_only) return 0;
    if (u >= 0x04 && u <= 0x1D) return 'a' + (u - 0x04);
    if (u == HID_PERIOD) return '.';
    if (u == HID_MINUS)  return '-';
    return 0;
}

static void edit_key(Supervisor_t* sv, uint8_t u) {
    char*  buf = (s_editing == FACT_HOST) ? p_host : p_port;
    size_t cap = (s_editing == FACT_HOST) ? HOST_MAX : PORT_MAX;
    size_t len = strlen(buf);

    if (u == HID_ENTER)      { end_edit(); }
    else if (u == HID_ESC)   { cancel_edit(); }
    else if (u == HID_BACKSPACE) { if (len > 0) buf[len - 1] = 0; }
    else {
        char c = usage_to_char(u, s_editing == FACT_PORT);
        if (c && len < cap) { buf[len] = c; buf[len + 1] = 0; }
    }
    sv->needs_redraw = true;
}

static void confirm_save(Supervisor_t* sv) {
    if (p_mode == BUS_MODE_EXTERNAL && p_host[0] == 0) {
        sv->menu_cursor = FACT_HOST;   // External needs a host — go type one
        sv->needs_redraw = true;
        return;
    }
    sv->prev_state = SV_FUJINET;
    sv->state = SV_CONFIRM_DIALOG;
    sv->confirm_message = "Save DriveWire\nsettings and restart?";
    sv->confirm_yes_selected = false;
    sv->confirm_callback = [](bool accepted, void* ctx) {
        Supervisor_t* s = (Supervisor_t*)ctx;
        if (accepted) {
            dw_bus_save_config(p_mode, String(p_host), (uint16_t)atoi(p_port), p_rom_to);
            supervisor_save_and_restart();   // never returns
        }
        s->state = SV_FUJINET;
        s->needs_redraw = true;
    };
    sv->confirm_context = sv;
    sv->needs_redraw = true;
}

static void fujinet_execute(Supervisor_t* sv, int action) {
    switch (action) {
        case FACT_MODE: cycle_mode();          break;
        case FACT_HOST:
        case FACT_PORT: begin_edit(action);    break;
        case FACT_ROM:  p_rom_to = !p_rom_to;  break;
        case FACT_SAVE: confirm_save(sv);      return;
    }
    sv->needs_redraw = true;
}

void sv_fujinet_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;

    if (s_editing >= 0) {
        if (hid_usage == HID_F1) { cancel_edit(); supervisor_toggle(); return; }
        edit_key(sv, hid_usage);
        return;
    }

    switch (hid_usage) {
        case HID_UP:
            if (sv->menu_cursor > 0) { sv->menu_cursor--; sv->needs_redraw = true; }
            break;
        case HID_DOWN:
            if (sv->menu_cursor < FACT_COUNT - 1) { sv->menu_cursor++; sv->needs_redraw = true; }
            break;
        case HID_ENTER:
            fujinet_execute(sv, sv->menu_cursor);
            break;
        case HID_ESC:   // discard pending edits
            sv->state = SV_SETTINGS;
            sv->menu_cursor = 0;
            sv->needs_redraw = true;
            break;
        case HID_F1:
            supervisor_toggle();
            break;
    }
}

void sv_fujinet_tick(Supervisor_t* sv) {
    const char* link = dw_bus_link_str();
    if (link != s_last_link) {
        s_last_link = link;
        sv->needs_redraw = true;
    }
}

void sv_fujinet_render(Supervisor_t* sv) {
    sv_render_frame("DriveWire / FujiNet",
                    s_editing >= 0 ? "Type  ENTER Done  ESC Undo" : "Up/Dn  ENTER  ESC");

    // --- Info rows: the running configuration (not the pending edits) ---
    char traffic[32];
    snprintf(traffic, sizeof(traffic), "%lu in / %lu out",
             (unsigned long)becker_bytes_to_coco(), (unsigned long)becker_bytes_from_coco());
    int idx = (g_machine_type == 4) ? 1 : 0;
    const char* rom = g_cart_rom_fallback[idx] ? "HDB-DOS MISSING"
                    : (g_cart_rom_loaded[idx] ? g_cart_rom_loaded[idx] : "none");
    char bus[24];
    snprintf(bus, sizeof(bus), "Bus %s", dw_bus_mode_str(dw_bus_mode()));
    sv_render_menu_item(0, bus, dw_bus_mode() == BUS_MODE_OFF ? "-" : dw_bus_link_str(), false);
    sv_render_menu_item(1, "Traffic", traffic, false);
    sv_render_menu_item(2, "ROM", rom, false);

    // --- Editable rows: FACT_VISIBLE fit below the 3 info rows, so scroll. ---
    const int row0 = 4;
    int scroll = (sv->menu_cursor < FACT_VISIBLE) ? 0 : sv->menu_cursor - FACT_VISIBLE + 1;

    char edit_buf[HOST_MAX + 2];
    for (int v = 0; v < FACT_VISIBLE; v++) {
        int i = scroll + v;
        if (i >= FACT_COUNT) break;
        const char* value = nullptr;
        switch (i) {
            case FACT_MODE: value = dw_bus_mode_str(p_mode); break;
            case FACT_HOST:
            case FACT_PORT: {
                const char* src = (i == FACT_HOST) ? p_host : p_port;
                if (s_editing == i) {
                    snprintf(edit_buf, sizeof(edit_buf), "%s_", src);
                    value = edit_buf;
                } else {
                    value = src[0] ? src : "-";
                }
                break;
            }
            case FACT_ROM:  value = p_rom_to ? "Timeout" : "Standard"; break;
            case FACT_SAVE: value = pending_differs() ? "*" : nullptr; break;
        }
        sv_render_menu_item(row0 + v, FUJI_ACTIONS[i], value, i == sv->menu_cursor);
    }

    sv_render_scrollbar(scroll, FACT_VISIBLE, FACT_COUNT, row0);
}
