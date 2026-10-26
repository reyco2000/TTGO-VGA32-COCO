#pragma GCC optimize("O2")
/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : tcc1014.cpp
 *  Module : TCC1014 (GIME) — registers, MMU, address decode, interrupts, and timer
 * ============================================================
*/

// Stores in this file go to internal RAM only (see the header for the rule).
#include "../utils/no_psram_memw.h"
#include "tcc1014.h"
#include "font_gime.h"
#include "../utils/perf_probe.h"
#include <string.h>

// ============================================================
// Lookup tables — from tcc1014.c:406-424
// ============================================================

// OPT-C3: DRAM mirror of the GIME font ROM. Populated once in tcc1014_init()
// from font_gime_flash[] (PROGMEM). Per-read access avoids the ~15ns
// pgm_read_byte() overhead in the scanline text renderer.
static uint8_t font_gime_dram[1536];

// OPT-G4: 1 = use the original monolithic scanline renderer (A/B probing).
#ifndef GIME_RENDER_LEGACY
#define GIME_RENDER_LEGACY 0
#endif

// Lines of top border by [H50][LPF] — from tcc1014.c:406-409
static const unsigned VRES_LPF_lTB[2][4] = {
    { 36, 34, 0xFFFF, 19 },  // 60Hz
    { 63, 59, 0xFFFF, 46 },  // 50Hz
};

// Lines of active area by LPF — from tcc1014.c:412
static const unsigned VRES_LPF_lAA[4] = { 192, 199, 0xFFFF, 225 };

// Bytes per row in graphics mode by HRES — from tcc1014.c:415
static const unsigned VRES_HRES_BPR[8] = { 16, 20, 32, 40, 64, 80, 128, 160 };

// Bytes per row in text mode by HRES — from tcc1014.c:416
static const unsigned VRES_HRES_BPR_TEXT[8] = { 32, 40, 32, 40, 64, 80, 64, 80 };

// Row mask by LPR value — from tcc1014.c:421
// LPR field encodes lines-per-row: 0=always reset, ..., 7(16)=never reset
static const unsigned LPR_rowmask[8] = { 0, 1, 2, 8, 9, 10, 11, 16 };

// SAM V row mask (for VDG compat) — from tcc1014.c:422
static const unsigned SAM_V_rowmask[8] = { 3, 3, 3, 2, 2, 1, 1, 1 };

// VSC row mask — from tcc1014.c:423
static const unsigned VSC_rowmask[16] = { 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 4, 3, 2, 1, 12 };

// Timer offset: '87 GIME = 1, '86 GIME = 2. We use '87.
static const int TIMER_OFFSET = 1;

// Timer clock in TINS=1 mode, in half-ticks per scanline: 3.58 MHz is a
// quarter of the 14.318 MHz master clock and a scanline is 912 master
// clocks, so 228 ticks (XRoar: expiry at counter << 2 master ticks).
static const int32_t TIMER_FAST_X2_PER_LINE = 456;

// (Re)start the timer from its registers — port of schedule_timer(),
// tcc1014.c:1535-1544.
static void arm_timer(TCC1014* gime) {
    unsigned timer_reset = ((gime->registers[4] & 0x0F) << 8) | gime->registers[5];
    gime->timer_counter = timer_reset ? (timer_reset + TIMER_OFFSET) : 0;
    gime->timer_left_x2 = gime->timer_counter * 2;
}

// 64-color GIME palette → RGB565 lookup
static uint16_t gime_rgb565_lut[64];

// ============================================================
// Internal forward declarations
// ============================================================

static void update_from_gime_registers(TCC1014* gime);
static void update_from_sam_register(TCC1014* gime);

// ============================================================
// Interrupt logic — port of SET_INTERRUPT macro, tcc1014.c:396-402
// ============================================================

static inline void set_interrupt(TCC1014* g, uint8_t flag) {
    // Both irq and firq state accumulate the flag, gated by their
    // respective enable registers ($FF92/$FF93)
    g->irq_state  |= (flag & g->registers[2]);
    g->firq_state |= (flag & g->registers[3]);
    // Output is gated by master enable bits in INIT0 ($FF90)
    g->IRQ  = (g->registers[0] & 0x20) ? (g->irq_state & 0x3F) : 0;
    g->FIRQ = (g->registers[0] & 0x10) ? (g->firq_state & 0x3F) : 0;
}

// Public wrapper
void tcc1014_set_interrupt(TCC1014* gime, uint8_t flag) {
    set_interrupt(gime, flag);
}

// Edge-detect IL2..0: an external source latches only on a 0->1 change of
// its input, not while it stays high — port of tcc1014.c:714-726 (XRoar
// d9bba8a3, from coco_key_test on real hardware for the keyboard; XRoar
// assumes the same for IL0/IL2).
static inline void edge_detect_il(TCC1014* g) {
    if (g->IL2 && !g->IL2_state) set_interrupt(g, TCC1014_INT_EI2);
    g->IL2_state = g->IL2 ? TCC1014_INT_EI2 : 0;
    if (g->IL1 && !g->IL1_state) set_interrupt(g, TCC1014_INT_EI1);
    g->IL1_state = g->IL1 ? TCC1014_INT_EI1 : 0;
    if (g->IL0 && !g->IL0_state) set_interrupt(g, TCC1014_INT_EI0);
    g->IL0_state = g->IL0 ? TCC1014_INT_EI0 : 0;
}

// $FF92/$FF93 write: keep only the pending bits still enabled, then latch
// sources that are already active and newly enabled (TMR at timer 0, IL
// lines high) — port of tcc1014.c:950-970. GIME-MMU-TESTER showed the TMR
// latch on real hardware.
static inline unsigned enable_latch(const TCC1014* g, unsigned state,
                                    unsigned val, unsigned changed) {
    const unsigned set_int = (g->timer_counter == 0 ? TCC1014_INT_TMR : 0) |
                             g->IL2_state | g->IL1_state | g->IL0_state;
    return (state & val) | (set_int & changed & val);
}

// ============================================================
// Palette LUT initialization
// ============================================================

void tcc1014_init_palette_lut(void) {
    // GIME palette entries are 6-bit INTERLEAVED: R1 G1 B1 R0 G0 B0
    // (NOT packed RRGGBB — confirmed from XRoar coco3.c:595-598)
    // R = (bit5<<1) | bit2, G = (bit4<<1) | bit1, B = (bit3<<1) | bit0
    for (int i = 0; i < 64; i++) {
        uint8_t r = (((i >> 4) & 2) | ((i >> 2) & 1)) * 85;
        uint8_t g = (((i >> 3) & 2) | ((i >> 1) & 1)) * 85;
        uint8_t b = (((i >> 2) & 2) | ((i >> 0) & 1)) * 85;
        gime_rgb565_lut[i] = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    }
}

// ============================================================
// Phase 5: Pre-compute active_banks[8] for fast-path MMU lookup
// Called on: MMU bank write, task register (TR) change, MMUEN change
// ============================================================

// OPT-M2: mirror of the RAM fast path in machine_read_coco3/machine_write_coco3
// (Z = (bank << 13 | offset) wrapped into physical RAM).
static void update_fast_pages(TCC1014* gime) {
    PERF_CPU(perf_cpu.page_rebuild++);
    for (int i = 0; i < 8; i++) {
        const unsigned bank = gime->active_banks[i];
        const uint32_t base = ((uint32_t)bank << 13) & gime->ram_mask;
        uint8_t* p = gime->ram ? gime->ram + base : nullptr;
        gime->wr_page[i] = p;
        // ROM select looks at the GIME's own six bits only
        gime->rd_page[i] = (gime->TY || (bank & 0x3F) < 0x3C) ? p : nullptr;
    }
}

void tcc1014_set_ram(TCC1014* gime, uint8_t* ram, uint32_t size) {
    gime->ram = ram;
    gime->ram_size = size;
    gime->ram_mask = size - 1;
    PERF_CPU(perf_cpu.ram_base = ram);
    gime->vram_mask = (size < 0x80000u ? size : 0x80000u) - 1;
    gime->vram_base = 0;
    gime->bank_mask = (size > 0x100000u) ? 0xFF : (size > 0x80000u) ? 0x7F : 0x3F;
    tcc1014_update_active_banks(gime);
}

