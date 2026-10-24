#include "scenario.h"
#include <stdio.h>
#include <stdlib.h>

// Timer scenarios (1987 GIME). Every tick checked is relative to an anchor
// taken in the same model: the arming write (MSB $FF94) unless a scenario
// says otherwise. Arming happens two whole lines after reset, which is a
// line boundary for ours and 10 ticks before an HS fall for XRoar (its
// first HS fall is at tick 922).

static const unsigned SETTLE_LINES = 2;
static const unsigned CAP_LINES = 50000;   // covers ev10 at TINS=0, reload $FFF

static uint8_t init1(bool tins) { return tins ? 0x20 : 0x00; }

// Reset, MMU on, then arm the timer (LSB first: the MSB write starts it).
// init0 is written once at reset; the anchor is taken at the MSB write.
static void arm(Ctx& ctx, bool tins, unsigned reload, uint8_t init0 = 0x40) {
    ctx.reset();
    ctx.write(0xFF90, init0);
    ctx.lines(SETTLE_LINES);
    ctx.write(0xFF91, init1(tins));
    ctx.write(0xFF95, (uint8_t)(reload & 0xFF));
    ctx.write(0xFF94, (uint8_t)(reload >> 8));
    ctx.drain();
    ctx.anchor();
}

// True once every model that reports TIMER events has n of them.
static bool have_events(Ctx& ctx, EvType t, unsigned n) {
    for (GimeModel* m : ctx.models)
        if ((m->caps() & CAP_TIMER_EV) && ctx.event_tick(m, t, n - 1) < 0) return false;
    return true;
}

// Advance until every model has seen n events of type t, at most cap lines.
static void run_until(Ctx& ctx, EvType t, unsigned n, unsigned cap = CAP_LINES) {
    const unsigned step = 64;
    for (unsigned done = 0; done < cap && !have_events(ctx, t, n); done += step)
        ctx.lines(cap - done < step ? cap - done : step);
}

static void check_timer_evs(Ctx& ctx, const std::string& id) {
    ctx.check_event(id + "_ev1", EvType::TIMER, 0);
    ctx.check_event(id + "_ev2", EvType::TIMER, 1);
    ctx.check_event(id + "_ev10", EvType::TIMER, 9);
}

SCENARIO(timer, period) {
    for (unsigned tins = 0; tins < 2; tins++)
        for (unsigned reload : { 0u, 1u, 2u, 3u, 100u, 596u, 0xFFFu }) {
            char id[48];
            snprintf(id, sizeof id, "timer.tins%u_period_%u", tins, reload);
            arm(ctx, tins != 0, reload);
            run_until(ctx, EvType::TIMER, 10);
            check_timer_evs(ctx, id);
        }
}

// Rewriting the MSB mid-period restarts the count: events are measured from
// the rewrite.
SCENARIO(timer, restart_on_msb_write) {
    struct { bool tins; unsigned reload, lines_before; } cases[] = {
        { false, 100, 50 }, { true, 0xFFF, 9 } };
    for (auto& c : cases) {
        std::string id = std::string("timer.restart_on_msb_write_tins") + (c.tins ? "1" : "0");
        arm(ctx, c.tins, c.reload);
        ctx.lines(c.lines_before);
        ctx.write(0xFF94, (uint8_t)(c.reload >> 8));
        ctx.drain();
        ctx.anchor();
        run_until(ctx, EvType::TIMER, 2);
        ctx.check_event(id + "_ev1", EvType::TIMER, 0);
        ctx.check_event(id + "_ev2", EvType::TIMER, 1);
    }
}

// An LSB write mid-period changes the reload but does not restart the
// count: ev1 keeps the old period, the next period uses the new value.
// Both are measured from the original arming write.
SCENARIO(timer, restart_on_lsb_write) {
    struct { bool tins; unsigned reload, lines_before; uint8_t lsb; } cases[] = {
        { false, 100, 50, 50 }, { true, 0x1FF, 1, 0x00 } };
    for (auto& c : cases) {
        std::string id = std::string("timer.restart_on_lsb_write_tins") + (c.tins ? "1" : "0");
        arm(ctx, c.tins, c.reload);
        ctx.lines(c.lines_before);
        ctx.write(0xFF95, c.lsb);
        run_until(ctx, EvType::TIMER, 2);
        ctx.check_event(id + "_ev1", EvType::TIMER, 0);
        ctx.check_event(id + "_ev2", EvType::TIMER, 1);
    }
}

