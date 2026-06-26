/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : mc6847.h
 *  Module : MC6847 VDG interface — text and graphics mode rendering
 * ============================================================
*/

#ifndef MC6847_H
#define MC6847_H

#include <Arduino.h>

// VDG mode bits (directly from PIA1 port B and data bus)
#define VDG_AG      0x80    // Alphanumeric/Graphics select
#define VDG_AS      0x40    // Alpha semigraphics select
#define VDG_INTEXT  0x20    // Internal/External character gen
#define VDG_INV     0x10    // Invert (from bit 6 of data byte)
#define VDG_CSS     0x08    // Color set select
#define VDG_GM2     0x04    // Graphics mode bit 2
#define VDG_GM1     0x02    // Graphics mode bit 1
#define VDG_GM0     0x01    // Graphics mode bit 0

// Display geometry
#define VDG_ACTIVE_WIDTH     256
#define VDG_ACTIVE_HEIGHT    192
#define VDG_TEXT_COLS        32
#define VDG_TEXT_ROWS        16
#define VDG_BYTES_PER_ROW    32

// VDG color palette indices
#define VDG_COLOR_GREEN      0
#define VDG_COLOR_YELLOW     1
#define VDG_COLOR_BLUE       2
#define VDG_COLOR_RED        3
#define VDG_COLOR_WHITE      4
#define VDG_COLOR_CYAN       5
#define VDG_COLOR_MAGENTA    6
#define VDG_COLOR_ORANGE     7
#define VDG_COLOR_BLACK      8
#define VDG_COLOR_DARK_GREEN 9
#define VDG_COLOR_DARK_ORANGE 10
#define VDG_COLOR_BRIGHT_ORANGE 11

typedef struct MC6847 {
    uint8_t mode;           // Current mode flags (GM, AG, CSS, etc.)
    int     scanline;       // Current scanline being rendered
    bool    fs;             // Field sync (vsync) active
    bool    hs;             // Horizontal sync active

    // Pointer to video RAM (set by machine based on SAM VDG address)
    const uint8_t* vram;
    uint16_t vram_offset;   // SAM base offset (from F0-F6)
    uint16_t row_address;   // SAM running row address (advances per scanline)

    // Scanline buffer for current line output
    uint8_t line_buffer[VDG_ACTIVE_WIDTH];
} MC6847;

// Initialize VDG
void mc6847_init(MC6847* vdg);

// Reset VDG
void mc6847_reset(MC6847* vdg);

// Set mode bits (called when PIA1 port B changes)
void mc6847_set_mode(MC6847* vdg, uint8_t mode);

// Render one scanline into vdg->line_buffer
// Returns true during active display area
bool mc6847_render_scanline(MC6847* vdg);

// Advance to next scanline, returns true on vsync
bool mc6847_next_scanline(MC6847* vdg);

#endif // MC6847_H