void tcc1014_update_active_banks(TCC1014* gime) {
    for (int i = 0; i < 8; i++) {
        gime->active_banks[i] = gime->MMUEN ? gime->mmu_bank[gime->TR | i]
                                             : (0x38 | i);
    }
    update_fast_pages(gime);
}

// ============================================================
// Init / Reset — port of tcc1014_allocate (line 551) and tcc1014_reset (line 661)
// ============================================================

void tcc1014_init(TCC1014* gime) {
    memset(gime, 0, sizeof(TCC1014));
    tcc1014_invalidate_lines();
    // Initial VRAM address — from tcc1014.c:557
    gime->B = 0x60400;
    // Initialize palette LUT
    tcc1014_init_palette_lut();
    // OPT-C3: Copy GIME font from flash (PROGMEM) into DRAM mirror so the
    // scanline text renderer can index it with a plain array load.
#ifdef ARDUINO
    memcpy_P(font_gime_dram, font_gime_flash, sizeof(font_gime_dram));
#else
    memcpy(font_gime_dram, font_gime_flash, sizeof(font_gime_dram));
#endif
}

void tcc1014_reset(TCC1014* gime) {
    tcc1014_invalidate_lines();
    // Reset all registers to 0 — from tcc1014.c:664-667
    for (int i = 0; i < 16; i++) {
        tcc1014_write_register(gime, i, 0);
        gime->palette_reg[i] = 0;
        // OPT-C4: byte-swapped RGB565 for direct sprite-FB writes.
        uint16_t native0 = gime_rgb565_lut[0];
        gime->palette_rgb565[i] = (uint16_t)((native0 >> 8) | (native0 << 8));
    }

    // Reset SAM register — from tcc1014.c:668
    tcc1014_set_sam_register(gime, 0);

    // Reset MMU banks to identity (top 512K mapping)
    for (int i = 0; i < 16; i++) {
        gime->mmu_bank[i] = 0x38 | (i & 7);
    }
    tcc1014_update_active_banks(gime);

    // Reset video state — from tcc1014.c:673-686
    gime->row = 0;
    gime->vertical.active_area = false;
    gime->vertical.lcount = 0;
    gime->PIA1B_shadow.pdr = 0;
    gime->PIA1B_shadow.ddr = false;
    gime->have_vdata_cache = false;
    gime->blink = false;
    gime->inverted_text = false;
    gime->dirty_frame = true;  // Force first frame push
    gime->timer_counter = 0;
    gime->timer_reload = 0;
    gime->timer_left_x2 = 0;
    gime->irq_state = 0;
    gime->firq_state = 0;
    gime->IRQ = false;
    gime->FIRQ = false;
    gime->IL0 = false;
    gime->IL1 = false;
    gime->IL2 = false;
    gime->IL0_state = 0;
    gime->IL1_state = 0;
    gime->IL2_state = 0;
    gime->line_width = 0;

    // B is set during vertical state init
    update_from_gime_registers(gime);
}

// ============================================================
// Address decode — port of tcc1014_mem_cycle() lines 689-806
//
// This is the most critical function. It sets S, Z, RAS for the
// caller to dispatch reads/writes. GIME-internal registers
// ($FF90-$FFDF) are handled here directly.
//
// Adaptations:
//   - XRoar reads/writes *CPUD directly; we pass data in/out
//   - XRoar calls render_scanline() before register writes; we skip
//     that (no mid-scanline rendering on ESP32)
//   - External interrupt update from IL0/IL1/IL2 preserved
// ============================================================

void tcc1014_mem_cycle(TCC1014* gime, uint16_t addr, bool RnW,
                       uint8_t write_data, uint8_t* read_data, bool* is_reg_access) {
    // External interrupt inputs, edge-detected first — tcc1014.c:714-726
    edge_detect_il(gime);

    gime->S = 7;
    gime->RAS = false;
    if (is_reg_access) *is_reg_access = false;

    // --- Main address space: $0000-$FEFF ---
    // Port of tcc1014.c:698-721
    if (addr < 0xFF00) {
        bool use_mmu = gime->MMUEN;

        // MC3 bit: $FE00-$FEFF always maps to top of RAM (bank 0x3F)
        // Port of tcc1014.c:701-706
        if (addr >= 0xFE00) {
            if (gime->MC3) {
                gime->RAS = true;
                use_mmu = false;
            }
        }

        // MMU bank translation — port of tcc1014.c:708-709
        unsigned bank = use_mmu ? gime->mmu_bank[gime->TR | (addr >> 13)]
                                : (0x38 | (addr >> 13));

        // ROM/CTS/RAM select — port of tcc1014.c:711-719
        if (!gime->TY && (bank & 0x3F) >= 0x3C) {
            if (!gime->MC1) {
                gime->S = ((bank & 0x3F) >= 0x3E) ? 1 : 0;
            } else {
                gime->S = gime->MC0 ? 1 : 0;
            }
        } else {
            gime->RAS = true;
        }

        // Physical address — port of tcc1014.c:721
        gime->Z = ((uint32_t)bank << 13) | (addr & 0x1FFF);

    // --- PIA space: $FF00-$FF3F ---
    // Port of tcc1014.c:723-736
    } else if (addr < 0xFF40) {
        if ((addr & 0x10) == 0) {
            gime->S = 2;  // PIA I/O

            // GIME snoops PIA1B writes for VDG compat — tcc1014.c:726-735
            if (addr == 0xFF22 && !RnW) {
                if (gime->PIA1B_shadow.ddr) {
                    gime->PIA1B_shadow.pdr = write_data & 0xF8;
                    update_from_gime_registers(gime);
                }
            } else if (addr == 0xFF23 && !RnW) {
                gime->PIA1B_shadow.ddr = write_data & 0x04;
            }
        }

    // --- SCS / CTS: $FF40-$FF5F ---
    // Port of tcc1014.c:738-741
    } else if (addr < 0xFF60) {
        if (gime->MC2 || addr >= 0xFF50) {
            gime->S = 6;  // SCS (disk controller)
        }

    // --- Reserved: $FF60-$FF8F ---
    // Port of tcc1014.c:743-744 — NOP
    } else if (addr < 0xFF90) {
        // Nothing

    // --- GIME registers: $FF90-$FF9F ---
    // Port of tcc1014.c:746-766
    } else if (addr < 0xFFA0) {
        if (is_reg_access) *is_reg_access = true;
        if (!RnW) {
            tcc1014_write_register(gime, addr & 0x0F, write_data);
        } else {
            // Only IRQ/FIRQ status registers are readable — tcc1014.c:750-765
            if (read_data) {
                if (addr == 0xFF92) {
                    // Read-acknowledge IRQ — port of tcc1014.c:788-791
                    *read_data = gime->irq_state;
                    gime->irq_state = 0;
                    gime->IRQ = false;
                } else if (addr == 0xFF93) {
                    // Read-acknowledge FIRQ — port of tcc1014.c:792-795
                    *read_data = gime->firq_state;
                    gime->firq_state = 0;
                    gime->FIRQ = false;
                } else {
                    *read_data = 0xFF;  // Other GIME regs are write-only
                    // ...except that a 1 MB / 2 MB board answers $FF9B
                    if (addr == 0xFF9B && gime->bank_mask != 0x3F)
                        *read_data = 0xFC | (uint8_t)(gime->vram_base >> 19);
                }
            }
        }

    // --- MMU banks: $FFA0-$FFAF ---
    // Port of tcc1014.c:768-773
    } else if (addr < 0xFFB0) {
        if (is_reg_access) *is_reg_access = true;
        if (!RnW) {
            tcc1014_write_mmu(gime, addr & 0x0F, write_data);
        } else {
            if (read_data) *read_data = tcc1014_read_mmu(gime, addr & 0x0F);
        }

    // --- Palette: $FFB0-$FFBF ---
    // Port of tcc1014.c:775-782
    } else if (addr < 0xFFC0) {
        if (is_reg_access) *is_reg_access = true;
        if (!RnW) {
            tcc1014_write_palette(gime, addr & 0x0F, write_data);
        } else {
            if (read_data) *read_data = tcc1014_read_palette(gime, addr & 0x0F);
        }

    // --- SAM compat: $FFC0-$FFDF ---
    // Port of tcc1014.c:784-793
    } else if (addr < 0xFFE0) {
        if (is_reg_access) *is_reg_access = true;
        if (!RnW) {
            tcc1014_write_sam(gime, addr - 0xFFC0);
        }
        // SAM registers are write-only; reads return open bus

    // --- Vectors: $FFE0-$FFFF ---
    // Port of tcc1014.c:795-797
    // Vectors always come from ROM. Must compute Z so machine layer
    // can calculate ROM offset. Bank = slot 7 mapping.
    } else {
        // Vectors ($FFE0-$FFFF) always from ROM — port of tcc1014.c:795-796
        // XRoar only sets S=0 here, does NOT compute Z.
        // The machine layer uses the CPU address directly for ROM reads.
        gime->S = 0;
    }

}

