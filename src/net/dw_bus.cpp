/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : dw_bus.cpp
 *  Module : DriveWire bus configuration and back-end selection
 * ============================================================
 */

#include "dw_bus.h"

#include <Preferences.h>

#include "dw_client.h"
#include "wifi_mgr.h"
#include "../core/machine.h"   // g_cart_rom_request
#include "../../config.h"
#include "../utils/debug.h"

static BusMode  s_mode = BUS_MODE_OFF;
static String   s_host;
static uint16_t s_port = DW_DEFAULT_PORT;
static bool     s_rom_to = false;

void dw_bus_load_config(void) {
    Preferences p;
    p.begin("sv", true);
    uint8_t m = p.getUChar("bus_mode", BUS_MODE_OFF);
    s_host    = p.getString("dw_host", "");
    s_port    = p.getUShort("dw_port", DW_DEFAULT_PORT);
    s_rom_to  = p.getBool("dw_rom_to", false);
    p.end();

    s_mode = (m < BUS_MODE_COUNT) ? (BusMode)m : BUS_MODE_OFF;
    if (!dw_bus_mode_supported(s_mode)) {
        DEBUG_PRINTF("dw_bus: mode %u not supported in this build — Off", m);
        s_mode = BUS_MODE_OFF;
    }
    if (s_mode == BUS_MODE_EXTERNAL && s_host.length() == 0) {
        DEBUG_PRINT("dw_bus: External mode without a host — Off");
        s_mode = BUS_MODE_OFF;
    }

    // Allocate the Becker rings now, so a failure turns the bus Off before
    // the HDB-DOS ROM (useless without the port) replaces disk11.rom.
    if (s_mode != BUS_MODE_OFF && !becker_init()) {
        DEBUG_PRINT("dw_bus: Becker ring allocation failed — Off");
        s_mode = BUS_MODE_OFF;
    }

    if (s_mode != BUS_MODE_OFF) {
        g_cart_rom_request[0] = s_rom_to ? ROM_BECKER_TO_COCO2_FILE : ROM_BECKER_COCO2_FILE;
        g_cart_rom_request[1] = s_rom_to ? ROM_BECKER_TO_COCO3_FILE : ROM_BECKER_COCO3_FILE;
    }
}

BusMode dw_bus_mode(void)   { return s_mode; }
String  dw_bus_host(void)   { return s_host; }
uint16_t dw_bus_port(void)  { return s_port; }
bool    dw_bus_rom_timeout(void) { return s_rom_to; }

const char* dw_bus_mode_str(BusMode mode) {
    switch (mode) {
        case BUS_MODE_OFF:         return "Off";
        case BUS_MODE_EXTERNAL:    return "External";
        case BUS_MODE_INTERNAL_DW: return "Internal DW";
        case BUS_MODE_FUJINET:     return "FujiNet";
        default:                   return "?";
    }
}

bool dw_bus_mode_supported(BusMode mode) {
    return mode == BUS_MODE_OFF || mode == BUS_MODE_EXTERNAL;
}

void dw_bus_save_config(BusMode mode, const String& host, uint16_t port,
                        bool rom_timeout) {
    Preferences p;
    p.begin("sv", false);
    p.putUChar("bus_mode", (uint8_t)mode);
    p.putString("dw_host", host);
    p.putUShort("dw_port", port);
    p.putBool("dw_rom_to", rom_timeout);
    p.end();
}

void dw_bus_begin(void) {
    if (s_mode == BUS_MODE_OFF) return;

    if (s_mode == BUS_MODE_EXTERNAL) {
        // The DriveWire link needs STA even when the user has not enabled
        // WiFi auto-connect for the debug server.
        if (wifi_mgr_state() == WIFI_MGR_OFF && wifi_mgr_has_creds()) {
            wifi_mgr_connect_saved();
        }
        dw_client_begin(s_host, s_port);
        DEBUG_PRINTF("dw_bus: External DriveWire -> %s:%u", s_host.c_str(), s_port);
    }
}

const char* dw_bus_link_str(void) {
    switch (s_mode) {
        case BUS_MODE_OFF:      return "Off";
        case BUS_MODE_EXTERNAL: return dw_client_state_str();
        default:                return becker_link_up() ? "Up" : "Down";
    }
}
