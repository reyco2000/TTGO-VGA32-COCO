#pragma GCC optimize("O2")
/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : machine.cpp
 *  Module : Machine integration — wires CPU, PIAs, and video/memory subsystem
 * ============================================================
*/

#include "machine.h"
#include "../hal/hal.h"
#include "../utils/debug.h"
#include "../utils/perf_probe.h"
#include "mc6551.h"   // RS-232 Pak (Deluxe RS-232 Program Pak) ACIA
#include "sound.h"    // Sound mixing core (mux/DAC/single-bit)

// Global machine pointer for CPU memory callbacks
static Machine* g_machine = nullptr;

// ============================================================
// Sound wiring — forward PIA state to the sound core (sound.cpp)
// Shared by the CoCo 2 and CoCo 3 write paths.
// ============================================================

static void sound_pia0_written(Machine* m, uint8_t reg) {
    // SEL1 = PIA0 CA2 (CRA bit 3), SEL2 = PIA0 CB2 (CRB bit 3)
    if (reg == PIA_REG_CRA || reg == PIA_REG_CRB) {
        sound_set_mux_source(((m->pia0.ctrl_b & 0x08) >> 2)
                           | ((m->pia0.ctrl_a & 0x08) >> 3));
    }
}

static void sound_pia1_written(Machine* m, uint8_t reg) {
    // 6-bit DAC: PIA1 PA bits 2-7 (reg 0 also covers DDR A writes)
    if (reg == PIA_REG_DA || reg == PIA_REG_CRA) {
        uint8_t pa = m->pia1.data_a & m->pia1.ddr_a;
        sound_set_dac_level((pa >> 2) & 0x3F);
    }
    if (reg == PIA_REG_DB || reg == PIA_REG_CRB) {
        // Single-bit sound: PB1 interacts with the mix only when the pin
        // is configured as an output; as an input it has no effect.
        sound_set_sbs((m->pia1.ddr_b & 0x02) != 0,
                      (m->pia1.data_b & 0x02) != 0);
        if (reg == PIA_REG_CRB)
            sound_set_mux_enabled((m->pia1.ctrl_b & 0x08) != 0);  // SNDEN
    }
}

// Runtime-active machine type. Set from NVS before machine_init() in the main
// sketch; defaults to the compile-time MACHINE_TYPE when NVS has no entry.
// Not yet branched on in core/HAL — that comes in later steps of coco2and3.md.
uint8_t g_machine_type = MACHINE_TYPE;

// Cycles per scanline: CPU_CLOCK_HZ / TARGET_FPS / SCANLINES_PER_FRAME
// 895000 / 60 / 262 ≈ 56.9 → use fixed-point for accuracy
static const int CYCLES_PER_SCANLINE_X4 = (CPU_CLOCK_HZ * 4) / (TARGET_FPS * SCANLINES_PER_FRAME);

// Precomputed per-scanline cycle targets (avoids multiply+divide per scanline)
static uint16_t scanline_cycle_targets[SCANLINES_PER_FRAME];

// ============================================================
// Memory allocation helper
// ============================================================

static uint8_t* machine_alloc(size_t size, const char* label) {
    uint8_t* ptr = nullptr;

#if USE_PSRAM
    if (ESP.getPsramSize() > 0) {
        ptr = (uint8_t*)ps_malloc(size);
        if (ptr) {
            DEBUG_PRINTF("  %s: %d bytes from PSRAM", label, (int)size);
            return ptr;
        }
        DEBUG_PRINTF("  %s: PSRAM alloc failed, falling back to heap", label);
    }
#endif

    ptr = (uint8_t*)malloc(size);
    if (ptr) {
        DEBUG_PRINTF("  %s: %d bytes from heap", label, (int)size);
    } else {
        Serial.printf("FATAL: %s allocation failed (%d bytes)!\n", label, (int)size);
    }
    return ptr;
}

// ############################################################
// ##  CoCo 3 — GIME-based machine (MACHINE_TYPE == 4)
// ############################################################


// ============================================================
// IRQ routing — CoCo 3
//
// Port of coco3.c:1255-1277 interrupt routing:
//   IRQ  = PIA0 IRQA | PIA0 IRQB | GIME IRQ
//   FIRQ = PIA1 IRQA | PIA1 IRQB | GIME FIRQ
//
// Unlike CoCo 2 where PIA0→IRQ and PIA1→FIRQ directly,
// CoCo 3 ORs GIME interrupt outputs into the routing.
// PIA callbacks still fire individually but the combined
// check happens each scanline in machine_run_scanline_coco3.
// ============================================================

static void pia0_irq_a_coco3(bool active) {
    (void)active;
    // Combined routing done in machine_run_scanline_coco3
}
static void pia0_irq_b_coco3(bool active) {
    (void)active;
}
static void pia1_irq_a_coco3(bool active) {
    (void)active;
}
static void pia1_irq_b_coco3(bool active) {
    (void)active;
}

// ============================================================
// Phase 5: MMU fast-path read
//
// For addresses $0000-$FDFF (vast majority of reads), the GIME
// address decode simplifies to: bank = active_banks[addr>>13],
// Z = (bank<<13) | (addr & 0x1FFF). If the bank maps to RAM
// (TY=1, or bank < 0x3C), we can read directly without calling
// tcc1014_mem_cycle().
//
// This avoids the full decode (register checks, PIA snoop,
// interrupt processing) on the hot path.
// ============================================================

// ============================================================
// Memory read — CoCo 3
// Port of coco3.c:read_byte() lines 1104-1168
// Phase 5: fast-path for addr < 0xFE00 that maps to RAM
// ============================================================