// ============================================================
// Register writes — port of tcc1014_set_register() lines 895-996
// ============================================================

void tcc1014_write_register(TCC1014* gime, unsigned reg, uint8_t val) {
    reg &= 0x0F;
    const unsigned changed = gime->registers[reg] ^ val;
    gime->registers[reg] = val;

    switch (reg) {
    // INIT0 ($FF90) — tcc1014.c:900-908
    case 0:
        gime->COCO  = val & 0x80;
        gime->MMUEN = val & 0x40;
        gime->MC3   = val & 0x08;
        gime->MC2   = val & 0x04;
        gime->MC1   = val & 0x02;
        gime->MC0   = val & 0x01;
        // IEN/FEN gate the outputs at once — tcc1014.c:935-936
        gime->IRQ  = (val & 0x20) ? (gime->irq_state & 0x3F) : 0;
        gime->FIRQ = (val & 0x10) ? (gime->firq_state & 0x3F) : 0;
        tcc1014_update_active_banks(gime);
        update_from_gime_registers(gime);
        break;

    // INIT1 ($FF91) — tcc1014.c:911-916
    case 1:
        {
            const bool tins = val & 0x20;
            const unsigned tr = (val & 0x01) ? 8 : 0;
            // A change of timer source restarts the timer (tcc1014.c:928).
            if (tins != gime->TINS) {
                gime->TINS = tins;
                arm_timer(gime);
            }
            // Rick Dangerous switches task twice per 6 kHz FIRQ; only
            // rebuild the page tables when TR really changed.
            if (tr != gime->TR) {
                gime->TR = tr;
                tcc1014_update_active_banks(gime);
            }
        }
        break;

    // IRQ enable ($FF92) — tcc1014.c:950-959
    case 2:
        gime->irq_state = enable_latch(gime, gime->irq_state, val, changed);
        gime->IRQ = (gime->registers[0] & 0x20) ? (gime->irq_state & 0x3F) : 0;
        break;

    // FIRQ enable ($FF93) — tcc1014.c:961-970
    case 3:
        gime->firq_state = enable_latch(gime, gime->firq_state, val, changed);
        gime->FIRQ = (gime->registers[0] & 0x10) ? (gime->firq_state & 0x3F) : 0;
        break;

    // Timer MSB ($FF94) — tcc1014.c:972-976. Re-arms only: a reload of 0
    // stops the timer without latching TMR (d9bba8a3).
    case 4:
        arm_timer(gime);
        break;

    // Timer LSB ($FF95) — tcc1014.c:944-946
    // Just stores the value; timer is only armed on MSB write
    case 5:
        break;

    // Regs 6, 7 unused
    case 6:
    case 7:
        break;

    // VMODE ($FF98) — tcc1014.c:949-958
    case 8:
        gime->BP   = val & 0x80;
        gime->BPI  = val & 0x20;
        gime->MOCH = val & 0x10;
        gime->H50  = val & 0x08;
        gime->LPR  = val & 7;
        gime->dirty_frame = true;
        update_from_gime_registers(gime);
        break;

    // VRES ($FF99) — tcc1014.c:960-965
    case 9:
        gime->LPF  = (val >> 5) & 3;
        gime->HRES = (val >> 2) & 7;
        gime->CRES = val & 3;
        gime->dirty_frame = true;
        update_from_gime_registers(gime);
        break;

    // BRDR ($FF9A) — tcc1014.c:968-971
    case 0x0A:
        gime->BRDR = val & 0x3F;
        gime->dirty_frame = true;
        update_from_gime_registers(gime);
        break;

    // $FF9B: with 1 MB / 2 MB fitted, bits 1-0 pick the 512 KB bank that
    // video is read from.
    case 0x0B:
        if (gime->bank_mask != 0x3F) {
            gime->vram_base = ((uint32_t)(val & 0x03) << 19) & gime->ram_mask;
            gime->dirty_frame = true;
        }
        break;

    // VSC ($FF9C) — tcc1014.c:974-977
    case 0x0C:
        gime->VSC = val & 0x0F;
        update_from_gime_registers(gime);
        break;

    // Vertical offset MSB ($FF9D) — tcc1014.c:980-982
    case 0x0D:
        gime->Y = ((uint32_t)val << 11) | ((uint32_t)gime->registers[0x0E] << 3);
        break;

    // Vertical offset LSB ($FF9E) — tcc1014.c:985-987
    case 0x0E:
        gime->Y = ((uint32_t)gime->registers[0x0D] << 11) | ((uint32_t)val << 3);
        break;

    // Horizontal offset ($FF9F) — tcc1014.c:990-994
    case 0x0F:
        gime->HVEN = val & 0x80;
        gime->X    = (val & 0x7F) << 1;
        update_from_gime_registers(gime);
        break;
    }
}

// ============================================================
// Register reads — port of tcc1014.c:750-765
// Only IRQ/FIRQ status are readable; others return 0xFF
// ============================================================

uint8_t tcc1014_read_register(TCC1014* gime, unsigned reg) {
    reg &= 0x0F;
    if (reg == 2) {
        // Read-acknowledge IRQ status — port of tcc1014.c:788-791. The
        // IL inputs are edge-detected first, as at the top of every
        // tcc1014_mem_cycle(), which this direct path skips. No TMR
        // re-latch on read (d9bba8a3).
        edge_detect_il(gime);
        uint8_t val = gime->irq_state;
        gime->irq_state = 0;
        gime->IRQ = false;  // Deassert IRQ output immediately
        return val;
    }
    if (reg == 3) {
        // Read-acknowledge FIRQ status — port of tcc1014.c:792-795
        edge_detect_il(gime);
        uint8_t val = gime->firq_state;
        gime->firq_state = 0;
        gime->FIRQ = false;  // Deassert FIRQ output immediately
        return val;
    }
    return 0xFF;
}

// ============================================================
// MMU bank writes/reads — port of tcc1014.c:768-773
// ============================================================

void tcc1014_write_mmu(TCC1014* gime, uint8_t offset, uint8_t val) {
    PERF_CPU(perf_cpu.mmu_wr++);
    gime->mmu_bank[offset & 0x0F] = val & gime->bank_mask;
    tcc1014_update_active_banks(gime);
}

uint8_t tcc1014_read_mmu(TCC1014* gime, uint8_t offset) {
    return gime->mmu_bank[offset & 0x0F];
}

// ============================================================
// Palette writes/reads — port of tcc1014.c:775-782
// Pre-compute RGB565 on write for zero-overhead rendering
// ============================================================

void tcc1014_write_palette(TCC1014* gime, uint8_t offset, uint8_t val) {
    offset &= 0x0F;
    gime->palette_reg[offset] = val & 0x3F;
    // OPT-C4: store byte-swapped RGB565 so the renderer can write directly
    // into the framebuffer (byte-swapped format used by the VGA LUT).
    uint16_t native = gime_rgb565_lut[val & 0x3F];
    gime->palette_rgb565[offset] = (uint16_t)((native >> 8) | (native << 8));
    gime->dirty_frame = true;
}

