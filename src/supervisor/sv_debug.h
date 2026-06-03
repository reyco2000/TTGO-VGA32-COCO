/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : sv_debug.h
 *  Module : Supervisor debug interface — CPU status, GIME state, and memory dump
 * ============================================================
*/

#ifndef SV_DEBUG_H
#define SV_DEBUG_H

#include <stdint.h>

struct Supervisor_t;

// Debug sub-pages
enum SV_DebugPage : uint8_t {
    SV_DBG_PAGE_STATUS = 0,  // CPU regs + GIME state
    SV_DBG_PAGE_DUMP,        // Memory hex dump
    SV_DBG_PAGE_RS232,       // RS-232 Pak ACIA registers + counters
    SV_DBG_PAGE_COUNT
};

// Dump address fields
enum SV_DebugField : uint8_t {
    SV_DBG_FIELD_ADDR = 0,   // Hex address being edited
    SV_DBG_FIELD_DUMP_BTN,   // "Dump to Serial" button
    SV_DBG_FIELD_COUNT
};

struct SV_DebugState {
    SV_DebugPage   page;
    SV_DebugField  active_field;
    uint16_t       dump_addr;       // Current dump address (CPU logical)
    uint8_t        hex_digit;       // Which nibble is being edited (0-3)
    bool           dumping;
};

void sv_debug_init(Supervisor_t* sv);
void sv_debug_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);
void sv_debug_render(Supervisor_t* sv);
void sv_debug_set_page(SV_DebugPage page);

#endif // SV_DEBUG_H