uint8_t machine_read_coco3(uint16_t addr) {
    Machine* m = g_machine;
    if (!m) return 0xFF;

    TCC1014* g = &m->gime;

    // Phase 5: Fast path — most reads are RAM below $FE00
    if (__builtin_expect(addr < 0xFE00, 1)) {
        unsigned bank = g->active_banks[addr >> 13];
        if (g->TY || bank < 0x3C) {
            uint32_t Z = ((uint32_t)bank << 13) | (addr & 0x1FFF);
            if (__builtin_expect(Z < COCO3_PHYSICAL_RAM, 1))
                return m->ram_physical[Z];
            return 0xFF;
        }
        // bank >= 0x3C, TY=0: ROM area — need full decode for ROM/CTS
        // Fall through to slow path
    }

    // RS-232 Pak ($FF68-$FF6B) — decoded ahead of the GIME so the ACIA
    // responds regardless of chip-select. No-op (zero behavior change) unless
    // the pak owns the serial port.
    if (rs232_pak_enabled() && (addr & 0xFFFC) == 0xFF68)
        return mc6551_read(addr & 0x03);

    // Slow path: GIME address decode for I/O, ROM, registers
    uint8_t reg_data = 0xFF;
    bool is_reg = false;
    tcc1014_mem_cycle(g, addr, /*RnW=*/true, 0, &reg_data, &is_reg);

    // If GIME handled this internally (register/MMU/palette read)
    if (is_reg) return reg_data;

    // RAM access
    if (g->RAS) {
        if (g->Z < COCO3_PHYSICAL_RAM)
            return m->ram_physical[g->Z];
        return 0xFF;
    }

    switch (g->S) {
    case 0: // ROM — use CPU address, NOT GIME Z (which depends on MMU banks)
        // XRoar: rombank_d8(ROM0, A, &D) — uses address A directly.
        // CoCo 3 ROM is 32KB mapped at $8000-$FFFF, so offset = addr & $7FFF.
        // This is critical: when OS-9 remaps MMU banks, Z no longer points
        // to ROM space, but vectors ($FFE0-$FFFF) must ALWAYS read from ROM.
        if (m->rom_coco3_loaded) {
            uint32_t rom_off = addr & 0x7FFF;  // $8000-$FFFF → $0000-$7FFF
            if (rom_off < COCO3_ROM_SIZE)
                return m->rom_coco3[rom_off];
        }
        return 0xFF;

    case 1: // CTS (cartridge) — use CPU address, not GIME Z
        // Disk BASIC ROM is 8KB mapped at $C000-$DFFF.
        if (m->rom_disk_loaded && addr >= 0xC000 && addr < 0xE000) {
            return m->rom_disk[addr - 0xC000];
        }
        // Fall back to internal ROM for other CTS ranges ($8000-$BFFF, $E000-$FEFF)
        if (m->rom_coco3_loaded) {
            uint32_t rom_off = addr & 0x7FFF;
            if (rom_off < COCO3_ROM_SIZE)
                return m->rom_coco3[rom_off];
        }
        return 0xFF;

    case 2: // PIA I/O — port of coco3.c:1120-1140
        if ((addr & 0x20) == 0) {
            uint8_t reg = addr & 0x03;
            // PIA0 port A read: scan keyboard + joystick
            if (reg == PIA_REG_DA && (m->pia0.ctrl_a & PIA_CR_DDR_SEL)) {
                uint8_t col_select = m->pia0.data_b & m->pia0.ddr_b;
                uint8_t row_data = 0xFF;
                for (int col = 0; col < 8; col++) {
                    if (!(col_select & (1 << col)))
                        row_data &= hal_keyboard_scan(col);
                }
                // Joystick buttons
                if (hal_joystick_read_button(0)) row_data &= ~0x01;
                if (hal_joystick_read_button(1)) row_data &= ~0x02;
                // Joystick comparator on PA7
                {
                    static uint32_t last_joy_scanline = UINT32_MAX;
                    uint32_t joy_slot = m->scanline >> 4;
                    if (joy_slot != last_joy_scanline) {
                        last_joy_scanline = joy_slot;
                        hal_joystick_update();
                    }
                    int joy_port = (m->pia0.ctrl_b & 0x08) >> 3;
                    int joy_axis = (m->pia0.ctrl_a & 0x08) >> 3;
                    int dac_value = ((m->pia1.data_a & m->pia1.ddr_a) & 0xFC) + 2;
                    int js_value = hal_joystick_read_axis(joy_port, joy_axis) * 4 + 2;
                    if (js_value >= dac_value)
                        row_data |= 0x80;
                    else
                        row_data &= ~0x80;
                }
                // GIME IL1: keyboard interrupt — port of coco3.c:1359
                g->IL1 = ((row_data | 0x80) != 0xFF);
                mc6821_set_input_a(&m->pia0, row_data);
            }
            return mc6821_read(&m->pia0, reg);
        } else {
            return mc6821_read(&m->pia1, addr & 0x03);
        }

    case 6: // SCS (FDC)
        return sv_disk_read(&m->fdc, addr);

    default:
        return 0xFF;
    }
}

// ============================================================
// Memory write — CoCo 3
// Port of coco3.c:write_byte() lines 1170-1240
// ============================================================

void machine_write_coco3(uint16_t addr, uint8_t val) {
    Machine* m = g_machine;
    if (!m) return;

    TCC1014* g = &m->gime;

    // Phase 5: Fast path for RAM writes below $FE00
    // IMPORTANT: On CoCo3, writes ALWAYS go to underlying RAM, even when
    // ROM is mapped for reads (bank >= 0x3C, TY=0). The ROM overlay only
    // affects reads. This is critical for the boot sequence which writes
    // to RAM underneath ROM before switching to all-RAM mode (TY=1).
    if (__builtin_expect(addr < 0xFE00, 1)) {
        unsigned bank = g->active_banks[addr >> 13];
        uint32_t Z = ((uint32_t)bank << 13) | (addr & 0x1FFF);
        if (__builtin_expect(Z < COCO3_PHYSICAL_RAM, 1)) {
            m->ram_physical[Z] = val;
            g->dirty_frame = true;
        }
        return;
    }

    // RS-232 Pak ($FF68-$FF6B) — see machine_read_coco3.
    if (rs232_pak_enabled() && (addr & 0xFFFC) == 0xFF68) {
        mc6551_write(addr & 0x03, val);
        return;
    }

    // Slow path: GIME address decode for I/O, registers
    bool is_reg = false;
    tcc1014_mem_cycle(g, addr, /*RnW=*/false, val, nullptr, &is_reg);

    // If GIME handled this internally
    if (is_reg) return;

    // RAM access (from slow path — $FE00+ with MC3, or other RAS paths)
    if (g->RAS) {
        if (g->Z < COCO3_PHYSICAL_RAM) {
            m->ram_physical[g->Z] = val;
            g->dirty_frame = true;
        }
        return;
    }

    switch (g->S) {
    case 2: // PIA I/O — port of coco3.c:1190-1216
        if ((addr & 0x20) == 0) {
            mc6821_write(&m->pia0, addr & 0x03, val);
            sound_pia0_written(m, addr & 0x03);
        } else {
            uint8_t reg = addr & 0x03;
            mc6821_write(&m->pia1, reg, val);
            // PIA1B snooping for VDG compat — handled inside tcc1014_mem_cycle
            // but also handle audio here (same as CoCo 2)
            sound_pia1_written(m, reg);
        }
        break;

    case 6: // SCS (FDC)
        sv_disk_write(&m->fdc, addr, val);
        break;
    }
}

// ============================================================
// Init — CoCo 3
// ============================================================

