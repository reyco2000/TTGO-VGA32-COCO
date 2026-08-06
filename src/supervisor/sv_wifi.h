/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 * ============================================================
 *  File   : sv_wifi.h
 *  Module : Supervisor "WiFi / Debug" status + control screen
 * ============================================================
 *
 * Status/control for the WiFi debug server. Shows STA/AP state, SSID, IP, and
 * the debug-server on/off state. Actions: Start Config Portal, Connect (saved),
 * Stop/Disconnect, Forget Credentials, Debug Server On/Off. No on-screen
 * password entry — the password is typed in the browser config portal.
 */
#ifndef SV_WIFI_H
#define SV_WIFI_H

#include <stdint.h>

typedef struct Supervisor_t Supervisor_t;

// Open the screen (sets state, resets cursor).
void sv_wifi_open(Supervisor_t* sv);

// HID key handler (Up/Down move, ENTER execute, ESC back to Settings).
void sv_wifi_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);

// Per-frame: refresh when the WiFi state or IP changes (so CONNECTING ->
// CONNECTED is reflected without a keypress).
void sv_wifi_tick(Supervisor_t* sv);

// Render the screen.
void sv_wifi_render(Supervisor_t* sv);

#endif // SV_WIFI_H
