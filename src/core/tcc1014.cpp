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

#include "tcc1014.h"
#include "font_gime.h"
#include <string.h>

// ============================================================
// Lookup tables — from tcc1014.c:406-424
// ============================================================

// OPT-C3: DRAM mirror of the GIME font ROM. Populated once in tcc1014_init()
// from font_gime_flash[] (PROGMEM). Per-read access avoids the ~15ns
// pgm_read_byte() overhead in the scanline text renderer.
static uint8_t font_gime_dram[1536];

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

void tcc1014_update_active_banks(TCC1014* gime) {
    for (int i = 0; i < 8; i++) {
        gime->active_banks[i] = gime->MMUEN ? gime->mmu_bank[gime->TR | i]
                                             : (0x38 | i);
    }
}

// ============================================================
// Init / Reset — port of tcc1014_allocate (line 551) and tcc1014_reset (line 661)
// ============================================================

void tcc1014_init(TCC1014* gime) {
    memset(gime, 0, sizeof(TCC1014));
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
    gime->irq_state = 0;
    gime->firq_state = 0;
    gime->IRQ = false;
    gime->FIRQ = false;
    gime->IL0 = false;
    gime->IL1 = false;
    gime->IL2 = false;
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
        if (!gime->TY && bank >= 0x3C) {
            if (!gime->MC1) {
                gime->S = (bank >= 0x3E) ? 1 : 0;
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
                    // Read-acknowledge IRQ — port of tcc1014.c:753-758
                    *read_data = gime->irq_state;
                    gime->irq_state = 0;
                    gime->IRQ = false;
                    if (gime->timer_counter == 0) {
                        set_interrupt(gime, TCC1014_INT_TMR);
                    }
                } else if (addr == 0xFF93) {
                    // Read-acknowledge FIRQ — port of tcc1014.c:759-764
                    *read_data = gime->firq_state;
                    gime->firq_state = 0;
                    gime->FIRQ = false;
                    if (gime->timer_counter == 0) {
                        set_interrupt(gime, TCC1014_INT_TMR);
                    }
                } else {
                    *read_data = 0xFF;  // Other GIME regs are write-only
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

    // --- External interrupt processing — port of tcc1014.c:799-802 ---
    // Process IL0/IL1 on every memory cycle (IL2 omitted for now)
    unsigned ext_int = 0;
    if (gime->IL0) ext_int |= TCC1014_INT_EI0;
    if (gime->IL1) ext_int |= TCC1014_INT_EI1;
    if (gime->IL2) ext_int |= TCC1014_INT_EI2;
    set_interrupt(gime, ext_int);
}

// ============================================================
// Register writes — port of tcc1014_set_register() lines 895-996
// ============================================================

void tcc1014_write_register(TCC1014* gime, unsigned reg, uint8_t val) {
    reg &= 0x0F;
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
        tcc1014_update_active_banks(gime);
        update_from_gime_registers(gime);
        break;

    // INIT1 ($FF91) — tcc1014.c:911-916
    case 1:
        gime->TINS = val & 0x20;
        gime->TR   = (val & 0x01) ? 8 : 0;
        tcc1014_update_active_banks(gime);
        break;

    // IRQ enable ($FF92) — tcc1014.c:919-923
    // Writing clears the specified interrupt bits
    case 2:
        gime->irq_state &= ~val;
        gime->IRQ &= ~val;
        break;

    // FIRQ enable ($FF93) — tcc1014.c:926-928
    case 3:
        gime->firq_state &= ~val;
        gime->FIRQ &= ~val;
        break;

    // Timer MSB ($FF94) — tcc1014.c:931-940
    case 4:
        {
            unsigned timer_reset = ((gime->registers[4] & 0x0F) << 8) | gime->registers[5];
            gime->timer_counter = timer_reset ? (timer_reset + TIMER_OFFSET) : 0;
            if (gime->timer_counter == 0) {
                set_interrupt(gime, TCC1014_INT_TMR);
            }
        }
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

    // $FF9B: Disto bank select (unused on ESP32)
    case 0x0B:
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
        // Read-acknowledge IRQ status — port of tcc1014.c:753-758
        uint8_t val = gime->irq_state;
        gime->irq_state = 0;
        gime->IRQ = false;  // Deassert IRQ output immediately
        // Re-assert TMR if timer already expired (XRoar tcc1014.c:756-758)
        if (gime->timer_counter == 0) {
            set_interrupt(gime, TCC1014_INT_TMR);
        }
        return val;
    }
    if (reg == 3) {
        // Read-acknowledge FIRQ status — port of tcc1014.c:759-764
        uint8_t val = gime->firq_state;
        gime->firq_state = 0;
        gime->FIRQ = false;  // Deassert FIRQ output immediately
        // Re-assert TMR if timer already expired
        if (gime->timer_counter == 0) {
            set_interrupt(gime, TCC1014_INT_TMR);
        }
        return val;
    }
    return 0xFF;
}

// ============================================================
// MMU bank writes/reads — port of tcc1014.c:768-773
// ============================================================

void tcc1014_write_mmu(TCC1014* gime, uint8_t offset, uint8_t val) {
    gime->mmu_bank[offset & 0x0F] = val & 0x3F;
    tcc1014_update_active_banks(gime);
}

uint8_t tcc1014_read_mmu(TCC1014* gime, uint8_t offset) {
    return gime->mmu_bank[offset & 0x0F] & 0x3F;
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
//   TINS=0: Decrement at scanline rate (~15.7 kHz)
//   TINS=1: Decrement at 3.58MHz/8 ≈ 447kHz (~28 ticks/scanline)
// ============================================================

void tcc1014_tick_scanline(TCC1014* gime) {
    if (gime->timer_counter <= 0) return;

    if (!gime->TINS) {
        // TINS=0: line-rate timer — 1 tick per scanline
        // Port of tcc1014.c:1017-1023
        gime->timer_counter--;
        if (gime->timer_counter <= 0) {
            // Timer expired — port of update_timer, tcc1014.c:1534-1546
            gime->blink = !gime->blink;
            unsigned timer_reset = ((gime->registers[4] & 0x0F) << 8) | gime->registers[5];
            gime->timer_counter = timer_reset ? (timer_reset + TIMER_OFFSET) : 0;
            set_interrupt(gime, TCC1014_INT_TMR);
        }
    } else {
        // TINS=1: fast timer ~447kHz ≈ 28 ticks per scanline
        // (3,579,545 Hz / 8 = 447,443 Hz) / (15,734 Hz) ≈ 28.4
        gime->timer_counter -= 28;
        if (gime->timer_counter <= 0) {
            gime->blink = !gime->blink;
            unsigned timer_reset = ((gime->registers[4] & 0x0F) << 8) | gime->registers[5];
            gime->timer_counter = timer_reset ? (timer_reset + TIMER_OFFSET) : 0;
            set_interrupt(gime, TCC1014_INT_TMR);
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
        addr &= (g->ram_size - 1);  // Wrap to physical RAM
        r = g->ram[addr];
        g->vdata_cache = g->ram[(addr + 1) & (g->ram_size - 1)];
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

void tcc1014_render_scanline(TCC1014* gime, unsigned scanline) {
    (void)scanline;  // scanline number not used — we use gime->row

    if (!gime->vertical.active_area || !gime->ram)
        return;

    uint16_t* pixel = gime->line_buffer;   // OPT-C4: RGB565 (byte-swapped)
    unsigned npixels = 0;

    // Reset horizontal offset for this scanline
    gime->Xoff = 0;
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