void machine_init_coco3(Machine* m) {
    DEBUG_PRINT("Machine: initializing CoCo 3...");
    memset(m, 0, sizeof(Machine));
    g_machine = m;

    // Allocate 512KB physical RAM
    m->ram_size = COCO3_PHYSICAL_RAM;
    m->ram_physical = machine_alloc(m->ram_size, "RAM-512K");
    if (!m->ram_physical) return;
    // Alias m->ram to the first 64KB so the CoCo 2 code path (not running
    // on this boot, but present in the binary) sees a valid buffer.
    m->ram = m->ram_physical;

    // Allocate 32KB ROM buffer
    m->rom_coco3 = machine_alloc(COCO3_ROM_SIZE, "ROM-CoCo3");
    if (!m->rom_coco3) return;
    memset(m->rom_coco3, 0xFF, COCO3_ROM_SIZE);

    // Allocate 8KB Disk BASIC ROM buffer
    m->rom_disk = machine_alloc(8192, "ROM-Disk");
    if (!m->rom_disk) return;
    memset(m->rom_disk, 0xFF, 8192);

    // CoCo 2 ROM buffers (Step 5 of coco2and3.md: always allocate so
    // machine_load_roms can populate both ROM sets on boot).
    m->rom_basic = machine_alloc(8192, "ROM-BASIC");
    if (!m->rom_basic) return;
    m->rom_extbas = machine_alloc(8192, "ROM-ExtBAS");
    if (!m->rom_extbas) return;
    m->rom_cart = machine_alloc(16384, "ROM-Cart");
    if (!m->rom_cart) return;
    memset(m->rom_basic, 0xFF, 8192);
    memset(m->rom_extbas, 0xFF, 8192);
    memset(m->rom_cart, 0xFF, 16384);

    // Initialize GIME
    tcc1014_init(&m->gime);

    // Initialize CPU
    mc6809_init(&m->cpu);
    m->cpu.read = machine_read_coco3;
    m->cpu.write = machine_write_coco3;

    // Initialize PIAs
    mc6821_init(&m->pia0);
    mc6821_init(&m->pia1);
    m->pia0.irq_a_callback = pia0_irq_a_coco3;
    m->pia0.irq_b_callback = pia0_irq_b_coco3;
    m->pia1.irq_a_callback = pia1_irq_a_coco3;
    m->pia1.irq_b_callback = pia1_irq_b_coco3;

    // Timing
    m->ntsc = true;
    m->cycles_per_frame = CYCLES_PER_FRAME;
    m->cart_inserted = false;

    m->initialized = true;
    DEBUG_PRINTF("Machine: CoCo 3 init complete. Free heap: %d", ESP.getFreeHeap());
}

// ============================================================
// Load ROMs — CoCo 3
// ============================================================

bool machine_load_roms_coco3(Machine* m, const char* rom_path) {
    if (!m->initialized) return false;

    DEBUG_PRINT("Machine: loading CoCo 3 ROM...");
    char path[64];

    // CoCo 3 ROM (32KB) — Super Extended Color BASIC
    snprintf(path, sizeof(path), "%s/%s", rom_path, ROM_COCO3_FILE);
    if (hal_storage_load_file(path, m->rom_coco3, COCO3_ROM_SIZE)) {
        m->rom_coco3_loaded = true;
        DEBUG_PRINTF("  Loaded %s (32KB)", ROM_COCO3_FILE);

        // Verify reset vector
        uint16_t reset_vec = ((uint16_t)m->rom_coco3[0x7FFE] << 8) | m->rom_coco3[0x7FFF];
        DEBUG_PRINTF("  Reset vector: $%04X", reset_vec);
    } else {
        DEBUG_PRINTF("  MISSING: %s", path);
    }

    // Disk BASIC ROM (8KB) — external cartridge at $C000-$DFFF
    // The CoCo3 checks for 'DK' signature at $C000 to detect Disk BASIC
    snprintf(path, sizeof(path), "%s/%s", rom_path, ROM_DISK_FILE);
    if (hal_storage_load_file(path, m->rom_disk, 8192)) {
        m->rom_disk_loaded = true;
        DEBUG_PRINTF("  Loaded %s (Disk BASIC 8KB)", ROM_DISK_FILE);
    } else {
        DEBUG_PRINTF("  Optional: %s not found (no Disk BASIC)", path);
    }

    return m->rom_coco3_loaded;
}

// ============================================================
// Reset — CoCo 3
// Port of coco3.c:coco3_reset() lines 839-856
// ============================================================

void machine_reset_coco3(Machine* m) {
    if (!m->initialized) return;

    DEBUG_PRINT("Machine: CoCo 3 RESET");

    // Clear RAM (CoCo 3 clears to 0 on hard reset)
    memset(m->ram_physical, 0, COCO3_PHYSICAL_RAM);

    // Reset GIME
    tcc1014_reset(&m->gime);

    // Wire GIME to physical RAM
    m->gime.ram = m->ram_physical;
    m->gime.ram_size = COCO3_PHYSICAL_RAM;

    // Reset PIAs
    mc6821_reset(&m->pia0);
    mc6821_reset(&m->pia1);
    sound_reset();
    m->pia0.irq_a_callback = pia0_irq_a_coco3;
    m->pia0.irq_b_callback = pia0_irq_b_coco3;
    m->pia1.irq_a_callback = pia1_irq_a_coco3;
    m->pia1.irq_b_callback = pia1_irq_b_coco3;

    // Reset timing
    m->cycles_this_frame = 0;
    m->scanline = 0;
    m->frame_count = 0;

    // CPU reset last (reads reset vector from ROM at $FFFE)
    mc6809_reset(&m->cpu);
}

// ============================================================
// Run one scanline — CoCo 3
// Port of coco3.c:cpu_cycle() / scanline handling
// ============================================================