// Rick Dangerous's sample FIRQ handler switches the MMU task (INIT1 TR)
// every interrupt without touching TINS. Toggle TR on every line and compare
// the first ten fire ticks with an untouched run in the same model.
SCENARIO(timer, init1_tr_only_keeps_running) {
    const unsigned RELOAD = 596, N = 10, FREE_LINES = 3, MAX_LINES = 1000;
    const std::string id = "timer.init1_tr_only_keeps_running";

    std::map<GimeModel*, std::vector<int64_t>> untouched;
    arm(ctx, true, RELOAD);
    run_until(ctx, EvType::TIMER, N);
    for (GimeModel* m : ctx.models)
        for (unsigned i = 0; i < N; i++)
            untouched[m].push_back(ctx.event_tick(m, EvType::TIMER, i));

    arm(ctx, true, RELOAD);
    ctx.lines(FREE_LINES);
    for (unsigned l = 0; l < MAX_LINES && !have_events(ctx, EvType::TIMER, N); l++) {
        ctx.write(0xFF91, init1(true) | 0x01);       // TR=1, TINS unchanged
        ctx.write(0xFF91, init1(true));              // TR=0
        ctx.lines(1);
    }
    check_timer_evs(ctx, id);
    // Largest shift of any of the ten fire ticks against the untouched run.
    ctx.check(id + "_max_shift", [&](GimeModel& m) {
        int64_t worst = 0;
        for (unsigned i = 0; i < N; i++) {
            int64_t d = llabs(ctx.event_tick(&m, EvType::TIMER, i) - untouched[&m][i]);
            if (d > worst) worst = d;
        }
        return worst;
    }, CAP_TIMER_EV);
}

// A change of TINS restarts the timer from its registers; events are
// measured from the INIT1 write.
SCENARIO(timer, tins_change_restarts) {
    const unsigned RELOAD = 300, LINES_BEFORE = 5;
    for (unsigned from = 0; from < 2; from++) {
        std::string id = std::string("timer.tins_change_restarts_") + (from ? "1to0" : "0to1");
        arm(ctx, from != 0, RELOAD);
        ctx.lines(LINES_BEFORE);
        ctx.write(0xFF91, init1(from == 0));
        ctx.drain();
        ctx.anchor();
        run_until(ctx, EvType::TIMER, 2);
        ctx.check_event(id + "_ev1", EvType::TIMER, 0);
        ctx.check_event(id + "_ev2", EvType::TIMER, 1);
    }
}

// TMR enabled in $FF92 (IRQ) or $FF93 (FIRQ) after arming, so the enable
// write never sees counter == 0. Checks the rising edge of the output, the
// status read (TMR bit, cleared by the read) and the output after the read.
static void tmr_flag(Ctx& ctx, bool firq) {
    const uint16_t reg = firq ? 0xFF93 : 0xFF92;
    const EvType rise = firq ? EvType::FIRQ_RISE : EvType::IRQ_RISE;
    struct { bool tins; unsigned reload, lines; } cases[] = {
        { false, 100, 101 },     // fires once: XRoar at +10+100 lines, ours at +101 lines
        { true, 596, 3 } };      // fires once: XRoar at +2388 ticks, ours at +3 lines
    for (auto& c : cases) {
        std::string id = std::string("timer.tmr_flag_") + (firq ? "firq" : "irq") +
                         "_tins" + (c.tins ? "1" : "0");
        arm(ctx, c.tins, c.reload, (uint8_t)(0x40 | (firq ? 0x10 : 0x20)));
        ctx.write(reg, 0x20);                        // TMR
        ctx.lines(c.lines);
        ctx.check_event(id + "_rise", rise, 0);
        ctx.check(id + "_line", [firq](GimeModel& m) {
            return (int64_t)(firq ? m.firq() : m.irq()); }, CAP_IRQ);
        ctx.check_read(id + "_read", reg, 0x3F);
        ctx.check(id + "_line_after_read", [firq](GimeModel& m) {
            return (int64_t)(firq ? m.firq() : m.irq()); }, CAP_IRQ);
        ctx.check_read(id + "_read2", reg, 0x3F);
    }
}

SCENARIO(timer, tmr_flag_irq) { tmr_flag(ctx, false); }
SCENARIO(timer, tmr_flag_firq) { tmr_flag(ctx, true); }
