#include "scenario.h"
#include <stdio.h>

// R6: the GIME drives only bits 5-0 of $FF92/$FF93, the MMU registers and the
// palette; bits 7-6 there are floating bus, which the harness doesn't model.
uint8_t regs_read_mask(unsigned a) {
    return (a == 0xFF92 || a == 0xFF93 || (a >= 0xFFA0 && a <= 0xFFBF)) ? 0x3F : 0xFF;
}

// Power-on state: every GIME-visible address reads the same, no interrupt.
SCENARIO(reset, state) {
    ctx.reset();
    for (unsigned a = 0xFF90; a <= 0xFFBF; a++) {
        char id[32];
        snprintf(id, sizeof id, "reset.read_%04X", a);
        ctx.check_read(id, (uint16_t)a, regs_read_mask(a));
    }
    ctx.check("reset.irq",  [](GimeModel& m) { return (int64_t)m.irq(); },  CAP_IRQ);
    ctx.check("reset.firq", [](GimeModel& m) { return (int64_t)m.firq(); }, CAP_IRQ);
}

// Power-on address decode of the ROM area: MMU off, TY=0, MC1/MC0=0, so
// $8000-$BFFF is internal ROM (S=0) and $C000-$FEFF the cartridge (S=1).
SCENARIO(reset, decode_rom) {
    ctx.reset();
    for (unsigned a : { 0x8000u, 0xA000u, 0xC000u, 0xFE00u }) {
        char id[32];
        snprintf(id, sizeof id, "reset.decode_S_%04X", a);
        ctx.check(id, [a](GimeModel& m) { return (int64_t)m.decode((uint16_t)a).S; },
                  CAP_DECODE);
    }
}

// Write each pattern to each GIME register and read it back. Each check
// starts from a fresh reset so one write can't leak into the next.
SCENARIO(regs, readback) {
    for (unsigned a = 0xFF90; a <= 0xFFBF; a++)
        for (unsigned v : { 0x00u, 0x55u, 0xAAu, 0xFFu }) {
            char id[40];
            snprintf(id, sizeof id, "regs.readback_%04X_%02X", a, v);
            ctx.reset();
            ctx.write((uint16_t)a, (uint8_t)v);
            ctx.check_read(id, (uint16_t)a, regs_read_mask(a));
        }
}

// Bits 7-6 of the masked groups (information: floating bus on hardware).
// Only bits 7-6 are read (mask $C0): in $FF92 bit 5 is the TMR status, and
// whether the $FF92 write latches TMR (timer at 0) is a different question,
// covered by regs.readback_FF92_*.
SCENARIO(regs, undriven_bits) {
    const struct { const char* id; uint16_t addr; } g[] = {
        { "regs.undriven_bits_ff92", 0xFF92 },
        { "regs.undriven_bits_mmu", 0xFFA0 },
        { "regs.undriven_bits_palette", 0xFFB0 },
    };
    for (auto& e : g) {
        ctx.reset();
        ctx.write(e.addr, 0xFF);
        ctx.check_read(e.id, e.addr, 0xC0);
    }
}