void machine_run_scanline_coco3(Machine* m) {
    if (!m->initialized) return;

    // CPU execution
    int cycles_to_run = (int)(scanline_cycle_targets[m->scanline] - m->cycles_this_frame);
    if (cycles_to_run < 1) cycles_to_run = 1;

    sv_disk_tick(&m->fdc);
    int actual;
    {
        PERF_PROBE_SCOPE(PROBE_CPU_RUN);
        actual = mc6809_run(&m->cpu, cycles_to_run);
    }
    m->cycles_this_frame += actual;

    // RS-232 Pak ACIA: pace TDRE/RDRF by the cycles just run this scanline.
    if (rs232_pak_enabled()) mc6551_tick((uint32_t)actual);

    // GIME timer tick (replaces XRoar event-driven timer)
    tcc1014_tick_scanline(&m->gime);

    // HS (horizontal sync) → PIA0 CA1 — port of gime_hs() coco3.c:1416
    // Each scanline has an HS falling then rising edge.
    // PIA0 CA1 is typically configured for falling edge (BASIC 60Hz IRQ).
    mc6821_ca1_transition(&m->pia0, false);   // HS falling
    mc6821_ca1_transition(&m->pia0, true);    // HS rising

    // Combined interrupt routing — port of coco3.c:1261-1262
    // IRQ = PIA0 (both ports) OR GIME IRQ
    // FIRQ = PIA1 (both ports) OR GIME FIRQ
    // PIA IRQ output = (IRQ1_flag && IRQ1_enable) || (IRQ2_flag && IRQ2_enable && !output_mode)
    // IRQ1_flag=bit7, IRQ1_enable=bit0, IRQ2_flag=bit6, IRQ2_enable=bit3, output_mode=bit5
    {
        auto pia_irq = [](uint8_t cr) -> bool {
            bool irq1 = (cr & 0x80) && (cr & 0x01);
            bool irq2 = (cr & 0x40) && (cr & 0x08) && !(cr & 0x20);
            return irq1 || irq2;
        };
        bool pia0_irq = pia_irq(m->pia0.ctrl_a) || pia_irq(m->pia0.ctrl_b);
        bool pia1_irq = pia_irq(m->pia1.ctrl_a) || pia_irq(m->pia1.ctrl_b);
        bool new_irq  = pia0_irq || m->gime.IRQ;
        // Deluxe Pak routes its ACIA IRQ to CART → FIRQ.
        bool new_firq = pia1_irq || m->gime.FIRQ
                     || (rs232_pak_enabled() && mc6551_irq_active());
        // Wake CPU from SYNC/CWAI when any interrupt signal goes active.
        // Direct assignment bypasses mc6809_irq() which normally clears
        // wait_for_interrupt — we must do it here to avoid SYNC deadlock.
        if (new_irq || new_firq)
            m->cpu.wait_for_interrupt = false;
        m->cpu.irq_pending  = new_irq;
        m->cpu.firq_pending = new_firq;
    }

    // --- Vertical timing ---
    // Port of XRoar's do_vb_irq (tcc1014.c:1087-1228) and gime_fs (coco3.c:1429)
    //
    // NTSC frame layout (262 scanlines):
    //   Scanlines 0-3:    Vertical sync (FS low)
    //   Scanline  4:      FS rising edge
    //   Scanlines 4-6:    Post-sync blanking
    //   Scanline  7:      VRAM address reset, start border counting
    //   Scanlines 7+lTB:  Active area begins (lTB lines of top border)
    //   Active area:      lAA lines (192/199/225)
    //   After active:     VBORD interrupt, bottom border until frame end

    TCC1014* g = &m->gime;
    unsigned scanline = m->scanline;  // 0-261 absolute

    // FS falling edge at start of frame — port of tcc1014.c:1163-1165
    if (scanline == 0) {
        mc6821_cb1_transition(&m->pia0, false);   // FS falling edge
        // Latch vertical parameters for this frame — tcc1014.c:1168-1174
        // Latch lAA from LPF — must match VRES_LPF_lAA[] in tcc1014.cpp
        // LPF: 0=192, 1=199, 2=0xFFFF (infinite), 3=225
        g->vertical.lAA = g->COCO ? 192 :
            (g->LPF == 0 ? 192 : g->LPF == 1 ? 199 : g->LPF == 3 ? 225 : 0xFFFF);
        if (g->COCO) {
            g->vertical.lTB = g->H50 ? 63 : 36;
        }
        // Non-COCO lTB already set by update_from_gime_registers() on register write;
        // XRoar re-latches it here (tcc1014.c:1173) but it's equivalent since we
        // don't do mid-frame register changes on ESP32's scanline-based model.
    }

    // FS rising edge after 4 lines of sync — port of tcc1014.c:1181-1183
    if (scanline == 4) {
        mc6821_cb1_transition(&m->pia0, true);    // FS rising edge
    }

    // End of sync+blanking (7 lines) — reset VRAM address — tcc1014.c:1186-1199
    if (scanline == 7) {
        if (g->COCO) {
            g->B = (g->Y & 0x701FF) | ((uint32_t)g->SAM_F << 9);
        } else {
            g->B = g->Y;
        }
        g->vertical.lcount = 0;
        g->vertical.active_area = false;
    }

    // After sync+blanking, use lcount for border/active area tracking
    if (scanline >= 7) {
        unsigned vline = g->vertical.lcount;
        unsigned top_border_end = g->vertical.lTB;

        // Transition into active area — port of tcc1014.c:1201-1213
        // Use == to enter active area exactly once (>= would re-enter after area ends)
        if (!g->vertical.active_area && vline == top_border_end) {
            g->vertical.active_area = true;
            // Initialize row counter from VSC — tcc1014.c:1205
            if (!g->COCO) {
                g->row = g->VSC;
                if ((g->row & g->rowmask) == g->rowmask) {
                    g->row = 0;
                }
            } else {
                g->row = 0;
            }
        }

        if (g->vertical.active_area) {
            unsigned display_line = vline - top_border_end;

            // Per-scanline row_stride and border_colour — tcc1014.c:1114-1136
            g->Xoff = g->X;
            if (g->COCO || g->BP) {
                g->row_stride = g->HVEN ? 256 : g->BPR;
            } else {
                g->row_stride = g->HVEN ? 256 : (g->BPR << (g->CRES & 1));
            }
            if (g->COCO) {
                if (!g->VDG.GnA) {
                    bool GM2 = g->PIA1B_shadow.pdr & 0x40;
                    bool text_border = !g->VDG.GM1 && GM2;
                    unsigned text_border_colour = g->VDG.CSS ? 0x26 : 0x12;
                    g->border_colour = text_border ? text_border_colour : 0;
                } else {
                    unsigned c = g->VDG.CSS ? TCC1014_RGCSS1_1 : TCC1014_RGCSS0_1;
                    g->border_colour = g->palette_reg[c];
                }
            } else {
                g->border_colour = g->BRDR;
            }

            // Render this scanline
            {
                PERF_PROBE_SCOPE(PROBE_RENDER_SCANLINE);
                tcc1014_render_scanline(g, display_line);
            }
            {
                PERF_PROBE_SCOPE(PROBE_HAL_SCANLINE);
                hal_video_render_scanline_gime(display_line, g->vertical.lAA,
                                               g->border_colour, g->line_buffer,
                                               g->line_width, g->palette_rgb565);
            }

            // Row/address advance — port of do_hb_irq/do_vb_irq tcc1014.c:1068-1112
            g->row = (g->row + 1) & 15;  // Wrap at 16 (XRoar tcc1014.c:1070)
            bool row_advance = false;
            if ((g->row & g->rowmask) == g->rowmask) {
                row_advance = true;
                g->row = 0;
            }
            // BOINK bodge — port of tcc1014.c:1102-1105
            // LPR=7 (rowmask=16) is a special case: never advance B
            if (!g->COCO && g->LPR == 7) {
                row_advance = false;
            }
            if (row_advance) {
                g->B += g->row_stride;
            }
            tcc1014_set_interrupt(g, TCC1014_INT_HBORD);

            // End of active area — VBORD interrupt — tcc1014.c:1220-1222
            if (display_line + 1 >= g->vertical.lAA) {
                g->vertical.active_area = false;
                tcc1014_set_interrupt(g, TCC1014_INT_VBORD);
            }
        }

        g->vertical.lcount++;
    }

    m->scanline++;
    if (m->scanline >= SCANLINES_PER_FRAME) {
        m->scanline = 0;  // Wrap handled in machine_run_frame_coco3
    }
}

