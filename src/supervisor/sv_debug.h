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

// "Dump RAM to SD" screen sub-phases (SV_DEBUG_DUMP_NAME state)
enum SV_DumpPhase : uint8_t {
    SV_DUMP_INPUT = 0,       // Typing the filename
    SV_DUMP_SAVING,          // Blocking write in progress
    SV_DUMP_RESULT,          // Showing saved/error message
};

#define SV_DUMP_NAME_MAX  8  // FAT-friendly 8-char base name

struct SV_DebugState {
    SV_DebugPage   page;
    SV_DebugField  active_field;
    uint16_t       dump_addr;       // Current dump address (CPU logical)
    uint8_t        hex_digit;       // Which nibble is being edited (0-3)
    bool           dumping;

    // "Dump RAM to SD" screen state
    SV_DumpPhase   dump_phase;
    char           dump_name[SV_DUMP_NAME_MAX + 1];
    uint8_t        dump_name_len;
    char           dump_result[4][44];  // up to 4 result/error lines
    uint8_t        dump_result_lines;
};

void sv_debug_init(Supervisor_t* sv);
void sv_debug_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);
void sv_debug_render(Supervisor_t* sv);
void sv_debug_set_page(SV_DebugPage page);

// "Dump RAM to SD" screen (SV_DEBUG_DUMP_NAME state)
void sv_debug_begin_dump(Supervisor_t* sv);          // enter filename-entry screen
void sv_debug_dump_on_key(Supervisor_t* sv, uint8_t hid_usage, bool pressed);
void sv_debug_dump_render(Supervisor_t* sv);

#endif // SV_DEBUG_H
