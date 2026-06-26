/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : wifi_mgr.h
 *  Module : WiFi state machine (SoftAP config portal + STA station)
 * ============================================================
 *
 * Flow: from the supervisor the user starts a SoftAP config portal; they join
 * that AP from a phone/PC, pick a network and type the password in the served
 * web page; credentials are saved to NVS, the device switches to STA and the
 * debug API listens on the home-LAN IP. No on-screen password entry.
 *
 * Credentials and the auto-connect flag live in NVS namespace "sv":
 *   wifi_ssid, wifi_pass, wifi_auto.
 */

#ifndef NET_WIFI_MGR_H
#define NET_WIFI_MGR_H

#include <Arduino.h>

typedef enum {
    WIFI_MGR_OFF,         // radio idle
    WIFI_MGR_AP_CONFIG,   // SoftAP config portal up
    WIFI_MGR_CONNECTING,  // STA association in progress
    WIFI_MGR_STA_RUNNING, // STA connected, IP assigned
    WIFI_MGR_FAILED,      // STA connect failed / timed out
} WifiMgrState;

// AP config-portal SSID (open network).
#define WIFI_MGR_AP_SSID "CoCo3-Setup"

void          wifi_mgr_init(void);          // load creds, WiFi.setSleep(false)
WifiMgrState  wifi_mgr_state(void);
const char*   wifi_mgr_state_str(void);
void          wifi_mgr_tick(void);          // advance CONNECTING; call periodically

// Config portal (AP mode)
void          wifi_mgr_start_ap(void);
IPAddress     wifi_mgr_ap_ip(void);

// Scanning (used by the portal page)
int           wifi_mgr_scan(void);          // blocking-ish; returns network count
int           wifi_mgr_scan_count(void);
String        wifi_mgr_scan_ssid(int i);
int           wifi_mgr_scan_rssi(int i);
bool          wifi_mgr_scan_secure(int i);

// Station (STA) mode
void          wifi_mgr_connect(const char* ssid, const char* pass);  // saves creds, begins
void          wifi_mgr_connect_saved(void);  // auto-connect with stored creds
void          wifi_mgr_stop(void);           // disconnect + AP off -> OFF

// Status getters
String        wifi_mgr_ip(void);
String        wifi_mgr_ssid(void);

// Credentials / settings (NVS "sv")
bool          wifi_mgr_has_creds(void);
void          wifi_mgr_forget(void);
bool          wifi_mgr_autoconnect(void);
void          wifi_mgr_set_autoconnect(bool on);

#endif // NET_WIFI_MGR_H