// ============================================================
// Run one frame — CoCo 3
// ============================================================

void machine_run_frame_coco3(Machine* m) {
    if (!m->initialized) return;
    PERF_PROBE_SCOPE(PROBE_FRAME);

    // Precompute cycle targets
    for (int i = 0; i < SCANLINES_PER_FRAME; i++) {
        scanline_cycle_targets[i] = ((i + 1) * m->cycles_per_frame) / SCANLINES_PER_FRAME;
    }

    m->cycles_this_frame = 0;
    m->scanline = 0;

    for (int line = 0; line < SCANLINES_PER_FRAME; line++) {
        machine_run_scanline_coco3(m);
        {
            PERF_PROBE_SCOPE(PROBE_AUDIO_SCANLINE);
            hal_audio_capture_scanline();
        }
    }

    hal_audio_commit_frame();
    {
        PERF_PROBE_SCOPE(PROBE_PUSH_SPRITE);
        hal_video_present_gime(&m->gime.dirty_frame);
    }

    m->frame_count++;
    perf_probe_frame_end();
}

// ============================================================
// Free — CoCo 3
// ============================================================

void machine_free_coco3(Machine* m) {
    if (m->ram_physical) { free(m->ram_physical); m->ram_physical = nullptr; }
    if (m->rom_coco3)    { free(m->rom_coco3);    m->rom_coco3 = nullptr; }
    if (m->rom_disk)     { free(m->rom_disk);      m->rom_disk = nullptr; }
    m->initialized = false;
    DEBUG_PRINT("Machine: freed");
}

// ############################################################
// ##  CoCo 2 — SAM/VDG-based machine (MACHINE_TYPE <= 3)
// ############################################################


// Update VDG mode from PIA1 (AG, CSS) + SAM (GM0-GM2)
static void update_vdg_mode_coco2(Machine* m) {
    uint8_t pb = m->pia1.data_b & m->pia1.ddr_b;
    uint8_t vdg_mode = 0;
    // AG from PIA1 PB7
    if (pb & 0x80) vdg_mode |= VDG_AG;
    // CSS from PIA1 PB3
    if (pb & 0x08) vdg_mode |= VDG_CSS;
    // GM0-GM2 from SAM V0-V2
    vdg_mode |= (m->sam.vdg_mode & 0x07);  // GM0=bit0, GM1=bit1, GM2=bit2
    mc6847_set_mode(&m->vdg, vdg_mode);
}

// ============================================================
// IRQ routing callbacks — CoCo 2
// ============================================================

// PIA0 IRQA/IRQB → CPU IRQ (60Hz vsync timer, keyboard hsync)
static void pia0_irq_a_coco2(bool active) {
    if (g_machine) mc6809_irq(&g_machine->cpu, active);
}

static void pia0_irq_b_coco2(bool active) {
    if (g_machine) mc6809_irq(&g_machine->cpu, active);
}

// PIA1 IRQA/IRQB → CPU FIRQ (cartridge)
static void pia1_irq_a_coco2(bool active) {
    if (g_machine) mc6809_firq(&g_machine->cpu, active);
}

static void pia1_irq_b_coco2(bool active) {
    if (g_machine) mc6809_firq(&g_machine->cpu, active);
}

// ============================================================
// Memory read callback (called by CPU) — CoCo 2
// ============================================================

