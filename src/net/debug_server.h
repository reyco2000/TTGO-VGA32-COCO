/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : debug_server.h
 *  Module : WiFi debug server (core-0 WebServer task)
 * ============================================================
 *
 * A single Arduino WebServer running in a core-0 task. It serves both the
 * AP-mode config portal and the STA-mode HTTP/JSON debug API. Emulator state
 * is reached only through debug_rpc (cross-core), never touched directly.
 */

#ifndef NET_DEBUG_SERVER_H
#define NET_DEBUG_SERVER_H

#include <stdbool.h>

// Firmware + API version reported by /api/status.
#define DEBUG_API_VERSION 1

// Create the core-0 server task. Routes are registered immediately; the
// underlying WebServer is started lazily once WiFi has an IP. Call once.
void debug_server_begin(void);

// Enable / disable client servicing (the "Debug Server On/Off" toggle).
void debug_server_set_enabled(bool on);
bool debug_server_enabled(void);

#endif // NET_DEBUG_SERVER_H
