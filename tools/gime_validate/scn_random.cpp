#include "scenario.h"
#include <random>
#include <stdio.h>

// Randomized runs: for each area, N runs of 1-20 random legal register writes
// (addresses from that area's register set), a random advance of 0-3 lines,
// then the area's scripted observations on one fixed probe set. Ids are
// random.<area>.<k>.<observation>. Each area has its own generator, seeded
// once with seed ^ fnv(area), so adding or reordering areas doesn't shift the
// others' sequences. Every draw is made into a named local, in a fixed order,
// never inside a call's argument list (whose evaluation order is unspecified),
// so a seed gives the same sequence on any compiler.

namespace {

uint64_t fnv(const char* s) {
    uint64_t h = 1469598103934665603ull;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 1099511628211ull; }
    return h;
}

struct Rng {
    std::mt19937 g;
    unsigned below(unsigned n) { return (unsigned)(g() % n); }
    uint8_t byte() { return (uint8_t)g(); }
};

// Apply 1-20 writes to addresses drawn from regs[0..nregs); $FF98 always gets H50
// (bit 3) cleared.
void random_writes(Ctx& ctx, Rng& r, const uint16_t* regs, unsigned nregs) {
    unsigned n = 1 + r.below(20);
    for (unsigned i = 0; i < n; i++) {
        unsigned which = r.below(nregs);
        uint8_t val = r.byte();
        if (regs[which] == 0xFF98) val &= (uint8_t)~0x08;   // H50 = 0: the suite is NTSC-only (drawn, then masked)
        ctx.write(regs[which], val);
    }
}

uint16_t range_list(uint16_t* out, unsigned lo, unsigned hi, unsigned at) {
    for (unsigned a = lo; a <= hi; a++) out[at++] = (uint16_t)a;
    return (uint16_t)at;
}

uint8_t ram_pattern(uint32_t a) { return (uint8_t)((a * 37) ^ (a >> 8)); }

std::string rid(const char* area, unsigned k, const char* what) {
    char b[96];
    snprintf(b, sizeof b, "random.%s.%u.%s", area, k, what);
    return b;
}

// regs: every GIME register that isn't an interrupt enable or the timer
// (those are the irq and timer areas). Probe: read each back.
void area_regs(Ctx& ctx, Rng& r, unsigned k) {
    uint16_t w[64];
    unsigned n = 0;
    n = range_list(w, 0xFF90, 0xFF91, n);
    n = range_list(w, 0xFF98, 0xFF9F, n);
    n = range_list(w, 0xFFA0, 0xFFBF, n);
    ctx.reset();
    random_writes(ctx, r, w, n);
    ctx.lines(r.below(4));
    for (unsigned i = 0; i < n; i++) {
        char b[24];
        snprintf(b, sizeof b, "read_%04X", w[i]);
        ctx.check_read(rid("regs", k, b), w[i], regs_read_mask(w[i]));
    }
}

// mmu: INIT0 (MMUEN, MC), INIT1 (TR), the 16 MMU registers, SAM TY.
// Probe: Z and S at one address in each slot plus $FE00 and $FEFF.
void area_mmu(Ctx& ctx, Rng& r, unsigned k) {
    uint16_t w[24];
    unsigned n = 0;
    n = range_list(w, 0xFF90, 0xFF91, n);
    n = range_list(w, 0xFFA0, 0xFFAF, n);
    w[n++] = 0xFFDE;
    w[n++] = 0xFFDF;
    ctx.reset();
    random_writes(ctx, r, w, n);
    ctx.lines(r.below(4));
    uint16_t probes[10];
    for (unsigned s = 0; s < 8; s++) probes[s] = (uint16_t)(s << 13 | 0x123);
    probes[8] = 0xFE00;
    probes[9] = 0xFEFF;
    for (uint16_t a : probes) {
        char b[24];
        snprintf(b, sizeof b, "decode_%04X", a);
        std::string id = rid("mmu", k, b);
        ctx.check_opt(id + "_Z", [a](GimeModel& m) -> std::optional<int64_t> {
            Decode d = m.decode(a);
            if (!d.z_valid) return std::nullopt;
            return (int64_t)d.Z; }, CAP_DECODE);
        ctx.check(id + "_S", [a](GimeModel& m) { return (int64_t)m.decode(a).S; }, CAP_DECODE);
    }
}