uint8_t machine_read_coco2(uint16_t addr) {
    Machine* m = g_machine;
    if (!m) return 0xFF;

    // --- I/O space: $FF00-$FFFF ---
    if (addr >= 0xFF00) {
        // PIA0: $FF00-$FF1F (4 registers mirrored every 4 bytes)
        if (addr < 0xFF20) {
            uint8_t reg = addr & 0x03;

            // Special handling for PIA0 port A read: scan keyboard matrix + joystick
            // Before reading, update input_a based on which columns port B selects
            if (reg == PIA_REG_DA && (m->pia0.ctrl_a & PIA_CR_DDR_SEL)) {
                // PIA0 port B drives column select (active low)
                uint8_t col_select = m->pia0.data_b & m->pia0.ddr_b;
                uint8_t row_data = 0xFF;

                for (int col = 0; col < 8; col++) {
                    if (!(col_select & (1 << col))) {
                        // Column driven low → scan this column
                        row_data &= hal_keyboard_scan(col);
                    }
                }

                // Joystick buttons: right button → PA0 low, left button → PA1 low
                if (hal_joystick_read_button(0))
                    row_data &= ~0x01;
                if (hal_joystick_read_button(1))
                    row_data &= ~0x02;

                // Joystick comparator on PA7 (matches XRoar joystick_update exactly):
                //   port = PIA0 CRB bit 3 (CB2 output select)
                //   axis = PIA0 CRA bit 3 (CA2 output select)
                //   dac  = (PIA1 DA & 0xFC) + 2  (8-bit, range 2-254)
                //   js   = axis_6bit * 4 + 2      (scale 0-63 → 2-254)
                //
                // Refresh ADC once per scanline (not every PIA read) so asm
                // programs polling in tight loops see updated joystick values.
                {
                    // Refresh ADC every 16 scanlines (~16 updates/frame).
                    // Enough for responsive input without excessive ADC overhead.
                    static uint32_t last_joy_scanline = UINT32_MAX;
                    uint32_t joy_slot = m->scanline >> 4;  // divide by 16
                    if (joy_slot != last_joy_scanline) {
                        last_joy_scanline = joy_slot;
                        hal_joystick_update();
                    }
                    int joy_port = (m->pia0.ctrl_b & 0x08) >> 3;
                    int joy_axis = (m->pia0.ctrl_a & 0x08) >> 3;
                    int dac_value = ((m->pia1.data_a & m->pia1.ddr_a) & 0xFC) + 2;
                    int js_value = hal_joystick_read_axis(joy_port, joy_axis) * 4 + 2;
                    if (js_value >= dac_value)
                        row_data |= 0x80;
                    else
                        row_data &= ~0x80;
                }

                mc6821_set_input_a(&m->pia0, row_data);
            }

            return mc6821_read(&m->pia0, reg);
        }

        // PIA1: $FF20-$FF3F (4 registers mirrored every 4 bytes)
        if (addr < 0xFF40) {
            return mc6821_read(&m->pia1, addr & 0x03);
        }

        // Disk controller: $FF40-$FF5F (WD1793 FDC)
        if (addr < 0xFF60) {
            return sv_disk_read(&m->fdc, addr);
        }

        // RS-232 Pak ($FF68-$FF6B) — Deluxe RS-232 Program Pak ACIA.
        // No-op unless the pak owns the serial port.
        if (rs232_pak_enabled() && (addr & 0xFFFC) == 0xFF68) {
            return mc6551_read(addr & 0x03);
        }

        // Reserved: $FF60-$FFBF
        if (addr < 0xFFC0) {
            return 0xFF;
        }

        // SAM: $FFC0-$FFDF (write-only, reads return open bus)
        if (addr < 0xFFE0) {
            return 0xFF;
        }

        // Vectors: $FFE0-$FFFF — read from ROM (end of Extended BASIC)
        // These map to $FFE0-$FFFF which is in the $8000-$9FFF region offset
        // Vectors are at the top of the address space in ROM
        if (m->rom_extbas_loaded) {
            // ExtBASIC ROM covers $8000-$9FFF (8K)
            // Vector $FFFx maps to ExtBASIC offset ($FFFx - $8000) mod 8K
            // But vectors are actually at the end of a 16K block...
            // In a real CoCo, all ROM appears as a single 16K block $8000-$BFFF
            // mapped via SAM. The vectors at $FFF0-$FFFF mirror the top of ROM.
            // Color BASIC has vectors at its top ($BFFA-$BFFF)
            if (m->rom_basic_loaded) {
                // BASIC ROM is at $A000-$BFFF, vectors point into it
                uint16_t rom_offset = addr - 0xA000;
                if (rom_offset < 0x2000) {
                    return m->rom_basic[rom_offset];
                }
                // Fallthrough for addresses > $BFFF
                // Map $FFEx-$FFFx: use basic ROM's vector region
                // CoCo vectors at $FFF0-$FFFF correspond to BASIC ROM $BFF0-$BFFF
                rom_offset = 0x1FF0 + (addr & 0x0F);
                if (addr >= 0xFFF0) {
                    return m->rom_basic[rom_offset];
                }
                // $FFE0-$FFEF: also from basic ROM
                rom_offset = 0x1FE0 + (addr & 0x1F);
                return m->rom_basic[rom_offset];
            }
        }
        return 0xFF;
    }

    // --- ROM space: $8000-$FEFF ---
    // SAM MAP TYPE (ty) = 1: all-RAM mode — ROMs unmapped, full 64K is RAM
    if (addr >= 0x8000) {
        if (m->sam.ty && (size_t)addr < m->ram_size) {
            return m->ram[addr];
        }
        if (addr >= 0xC000) {
            // Cartridge ROM / Disk BASIC: $C000-$FEFF
            if (m->rom_cart_loaded) {
                return m->rom_cart[addr - 0xC000];
            }
            return 0xFF;
        }
        if (addr >= 0xA000) {
            // Color BASIC: $A000-$BFFF
            if (m->rom_basic_loaded) {
                return m->rom_basic[addr - 0xA000];
            }
            return 0xFF;
        }
        // Extended BASIC: $8000-$9FFF
        if (m->rom_extbas_loaded) {
            return m->rom_extbas[addr - 0x8000];
        }
        return 0xFF;
    }

    // --- RAM: $0000-$7FFF ---
    if ((size_t)addr < m->ram_size) {
        return m->ram[addr];
    }

    return 0xFF;
}

// ============================================================
// Memory write callback (called by CPU) — CoCo 2
// ============================================================

void machine_write_coco2(uint16_t addr, uint8_t val) {
    Machine* m = g_machine;
    if (!m) return;

    // --- I/O space: $FF00-$FFFF ---
    if (addr >= 0xFF00) {
        // PIA0: $FF00-$FF1F
        if (addr < 0xFF20) {
            mc6821_write(&m->pia0, addr & 0x03, val);
            sound_pia0_written(m, addr & 0x03);
            return;
        }

        // PIA1: $FF20-$FF3F
        if (addr < 0xFF40) {
            uint8_t reg = addr & 0x03;
            mc6821_write(&m->pia1, reg, val);

            // PIA1 port B drives VDG AG (bit 7) and CSS (bit 3)
            // GM0-GM2 come from SAM V0-V2, not PIA1
            if (reg == PIA_REG_DB || reg == PIA_REG_CRB) {
                update_vdg_mode_coco2(m);
            }
            sound_pia1_written(m, reg);
            return;
        }

        // Disk controller: $FF40-$FF5F (WD1793 FDC)
        if (addr < 0xFF60) {
            sv_disk_write(&m->fdc, addr, val);
            return;
        }

        // RS-232 Pak ($FF68-$FF6B) — see machine_read_coco2.
        if (rs232_pak_enabled() && (addr & 0xFFFC) == 0xFF68) {
            mc6551_write(addr & 0x03, val);
            return;
        }

        // Reserved: $FF60-$FFBF
        if (addr < 0xFFC0) {
            return;
        }

        // SAM: $FFC0-$FFDF (write-only bit set/clear pairs)
        if (addr <= 0xFFDF) {
            sam6883_write(&m->sam, addr - 0xFFC0);
            // Update VDG display base address from SAM F0-F6
            m->vdg.vram_offset = sam6883_get_vdg_address(&m->sam);
            // SAM V0-V2 control VDG GM0-GM2 bits
            update_vdg_mode_coco2(m);
            return;
        }

        // $FFE0-$FFFF: ROM vectors, writes ignored
        return;
    }

    // --- ROM space: $8000-$FEFF ---
    // SAM MAP TYPE (ty) = 1: all-RAM mode — writes go to RAM
    if (addr >= 0x8000) {
        if (m->sam.ty && (size_t)addr < m->ram_size) {
            m->ram[addr] = val;
        }
        // Normal mode: writes to ROM space are ignored
        return;
    }

    // --- RAM: $0000-$7FFF ---
    if ((size_t)addr < m->ram_size) {
        m->ram[addr] = val;
    }
}

// ============================================================
// Public API: init — CoCo 2
// ============================================================

