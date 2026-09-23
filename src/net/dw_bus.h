/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : dw_bus.h
 *  Module : DriveWire bus configuration and back-end selection
 * ============================================================
 *
 * Settings live in NVS namespace "sv":
 *   bus_mode  (uchar)  BusMode — see src/core/becker.h
 *   dw_host   (string) External server host name or IP
 *   dw_port   (ushort) External server TCP port (default 65504)
 *   dw_rom_to (bool)   use the HDB-DOS timeout ROM build (hdbdw3bc*t.rom)
 *
 * The mode is read once at boot; changing it saves and restarts.
 */

#ifndef NET_DW_BUS_H
#define NET_DW_BUS_H

#include <Arduino.h>
#include "../core/becker.h"

#define DW_DEFAULT_PORT 65504

// Read NVS and, when the bus is on, request the HDB-DOS Becker ROMs in place
// of disk11.rom. Call before machine_load_roms().
void        dw_bus_load_config(void);
BusMode     dw_bus_mode(void);         // mode in effect for this boot
const char* dw_bus_mode_str(BusMode mode);
String      dw_bus_host(void);
uint16_t    dw_bus_port(void);
bool        dw_bus_rom_timeout(void);  // saved ROM variant (timeout build)

// True when this build can run the given mode (later phases add more).
bool        dw_bus_mode_supported(BusMode mode);

// Persist new settings (takes effect after a restart).
void        dw_bus_save_config(BusMode mode, const String& host, uint16_t port,
                               bool rom_timeout);

// Enable the Becker port and start the back end for the configured mode.
// Call after WiFi init; the back end waits for STA itself.
void        dw_bus_begin(void);

// Short human-readable link state for the OSD / debug API.
const char* dw_bus_link_str(void);

// Close the back end's connection before a software restart.
void        dw_bus_shutdown(void);

#endif // NET_DW_BUS_H
