#include "scenario.h"
#include <stdio.h>

// Power-on state: every GIME-visible address reads the same, no interrupt.
SCENARIO(reset, state) {
    ctx.reset();
    for (unsigned a = 0xFF90; a <= 0xFFBF; a++) {
        char id[32];
        snprintf(id, sizeof id, "reset.read_%04X", a);
        ctx.check_read(id, (uint16_t)a);
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
