#include "scenario.h"
#include <stdio.h>

// Interrupt scenarios (1987 GIME). INIT0 IEN is bit 5, FEN bit 4; $FF92 holds
// the IRQ source enables, $FF93 the FIRQ ones. Source bits: TMR $20, HBORD
// $10, VBORD $08, EI2 $04, EI1 $02, EI0 $01. Status reads compare bits 5-0
// only (mask $3F, ruling R6).
//
// IL0-IL2 are driven through Ctx::set_il. XRoar edge-detects them in
// tcc1014_mem_cycle(); ours samples the levels in tcc1014_mem_cycle() on
// every access (tcc1014.cpp "External interrupt processing"). Neither looks
// at IL in set_il itself, so after set_il every scenario performs a harmless
// RAM read (kIlSample) identically in all models.

namespace {

const uint16_t kIlSample = 0x0000;

struct Src { const char* name; uint8_t bit; };
const Src SRCS[] = { { "tmr", 0x20 }, { "hbord", 0x10 }, { "vbord", 0x08 },
                     { "ei2", 0x04 }, { "ei1", 0x02 }, { "ei0", 0x01 } };

unsigned il_of(uint8_t bit) { return bit == 0x01 ? 0 : bit == 0x02 ? 1 : 2; }
bool is_ei(uint8_t bit) { return bit <= 0x04; }

void il(Ctx& ctx, unsigned line, bool level) {
    ctx.set_il(line, level);
    ctx.read(kIlSample);
}

// Start the source. TMR: arm a TINS=0 timer that fires after about 6 lines.
// HBORD/VBORD: run to a point after the first border event but before the
// next frame. EI: raise IL.
void arm_timer_if_tmr(Ctx& ctx, uint8_t bit) {
    // Armed before the enable write: with the counter at 0 XRoar latches
    // INT_TMR at the $FF92/$FF93 write (see the timer scenarios), which is not
    // what this matrix tests.
    if (bit == 0x20) {
        ctx.write(0xFF95, 5);
        ctx.write(0xFF94, 0);
    }
}

void fire(Ctx& ctx, uint8_t bit) {
    if (bit == 0x20) {
        ctx.lines(8);
    } else if (bit == 0x10) {
        ctx.lines(60);
    } else if (bit == 0x08) {
        ctx.lines(250);
    } else {
        il(ctx, il_of(bit), true);
    }
}

void line_checks(Ctx& ctx, const std::string& id, bool firq) {
    ctx.check(id + "_line", [firq](GimeModel& m) {
        return (int64_t)(firq ? m.firq() : m.irq()); }, CAP_IRQ);
}

} // namespace

// Each source x output x (source enable on/off) x (INIT0 master on/off).
SCENARIO(irq, enable_matrix) {
    for (const Src& s : SRCS)
        for (unsigned firq = 0; firq < 2; firq++)
            for (unsigned en = 0; en < 2; en++)
                for (unsigned master = 0; master < 2; master++) {
                    char id[64];
                    snprintf(id, sizeof id, "irq.enable_%s_%s_en%u_m%u",
                             s.name, firq ? "firq" : "irq", en, master);
                    const uint16_t reg = firq ? 0xFF93 : 0xFF92;
                    const uint8_t mbit = firq ? 0x10 : 0x20;
                    ctx.reset();
                    ctx.write(0xFF90, 0x40 | (master ? mbit : 0));
                    arm_timer_if_tmr(ctx, s.bit);
                    if (en) ctx.write(reg, s.bit);
                    fire(ctx, s.bit);
                    ctx.check_event(std::string(id) + "_rise",
                                    firq ? EvType::FIRQ_RISE : EvType::IRQ_RISE, 0);
                    // TMR (a TINS=0 timer), HBORD and VBORD are line-rate.
                    if (!is_ei(s.bit))
                        ctx.check_event_line(std::string(id) + "_rise",
                                             firq ? EvType::FIRQ_RISE : EvType::IRQ_RISE, 0);
                    line_checks(ctx, id, firq);
                    ctx.check_read(std::string(id) + "_read", reg, 0x3F);
                    ctx.check(std::string(id) + "_line_after_read", [firq](GimeModel& m) {
                        return (int64_t)(firq ? m.firq() : m.irq()); }, CAP_IRQ);
                }
}

