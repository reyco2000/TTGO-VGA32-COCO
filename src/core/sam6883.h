/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : sam6883.h
 *  Module : SAM6883 interface — memory mapping, CPU clock, and VDG display address
 * ============================================================
*/

/*
 * sam6883.h - Synchronous Address Multiplexer (SAM)
 *
 * Controls memory mapping, CPU clock speed, and VDG display address.
 * Mapped at $FFC0-$FFDF (32 bit-pair registers, set/clear).
 *
 * The SAM maintains a running VDG address counter that advances
 * per-scanline (hsync) and resets per-frame (fsync). Different
 * graphics modes use divide-by-X/Y counters for row repetition.
 */

#ifndef SAM6883_H
#define SAM6883_H

#include <Arduino.h>

// SAM register bit addresses ($FFC0-$FFDF)
// Each bit has a clear address (even) and set address (odd)
#define SAM_REG_V0    0    // VDG mode bit 0
#define SAM_REG_V1    1    // VDG mode bit 1
#define SAM_REG_V2    2    // VDG mode bit 2
#define SAM_REG_F0    3    // Display offset bit 0
#define SAM_REG_F1    4    // Display offset bit 1
#define SAM_REG_F2    5    // Display offset bit 2
#define SAM_REG_F3    6    // Display offset bit 3
#define SAM_REG_F4    7    // Display offset bit 4
#define SAM_REG_F5    8    // Display offset bit 5
#define SAM_REG_F6    9    // Display offset bit 6
#define SAM_REG_P1    10   // Page select
#define SAM_REG_R0    11   // CPU rate bit 0
#define SAM_REG_R1    12   // CPU rate bit 1
#define SAM_REG_M0    13   // Memory size bit 0
#define SAM_REG_M1    14   // Memory size bit 1
#define SAM_REG_TY    15   // Map type

typedef struct SAM6883 {
    uint16_t reg;           // 16-bit register value (all SAM bits packed)
    uint16_t vdg_base;      // VDG display base address (from F0-F6)
    uint8_t  vdg_mode;      // VDG mode bits (V0-V2)
    uint8_t  mem_size;      // Memory size setting (M0-M1)
    bool     ty;            // Map type (ROM/RAM mapping)
    bool     page1;         // Page select (all-RAM mode)

    // Running VDG address counter (advances per-scanline)
    uint16_t vdg_address;   // Current VDG address counter
    int      vdg_xcount;    // Divide-by-X counter
    int      vdg_ycount;    // Divide-by-Y counter
    int      vdg_mod_xdiv;  // X divider for current mode
    int      vdg_mod_ydiv;  // Y divider for current mode
    int      vdg_mod_add;   // Bytes to add per hsync
    uint16_t vdg_mod_clear; // Bit mask for clearing after add
} SAM6883;

// Initialize SAM
void sam6883_init(SAM6883* sam);

// Reset SAM to power-on state
void sam6883_reset(SAM6883* sam);

// Write to SAM register space ($FFC0-$FFDF)
// addr: offset from $FFC0 (0-31)
void sam6883_write(SAM6883* sam, uint8_t addr);

// Get current VDG display base address (from F0-F6 register bits)
uint16_t sam6883_get_vdg_address(SAM6883* sam);

// Get current running VDG row address (advances per scanline)
uint16_t sam6883_get_vdg_row_address(SAM6883* sam);

// VDG sync signals — drive the address counter
void sam6883_vdg_fsync(SAM6883* sam, bool level);  // Field sync (vsync)
void sam6883_vdg_hsync(SAM6883* sam, bool level);  // Horizontal sync

// Simulate VDG data fetch (advances counter by nbytes, with divide logic)
// Must be called for each active scanline BEFORE hsync, to match XRoar's
// sam_vdg_bytes() which is driven by the VDG data clock.
void sam6883_vdg_fetch_bytes(SAM6883* sam, int nbytes);

#endif // SAM6883_H
