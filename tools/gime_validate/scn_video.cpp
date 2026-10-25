#include "scenario.h"
#include <stdio.h>
#include <algorithm>

// Video timing scenarios (1987 GIME). Every value is relative to an event
// or a write in the same model and run (ruling R7), never an absolute tick.
//
// Registers: INIT0 $FF90 bit 7 COCO (0 = GIME modes), bit 6 MMU, bit 5 IEN.
// VMODE $FF98 bit 7 BP (graphics), bits 2-0 LPR. VRES $FF99 bits 6-5 LPF
// (0 = 192 lines, 1 = 199, 2 = no bottom border / "infinite", 3 = 225),
// bits 4-2 HRES, bits 1-0 CRES: tcc1014.cpp case 9 and XRoar tcc1014.c case 9
// both decode LPF = (val >> 5) & 3. $FF9C VSC, $FF9D/$FF9E vertical offset
// Y = MSB << 11 | LSB << 3.
//
// Event-to-event distances whose two edges are produced at the same point of
// a line (FS_FALL to FS_FALL, FS_FALL to FS_RISE, FS_FALL to VBORD: all at
// XRoar's vb_irq point, all at the line end in ours) carry no quantisation
// error, so they are compared exactly (tolerance 0). HBORD relative to HS
// fall is a sub-line position and uses the models' sync resolution.

namespace {

const uint8_t INIT0 = 0x40;          // MMU on, COCO off
const uint8_t VRES_GFX = 0x1E;       // HRES 7 (160 bytes/row), CRES 2: 320x16 colours
const uint8_t VRES_TXT = 0x1C;       // HRES 7 (80 columns), CRES 0: no attributes
const unsigned GFX_BPR = 160;
const uint64_t LINE = 912;
const uint64_t HALF = 456;

// Each byte encodes its own address: the word at an even address A holds
// A >> 3 (big-endian), so the first two bytes rendered on a line give the
// address of the line's first fetch, divided by 8.
uint8_t addr_pattern(uint32_t a) {
    return (a & 1) ? (uint8_t)(a >> 3) : (uint8_t)(a >> 11);
}

void setup(Ctx& ctx, bool gfx, uint8_t vres, uint8_t lpr = 0, uint8_t irq_en = 0) {
    ctx.reset();
    ctx.fill_ram(addr_pattern);
    ctx.write(0xFF90, INIT0 | (irq_en ? 0x20 : 0x00));
    ctx.write(0xFF98, (uint8_t)((gfx ? 0x80 : 0x00) | lpr));
    ctx.write(0xFF99, vres);
    ctx.write(0xFF9D, 0x20);                         // Y = $10000
    ctx.write(0xFF9E, 0x00);
    for (unsigned i = 0; i < 16; i++) ctx.write((uint16_t)(0xFFB0 + i), (uint8_t)i);
    if (irq_en) ctx.write(0xFF92, irq_en);
    ctx.drain();
}

uint32_t sync_tol(Ctx& ctx) {
    uint32_t t = 0;
    for (GimeModel* m : ctx.models)
        if (m->caps() & CAP_SYNC_EV) t = std::max(t, m->resolution(CAP_SYNC_EV));
    return t;
}

// Tick of the first event of type t after tick `after` (both relative to the
// model's anchor); -1 if none.
int64_t first_after(Ctx& ctx, GimeModel* m, EvType t, int64_t after) {
    for (unsigned n = 0;; n++) {
        int64_t e = ctx.event_tick(m, t, n);
        if (e < 0) return -1;
        if (e > after) return e;
    }
}

// Tick of the last event of type t at or before tick `at`; -1 if none.
int64_t last_at_or_before(Ctx& ctx, GimeModel* m, EvType t, int64_t at) {
    int64_t r = -1;
    for (unsigned n = 0;; n++) {
        int64_t e = ctx.event_tick(m, t, n);
        if (e < 0 || e > at) return r;
        r = e;
    }
}

const char* mode_name(bool gfx) { return gfx ? "gfx" : "txt"; }

// ---- per-line render traces ----

// Address / 8 of the first byte fetched on the last finished line, decoded
// from its colours (identity palette, VRES_GFX: two 4-bit pixels per byte).
// Works at any horizontal sample rate: colours.size() / 160 samples per byte.
// -1 if the model rendered no active area for that line.
int64_t first_fetch(GimeModel& m) {
    std::vector<uint8_t> c;
    int px;
    if (!m.last_line(c, px) || c.size() < 2 * GFX_BPR) return -1;
    size_t tpb = c.size() / GFX_BPR, half = tpb / 2;
    unsigned b0 = ((c[0] & 15u) << 4) | (c[half] & 15u);
    unsigned b1 = ((c[tpb] & 15u) << 4) | (c[tpb + half] & 15u);
    return (int64_t)((b0 << 8) | b1);
}

int64_t line_hash(GimeModel& m) {
    std::vector<uint8_t> c;
    int px;
    if (!m.last_line(c, px)) return -1;
    uint64_t h = 1469598103934665603ull;                     // FNV-1a 64
    for (uint8_t v : c) { h ^= v; h *= 1099511628211ull; }
    return (int64_t)(h & 0x7FFFFFFFFFFFFFFFull);
}

struct Trace {
    std::vector<int64_t> v;                  // one value per line
    std::vector<std::vector<uint8_t>> px;    // that line's colours (if kept)
};
using Traces = std::map<GimeModel*, Trace>;

// Advance n lines; after each, record f(model) for every model with
// CAP_RENDER. Entry 0 is the line that was in progress when tracing began.
Traces trace(Ctx& ctx, unsigned n, int64_t (*f)(GimeModel&), bool keep_px = false) {
    Traces t;
    for (unsigned i = 0; i < n; i++) {
        ctx.lines(1);
        for (GimeModel* m : ctx.models) {
            if (!(m->caps() & CAP_RENDER)) continue;
            t[m].v.push_back(f(*m));
            if (keep_px) {
                std::vector<uint8_t> c;
                int px;
                if (!m->last_line(c, px)) c.clear();
                t[m].px.push_back(c);
            }
        }
    }
    return t;
}

int64_t first_diff(const Trace& a, const Trace& b) {
    for (size_t i = 0; i < a.v.size() && i < b.v.size(); i++)
        if (a.v[i] != b.v[i]) return (int64_t)i;
    return -1;
}

// Run the same setup twice, once without and once with `op` at `offset`
// ticks after an FS fall; trace both from the op's position. Checks:
//   <id>        lines from the op to the first line that differs (0 = the
//               line the op lands in), -1 if none within n lines
//   <id>_value  the traced value on that line in the run with the op
void latch_check(Ctx& ctx, const std::string& id, void (*prep)(Ctx&), uint64_t offset,
                 void (*op)(Ctx&), unsigned n) {
    Traces base, with;
    for (int pass = 0; pass < 2; pass++) {
        prep(ctx);
        ctx.sync_after(EvType::FS_FALL, LINE);       // first FS fall after setup
        ctx.drain();
        ctx.sync_after(EvType::FS_FALL, offset);     // the next one, + offset
        if (pass) op(ctx);
        (pass ? with : base) = trace(ctx, n, first_fetch);
    }
    ctx.check(id, [&](GimeModel& m) { return first_diff(base[&m], with[&m]); }, CAP_RENDER);
    ctx.check(id + "_value", [&](GimeModel& m) {
        int64_t i = first_diff(base[&m], with[&m]);
        return i < 0 ? (int64_t)-1 : with[&m].v[(size_t)i]; }, CAP_RENDER);
}

struct When { const char* name; uint64_t offset; };
// Offsets after FS fall: during vertical sync (before the B reload 7 lines
// after FS fall), in the top border after the reload, and mid active area.
const When WHENS[] = { { "vsync", 2 * LINE + HALF },
                       { "top_border", 20 * LINE + HALF },
                       { "active", 100 * LINE + HALF } };
const unsigned LATCH_LINES = 340;            // the rest of the field and most of the next

void prep_gfx(Ctx& ctx) { setup(ctx, true, VRES_GFX); }
void prep_gfx_lpr3(Ctx& ctx) { setup(ctx, true, VRES_GFX, 3); }   // 8 lines per row

} // namespace