uint8_t tcc1014_read_palette(TCC1014* gime, uint8_t offset) {
    return gime->palette_reg[offset & 0x0F] & 0x3F;
}

// ============================================================
// SAM compat writes — port of tcc1014.c:784-793
// SAM register uses set/clear bit pairs at $FFC0-$FFDF
// ============================================================

void tcc1014_write_sam(TCC1014* gime, uint8_t offset) {
    // Even addr = clear bit, odd addr = set bit
    // offset = addr - 0xFFC0 (0-31)
    unsigned bit = 1 << ((offset >> 1) & 0x0F);
    if (offset & 1) {
        gime->SAM_register |= bit;
    } else {
        gime->SAM_register &= ~bit;
    }
    update_from_sam_register(gime);
}

void tcc1014_set_sam_register(TCC1014* gime, unsigned val) {
    gime->SAM_register = val;
    update_from_sam_register(gime);
}

// ============================================================
// PIA1B snooping — port of tcc1014.c:726-735
// Called when machine layer writes to PIA1 port B ($FF22/$FF23)
// ============================================================

void tcc1014_snoop_pia1b(TCC1014* gime, uint16_t addr, uint8_t val) {
    if (addr == 0xFF22) {
        if (gime->PIA1B_shadow.ddr) {
            gime->PIA1B_shadow.pdr = val & 0xF8;
            update_from_gime_registers(gime);
        }
    } else if (addr == 0xFF23) {
        gime->PIA1B_shadow.ddr = val & 0x04;
    }
}

// ============================================================
// Per-scanline timer tick — replaces XRoar event-driven timing
//
// Port of do_hs_fall() timer logic (line 1017-1023) and
// schedule_timer/update_timer (lines 1523-1547)
//
// Two modes:
//   TINS=0: decrement at scanline rate (~15.7 kHz)
//   TINS=1: 3.58 MHz, 228 ticks per scanline. (Until 2026-10 this was
//           28 per scanline, from reading XRoar's "1/8 the GIME clock" as
//           3.58 MHz/8; the GIME clock is 28.6 MHz. Timer-driven sound,
//           e.g. Rick Dangerous's 6 kHz sample FIRQ, ran 8x slow.)
// Interrupts are raised at the end of the scanline, not mid-line.
// ============================================================

static void timer_expired(TCC1014* gime) {
    // Port of update_timer, tcc1014.c:1546-1550
    gime->blink = !gime->blink;
    gime->timer_fires++;
    set_interrupt(gime, TCC1014_INT_TMR);
}

void tcc1014_tick_scanline(TCC1014* gime) {
    if (gime->timer_counter <= 0) return;

    if (!gime->TINS) {
        // TINS=0: line-rate timer — 1 tick per scanline
        // Port of tcc1014.c:1017-1023
        gime->timer_counter--;
        if (gime->timer_counter <= 0) {
            timer_expired(gime);
            arm_timer(gime);
        }
    } else {
        gime->timer_left_x2 -= TIMER_FAST_X2_PER_LINE;
        // A reload shorter than a scanline expires several times per line.
        // Each expiry re-reads $FF94/$FF95 (update_timer → schedule_timer,
        // tcc1014.c:1552-1567), so a reload written while the timer runs
        // takes effect on the next period, and a reload of 0 stops it. The
        // next period counts from the exact expiry point.
        while (gime->timer_left_x2 <= 0) {
            const int32_t over_x2 = gime->timer_left_x2;
            timer_expired(gime);
            arm_timer(gime);
            if (gime->timer_counter == 0) break;
            gime->timer_left_x2 += over_x2;
        }
    }
}

// ============================================================
// update_from_gime_registers — port of tcc1014.c:1558-1599
//
// Recalculates all derived video state from current register
// values. Called whenever GIME registers or PIA1B shadow change.
// ============================================================

static void update_from_gime_registers(TCC1014* gime) {
    // Decode VDG-compatible mode setting — tcc1014.c:1562-1566
    gime->VDG.GnA = gime->PIA1B_shadow.pdr & 0x80;
    gime->VDG.GM1 = gime->PIA1B_shadow.pdr & 0x20;
    gime->VDG.GM0 = gime->PIA1B_shadow.pdr & 0x10;
    gime->VDG.CSS = gime->PIA1B_shadow.pdr & 0x08;

    if (gime->COCO) {
        // VDG compatible mode — tcc1014.c:1568-1582
        if (!gime->VDG.GnA || !(gime->SAM_V & 1)) {
            gime->BPR = 32;
            gime->resolution = 1;
        } else {
            gime->BPR = 16;
            gime->resolution = 0;
        }

        gime->vertical.lTB = gime->H50 ? 63 : 36;
        gime->rowmask = gime->VDG.GnA ? SAM_V_rowmask[gime->SAM_V]
                                       : VSC_rowmask[gime->VSC];
    } else {
        // CoCo 3 native modes — tcc1014.c:1583-1597
        if (gime->BP) {
            gime->BPR = VRES_HRES_BPR[gime->HRES];
            gime->resolution = gime->HRES >> 1;
        } else {
            gime->BPR = VRES_HRES_BPR_TEXT[gime->HRES];
            gime->resolution = (gime->HRES & 4) ? 2 : 1;
        }

        gime->vertical.lTB = VRES_LPF_lTB[gime->H50][gime->LPF];
        gime->rowmask = LPR_rowmask[gime->LPR];
    }

    // Active lines — from tcc1014.c:1168
    gime->vertical.lAA = gime->COCO ? 192 : VRES_LPF_lAA[gime->LPF];

    // Row stride — HVEN overrides to 256 bytes per row
    if (gime->COCO || gime->BP) {
        gime->row_stride = gime->HVEN ? 256 : gime->BPR;
    } else {
        // Text mode: stride includes attribute bytes
        gime->row_stride = gime->HVEN ? 256 : (gime->BPR << (gime->CRES & 1));
    }

    // Border colour — port of tcc1014.c:1124-1136
    if (gime->COCO) {
        if (!gime->VDG.GnA) {
            bool GM2 = gime->PIA1B_shadow.pdr & 0x40;
            bool text_border = !gime->VDG.GM1 && GM2;
            unsigned text_border_colour = gime->VDG.CSS ? 0x26 : 0x12;
            gime->border_colour = text_border ? text_border_colour : 0;
        } else {
            unsigned c = gime->VDG.CSS ? TCC1014_RGCSS1_1 : TCC1014_RGCSS0_1;
            gime->border_colour = gime->palette_reg[c];
        }
    } else {
        gime->border_colour = gime->BRDR;
    }
}

// ============================================================
// update_from_sam_register — port of tcc1014.c:1603-1609
// Decode SAM register bits and call update_from_gime_registers
// ============================================================

static void update_from_sam_register(TCC1014* gime) {
    gime->TY    = gime->SAM_register & 0x8000;
    gime->R1    = gime->SAM_register & 0x1000;
    gime->SAM_F = (gime->SAM_register >> 3) & 0x7F;
    gime->SAM_V = gime->SAM_register & 0x07;
    update_fast_pages(gime);   // TY decides whether pages under ROM read as RAM
    update_from_gime_registers(gime);
}

// ============================================================
// Set inverted text flag
// ============================================================

void tcc1014_set_inverted_text(TCC1014* gime, bool value) {
    gime->inverted_text = value;
}

// ============================================================
// Mode update (public wrapper)
// ============================================================

void tcc1014_update_mode(TCC1014* gime) {
    update_from_gime_registers(gime);
}

// ============================================================
// VRAM fetch — port of fetch_byte_vram() tcc1014.c:1233-1249
//
// XRoar fetches 16 bits via delegate, caches second byte.
// ESP32: direct RAM read with same caching logic.
// ============================================================