// Enabling a source that is already pending (IL held high before the enable
// write) raises the output at the write. The model adapters stamp that rise
// at the write.
SCENARIO(irq, enable_pending_ei) {
    for (const Src& s : SRCS) {
        if (!is_ei(s.bit)) continue;
        for (unsigned firq = 0; firq < 2; firq++) {
            char id[64];
            snprintf(id, sizeof id, "irq.enable_pending_%s_%s", s.name, firq ? "firq" : "irq");
            const uint16_t reg = firq ? 0xFF93 : 0xFF92;
            ctx.reset();
            ctx.write(0xFF90, 0x40 | 0x30);
            il(ctx, il_of(s.bit), true);
            ctx.lines(2);
            ctx.drain();
            ctx.anchor();
            ctx.write(reg, s.bit);
            ctx.check_event(std::string(id) + "_rise",
                            firq ? EvType::FIRQ_RISE : EvType::IRQ_RISE, 0);
            line_checks(ctx, id, firq);
            ctx.check_read(std::string(id) + "_read", reg, 0x3F);
        }
    }
}

// A second read returns 0 and the output drops.
SCENARIO(irq, clear_on_read_ff92) {
    ctx.reset();
    ctx.write(0xFF90, 0x60);
    ctx.write(0xFF92, 0x01);
    il(ctx, 0, true);
    ctx.check("irq.clear_on_read_ff92_line", [](GimeModel& m) { return (int64_t)m.irq(); }, CAP_IRQ);
    ctx.check_read("irq.clear_on_read_ff92_read1", 0xFF92, 0x3F);
    ctx.check("irq.clear_on_read_ff92_line_after", [](GimeModel& m) { return (int64_t)m.irq(); }, CAP_IRQ);
    ctx.check_read("irq.clear_on_read_ff92_read2", 0xFF92, 0x3F);
}

SCENARIO(irq, clear_on_read_ff93) {
    ctx.reset();
    ctx.write(0xFF90, 0x50);
    ctx.write(0xFF93, 0x01);
    il(ctx, 0, true);
    ctx.check("irq.clear_on_read_ff93_line", [](GimeModel& m) { return (int64_t)m.firq(); }, CAP_IRQ);
    ctx.check_read("irq.clear_on_read_ff93_read1", 0xFF93, 0x3F);
    ctx.check("irq.clear_on_read_ff93_line_after", [](GimeModel& m) { return (int64_t)m.firq(); }, CAP_IRQ);
    ctx.check_read("irq.clear_on_read_ff93_read2", 0xFF93, 0x3F);
}

// The same source enabled in both registers with both masters on. Reading
// one register clears only its own side.
SCENARIO(irq, both_outputs) {
    for (const Src& s : SRCS) {
        std::string id = std::string("irq.both_outputs_") + s.name;
        ctx.reset();
        ctx.write(0xFF90, 0x70);
        ctx.write(0xFF92, s.bit);
        arm_timer_if_tmr(ctx, s.bit);
        ctx.write(0xFF93, s.bit);
        fire(ctx, s.bit);
        ctx.check(id + "_irq", [](GimeModel& m) { return (int64_t)m.irq(); }, CAP_IRQ);
        ctx.check(id + "_firq", [](GimeModel& m) { return (int64_t)m.firq(); }, CAP_IRQ);
        ctx.check_read(id + "_read92", 0xFF92, 0x3F);
        ctx.check(id + "_irq_after92", [](GimeModel& m) { return (int64_t)m.irq(); }, CAP_IRQ);
        ctx.check(id + "_firq_after92", [](GimeModel& m) { return (int64_t)m.firq(); }, CAP_IRQ);
        ctx.check_read(id + "_read93", 0xFF93, 0x3F);
        ctx.check(id + "_firq_after93", [](GimeModel& m) { return (int64_t)m.firq(); }, CAP_IRQ);
    }
}

// Holding IL high does not re-latch after a clear; a new rising edge does.
SCENARIO(irq, edge_only) {
    for (unsigned n = 0; n < 3; n++) {
        char id[48];
        snprintf(id, sizeof id, "irq.edge_only_il%u", n);
        std::string s = id;
        const uint8_t bit = (uint8_t)(1u << n);
        ctx.reset();
        ctx.write(0xFF90, 0x60);
        ctx.write(0xFF92, bit);
        il(ctx, n, true);
        ctx.check_read(s + "_read1", 0xFF92, 0x3F);
        il(ctx, n, true);                            // still high: another sample
        ctx.read(kIlSample);
        ctx.check(s + "_line_held", [](GimeModel& m) { return (int64_t)m.irq(); }, CAP_IRQ);
        ctx.check_read(s + "_read2", 0xFF92, 0x3F);
        il(ctx, n, false);
        il(ctx, n, true);                            // a new edge
        ctx.check(s + "_line_new_edge", [](GimeModel& m) { return (int64_t)m.irq(); }, CAP_IRQ);
        ctx.check_read(s + "_read3", 0xFF92, 0x3F);
    }
}
