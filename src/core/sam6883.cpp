/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : sam6883.cpp
 *  Module : SAM6883 address multiplexer emulation — VDG address counter and memory mapping
 * ============================================================
*/

/*
 * sam6883.cpp - SAM emulation with VDG address counter
 *
 * Derived from XRoar's sam.c by Ciaran Anscomb.
 *
 * The SAM maintains a running VDG address counter that:
 * - Resets to vdg_base on field sync (vsync rising edge)
 * - Advances on each horizontal sync using divide-by-X/Y counters
 * - Provides the correct row address for each VDG scanline
 *
 * The divide tables control row repetition for different modes:
 * - CG1 (GM=0): each row repeated 12× vertically (64 rows → 192 scanlines)
 * - RG1 (GM=1): each row repeated 3× (64 rows → 192 scanlines)
 * - CG6 (GM=6): no repetition (192 rows → 192 scanlines)
 * - etc.
 */

#include "sam6883.h"
#include "../utils/debug.h"

// VDG address counter divide tables (from XRoar sam.c)
// Index by GM value (V0-V2 = SAM register bits 0-2)
static const int vdg_mod_xdivs[8]  = { 1, 3, 1, 2, 1, 1, 1, 1 };
static const int vdg_mod_ydivs[8]  = { 12, 1, 3, 1, 2, 1, 1, 1 };
static const int vdg_mod_adds[8]   = { 16, 8, 16, 8, 16, 8, 16, 0 };
static const uint16_t vdg_mod_clears[8] = {
    (uint16_t)~30, (uint16_t)~14, (uint16_t)~30, (uint16_t)~14,
    (uint16_t)~30, (uint16_t)~14, (uint16_t)~30, (uint16_t)~0
};

// Update derived fields from register value
static void update_from_register(SAM6883* sam) {
    // VDG mode bits V0-V2
    sam->vdg_mode = sam->reg & 0x07;

    // VDG display base address: F0-F6 (bits 3-9) << 6 = address bits A6-A15
    // This gives 64-byte granularity for the display start address
    sam->vdg_base = (sam->reg & 0x03F8) << 6;

    // Update divide counters for current mode
    sam->vdg_mod_xdiv  = vdg_mod_xdivs[sam->vdg_mode];
    sam->vdg_mod_ydiv  = vdg_mod_ydivs[sam->vdg_mode];
    sam->vdg_mod_add   = vdg_mod_adds[sam->vdg_mode];
    sam->vdg_mod_clear = vdg_mod_clears[sam->vdg_mode];

    // Other register fields
    sam->page1    = (sam->reg >> SAM_REG_P1) & 1;
    sam->mem_size = (sam->reg >> SAM_REG_M0) & 0x03;
    sam->ty       = (sam->reg >> SAM_REG_TY) & 1;
}

void sam6883_init(SAM6883* sam) {
    DEBUG_PRINT("  SAM: SAM6883 init");
    memset(sam, 0, sizeof(SAM6883));
}

void sam6883_reset(SAM6883* sam) {
    DEBUG_PRINT("  SAM: SAM6883 reset");
    sam->reg = 0;
    update_from_register(sam);
    // Reset counter to default text screen at $0400
    // (F0-F6 = 0 gives base $0000, but BASIC sets F1=1 → $0400)
    sam->vdg_address = sam->vdg_base;
    sam->vdg_xcount = 0;
    sam->vdg_ycount = 0;
}

void sam6883_write(SAM6883* sam, uint8_t addr) {
    if (addr > 31) return;

    // Each register bit has two addresses:
    //   even address = clear bit
    //   odd address  = set bit
    int bit_num = addr >> 1;
    bool set = (addr & 1) != 0;

    if (set) {
        sam->reg |= (1 << bit_num);
    } else {
        sam->reg &= ~(1 << bit_num);
    }

    update_from_register(sam);
}

uint16_t sam6883_get_vdg_address(SAM6883* sam) {
    return sam->vdg_base;
}

uint16_t sam6883_get_vdg_row_address(SAM6883* sam) {
    return sam->vdg_address;
}

// Field sync: reset VDG address counter to base on rising edge
void sam6883_vdg_fsync(SAM6883* sam, bool level) {
    if (!level) return;  // Only act on rising edge
    sam->vdg_address = sam->vdg_base;
    sam->vdg_xcount = 0;
    sam->vdg_ycount = 0;
}

// Internal: advance VDG address counter with divide-by-X/Y logic
// Matches XRoar's vdg_address_add() exactly.
static void vdg_address_add(SAM6883* sam, int n) {
    uint16_t new_addr = sam->vdg_address + n;
    if ((sam->vdg_address ^ new_addr) & 0x10) {
        sam->vdg_xcount = (sam->vdg_xcount + 1) % sam->vdg_mod_xdiv;
        if (sam->vdg_xcount != 0) {
            new_addr -= 0x10;
        } else {
            if ((sam->vdg_address ^ new_addr) & 0x20) {
                sam->vdg_ycount = (sam->vdg_ycount + 1) % sam->vdg_mod_ydiv;
                if (sam->vdg_ycount != 0) {
                    new_addr -= 0x20;
                }
            }
        }
    }
    sam->vdg_address = new_addr;
}

// Simulate VDG data fetch: advance counter by nbytes.
// In XRoar, sam_vdg_bytes() is called by the VDG as it reads data.
// We call this once per active scanline with the total bytes_per_row.
// Processes in 16-byte chunks (matching sam_vdg_bytes chunking).
void sam6883_vdg_fetch_bytes(SAM6883* sam, int nbytes) {
    while (nbytes > 0) {
        int b3_0 = sam->vdg_address & 0x0F;
        int chunk = 16 - b3_0;
        if (chunk > nbytes) chunk = nbytes;
        if (chunk < (16 - b3_0)) {
            // Doesn't cross a 16-byte boundary: simple advance
            sam->vdg_address += chunk;
        } else {
            // Crosses 16-byte boundary: use divide logic
            vdg_address_add(sam, chunk);
        }
        nbytes -= chunk;
    }
}

// Horizontal sync: advance VDG address counter on falling edge
// This is SUPPLEMENTARY to the data fetch advancement.
// In XRoar: sam_vdg_hsync() adds vdg_mod_add then clears bits.
void sam6883_vdg_hsync(SAM6883* sam, bool level) {
    if (level) return;  // Only act on falling edge

    vdg_address_add(sam, sam->vdg_mod_add);
    sam->vdg_address &= sam->vdg_mod_clear;
}