// Field length (FS fall to FS fall) for each LPF, in text and graphics.
SCENARIO(video, lines_per_field) {
    for (unsigned lpf = 0; lpf < 4; lpf++)
        for (unsigned gfx = 0; gfx < 2; gfx++) {
            char id[64];
            snprintf(id, sizeof id, "video.lines_per_field_lpf%u_%s", lpf, mode_name(gfx));
            setup(ctx, gfx, (uint8_t)((lpf << 5) | (gfx ? VRES_GFX : VRES_TXT)));
            ctx.lines(3 * 263 + 20);
            ctx.check(id, [&](GimeModel& m) {
                int64_t a = ctx.event_tick(&m, EvType::FS_FALL, 1);
                int64_t b = ctx.event_tick(&m, EvType::FS_FALL, 2);
                return (a < 0 || b < 0) ? (int64_t)-1 : b - a; }, CAP_SYNC_EV);
        }
}

// FS low time: FS fall to the following FS rise.
SCENARIO(video, fs_low) {
    setup(ctx, true, VRES_GFX);
    ctx.lines(2 * 263 + 20);
    ctx.check("video.fs_low_ticks", [&](GimeModel& m) {
        int64_t f = ctx.event_tick(&m, EvType::FS_FALL, 1);
        int64_t r = f < 0 ? -1 : first_after(ctx, &m, EvType::FS_RISE, f);
        return r < 0 ? (int64_t)-1 : r - f; }, CAP_SYNC_EV);
}