// timer: INIT0, INIT1, the two timer registers. Probe: the first two fire
// ticks from the point after the random advance, over a fixed window, and
// with TINS=0 at the end also their line indices (_line). The
// reload's MSB is mostly 0 so that most timers fire inside the window.
void area_timer(Ctx& ctx, Rng& r, unsigned k) {
    ctx.reset();
    ctx.write(0xFF90, 0x40);
    bool tins = false;                                    // INIT1 TINS after the writes
    unsigned n = 1 + r.below(20);
    for (unsigned i = 0; i < n; i++) {
        unsigned kind = r.below(4);
        switch (kind) {
        case 0: {                                         // TINS, TR
            uint8_t b = r.byte();
            ctx.write(0xFF91, (uint8_t)(b & 0x21));
            tins = (b & 0x20) != 0;
            break;
        }
        case 1: { uint8_t b = r.byte(); ctx.write(0xFF95, b); break; }
        case 2: {
            unsigned nonzero = r.below(4);                // 3 in 4: MSB 0
            uint8_t b = nonzero ? 0 : r.byte();           // drawn only when not zeroed
            ctx.write(0xFF94, (uint8_t)(b & 0x0F));
            break;
        }
        default: { uint8_t b = r.byte(); ctx.write(0xFF90, (uint8_t)(0x40 | (b & 0x07))); break; }
        }
    }
    ctx.lines(r.below(4));
    ctx.drain();
    ctx.anchor();
    ctx.lines(300);
    ctx.check_event(rid("timer", k, "ev1"), EvType::TIMER, 0);
    ctx.check_event(rid("timer", k, "ev2"), EvType::TIMER, 1);
    if (!tins) {                                          // a line-rate timer
        // The anchor is on a line boundary: on ours' HS fall (drained, not
        // counted) and 10 ticks before XRoar's (counted), which would put the
        // line indices one apart for no timer difference. Count HS falls from
        // half a line on instead (the phase rule of scn_timer.cpp).
        ctx.check_event_line(rid("timer", k, "ev1"), EvType::TIMER, 0, 456);
        ctx.check_event_line(rid("timer", k, "ev2"), EvType::TIMER, 1, 456);
    }
}

// irq: INIT0 (IEN, FEN), the two enable registers and the IL lines.
// Probe: both outputs, both status reads, both outputs after the reads.
void area_irq(Ctx& ctx, Rng& r, unsigned k) {
    ctx.reset();
    unsigned n = 1 + r.below(20);
    for (unsigned i = 0; i < n; i++) {
        unsigned kind = r.below(4);
        switch (kind) {
        case 0: { uint8_t b = r.byte(); ctx.write(0xFF90, (uint8_t)(0x40 | (b & 0x30))); break; }
        case 1: { uint8_t b = r.byte(); ctx.write(0xFF92, b); break; }
        case 2: { uint8_t b = r.byte(); ctx.write(0xFF93, b); break; }
        default: {
            unsigned line = r.below(3);
            bool level = r.below(2) != 0;
            ctx.set_il(line, level);
            ctx.read(0x0000);                         // IL is sampled on an access
            break;
        }
        }
    }
    ctx.lines(r.below(4));
    ctx.check(rid("irq", k, "irq"),  [](GimeModel& m) { return (int64_t)m.irq(); },  CAP_IRQ);
    ctx.check(rid("irq", k, "firq"), [](GimeModel& m) { return (int64_t)m.firq(); }, CAP_IRQ);
    ctx.check_read(rid("irq", k, "read_ff92"), 0xFF92, 0x3F);
    ctx.check_read(rid("irq", k, "read_ff93"), 0xFF93, 0x3F);
    ctx.check(rid("irq", k, "irq_after"),  [](GimeModel& m) { return (int64_t)m.irq(); },  CAP_IRQ);
    ctx.check(rid("irq", k, "firq_after"), [](GimeModel& m) { return (int64_t)m.firq(); }, CAP_IRQ);
}

