/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : debug.h
 *  Module : Debug output macros — compile-time Serial.printf wrappers (DEBUG_LOG, etc.)
 * ============================================================
*/

/*
 * debug.h - Debug output macros
 */

#ifndef DEBUG_H
#define DEBUG_H

#include <stdint.h>
#include "../../config.h"

// ------------------------------------------------------------
// Serial-port ownership mode (single UART0 on the TTGO VGA32).
//
// Declared here because the DEBUG_* macros below gate on it and are expanded
// across the whole codebase. The variable is defined in hal.cpp and applied
// from NVS at boot (see serial_mode_apply / supervisor_load_serial_mode).
//
// Debug output and the RS-232 Pak share one physical wire, so at most one may
// own it at a time — hence a single enum rather than two booleans.
// ------------------------------------------------------------
enum SerialPortMode : uint8_t {
    SERIAL_MODE_OFF   = 0,  // UART0 idle: no debug output, no pak host I/O
    SERIAL_MODE_DEBUG = 1,  // UART0 carries debug logs; pak register decode inactive
    SERIAL_MODE_RS232 = 2,  // UART0 carries RS-232 pak data; debug muted
};
extern volatile SerialPortMode g_serial_mode;

#if DEBUG_ENABLED
    #define DEBUG_PRINT(msg)          do { if (g_serial_mode == SERIAL_MODE_DEBUG) Serial.println(msg); } while (0)
    #define DEBUG_PRINTF(fmt, ...)    do { if (g_serial_mode == SERIAL_MODE_DEBUG) Serial.printf(fmt "\n", ##__VA_ARGS__); } while (0)
    #define DEBUG_TODO(func)          do { if (g_serial_mode == SERIAL_MODE_DEBUG) Serial.printf("TODO: %s() not implemented\n", func); } while (0)
#else
    #define DEBUG_PRINT(msg)
    #define DEBUG_PRINTF(fmt, ...)
    #define DEBUG_TODO(func)
#endif

#endif // DEBUG_H