static inline uint8_t fetch_byte_vram(TCC1014* g) {
    uint8_t r;
    if (g->have_vdata_cache) {
        r = g->vdata_cache;
        g->have_vdata_cache = false;
    } else {
        // X offset is dynamically added — from tcc1014.c:1242
        uint32_t addr = g->B + (g->Xoff & 0xFF);
        addr &= g->vram_mask;       // Wrap inside the video bank
        r = g->ram[g->vram_base + addr];
        g->vdata_cache = g->ram[g->vram_base + ((addr + 1) & g->vram_mask)];
        g->have_vdata_cache = true;
        // Xoff only advances on actual fetch, NOT on cached return
        // (XRoar tcc1014.c:1243 — Xoff+=2 is inside the else block)
        g->Xoff += 2;
    }
    return r;
}

// ============================================================
// Render scanline — port of render_scanline() tcc1014.c:1261-1517
//
// Renders one scanline of active-area video to line_buffer[].
// Handles VDG compat mode (SG/CG/RG) and CoCo3 native (text/gfx).
// Output is palette register values (6-bit GIME color indices),
// NOT raw palette indices — the HAL maps these through palette_rgb565[].
//
// Key difference from XRoar: no beam-tracking or mid-scanline updates.
// We render the full line in one pass, writing only active pixels.
// ============================================================

// OPT-G4: this monolith is the reference implementation. It re-evaluates the
// COCO/BP/CRES/resolution branch tree for every nibble, and because it stores
// through a uint16_t* into gime->line_buffer the compiler must reload every
// gime-> field after each pixel store. The per-mode renderers below replace it;
// it is kept for A/B probing (GIME_RENDER_LEGACY) and as the oracle for the
// host-side equivalence test (tools/gime_render_test).
#if GIME_RENDER_LEGACY || defined(TCC1014_RENDER_TEST)
void tcc1014_render_scanline_legacy(TCC1014* gime) {
    if (!gime->vertical.active_area || !gime->ram)
        return;

    uint16_t* pixel = gime->line_buffer;   // OPT-C4: RGB565 (byte-swapped)
    unsigned npixels = 0;

    // Horizontal offset for this scanline (tcc1014.c:1144: Xoff = X), native
    // modes only: XRoar also applies it in COCO mode, MAME doesn't, and the
    // suite has no verdict for that yet.
    gime->Xoff = gime->COCO ? 0 : gime->X;
    gime->have_vdata_cache = false;

    // Number of bytes to process = BPR (bytes per row)
    unsigned bytes_to_process = gime->BPR;

    for (unsigned byte_idx = 0; byte_idx < bytes_to_process; byte_idx++) {
        enum tcc1014_render_mode render_mode;
        uint8_t gdata;
        uint8_t fg_colour = 0;
        uint8_t bg_colour = 0;
        uint8_t cg_colours = 0;

        // --- VRAM fetch and mode interpretation — tcc1014.c:1295-1382 ---

        if (gime->COCO) {
            // VDG compatible mode — tcc1014.c:1295-1347
            uint8_t vdata = fetch_byte_vram(gime);
            unsigned font_row = gime->row & 0x0F;
            bool SnA = vdata & 0x80;

            if (gime->VDG.GnA) {
                // Graphics mode
                gdata = vdata;
                fg_colour = gime->VDG.CSS ? TCC1014_RGCSS1_1 : TCC1014_RGCSS0_1;
                bg_colour = gime->VDG.CSS ? TCC1014_RGCSS1_0 : TCC1014_RGCSS0_0;
                cg_colours = !gime->VDG.CSS ? TCC1014_GREEN : TCC1014_WHITE;
                if (gime->VDG.GM0) {
                    if (gime->resolution || (gime->PIA1B_shadow.pdr & 0x70) == 0x70) {
                        render_mode = TCC1014_RENDER_RG;
                    } else {
                        render_mode = TCC1014_RENDER_RG2;
                    }
                } else {
                    render_mode = TCC1014_RENDER_CG;
                }
            } else {
                if (SnA) {
                    // Semigraphics — tcc1014.c:1317-1326
                    if (font_row < 6) {
                        gdata = vdata >> 2;
                    } else {
                        gdata = vdata;
                    }
                    fg_colour = (vdata >> 4) & 7;
                    bg_colour = TCC1014_RGCSS0_0;
                    render_mode = TCC1014_RENDER_SG;
                } else {
                    // Alphanumeric — tcc1014.c:1328-1345
                    bool INV = vdata & 0x40;
                    INV ^= gime->VDG.GM1;  // 6847T1-compatible invert flag
                    uint8_t c = vdata & 0x7F;
                    if (c < 0x20) {
                        c |= (gime->VDG.GM0 ? 0x60 : 0x40);
                        INV ^= gime->VDG.GM0;
                    } else if (c >= 0x60) {
                        c ^= 0x40;
                    }
                    gdata = font_gime_dram[c * 12 + font_row];

                    if (INV ^ gime->inverted_text)
                        gdata = ~gdata;
                    fg_colour = gime->VDG.CSS ? TCC1014_BRIGHT_ORANGE : TCC1014_BRIGHT_GREEN;
                    bg_colour = gime->VDG.CSS ? TCC1014_DARK_ORANGE : TCC1014_DARK_GREEN;
                    render_mode = TCC1014_RENDER_RG;
                }
            }

        } else {
            // CoCo 3 native mode — tcc1014.c:1349-1382
            uint8_t vdata = fetch_byte_vram(gime);
            unsigned font_row = (gime->row + 1) & 0x0F;
            if (font_row > 11) {
                font_row = 0;
            }

            if (gime->BP) {
                // CoCo 3 graphics — tcc1014.c:1357-1363
                gdata = vdata;
                // 16 colour, 16 byte-per-row modes zero the second byte
                if (gime->HRES == 0 && gime->CRES >= 2) {
                    gime->vdata_cache = 0;
                }
            } else {
                // CoCo 3 text — tcc1014.c:1364-1380
                int c = vdata & 0x7F;
                gdata = font_gime_dram[c * 12 + font_row];
                if (gime->CRES & 1) {
                    // Attribute byte mode
                    uint8_t attr = fetch_byte_vram(gime);
                    fg_colour = 8 | ((attr >> 3) & 7);
                    bg_colour = attr & 7;
                    if ((attr & 0x80) && gime->blink)
                        fg_colour = bg_colour;
                    if ((attr & 0x40) && ((font_row & gime->rowmask) == gime->rowmask))
                        gdata = 0xFF;  // Underline
                } else {
                    fg_colour = 1;
                    bg_colour = 0;
                }
            }
            render_mode = TCC1014_RENDER_RG;
        }

        // --- Pixel rendering — tcc1014.c:1384-1503 ---
        // Process 4 bits at a time, twice (high nibble then low nibble)

        for (int i = 2; i > 0; --i) {
            // OPT-C4: emit pre-converted RGB565 directly (byte-swapped for sprite FB).
            uint16_t c0, c1, c2, c3;

            if (gime->COCO) {
                // VDG compatible mode — tcc1014.c:1388-1412
                switch (render_mode) {
                case TCC1014_RENDER_SG: default:
                    c0 = c1 = c2 = c3 = gime->palette_rgb565[(gdata & 0x02) ? fg_colour : bg_colour];
                    gdata <<= 1;
                    break;
                case TCC1014_RENDER_CG:
                    c0 = c1 = gime->palette_rgb565[cg_colours + ((gdata >> 6) & 3)];
                    c2 = c3 = gime->palette_rgb565[cg_colours + ((gdata >> 4) & 3)];
                    gdata <<= 4;
                    break;
                case TCC1014_RENDER_RG:
                    c0 = gime->palette_rgb565[(gdata & 0x80) ? fg_colour : bg_colour];
                    c1 = gime->palette_rgb565[(gdata & 0x40) ? fg_colour : bg_colour];
                    c2 = gime->palette_rgb565[(gdata & 0x20) ? fg_colour : bg_colour];
                    c3 = gime->palette_rgb565[(gdata & 0x10) ? fg_colour : bg_colour];
                    gdata <<= 4;
                    break;
                case TCC1014_RENDER_RG2:
                    c0 = c1 = gime->palette_rgb565[(gdata & 0x40) ? fg_colour : bg_colour];
                    c2 = c3 = gime->palette_rgb565[(gdata & 0x10) ? fg_colour : bg_colour];
                    gdata <<= 4;
                    break;
                }

            } else {
                // CoCo 3 modes — tcc1014.c:1414-1449
                // No composite monochrome on ESP32 (no composite out)
                if (gime->BP) {
                    switch (gime->CRES) {
                    case 0: default:
                        c0 = gime->palette_rgb565[(gdata >> 7) & 1];
                        c1 = gime->palette_rgb565[(gdata >> 6) & 1];
                        c2 = gime->palette_rgb565[(gdata >> 5) & 1];
                        c3 = gime->palette_rgb565[(gdata >> 4) & 1];
                        break;
                    case 1:
                        c0 = c1 = gime->palette_rgb565[(gdata >> 6) & 3];
                        c2 = c3 = gime->palette_rgb565[(gdata >> 4) & 3];
                        break;
                    case 2: case 3:
                        c0 = c1 = c2 = c3 = gime->palette_rgb565[(gdata >> 4) & 15];
                        break;
                    }
                } else {
                    // CoCo3 text — tcc1014.c:1442-1447
                    c0 = gime->palette_rgb565[(gdata & 0x80) ? fg_colour : bg_colour];
                    c1 = gime->palette_rgb565[(gdata & 0x40) ? fg_colour : bg_colour];
                    c2 = gime->palette_rgb565[(gdata & 0x20) ? fg_colour : bg_colour];
                    c3 = gime->palette_rgb565[(gdata & 0x10) ? fg_colour : bg_colour];
                }
                gdata <<= 4;
            }

            // --- Resolution expansion — tcc1014.c:1452-1502 ---
            switch (gime->resolution) {
            case 0:
                // 4x expansion: 4 pixels → 16
                pixel[0]  = pixel[1]  = pixel[2]  = pixel[3]  = c0;
                pixel[4]  = pixel[5]  = pixel[6]  = pixel[7]  = c1;
                pixel[8]  = pixel[9]  = pixel[10] = pixel[11] = c2;
                pixel[12] = pixel[13] = pixel[14] = pixel[15] = c3;
                pixel += 16;
                npixels += 16;
                break;
            case 1:
                // 2x expansion: 4 pixels → 8
                pixel[0] = pixel[1] = c0;
                pixel[2] = pixel[3] = c1;
                pixel[4] = pixel[5] = c2;
                pixel[6] = pixel[7] = c3;
                pixel += 8;
                npixels += 8;
                break;
            case 2:
                // 1:1: 4 pixels → 4
                pixel[0] = c0;
                pixel[1] = c1;
                pixel[2] = c2;
                pixel[3] = c3;
                pixel += 4;
                npixels += 4;
                break;
            case 3:
                // Skip: 4 pixels → 2
                pixel[0] = c0;
                pixel[1] = c2;
                pixel += 2;
                npixels += 2;
                break;
            }

            // Safety: don't overflow line_buffer[640]
            if (npixels >= 640) goto done;
        }
    }

done:
    gime->line_width = (npixels <= 640) ? npixels : 640;
}
#endif  // GIME_RENDER_LEGACY || TCC1014_RENDER_TEST