// video: VMODE, VRES, vertical scroll and offset, horizontal offset. COCO
// stays off. Probe: the field length (FS fall to FS fall) and the FS low time.
void area_video(Ctx& ctx, Rng& r, unsigned k) {
    static const uint16_t w[] = { 0xFF98, 0xFF99, 0xFF9C, 0xFF9D, 0xFF9E, 0xFF9F };
    ctx.reset();
    ctx.fill_ram(ram_pattern);
    ctx.write(0xFF90, 0x40);
    random_writes(ctx, r, w, sizeof w / sizeof w[0]);
    ctx.lines(r.below(4));
    ctx.drain();
    ctx.lines(3 * 263 + 20);
    ctx.check(rid("video", k, "field_ticks"), [&](GimeModel& m) {
        int64_t a = ctx.event_tick(&m, EvType::FS_FALL, 1);
        int64_t b = ctx.event_tick(&m, EvType::FS_FALL, 2);
        return (a < 0 || b < 0) ? (int64_t)-1 : b - a; }, CAP_SYNC_EV);
    ctx.check(rid("video", k, "fs_low_ticks"), [&](GimeModel& m) {
        int64_t f = ctx.event_tick(&m, EvType::FS_FALL, 1);
        if (f < 0) return (int64_t)-1;
        for (unsigned n = 0;; n++) {
            int64_t e = ctx.event_tick(&m, EvType::FS_RISE, n);
            if (e < 0) return (int64_t)-1;
            if (e > f) return e - f;
        } }, CAP_SYNC_EV);
}

// render: INIT0 COCO bit (VDG compatible modes), VMODE, VRES, $FF9F and the
// palette. After the random writes INIT0 is rewritten as $40 or $C0 (COCO set
// half the time; MMUEN stays on, the other bits are as the setup left them).
// With COCO set the VDG mode comes from the SAM V bits (0 after reset, never
// written here) and PIA1 B, which is set to the alpha mode like scripted
// render.vdg_alpha so that every model starts from the same state. Probe: render_observe() (line hashes, active line count, left border).
void area_render(Ctx& ctx, Rng& r, unsigned k) {
    static const uint16_t w[] = { 0xFF98, 0xFF99, 0xFF9F, 0xFF9D, 0xFF9E,
                                  0xFFB0, 0xFFB1, 0xFFB2, 0xFFB3, 0xFFB4, 0xFFB5, 0xFFB6, 0xFFB7,
                                  0xFFB8, 0xFFB9, 0xFFBA, 0xFFBB, 0xFFBC, 0xFFBD, 0xFFBE, 0xFFBF };
    ctx.reset();
    ctx.fill_ram(ram_pattern);
    ctx.write(0xFF90, 0x40);
    random_writes(ctx, r, w, sizeof w / sizeof w[0]);
    bool coco = r.below(2) != 0;
    if (coco) {
        ctx.write(0xFF23, 0x04);                          // CRB bit 2: $FF22 is the data register
        ctx.write(0xFF22, 0x00);                          // alpha, CSS 0
        ctx.write(0xFF90, 0xC0);
    } else {
        ctx.write(0xFF90, 0x40);
    }
    ctx.lines(r.below(4));
    ctx.drain();
    char b[32];
    snprintf(b, sizeof b, "random.render.%u", k);
    render_observe_after_setup(ctx, b);
}

struct Area { const char* name; void (*fn)(Ctx&, Rng&, unsigned); };
const Area AREAS[] = {
    { "regs", area_regs }, { "mmu", area_mmu }, { "timer", area_timer },
    { "irq", area_irq }, { "video", area_video }, { "render", area_render } };

} // namespace

void run_random(Ctx& ctx, uint32_t seed, unsigned n) {
    for (const Area& a : AREAS) {
        Rng r;
        r.g.seed((uint32_t)((uint64_t)seed ^ fnv(a.name)));
        ctx.scenario = std::string("random.") + a.name;
        for (unsigned k = 0; k < n; k++) a.fn(ctx, r, k);
    }
}
