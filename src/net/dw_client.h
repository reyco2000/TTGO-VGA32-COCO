/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : dw_client.h
 *  Module : External DriveWire back end — TCP client on core 0
 * ============================================================
 *
 * Shuttles bytes between the Becker rings and a DriveWire server reached over
 * WiFi (pyDriveWire, DW4, FujiNet-PC — all listen on TCP 65504, the same
 * endpoint XRoar's Becker port connects to). Reconnects automatically.
 */

#ifndef NET_DW_CLIENT_H
#define NET_DW_CLIENT_H

#include <Arduino.h>

typedef enum {
    DW_CLIENT_IDLE,        // task not started
    DW_CLIENT_WAIT_WIFI,   // waiting for STA
    DW_CLIENT_CONNECTING,  // TCP connect in progress
    DW_CLIENT_CONNECTED,   // link up
    DW_CLIENT_BACKOFF,     // connect failed / dropped, retrying soon
} DwClientState;

void          dw_client_begin(const String& host, uint16_t port);
DwClientState dw_client_state(void);
const char*   dw_client_state_str(void);
uint32_t      dw_client_connects(void);  // successful connections since boot

// Delivery diagnostics: server bytes queued -> all read by the CoCo. Large
// values mean an emulator-side stall rather than a network one.
uint32_t      dw_client_max_reply_ms(void);
uint32_t      dw_client_slow_replies(void);   // deliveries > 400 ms

// Close the TCP connection cleanly (FIN) before a software restart; waits up
// to timeout_ms for the client task to do it.
void          dw_client_shutdown(uint32_t timeout_ms);

#endif // NET_DW_CLIENT_H
