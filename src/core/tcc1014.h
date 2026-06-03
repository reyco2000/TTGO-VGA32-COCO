/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : tcc1014.h
 *  Module : TCC1014 (GIME) — registers, MMU, address decode, interrupts
 * ============================================================
*/

#ifndef TCC1014_H
#define TCC1014_H

#include <stdint.h>
#include <stdbool.h>

// GIME interrupt flags — from tcc1014.c:388-394
#define TCC1014_INT_TMR    0x20
#define TCC1014_INT_HBORD  0x10
#define TCC1014_INT_VBORD  0x08
#define TCC1014_INT_EI2    0x04
#define TCC1014_INT_EI1    0x02
#define TCC1014_INT_EI0    0x01

// Render modes — from tcc1014.c:58-63
enum tcc1014_render_mode {
    TCC1014_RENDER_SG,   // Semigraphics
    TCC1014_RENDER_CG,   // Color Graphics
    TCC1014_RENDER_RG,   // Resolution Graphics
    TCC1014_RENDER_RG2,  // Resolution Graphics 2x
};

// GIME palette indices — from tcc1014.h:34-39
// Reflect the usual use of palette entries in VDG compat mode
enum tcc1014_colour {
    TCC1014_GREEN, TCC1014_YELLOW, TCC1014_BLUE, TCC1014_RED,
    TCC1014_WHITE, TCC1014_CYAN, TCC1014_MAGENTA, TCC1014_ORANGE,
    TCC1014_RGCSS0_0, TCC1014_RGCSS0_1, TCC1014_RGCSS1_0, TCC1014_RGCSS1_1,
    TCC1014_DARK_GREEN, TCC1014_BRIGHT_GREEN, TCC1014_DARK_ORANGE, TCC1014_BRIGHT_ORANGE
};

// Scanline length in pixels (constant across GIME variants)
#define TCC1014_tSL  912

typedef struct TCC1014 {
    // --- Address decode output (public, from tcc1014.h:44-46) ---
    unsigned S;              // Address space select (0=ROM,1=CTS,2=IO,6=SCS,7=RAM)
    uint32_t Z;              // Decoded physical address (19-bit)
    bool     RAS;            // RAM access strobe

    // --- Interrupt output (public, from tcc1014.h:48-49) ---
    bool     FIRQ;
    bool     IRQ;

    // --- External interrupt inputs (public, from tcc1014.h:51-52) ---
    bool     IL0;            // Directly wired: cartridge
    bool     IL1;            // Directly wired: keyboard
    bool     IL2;            // Directly wired: serial

    // --- Registers (from tcc1014.c:183-211) ---
    // Raw register values $FF90-$FF9F
    uint8_t  registers[16];

    // Decoded INIT0 ($FF90) — from tcc1014.c:186-191
    bool     COCO;           // bit 7: VDG compatibility mode
    bool     MMUEN;          // bit 6: MMU enabled
    bool     MC3;            // bit 3: constant RAM at $FE00-$FEFF
    bool     MC2;            // bit 2: SCS select
    bool     MC1;            // bit 1: ROM map control
    bool     MC0;            // bit 0: ROM map control

    // Decoded INIT1 ($FF91) — from tcc1014.c:194-195
    bool     TINS;           // bit 5: timer source (0=scanline, 1=3.58MHz)
    unsigned TR;             // MMU task select: 0=task1, 8=task2

    // Decoded VMODE ($FF98) — from tcc1014.c:198-203
    bool     BP;             // bit 7: 1=graphics, 0=text
    bool     BPI;            // bit 5: burst phase invert
    bool     MOCH;           // bit 4: monochrome
    bool     H50;            // bit 3: 50Hz
    unsigned LPR;            // bits 2-0: lines per row (raw, index into LPR_rowmask)

    // Decoded VRES ($FF99) — from tcc1014.c:206-208
    unsigned LPF;            // bits 7-5: lines per field selector
    unsigned HRES;           // bits 4-2: horizontal resolution
    unsigned CRES;           // bits 1-0: color resolution (bpp)

    // Other video registers
    uint8_t  BRDR;           // $FF9A: border color (6-bit)
    unsigned VSC;            // $FF9C: vertical scroll (4-bit)
    uint32_t Y;              // $FF9D-$FF9E: VRAM start address (19-bit)
    bool     HVEN;           // $FF9F bit 7: horizontal virtual enable
    unsigned X;              // $FF9F bits 6-0: horizontal offset (shifted <<1)

    // --- MMU (from tcc1014.c:224-226) ---
    // $FFA0-$FFA7: task 1 banks, $FFA8-$FFAF: task 2 banks
    uint8_t  mmu_bank[16];   // 6-bit bank numbers
    // Phase 5: Pre-computed active bank mapping for current task register
    // active_banks[slot] = mmu_bank[TR | slot] when MMUEN, else 0x38|slot
    uint8_t  active_banks[8];

    // --- Palette (from tcc1014.c:228-229) ---
    uint8_t  palette_reg[16]; // $FFB0-$FFBF: 6-bit RGBRGB
    uint16_t palette_rgb565[16]; // Pre-computed RGB565 palette

    // --- SAM compatibility (from tcc1014.c:231-248) ---
    uint16_t SAM_register;   // Full 16-bit SAM state
    uint8_t  SAM_V;          // V0-V2: VDG mode
    uint16_t SAM_F;          // F0-F6: display offset
    bool     R1;             // MPU rate (0=slow, 1=fast)
    bool     TY;             // Map type (1=all-RAM)

    // --- PIA1B shadow for VDG compat (from tcc1014.c:156-166) ---
    struct {
        bool     ddr;        // Snooped data direction register bit
        unsigned pdr;        // Snooped peripheral data register
    } PIA1B_shadow;

    struct {
        bool GnA;            // Graphics/Alpha (PB7)
        bool GM1;            // Graphics mode 1 (PB5)
        bool GM0;            // Graphics mode 0 (PB4)
        bool CSS;            // Color set select (PB3)
    } VDG;

    // --- Interrupt state (from tcc1014.c:249-250) ---
    unsigned irq_state;
    unsigned firq_state;

    // --- Timer (from tcc1014.c:136-140) ---
    int      timer_counter;
    uint16_t timer_reload;
    bool     blink;          // Toggle on timer zero (text blink)

    // --- Video address state (from tcc1014.c:256-265) ---
    uint32_t B;              // Current VRAM address (running counter)
    unsigned row;            // Row counter within character row (0-15)
    unsigned rowmask;        // Row wrap mask (from LPR)
    bool     row_advance;    // Advance VRAM address this scanline
    unsigned Xoff;           // Horizontal offset accumulator

    // --- Video resolution (from tcc1014.c:263-265) ---
    unsigned BPR;            // Bytes per row (decoded from HRES)
    unsigned row_stride;     // Bytes per row stride (HVEN may override)
    unsigned resolution;     // Horizontal resolution selector

    // --- Vertical state machine (from tcc1014.c:278-286) ---
    struct {
        unsigned lTB;        // Lines of top border
        unsigned lAA;        // Lines of active area (192/199/225)
        bool     active_area;
        unsigned lcount;     // Scanline counter in current vertical state
    } vertical;

    uint8_t  border_colour;
    bool     have_vdata_cache;
    uint8_t  vdata_cache;

    // --- Flags ---
    bool     inverted_text;
    bool     dirty_frame;    // Phase 5: set on VRAM write, cleared after present

    // --- Scanline output buffer ---
    // XRoar uses pixel_data[912] for full scanline with borders.
    // ESP32: only active pixels, palette indices for HAL.
    uint16_t line_buffer[640]; // OPT-C4: pre-converted RGB565 (byte-swapped for direct sprite writes)
    uint16_t line_width;       // Actual pixel width of current line

    // --- External memory pointers (set by machine) ---
    uint8_t* ram;
    uint32_t ram_size;
} TCC1014;

