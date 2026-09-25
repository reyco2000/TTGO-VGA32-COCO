/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : dw_server.h
 *  Module : Internal DriveWire back end — disk-only DW3/DW4 server on core 0
 * ============================================================
 *
 * Serves the Disk Manager's drives 0-3 (the same PSRAM-cached images the
 * WD1793 uses, src/supervisor/sv_disk.cpp) as DriveWire drives 0-3, so HDB-DOS
 * and Becker builds of NitrOS-9 boot with no external server and no WiFi.
 *
 * Opcodes: DWINIT, INIT/TERM, TIME, READ/READEX (+RE-), WRITE (+RE-), GETSTAT/
 * SETSTAT, the RESET codes. Virtual-serial, print and named-object ops are
 * consumed and answered "no data" / "not found".
 *
 * Writes go to the PSRAM cache and are flushed to SD in the background after
 * DW_FLUSH_IDLE_MS of write inactivity (plus on eject, reset and restart,
 * like the floppy path).
 */

#ifndef NET_DW_SERVER_H
#define NET_DW_SERVER_H

#include <Arduino.h>

struct SV_DiskController;

void        dw_server_begin(SV_DiskController* drives);
const char* dw_server_state_str(void);

// Counters for /api/bus.
uint32_t    dw_server_reads(void);
uint32_t    dw_server_writes(void);
uint32_t    dw_server_errors(void);        // non-zero status replies
uint32_t    dw_server_flushes(void);       // background flushes to SD
uint8_t     dw_server_last_op(void);
uint32_t    dw_server_stack_free(void);    // task stack high-water mark (bytes)

// Flush pending writes to SD now (before a restart); waits up to timeout_ms.
void        dw_server_shutdown(uint32_t timeout_ms);

#endif // NET_DW_SERVER_H