// ============================================================
// OPT-G3/G4: line-batched fetch + per-mode scanline renderers
//
// Mode dispatch happens once per scanline; each leaf is a tight loop with
// no mode branches (MAME-style). All leaves take __restrict pointers so the
// compiler can keep the palette and state in registers across pixel stores.
//
// The legacy fetch_byte_vram() pair-cache consumes a strictly sequential
// byte stream: byte k of the line is
// ram[vram_base + ((B + ((Xoff0 + k) & 0xFF)) & vram_mask)], where Xoff0 is
// the X offset ($FF9F bits 6-0, doubled; even) in native modes and 0 in COCO
// mode. So the renderers read a plain byte array: a direct pointer into RAM
// when the line wraps neither the 256-byte X window nor the bank, otherwise
// a staged copy.
// ============================================================

// Max bytes consumed per line: 80-col attribute text = 2 x 80.
static uint8_t s_line_bytes[160];

static inline const uint8_t* fetch_line_bytes(const TCC1014* g, unsigned n) {
    const uint32_t mask = g->vram_mask;
    const unsigned x = g->COCO ? 0 : g->X;
    const uint32_t a = (g->B + x) & mask;
    const uint8_t* bank = g->ram + g->vram_base;
    if (x + n <= 256 && a + n <= mask + 1) return bank + a;
    for (unsigned k = 0; k < n; k++) s_line_bytes[k] = bank[(g->B + ((x + k) & 0xFF)) & mask];
    return s_line_bytes;
}

// Resolution expansion — tcc1014.c:1452-1502. Emits one nibble's four
// colours at 16/8/4/2 output pixels.
template<unsigned RES, typename P>
static inline P* emit4(P* __restrict d, P c0, P c1, P c2, P c3) {
    if (RES == 0) {
        d[0]  = d[1]  = d[2]  = d[3]  = c0;
        d[4]  = d[5]  = d[6]  = d[7]  = c1;
        d[8]  = d[9]  = d[10] = d[11] = c2;
        d[12] = d[13] = d[14] = d[15] = c3;
        return d + 16;
    } else if (RES == 1) {
        d[0] = d[1] = c0;
        d[2] = d[3] = c1;
        d[4] = d[5] = c2;
        d[6] = d[7] = c3;
        return d + 8;
    } else if (RES == 2) {
        d[0] = c0; d[1] = c1; d[2] = c2; d[3] = c3;
        return d + 4;
    } else {
        d[0] = c0; d[1] = c2;
        return d + 2;
    }
}

// Two-colour 8-pixel byte (RG, CoCo 3 text): bit 7 first.
template<unsigned RES, typename P>
static inline P* emit_bits8(P* __restrict d, uint8_t g, P fg, P bg) {
    d = emit4<RES, P>(d, (g & 0x80) ? fg : bg, (g & 0x40) ? fg : bg,
                      (g & 0x20) ? fg : bg, (g & 0x10) ? fg : bg);
    return emit4<RES, P>(d, (g & 0x08) ? fg : bg, (g & 0x04) ? fg : bg,
                         (g & 0x02) ? fg : bg, (g & 0x01) ? fg : bg);
}

// CoCo 3 native graphics — tcc1014.c:1357-1363, 1414-1440. CRES 3 == 2.
template<unsigned CRES, unsigned RES, typename P>
static void render_native_gfx(const uint8_t* __restrict src, unsigned n,
                              const P* __restrict pal, P* __restrict d) {
    for (unsigned k = 0; k < n; k++) {
        const uint8_t v = src[k];
        if (CRES == 0) {
            d = emit4<RES, P>(d, pal[(v >> 7) & 1], pal[(v >> 6) & 1],
                              pal[(v >> 5) & 1], pal[(v >> 4) & 1]);
            d = emit4<RES, P>(d, pal[(v >> 3) & 1], pal[(v >> 2) & 1],
                              pal[(v >> 1) & 1], pal[v & 1]);
        } else if (CRES == 1) {
            const P a = pal[(v >> 6) & 3], b = pal[(v >> 4) & 3];
            const P c = pal[(v >> 2) & 3], e = pal[v & 3];
            d = emit4<RES, P>(d, a, a, b, b);
            d = emit4<RES, P>(d, c, c, e, e);
        } else {
            const P a = pal[v >> 4], b = pal[v & 15];
            d = emit4<RES, P>(d, a, a, a, a);
            d = emit4<RES, P>(d, b, b, b, b);
        }
    }
}