// VBORD interrupt (IRQ rise with only VBORD enabled) relative to the FS fall
// that starts its field. -1: no VBORD in that field (LPF 2).
SCENARIO(video, vbord_line) {
    for (unsigned lpf = 0; lpf < 4; lpf++)
        for (unsigned gfx = 0; gfx < 2; gfx++) {
            char id[64];
            snprintf(id, sizeof id, "video.vbord_line_lpf%u_%s", lpf, mode_name(gfx));
            setup(ctx, gfx, (uint8_t)((lpf << 5) | (gfx ? VRES_GFX : VRES_TXT)), 0, 0x08);
            ctx.sync_after(EvType::FS_FALL, 2 * LINE + HALF);
            std::map<GimeModel*, int64_t> fs;
            for (GimeModel* m : ctx.models) fs[m] = ctx.event_tick(m, EvType::FS_FALL, 0);
            ctx.read(0xFF92);                        // clear an earlier VBORD
            ctx.drain();
            ctx.lines(300);
            ctx.check(id, [&](GimeModel& m) {
                int64_t r = ctx.event_tick(&m, EvType::IRQ_RISE, 0);
                if (r < 0 || fs[&m] < 0) return (int64_t)-1;
                // the rise must belong to this field: before the next FS fall
                int64_t nf = ctx.event_tick(&m, EvType::FS_FALL, 0);
                if (nf >= 0 && r > nf) return (int64_t)-1;
                return r - fs[&m]; }, CAP_SYNC_EV);
        }
}

// HBORD interrupt (IRQ rise with only HBORD enabled) relative to the HS fall
// that starts its line, mid active area and in the vertical sync lines.
SCENARIO(video, hbord_tick) {
    struct { const char* where; uint64_t offset; } places[] = {
        { "", 100 * LINE + HALF }, { "_vsync", 2 * LINE + HALF } };
    for (auto& p : places)
        for (unsigned gfx = 0; gfx < 2; gfx++) {
            std::string id = std::string("video.hbord_tick") + p.where + "_" + mode_name(gfx);
            setup(ctx, gfx, gfx ? VRES_GFX : VRES_TXT, 0, 0x10);
            ctx.sync_after(EvType::FS_FALL, p.offset);
            // Events are kept (no drain): the HS fall that starts the rise's
            // line can precede the clearing read.
            std::map<GimeModel*, int64_t> read_at;
            for (GimeModel* m : ctx.models) read_at[m] = (int64_t)m->now();   // anchor is 0
            ctx.read(0xFF92);                        // clear the previous HBORD
            ctx.lines(2);
            ctx.check(id, [&](GimeModel& m) {
                int64_t r = first_after(ctx, &m, EvType::IRQ_RISE, read_at[&m]);
                int64_t h = r < 0 ? -1 : last_at_or_before(ctx, &m, EvType::HS_FALL, r);
                return h < 0 ? (int64_t)-1 : r - h; }, CAP_SYNC_EV, sync_tol(ctx));
        }
}

// VSC written mid-field (active area). With LPR 3 (rows of 8 lines) VSC 4
// shortens the field's first row, which moves the first B advance.
SCENARIO(video, vsc_latch) {
    latch_check(ctx, "video.vsc_latch", prep_gfx_lpr3, 100 * LINE + HALF,
                [](Ctx& c) { c.write(0xFF9C, 4); }, LATCH_LINES);
}

// Vertical offset written at three points of the field: MSB ($FF9D $20->$30,
// Y $10000->$18000) and LSB ($FF9E $00->$10, Y $10000->$10080) separately.
SCENARIO(video, voffset_latch) {
    for (const When& w : WHENS) {
        latch_check(ctx, std::string("video.voffset_latch_") + w.name, prep_gfx, w.offset,
                    [](Ctx& c) { c.write(0xFF9D, 0x30); }, LATCH_LINES);
        latch_check(ctx, std::string("video.voffset_latch_lsb_") + w.name, prep_gfx, w.offset,
                    [](Ctx& c) { c.write(0xFF9E, 0x10); }, LATCH_LINES);
    }
}

// VMODE written mid-line in the active area (graphics -> text): does the
// line in progress change, or the next? Compared by render hash per line.
//   _first_px: on the first changed line, the first changed sample, scaled
//   to 14 MHz ticks from the start of the active area (640 per line here).
SCENARIO(video, reg_effect_same_line) {
    Traces base, with;
    for (int pass = 0; pass < 2; pass++) {
        prep_gfx(ctx);
        ctx.sync_after(EvType::FS_FALL, LINE);
        ctx.drain();
        ctx.sync_after(EvType::FS_FALL, 100 * LINE);
        ctx.drain();
        ctx.sync_after(EvType::HS_FALL, LINE + 400); // 400 ticks into a line
        if (pass) ctx.write(0xFF98, 0x00);
        (pass ? with : base) = trace(ctx, 4, line_hash, true);
    }
    ctx.check("video.reg_effect_same_line",
              [&](GimeModel& m) { return first_diff(base[&m], with[&m]); }, CAP_RENDER);
    ctx.check("video.reg_effect_same_line_first_px", [&](GimeModel& m) {
        int64_t i = first_diff(base[&m], with[&m]);
        if (i < 0) return (int64_t)-1;
        const std::vector<uint8_t>& a = base[&m].px[(size_t)i];
        const std::vector<uint8_t>& b = with[&m].px[(size_t)i];
        size_t n = std::min(a.size(), b.size());
        for (size_t k = 0; k < n; k++)
            if (a[k] != b[k]) return (int64_t)(k * 640 / n);
        return (int64_t)-1; }, CAP_RENDER);
}