// === Public API ===

// Initialize GIME state (replaces XRoar tcc1014_allocate, line 551)
void tcc1014_init(TCC1014* gime);

// Reset GIME (port of tcc1014_reset, line 661)
void tcc1014_reset(TCC1014* gime);

// Address decode — port of tcc1014_mem_cycle() line 689
// Sets gime->S, gime->Z, gime->RAS
// For writes to GIME registers ($FF90-$FFDF), handles internally
// Returns data byte for register reads, 0 if not a register read
// is_reg_read set to true if the access was a GIME register read
void tcc1014_mem_cycle(TCC1014* gime, uint16_t addr, bool RnW,
                       uint8_t write_data, uint8_t* read_data, bool* is_reg_access);

// Register write — port of tcc1014_set_register() line 895
void tcc1014_write_register(TCC1014* gime, unsigned reg, uint8_t val);

// Register read — port of tcc1014.c line 750-765
uint8_t tcc1014_read_register(TCC1014* gime, unsigned reg);

// MMU bank write
void tcc1014_write_mmu(TCC1014* gime, uint8_t offset, uint8_t val);

// MMU bank read
uint8_t tcc1014_read_mmu(TCC1014* gime, uint8_t offset);

// Palette write
void tcc1014_write_palette(TCC1014* gime, uint8_t offset, uint8_t val);

// Palette read
uint8_t tcc1014_read_palette(TCC1014* gime, uint8_t offset);

// SAM compat write — port of tcc1014.c line 784-793
void tcc1014_write_sam(TCC1014* gime, uint8_t offset);

// Set full SAM register — port of tcc1014_set_sam_register() line 847
void tcc1014_set_sam_register(TCC1014* gime, unsigned val);

// Per-scanline tick — replaces XRoar event-driven timing
void tcc1014_tick_scanline(TCC1014* gime);

// Set interrupt flag — port of SET_INTERRUPT macro, line 396
void tcc1014_set_interrupt(TCC1014* gime, uint8_t flag);

// Rendering — stub for Phase 1, implemented in Phase 3
void tcc1014_render_scanline(TCC1014* gime, unsigned scanline);

// PIA1B snooping — port of tcc1014.c:726-730
void tcc1014_snoop_pia1b(TCC1014* gime, uint16_t addr, uint8_t val);

// Mode update — port of update_from_gime_registers() line 1558
void tcc1014_update_mode(TCC1014* gime);

// Set inverted text flag
void tcc1014_set_inverted_text(TCC1014* gime, bool value);

// Initialize palette LUT (64-color GIME → RGB565)
void tcc1014_init_palette_lut(void);

// Phase 5: Recompute active_banks[8] from current MMU state
void tcc1014_update_active_banks(TCC1014* gime);

#endif // TCC1014_H