// CoCo 3 native text — tcc1014.c:1364-1380, 1442-1447. With ATTR each
// character is followed by an attribute byte (fg/bg/blink/underline).
template<bool ATTR, unsigned RES, typename P>
static void render_native_text(const TCC1014* g, const uint8_t* __restrict src, unsigned n,
                               const P* __restrict pal, P* __restrict d) {
    unsigned font_row = (g->row + 1) & 0x0F;
    if (font_row > 11) font_row = 0;
    const uint8_t* __restrict font = font_gime_dram + font_row;
    const bool blink = g->blink;
    const bool underline = (font_row & g->rowmask) == g->rowmask;
    for (unsigned k = 0; k < n; k++) {
        uint8_t gdata = font[(src[0] & 0x7F) * 12];
        if (ATTR) {
            const uint8_t attr = src[1];
            src += 2;
            unsigned fg = 8 | ((attr >> 3) & 7);
            const unsigned bg = attr & 7;
            if ((attr & 0x80) && blink) fg = bg;
            if ((attr & 0x40) && underline) gdata = 0xFF;
            d = emit_bits8<RES, P>(d, gdata, pal[fg], pal[bg]);
        } else {
            src += 1;
            d = emit_bits8<RES, P>(d, gdata, pal[1], pal[0]);
        }
    }
}

// VDG-compatible graphics (GnA=1) — tcc1014.c:1300-1315, 1388-1412.
// The CG/RG/RG2 choice depends only on line-stable state.
template<unsigned RES, typename P>
static void render_coco_cg(const TCC1014* g, const uint8_t* __restrict src, unsigned n,
                           const P* __restrict pal, P* __restrict d) {
    const P* __restrict cg = pal + (g->VDG.CSS ? TCC1014_WHITE : TCC1014_GREEN);
    for (unsigned k = 0; k < n; k++) {
        const uint8_t v = src[k];
        const P a = cg[(v >> 6) & 3], b = cg[(v >> 4) & 3];
        const P c = cg[(v >> 2) & 3], e = cg[v & 3];
        d = emit4<RES, P>(d, a, a, b, b);
        d = emit4<RES, P>(d, c, c, e, e);
    }
}

template<unsigned RES, typename P>
static void render_coco_rg(const TCC1014* g, const uint8_t* __restrict src, unsigned n,
                           const P* __restrict pal, P* __restrict d) {
    const P fg = pal[g->VDG.CSS ? TCC1014_RGCSS1_1 : TCC1014_RGCSS0_1];
    const P bg = pal[g->VDG.CSS ? TCC1014_RGCSS1_0 : TCC1014_RGCSS0_0];
    for (unsigned k = 0; k < n; k++) d = emit_bits8<RES, P>(d, src[k], fg, bg);
}

// PMODE 4 artifact colours: each pixel pair is one NTSC colour (bits 00/01/
// 10/11 -> black/blue/orange/white), the same table as the CoCo 2 renderer
// (mc6847.cpp). pal4 holds those four colours, not palette-register colours:
// they come from the TV, not the GIME palette.
template<unsigned RES, typename P>
static void render_coco_rg_artifact(const uint8_t* __restrict src, unsigned n,
                                    const P* __restrict pal4, P* __restrict d) {
    for (unsigned k = 0; k < n; k++) {
        const uint8_t v = src[k];
        const P a = pal4[v >> 6], b = pal4[(v >> 4) & 3];
        const P c = pal4[(v >> 2) & 3], e = pal4[v & 3];
        d = emit4<RES, P>(d, a, a, b, b);
        d = emit4<RES, P>(d, c, c, e, e);
    }
}

template<unsigned RES, typename P>
static void render_coco_rg2(const TCC1014* g, const uint8_t* __restrict src, unsigned n,
                            const P* __restrict pal, P* __restrict d) {
    const P fg = pal[g->VDG.CSS ? TCC1014_RGCSS1_1 : TCC1014_RGCSS0_1];
    const P bg = pal[g->VDG.CSS ? TCC1014_RGCSS1_0 : TCC1014_RGCSS0_0];
    for (unsigned k = 0; k < n; k++) {
        const uint8_t v = src[k];
        const P a = (v & 0x40) ? fg : bg, b = (v & 0x10) ? fg : bg;
        const P c = (v & 0x04) ? fg : bg, e = (v & 0x01) ? fg : bg;
        d = emit4<RES, P>(d, a, a, b, b);
        d = emit4<RES, P>(d, c, c, e, e);
    }
}

// VDG-compatible alphanumerics + semigraphics (GnA=0) — tcc1014.c:1317-1345.
// SnA is per byte (data-dependent), so it stays in the loop.
template<unsigned RES, typename P>
static void render_coco_text(const TCC1014* g, const uint8_t* __restrict src, unsigned n,
                             const P* __restrict pal, P* __restrict d) {
    const unsigned font_row = g->row & 0x0F;
    const uint8_t* __restrict font = font_gime_dram + font_row;
    const bool GM0 = g->VDG.GM0;
    const bool inv_base = g->VDG.GM1 ^ g->inverted_text;
    const P fg = pal[g->VDG.CSS ? TCC1014_BRIGHT_ORANGE : TCC1014_BRIGHT_GREEN];
    const P bg = pal[g->VDG.CSS ? TCC1014_DARK_ORANGE : TCC1014_DARK_GREEN];
    const P sg_bg = pal[TCC1014_RGCSS0_0];
    for (unsigned k = 0; k < n; k++) {
        const uint8_t v = src[k];
        if (v & 0x80) {
            // Semigraphics: one colour per nibble
            const uint8_t gd = (font_row < 6) ? (v >> 2) : v;
            const P sfg = pal[(v >> 4) & 7];
            const P a = (gd & 0x02) ? sfg : sg_bg;
            const P b = (gd & 0x01) ? sfg : sg_bg;
            d = emit4<RES, P>(d, a, a, a, a);
            d = emit4<RES, P>(d, b, b, b, b);
        } else {
            bool inv = (v & 0x40) != 0;
            unsigned c = v & 0x7F;
            if (c < 0x20) {
                c |= GM0 ? 0x60 : 0x40;
                inv ^= GM0;
            } else if (c >= 0x60) {
                c ^= 0x40;
            }
            uint8_t gd = font[c * 12];
            if (inv ^ inv_base) gd = ~gd;
            d = emit_bits8<RES, P>(d, gd, fg, bg);
        }
    }
}

// Instantiate the renderer call for the line's resolution: the call names the
// resolution as R, e.g. GIME_DISPATCH_RES(res, render_coco_rg<R>(...)).
#define GIME_DISPATCH_RES(res, ...) \
    switch (res) { \
    case 0:  { constexpr unsigned R = 0; __VA_ARGS__; } break; \
    case 1:  { constexpr unsigned R = 1; __VA_ARGS__; } break; \
    case 2:  { constexpr unsigned R = 2; __VA_ARGS__; } break; \
    default: { constexpr unsigned R = 3; __VA_ARGS__; } break; }

static bool s_artifacts = false;

// True when this line is PMODE 4 (RG6: GM2..0 = 111 at the 256-pixel
// geometry) with CSS=1 and artifacts are enabled.
static inline bool artifact_line(const TCC1014* g) {
    return s_artifacts && g->COCO && g->VDG.GnA && g->VDG.CSS &&
           (g->resolution & 3) == 1 && (g->PIA1B_shadow.pdr & 0x70) == 0x70;
}

// GIME colours closest to the CoCo 2 artifact colours (RGB222 0,0,2 and 3,1,0).
static const uint8_t k_artifact_gime[4] = { 0x00, 0x08, 0x26, 0x3F };

