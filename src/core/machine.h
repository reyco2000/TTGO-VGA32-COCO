/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : machine.h
 *  Module : CoCo machine interface — CPU, memory map, I/O dispatch, and IRQ routing
 * ============================================================
*/

/*
 * machine.h - CoCo/Dragon system integration
 *
 * Wires all emulation modules together into a complete machine.
 * Handles memory map, I/O dispatch, frame timing, and IRQ routing.
 *
 * CoCo 2 memory map (64K):
 *   $0000-$7FFF  RAM (32K visible, full 64K with SAM page select)
 *   $8000-$9FFF  Extended BASIC ROM (8K)
 *   $A000-$BFFF  Color BASIC ROM (8K)
 *   $C000-$FEFF  Cartridge ROM / Disk BASIC (16K max)
 *   $FF00-$FF1F  PIA0 (keyboard, joystick, hsync/vsync)
 *   $FF20-$FF3F  PIA1 (cassette, sound, VDG mode, cart IRQ)
 *   $FF40-$FF5F  Disk controller (if present)
 *   $FFC0-$FFDF  SAM control bits (write-only)
 *   $FFF0-$FFFF  Interrupt vectors (from ROM)
 *
 * CoCo 3 memory map (512K RAM + GIME):
 *   $0000-$FEFF  MMU-mapped via GIME (8 x 8KB pages from 512KB physical)
 *   $FF00-$FF3F  PIA0/PIA1 (same as CoCo 2)
 *   $FF40-$FF5F  Disk controller / SCS
 *   $FF90-$FF9F  GIME registers
 *   $FFA0-$FFAF  MMU bank registers (2 tasks x 8 banks)
 *   $FFB0-$FFBF  Palette registers (16 x 6-bit)
 *   $FFC0-$FFDF  SAM compat (write-only)
 *   $FFE0-$FFFF  Vectors (from ROM)
 */

#ifndef MACHINE_H
#define MACHINE_H

#include <Arduino.h>
#include "mc6809.h"
#include "mc6821.h"
#include "../../config.h"

// Step 2 of coco2and3.md: always compile in both machine paths so the active
// one can be selected at boot via g_machine_type. The unused path's chip
// state (TCC1014 or MC6847+SAM6883) lives in the struct but is not ticked.
#include "tcc1014.h"
#include "mc6847.h"
#include "sam6883.h"

#include "../supervisor/sv_disk.h"

// Runtime-active machine type (see config.h MACHINE_TYPE values).
// Initialized from NVS at boot, default = compile-time MACHINE_TYPE.
// Step 1: declared but not yet branched on — all paths still use #if MACHINE_TYPE.
extern uint8_t g_machine_type;

typedef struct Machine {
    // --- Core chips ---
    MC6809   cpu;
    MC6821   pia0;      // $FF00-$FF03: keyboard, joystick, hsync/vsync
    MC6821   pia1;      // $FF20-$FF23: cassette, sound, VDG mode, cart IRQ

    // CoCo 3 (GIME) state — used when g_machine_type == 4
    TCC1014  gime;                  // GIME (TCC1014) — replaces SAM + VDG
    uint8_t* ram_physical;          // 512KB in PSRAM
    uint8_t* rom_coco3;             // 32KB Super Extended Color BASIC
    uint8_t* rom_disk;              // 8KB Disk BASIC (external cartridge ROM)
    bool     rom_coco3_loaded;
    bool     rom_disk_loaded;

    // CoCo 2 (SAM + VDG) state — used when g_machine_type == 3
    MC6847   vdg;
    SAM6883  sam;
    uint8_t* ram;                   // 64 KB main RAM
    uint8_t* rom_basic;             // 8 KB Color BASIC ($A000-$BFFF)
    uint8_t* rom_extbas;            // 8 KB Extended BASIC ($8000-$9FFF)
    uint8_t* rom_cart;              // 16 KB Cartridge / Disk BASIC ($C000-$FEFF)
    bool     rom_basic_loaded;
    bool     rom_extbas_loaded;
    bool     rom_cart_loaded;

    SV_DiskController fdc;  // $FF40-$FF5F: WD1793 floppy disk controller

    // --- Memory ---
    size_t   ram_size;

    // --- Timing ---
    uint32_t cycles_per_frame;    // 14916 for NTSC
    uint32_t cycles_this_frame;
    uint32_t scanline;            // Current scanline (0-261)

    // --- Flags ---
    bool     initialized;
    bool     cart_inserted;
    bool     ntsc;                // true = 60 Hz NTSC, false = 50 Hz PAL
    uint32_t frame_count;
} Machine;

// Initialize machine (allocate memory, wire components)
void machine_init(Machine* m);

// Load ROM images from storage
// rom_path: base directory, e.g. "/roms"
bool machine_load_roms(Machine* m, const char* rom_path = ROM_BASE_PATH);

// Reset machine (cold reset)
void machine_reset(Machine* m);

// Run one video frame: 262 scanlines with interleaved CPU + VDG
void machine_run_frame(Machine* m);

// Run one scanline (~57 CPU cycles + VDG render if active)
void machine_run_scanline(Machine* m);

// Free all allocated memory
void machine_free(Machine* m);

// Memory access (used by CPU callbacks)
uint8_t machine_read(uint16_t addr);
void machine_write(uint16_t addr, uint8_t val);

#endif // MACHINE_H
