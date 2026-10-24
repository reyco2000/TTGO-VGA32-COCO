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