template<typename P>
static void render_line(TCC1014* gime, const uint8_t* src, const P* __restrict pal,
                        P* __restrict d, unsigned n, const P* __restrict pal4) {
    const unsigned res = gime->resolution & 3;
    if (gime->COCO) {
        if (gime->VDG.GnA) {
            if (!gime->VDG.GM0) {
                GIME_DISPATCH_RES(res, render_coco_cg<R, P>(gime, src, n, pal, d));
            } else if (artifact_line(gime)) {
                render_coco_rg_artifact<1, P>(src, n, pal4, d);
            } else if (gime->resolution || (gime->PIA1B_shadow.pdr & 0x70) == 0x70) {
                GIME_DISPATCH_RES(res, render_coco_rg<R, P>(gime, src, n, pal, d));
            } else {
                GIME_DISPATCH_RES(res, render_coco_rg2<R, P>(gime, src, n, pal, d));
            }
        } else {
            GIME_DISPATCH_RES(res, render_coco_text<R, P>(gime, src, n, pal, d));
        }
    } else if (gime->BP) {
        if (gime->HRES == 0 && gime->CRES >= 2) {
            // 16-colour, 16-byte-per-row mode zeroes every second byte
            // (legacy: vdata_cache = 0, tcc1014.c:1360-1362).
            for (unsigned k = 0; k < n; k++) s_line_bytes[k] = (k & 1) ? 0 : src[k];
            src = s_line_bytes;
        }
        switch (gime->CRES) {
        case 0:  GIME_DISPATCH_RES(res, render_native_gfx<0, R, P>(src, n, pal, d)); break;
        case 1:  GIME_DISPATCH_RES(res, render_native_gfx<1, R, P>(src, n, pal, d)); break;
        default: GIME_DISPATCH_RES(res, render_native_gfx<2, R, P>(src, n, pal, d)); break;
        }
    } else {
        if (gime->CRES & 1) {
            GIME_DISPATCH_RES(res, render_native_text<true, R, P>(gime, src, n, pal, d));
        } else {
            GIME_DISPATCH_RES(res, render_native_text<false, R, P>(gime, src, n, pal, d));
        }
    }
}

// ============================================================
// OPT-G5: dirty-line skip
//
// A display line is skipped when everything that determines its pixels is
// unchanged since the previous frame. Rather than tracking writes, each
// line keeps a 64-bit signature of (a) the VRAM bytes it consumes and
// (b) every piece of state the renderers and the HAL read for it. That is
// self-invalidating for register, palette, blink, scroll and VRAM changes;
// only drawing done *outside* this renderer needs tcc1014_invalidate_lines().
// ============================================================

#ifndef GIME_LINE_SKIP
#define GIME_LINE_SKIP 1
#endif

#define GIME_MAX_LINES 225
static uint64_t s_line_sig[GIME_MAX_LINES];   // 0 = no valid signature

void tcc1014_invalidate_lines(void) {
    memset(s_line_sig, 0, sizeof(s_line_sig));
}

void tcc1014_set_artifacts(bool on) {
    if (s_artifacts == on) return;
    s_artifacts = on;
    tcc1014_invalidate_lines();
}

bool tcc1014_get_artifacts(void) {
    return s_artifacts;
}

static inline uint32_t rotl32(uint32_t v, unsigned r) { return (v << r) | (v >> (32 - r)); }

// Two independent 32-bit multiply-rotate lanes (the ESP32 has no cheap
// 64-bit multiply).
#define SIG_MIX(w) do { const uint32_t w_ = (w); \
    h1 = rotl32(h1 ^ w_, 5) * 0x9E3779B1u; \
    h2 = rotl32(h2 + w_, 11) * 0xC2B2AE35u; } while (0)

static uint64_t line_signature(const TCC1014* g, const uint8_t* src, unsigned nbytes,
                               unsigned n, bool attr) {
    uint32_t h1 = 0x811C9DC5u, h2 = 0x01000193u;

    // Mode + per-line state. blink only matters where it is rendered
    // (attribute text), so a free-running GIME timer doesn't defeat the skip.
    SIG_MIX((uint32_t)g->COCO | (uint32_t)g->BP << 1 | (g->CRES & 3) << 2 |
            (g->HRES & 7) << 4 | (g->resolution & 3) << 7 |
            (uint32_t)g->VDG.GnA << 9 | (uint32_t)g->VDG.GM1 << 10 |
            (uint32_t)g->VDG.GM0 << 11 | (uint32_t)g->VDG.CSS << 12 |
            (uint32_t)g->inverted_text << 13 | (uint32_t)(attr && g->blink) << 14 |
            (uint32_t)((g->PIA1B_shadow.pdr & 0x70) == 0x70) << 15 |
            (g->row & 15) << 16 | (g->rowmask & 31) << 20 |
            (uint32_t)(g->border_colour & 0x3F) << 25 |
            (uint32_t)s_artifacts << 31);
    SIG_MIX(n | g->vertical.lAA << 8);
    uint32_t pal[4];
    memcpy(pal, g->palette_reg, sizeof(pal));
    SIG_MIX(pal[0]); SIG_MIX(pal[1]); SIG_MIX(pal[2]); SIG_MIX(pal[3]);

    // VRAM bytes (nbytes is always a multiple of 4)
    if (((uintptr_t)src & 3) == 0) {
        const uint8_t* p = (const uint8_t*)__builtin_assume_aligned(src, 4);
        for (unsigned k = 0; k < nbytes; k += 4) {
            uint32_t w;
            memcpy(&w, p + k, 4);
            SIG_MIX(w);
        }
    } else {
        for (unsigned k = 0; k < nbytes; k += 4)
            SIG_MIX(src[k] | src[k + 1] << 8 | src[k + 2] << 16 | (uint32_t)src[k + 3] << 24);
    }
    h1 ^= h1 >> 15; h2 ^= h2 >> 13;
    const uint64_t sig = (uint64_t)h1 << 32 | h2;
    return sig ? sig : 1;
}

// OPT-G6: display-provided GIME colour (6-bit) → raw pixel byte table.
static const uint8_t* s_raw_lut = nullptr;

void tcc1014_set_raw_lut(const uint8_t* lut64) {
    s_raw_lut = lut64;
}

void tcc1014_render_scanline(TCC1014* gime, unsigned scanline) {
    // scanline = display line within the active area; it only indexes the
    // dirty-line signatures (the renderers use gime->row / gime->B).

    if (!gime->vertical.active_area || !gime->ram)
        return;

    gime->line_clean = false;

#if GIME_RENDER_LEGACY
    gime->raw_output = false;
    tcc1014_render_scanline_legacy(gime);
    return;
#endif

    // Output pixels per source byte is 32 >> resolution; the legacy loop
    // stops at 640 pixels, which always falls on a byte boundary.
    const unsigned res = gime->resolution & 3;
    unsigned n = gime->BPR;
    const unsigned max_n = 640u / (32u >> res);
    if (n > max_n) n = max_n;

    const bool attr = !gime->COCO && !gime->BP && (gime->CRES & 1);
    const unsigned nbytes = attr ? 2 * n : n;
    const uint8_t* src = fetch_line_bytes(gime, nbytes);
    gime->line_width = n * (32u >> res);

#if GIME_LINE_SKIP
    if (scanline < GIME_MAX_LINES) {
        const uint64_t sig = line_signature(gime, src, nbytes, n, attr);
        // Only skip when the caller wants display output (raw); a screenshot
        // frame (RGB565) must produce every line.
        if (gime->raw_output && s_raw_lut && sig == s_line_sig[scanline]) {
            gime->line_clean = true;
            return;
        }
        s_line_sig[scanline] = sig;
    }
#endif

    if (gime->raw_output && s_raw_lut) {
        // Resolve the 16 palette slots to raw bytes once per line; mid-frame
        // palette writes still take effect on the next scanline.
        uint8_t pal8[16];
        for (int i = 0; i < 16; i++) pal8[i] = s_raw_lut[gime->palette_reg[i] & 0x3F];
        uint8_t art8[4];
        for (int i = 0; i < 4; i++) art8[i] = s_raw_lut[k_artifact_gime[i]];
        render_line<uint8_t>(gime, src, pal8, gime->line_raw, n, art8);
    } else {
        gime->raw_output = false;
        uint16_t art565[4];   // byte-swapped, like palette_rgb565
        for (int i = 0; i < 4; i++)
            art565[i] = __builtin_bswap16(gime_rgb565_lut[k_artifact_gime[i]]);
        render_line<uint16_t>(gime, src, gime->palette_rgb565, gime->line_buffer, n, art565);
    }
}
