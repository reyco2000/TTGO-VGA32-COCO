/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 * ============================================================
 *  File   : sv_wifi.cpp
 *  Module : Supervisor "WiFi / Debug" status + control screen
 * ============================================================
 */
#include "sv_wifi.h"
#include "supervisor.h"
#include "sv_render.h"
#include "../net/wifi_mgr.h"
#include "../net/debug_server.h"

#define HID_UP    0x52
#define HID_DOWN  0x51
#define HID_ENTER 0x28
#define HID_ESC   0x29
#define HID_F1    0x3A

// Action rows (navigable). Info rows are drawn above and are not selectable.
enum {
    WACT_PORTAL = 0,   // Start config portal (SoftAP)
    WACT_CONNECT,      // Connect using saved credentials (STA)
    WACT_STOP,         // Stop / disconnect
    WACT_FORGET,       // Forget saved credentials
    WACT_SERVER,       // Toggle debug server on/off
    WACT_COUNT
};

// Only this many action rows fit below the 3 info rows; the list scrolls.
#define WACT_VISIBLE 4

static const char* const WIFI_ACTIONS[WACT_COUNT] = {
    "Start Config Portal",
    "Connect (saved)",
    "Stop / Disconnect",
    "Forget Credentials",
    "Debug Server",
};

// Redraw bookkeeping so tick() only repaints on a real change.
static WifiMgrState s_last_state = WIFI_MGR_OFF;
static String       s_last_ip;

void sv_wifi_open(Supervisor_t* sv) {
    sv->state = SV_WIFI;
    sv->menu_cursor = 0;
    s_last_state = wifi_mgr_state();
    s_last_ip    = wifi_mgr_ip();
    sv->needs_redraw = true;
}

static void wifi_execute(Supervisor_t* sv, int action) {
    switch (action) {
        case WACT_PORTAL:  wifi_mgr_start_ap();                                  break;
        case WACT_CONNECT: wifi_mgr_connect_saved();                             break;
        case WACT_STOP:    wifi_mgr_stop();                                      break;
        case WACT_FORGET:  wifi_mgr_forget();                                    break;
        case WACT_SERVER:  debug_server_set_enabled(!debug_server_enabled());    break;
    }
    sv->needs_redraw = true;
}

void sv_wifi_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed) {
    if (!pressed) return;

    switch (hid_usage) {
        case HID_UP:
            if (sv->menu_cursor > 0) { sv->menu_cursor--; sv->needs_redraw = true; }
            break;
        case HID_DOWN:
            if (sv->menu_cursor < WACT_COUNT - 1) { sv->menu_cursor++; sv->needs_redraw = true; }
            break;
        case HID_ENTER:
            wifi_execute(sv, sv->menu_cursor);
            break;
        case HID_ESC:
            sv->state = SV_SETTINGS;
            sv->menu_cursor = 0;
            sv->needs_redraw = true;
            break;
        case HID_F1:
            supervisor_toggle();
            break;
    }
}

void sv_wifi_tick(Supervisor_t* sv) {
    WifiMgrState st = wifi_mgr_state();
    String ip = wifi_mgr_ip();
    if (st != s_last_state || ip != s_last_ip) {
        s_last_state = st;
        s_last_ip = ip;
        sv->needs_redraw = true;
    }
}

void sv_wifi_render(Supervisor_t* sv) {
    sv_render_frame("WiFi / Debug", "Up/Dn  ENTER  ESC");

    // --- Info rows (not selectable) ---
    String ssid = wifi_mgr_ssid();
    if (ssid.length() == 0) ssid = "-";
    sv_render_menu_item(0, "State", wifi_mgr_state_str(), false);
    sv_render_menu_item(1, "SSID",  ssid.c_str(),         false);
    sv_render_menu_item(2, "IP",    wifi_mgr_ip().c_str(), false);

    // --- Action rows: only WACT_VISIBLE fit below the 3 info rows, so scroll. ---
    const int row0 = 4;
    int scroll = (sv->menu_cursor < WACT_VISIBLE)
                     ? 0
                     : sv->menu_cursor - WACT_VISIBLE + 1;

    for (int v = 0; v < WACT_VISIBLE; v++) {
        int i = scroll + v;
        if (i >= WACT_COUNT) break;
        const char* value = nullptr;
        if (i == WACT_PORTAL && wifi_mgr_state() == WIFI_MGR_AP_CONFIG) value = "UP";
        else if (i == WACT_CONNECT && !wifi_mgr_has_creds())           value = "none";
        else if (i == WACT_SERVER)                                     value = debug_server_enabled() ? "ON" : "OFF";
        sv_render_menu_item(row0 + v, WIFI_ACTIONS[i], value, i == sv->menu_cursor);
    }

    sv_render_scrollbar(scroll, WACT_VISIBLE, WACT_COUNT, row0);
}