void machine_init_coco2(Machine* m) {
    DEBUG_PRINT("Machine: initializing...");
    memset(m, 0, sizeof(Machine));
    g_machine = m;

    // --- Allocate memory (Step 5 of coco2and3.md: invariant 512KB superset) ---
    // Always allocate 512KB in PSRAM and alias m->ram to the first 64KB so
    // CoCo 2 addressing (0x0000-0xFFFF via SAM) is unchanged; the upper 448KB
    // is unused here but keeps allocation identical to the CoCo 3 boot path.
    m->ram_physical = machine_alloc(COCO3_PHYSICAL_RAM, "RAM-512K");
    if (!m->ram_physical) return;
    m->ram = m->ram_physical;   // CoCo 2 uses only the first 64KB
    m->ram_size = COCO_RAM_SIZE;

    // CoCo 2 ROM buffers
    m->rom_basic = machine_alloc(8192, "ROM-BASIC");
    if (!m->rom_basic) return;
    m->rom_extbas = machine_alloc(8192, "ROM-ExtBAS");
    if (!m->rom_extbas) return;
    m->rom_cart = machine_alloc(16384, "ROM-Cart");
    if (!m->rom_cart) return;

    // CoCo 3 ROM buffers (Step 5: always allocate so the ROM loader can fill
    // both sets on boot, matching the CoCo 3 init's allocation footprint).
    m->rom_coco3 = machine_alloc(COCO3_ROM_SIZE, "ROM-CoCo3");
    if (!m->rom_coco3) return;
    m->rom_disk = machine_alloc(8192, "ROM-Disk");
    if (!m->rom_disk) return;

    // Fill ROM buffers with $FF (unloaded state)
    memset(m->rom_basic, 0xFF, 8192);
    memset(m->rom_extbas, 0xFF, 8192);
    memset(m->rom_cart, 0xFF, 16384);
    memset(m->rom_coco3, 0xFF, COCO3_ROM_SIZE);
    memset(m->rom_disk, 0xFF, 8192);

    // --- Initialize core chips ---
    mc6809_init(&m->cpu);
    m->cpu.read = machine_read_coco2;
    m->cpu.write = machine_write_coco2;

    mc6821_init(&m->pia0);
    mc6821_init(&m->pia1);

    // Wire IRQ callbacks
    m->pia0.irq_a_callback = pia0_irq_a_coco2;
    m->pia0.irq_b_callback = pia0_irq_b_coco2;
    m->pia1.irq_a_callback = pia1_irq_a_coco2;
    m->pia1.irq_b_callback = pia1_irq_b_coco2;

    mc6847_init(&m->vdg);
    sam6883_init(&m->sam);

    // --- Timing ---
    m->ntsc = true;
    m->cycles_per_frame = CYCLES_PER_FRAME;
    m->cart_inserted = false;

    m->initialized = true;
    DEBUG_PRINTF("Machine: init complete. Free heap: %d", ESP.getFreeHeap());
}

// ============================================================
// Public API: load ROMs — CoCo 2
// ============================================================

bool machine_load_roms_coco2(Machine* m, const char* rom_path) {
    if (!m->initialized) return false;

    DEBUG_PRINT("Machine: loading ROMs...");
    char path[64];

    // Color BASIC ROM → $A000-$BFFF (8K)
    snprintf(path, sizeof(path), "%s/%s", rom_path, ROM_BASIC_FILE);
    if (hal_storage_load_file(path, m->rom_basic, 8192)) {
        m->rom_basic_loaded = true;
        DEBUG_PRINTF("  Loaded %s → $A000-$BFFF", ROM_BASIC_FILE);
    } else {
        DEBUG_PRINTF("  MISSING: %s", path);
    }

    // Extended BASIC ROM → $8000-$9FFF (8K)
    snprintf(path, sizeof(path), "%s/%s", rom_path, ROM_EXT_BASIC_FILE);
    if (hal_storage_load_file(path, m->rom_extbas, 8192)) {
        m->rom_extbas_loaded = true;
        DEBUG_PRINTF("  Loaded %s → $8000-$9FFF", ROM_EXT_BASIC_FILE);
    } else {
        DEBUG_PRINTF("  MISSING: %s", path);
    }

    // Disk BASIC / Cartridge ROM → $C000-$DFFF (8K-16K, optional)
    snprintf(path, sizeof(path), "%s/%s", rom_path, ROM_DISK_FILE);
    if (hal_storage_load_file(path, m->rom_cart, 8192)) {
        m->rom_cart_loaded = true;
        m->cart_inserted = true;
        DEBUG_PRINTF("  Loaded %s → $C000", ROM_DISK_FILE);
    } else {
        DEBUG_PRINTF("  Optional: %s not found", path);
    }

    // Verify reset vector is readable
    if (m->rom_basic_loaded) {
        uint16_t reset_vec = ((uint16_t)m->rom_basic[0x1FFE] << 8) | m->rom_basic[0x1FFF];
        DEBUG_PRINTF("  Reset vector in BASIC ROM: $%04X", reset_vec);
    }

    return m->rom_basic_loaded;
}

// ============================================================
// Public API: reset — CoCo 2
// ============================================================

void machine_reset_coco2(Machine* m) {
    if (!m->initialized) return;

    DEBUG_PRINT("Machine: RESET");

    // Clear RAM
    memset(m->ram, 0x00, m->ram_size);

    // Reset all components
    mc6821_reset(&m->pia0);
    mc6821_reset(&m->pia1);
    sound_reset();
    mc6847_reset(&m->vdg);
    sam6883_reset(&m->sam);

    // Re-wire IRQ callbacks (reset clears them)
    m->pia0.irq_a_callback = pia0_irq_a_coco2;
    m->pia0.irq_b_callback = pia0_irq_b_coco2;
    m->pia1.irq_a_callback = pia1_irq_a_coco2;
    m->pia1.irq_b_callback = pia1_irq_b_coco2;

    // Set VDG VRAM pointer to machine RAM
    m->vdg.vram = m->ram;
    m->vdg.vram_offset = sam6883_get_vdg_address(&m->sam);
    m->vdg.row_address = m->vdg.vram_offset;
    // Reset SAM VDG counter
    sam6883_vdg_fsync(&m->sam, true);

    // Reset timing state
    m->cycles_this_frame = 0;
    m->scanline = 0;
    m->frame_count = 0;

    // CPU reset last (reads reset vector from ROM)
    mc6809_reset(&m->cpu);
}

// ============================================================
// Public API: run one scanline — CoCo 2
// ============================================================

void machine_run_scanline_coco2(Machine* m) {
    if (!m->initialized) return;

    // Use precomputed table (filled in machine_run_frame_coco2) to avoid
    // per-scanline multiply+divide
    int cycles_to_run = (int)(scanline_cycle_targets[m->scanline] - m->cycles_this_frame);
    if (cycles_to_run < 1) cycles_to_run = 1;

    // Tick FDC deferred INTRQ (fires NMI after sector transfer completes)
    sv_disk_tick(&m->fdc);

    // Execute CPU
    int actual = mc6809_run(&m->cpu, cycles_to_run);
    m->cycles_this_frame += actual;

    // RS-232 Pak ACIA. Its IRQ shares the cartridge FIRQ line (CART), so when
    // the pak is active we OR it with the PIA1 cartridge FIRQ each scanline.
    // Guarded: when the pak is off, the CoCo2 FIRQ path is byte-for-byte as before.
    if (rs232_pak_enabled()) {
        mc6551_tick((uint32_t)actual);
        auto pia_irq = [](uint8_t cr) -> bool {
            bool irq1 = (cr & 0x80) && (cr & 0x01);
            bool irq2 = (cr & 0x40) && (cr & 0x08) && !(cr & 0x20);
            return irq1 || irq2;
        };
        bool pia1_firq = pia_irq(m->pia1.ctrl_a) || pia_irq(m->pia1.ctrl_b);
        bool firq = pia1_firq || mc6551_irq_active();
        m->cpu.firq_pending = firq;
        if (firq) m->cpu.wait_for_interrupt = false;
    }

    // --- VDG/SAM scanline processing ---

    // FS (field sync) — PIA0 CB1 — 60Hz interrupt
    if (m->scanline == 0) {
        // End of vertical blank: FS goes HIGH (rising edge)
        m->vdg.fs = false;
        mc6821_cb1_transition(&m->pia0, true);
        // Reset SAM VDG address counter to display base
        sam6883_vdg_fsync(&m->sam, true);
    }

    if (m->scanline < ACTIVE_SCANLINES) {
        // Get row address BEFORE fetch advancement (VDG reads from current address)
        m->vdg.row_address = sam6883_get_vdg_row_address(&m->sam);
        m->vdg.scanline = m->scanline;
        if (mc6847_render_scanline(&m->vdg)) {
            hal_video_render_scanline(m->scanline, m->vdg.line_buffer, VDG_ACTIVE_WIDTH);
        }

        // Simulate VDG data fetch: advance SAM counter by bytes_per_row.
        // In XRoar, sam_vdg_bytes() is called by VDG as it reads each byte.
        // We do it in one call after rendering with the total for the row.
        // bytes_per_row: GM=0,1,3,5→16, GM=2,4,6,7→32
        // Text mode also fetches 32 bytes (handled by SAM divide counters).
        {
            static const int bytes_per_row[8] = { 16, 16, 32, 16, 32, 16, 32, 32 };
            int bpr = (m->vdg.mode & VDG_AG) ? bytes_per_row[m->sam.vdg_mode] : 32;
            sam6883_vdg_fetch_bytes(&m->sam, bpr);
        }
    }

    // HS (horizontal sync) — supplementary counter adjustment per XRoar.
    // On real CoCo: VDG HS → PIA0 CA1, but CoCo BASIC doesn't enable
    // CA1 IRQ so we skip PIA transitions for performance.
    sam6883_vdg_hsync(&m->sam, false);  // Falling edge does fixup

    if (m->scanline == ACTIVE_SCANLINES) {
        // Start of vertical blank: FS goes LOW (falling edge)
        m->vdg.fs = true;
        mc6821_cb1_transition(&m->pia0, false);
    }

    m->scanline++;
}

// ============================================================
// Public API: run one frame (262 scanlines) — CoCo 2
// ============================================================

void machine_run_frame_coco2(Machine* m) {
    if (!m->initialized) return;

    // Precompute cycle targets for all 262 scanlines
    for (int i = 0; i < SCANLINES_PER_FRAME; i++) {
        scanline_cycle_targets[i] = ((i + 1) * m->cycles_per_frame) / SCANLINES_PER_FRAME;
    }

    m->cycles_this_frame = 0;
    m->scanline = 0;

    for (int line = 0; line < SCANLINES_PER_FRAME; line++) {
        machine_run_scanline_coco2(m);
        // Capture audio level after each scanline for pitch-correct playback
        hal_audio_capture_scanline();
    }

    // Commit scanline audio buffer to ISR for playback at correct CoCo rate
    hal_audio_commit_frame();

    // Present completed frame (with VRAM shadow compare — OPT-16)
    hal_video_present(m->ram, m->sam.vdg_base, m->vdg.mode);

    m->frame_count++;
}

// ============================================================
// Public API: free — CoCo 2
// ============================================================

void machine_free_coco2(Machine* m) {
    if (m->ram)        { free(m->ram);        m->ram = nullptr; }
    if (m->rom_basic)  { free(m->rom_basic);  m->rom_basic = nullptr; }
    if (m->rom_extbas) { free(m->rom_extbas); m->rom_extbas = nullptr; }
    if (m->rom_cart)   { free(m->rom_cart);    m->rom_cart = nullptr; }
    m->initialized = false;
    DEBUG_PRINT("Machine: freed");
}


// ############################################################
// ##  Runtime dispatchers — Step 3 of coco2and3.md
// ##
// ##  Public machine_* API branches on g_machine_type and calls
// ##  the per-machine implementation. All external callers (CPU
// ##  callbacks, HAL, supervisor, tests) use these entry points.
// ############################################################

uint8_t machine_read(uint16_t addr) {
    return (g_machine_type == 4) ? machine_read_coco3(addr)
                                 : machine_read_coco2(addr);
}

void machine_write(uint16_t addr, uint8_t val) {
    if (g_machine_type == 4) machine_write_coco3(addr, val);
    else                     machine_write_coco2(addr, val);
}

void machine_init(Machine* m) {
    if (g_machine_type == 4) machine_init_coco3(m);
    else                     machine_init_coco2(m);
}

bool machine_load_roms(Machine* m, const char* rom_path) {
    // Step 5 of coco2and3.md: load BOTH ROM sets on boot (CoCo 2: 24 KB
    // + CoCo 3: 40 KB). Cheap on SD, gives users early warning if a ROM
    // is missing for the machine they might switch to, and keeps the
    // boot allocation/load footprint invariant across machine types.
    bool ok_coco2 = machine_load_roms_coco2(m, rom_path);
    bool ok_coco3 = machine_load_roms_coco3(m, rom_path);
    return (g_machine_type == 4) ? ok_coco3 : ok_coco2;
}

void machine_reset(Machine* m) {
    if (g_machine_type == 4) machine_reset_coco3(m);
    else                     machine_reset_coco2(m);
    // RS-232 Pak ACIA hard reset. Serial-port ownership (g_serial_mode) is
    // deliberately NOT touched here — a CoCo reset must not let debug noise
    // corrupt an active host link.
    mc6551_reset();
}

void machine_run_scanline(Machine* m) {
    if (g_machine_type == 4) machine_run_scanline_coco3(m);
    else                     machine_run_scanline_coco2(m);
}

void machine_run_frame(Machine* m) {
    if (g_machine_type == 4) machine_run_frame_coco3(m);
    else                     machine_run_frame_coco2(m);
}

void machine_free(Machine* m) {
    if (g_machine_type == 4) machine_free_coco3(m);
    else                     machine_free_coco2(m);
}
