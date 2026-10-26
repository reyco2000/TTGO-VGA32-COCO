#include "scenario.h"
#include <stdio.h>

// Rendering scenarios (1987 GIME): the colours (6-bit) of whole active-area
// lines, per mode, hashed with line_hash(). Lines are matched by their
// position in the field's active area (display line y = the y-th line a model
// reports as active after the FS fall), never by absolute tick, so the two
// models' different vertical/horizontal phase doesn't matter.
//
// Palette $FFB0+i = i*4+1: sixteen distinct 6-bit colours, so a colour names
// its palette slot. RAM (addr*37) ^ (addr>>8): every byte value, varied per row.
//
// Register reference: INIT0 $FF90 bit 7 COCO, bit 6 MMU. VMODE $FF98 bit 7 BP
// (graphics), bits 2-0 LPR. VRES $FF99 bits 4-2 HRES, bits 1-0 CRES (text:
// bit 0 = attributes). Graphics HRES 0-7 = 16, 20, 32, 40, 64, 80, 128, 160
// bytes per row; text HRES 0/1/4/5 = 32/40/64/80 columns (tcc1014.cpp
// VRES_HRES_BPR / VRES_HRES_BPR_TEXT). $FF9F bit 7 HVEN, bits 6-0 X.
// VDG compatible mode: SAM V0-V2 by $FFC0-$FFC5 (even clears, odd sets), and
// the PIA1B video lines GnA/GM2-0/CSS (bits 7-3 of $FF22), which both GIMEs
// snoop from the CPU write once $FF23 bit 2 selects the data register.

namespace {

const uint64_t LINE = 912;
const unsigned FIELD_SCAN = 250;    // lines traced from FS fall + 1: past the last active line
const unsigned YS[] = { 0, 1, 7, 8, 100 };
const unsigned LAST_Y = 191;        // every mode here has 192 active lines (LPF 0 / COCO)

uint8_t ram_pattern(uint32_t a) { return (uint8_t)((a * 37) ^ (a >> 8)); }
const uint8_t BRDR = 0x2A;          // GIME-mode border ($FF9A): not one of the palette's i*4+1
const unsigned BORDER_Y = 100;      // display line whose border colour is compared

struct Mode {
    std::string name;
    bool coco;
    uint8_t vmode, vres, ff9f;      // GIME modes
    uint8_t sam_v, pia1b;           // VDG compatible modes
};

void setup(Ctx& ctx, const Mode& m) {
    ctx.reset();
    ctx.fill_ram(ram_pattern);
    for (unsigned i = 0; i < 16; i++) ctx.write((uint16_t)(0xFFB0 + i), (uint8_t)(i * 4 + 1));
    if (m.coco) {
        ctx.write(0xFF90, 0xC0);                     // COCO, MMU on
        for (unsigned k = 0; k < 3; k++)
            ctx.write((uint16_t)(0xFFC0 + 2 * k + ((m.sam_v >> k) & 1)), 0);
        // Both adapters' write() is a GIME mem_cycle, which snoops these:
        // ours in tcc1014_mem_cycle (tcc1014.cpp:283-292) as on the machine,
        // where every CPU access goes through it (nothing in src/ calls
        // tcc1014_snoop_pia1b); XRoar in tcc1014.c:761-769. No PIA modelled.
        ctx.write(0xFF23, 0x04);                     // CRB bit 2: $FF22 is the data register
        ctx.write(0xFF22, m.pia1b);
    } else {
        ctx.write(0xFF90, 0x40);                     // MMU on, COCO off
        ctx.write(0xFF98, m.vmode);
        ctx.write(0xFF99, m.vres);
        ctx.write(0xFF9D, 0x20);                     // Y = $10000
        ctx.write(0xFF9E, 0x00);
        ctx.write(0xFF9F, m.ff9f);
        ctx.write(0xFF9A, BRDR);
    }
    ctx.drain();
}

std::vector<Mode> modes() {
    static const unsigned bpr[8] = { 16, 20, 32, 40, 64, 80, 128, 160 };
    std::vector<Mode> v;
    char n[64];
    // Every HRES x CRES: the GIME accepts all of them (CRES 3 renders as
    // 16 colours in both models' tables); the width is set by HRES alone.
    for (unsigned h = 0; h < 8; h++)
        for (unsigned c = 0; c < 4; c++) {
            snprintf(n, sizeof n, "gfx_bpr%u_cres%u", bpr[h], c);
            v.push_back({ n, false, 0x80, (uint8_t)(h << 2 | c), 0x00, 0, 0 });
        }
    static const unsigned cols[4] = { 32, 40, 64, 80 }, thres[4] = { 0, 1, 4, 5 };
    for (unsigned i = 0; i < 4; i++)
        for (unsigned attr = 0; attr < 2; attr++) {
            snprintf(n, sizeof n, "txt%u_%s", cols[i], attr ? "attr" : "noattr");
            // LPR 3: 8 lines per character row, so the font rows vary
            v.push_back({ n, false, 0x03, (uint8_t)(thres[i] << 2 | attr), 0x00, 0, 0 });
        }
    static const unsigned xs[3] = { 0, 1, 64 };
    for (unsigned x : xs) {
        snprintf(n, sizeof n, "hven_x%u", x);
        v.push_back({ n, false, 0x80, 0x1E, (uint8_t)(0x80 | x), 0, 0 });  // 160 bytes, 16 colours
    }
    v.push_back({ "vdg_alpha", true, 0, 0, 0, 0, 0x00 });
    v.push_back({ "vdg_alpha_css", true, 0, 0, 0, 0, 0x08 });
    v.push_back({ "vdg_cg1", true, 0, 0, 0, 1, 0x80 });       // GnA, GM 000
    v.push_back({ "vdg_cg6", true, 0, 0, 0, 6, 0xE0 });       // GnA, GM 110
    v.push_back({ "vdg_rg6", true, 0, 0, 0, 6, 0xF0 });       // GnA, GM 111 (PMODE 4)
    v.push_back({ "vdg_rg6_css", true, 0, 0, 0, 6, 0xF8 });
    return v;
}

struct Line { bool ok; std::vector<uint8_t> c; int first_px; int border; std::string note; };
using Field = std::map<GimeModel*, std::vector<Line>>;

// The active-area lines of one field, in order, per model with CAP_RENDER.
// A line that was rendered but couldn't be decoded (last_line false with a
// note) is kept, with ok = false, so later lines keep their index.
Field trace_field(Ctx& ctx) {
    ctx.sync_after(EvType::FS_FALL, LINE);           // first FS fall: settle the setup
    ctx.drain();
    ctx.sync_after(EvType::FS_FALL, LINE);           // the next one: a whole field ahead
    Field f;
    for (unsigned i = 0; i < FIELD_SCAN; i++) {
        ctx.lines(1);
        for (GimeModel* m : ctx.models) {
            if (!(m->caps() & CAP_RENDER)) continue;
            Line l;
            l.ok = m->last_line(l.c, l.first_px);
            l.border = l.ok ? m->last_line_border() : -1;
            if (!l.ok) {
                l.note = m->last_line_note();
                if (l.note.empty()) continue;        // not an active line
                l.c.clear();
            }
            f[m].push_back(l);
        }
    }
    return f;
}

const Line* line_at(Field& f, GimeModel* m, unsigned y) {
    auto it = f.find(m);
    if (it == f.end() || y >= it->second.size()) return nullptr;
    return &it->second[y];
}

GimeModel* model(Ctx& ctx, const char* name) {
    for (GimeModel* m : ctx.models) if (std::string(m->name()) == name) return m;
    return nullptr;
}

// First pixel where line a differs from line b (or their widths, if those
// differ); empty if they are the same or either is missing.
std::string first_diff(const char* an, const Line* a, const char* bn, const Line* b) {
    if (!a || !b || !a->ok || !b->ok) return std::string();
    char s[112] = "";
    if (a->c.size() != b->c.size()) {
        snprintf(s, sizeof s, "width %s=%zu %s=%zu", an, a->c.size(), bn, b->c.size());
    } else {
        for (size_t k = 0; k < a->c.size(); k++)
            if (a->c[k] != b->c[k]) {
                snprintf(s, sizeof s, "first diff px %zu (%s=%u %s=%u)", k, an, a->c[k], bn, b->c[k]);
                break;
            }
    }
    return s;
}

// Note for line y: ours' decoder note, and the first pixel where ours, and
// then mame, differ from xroar.
std::string diff_note(Ctx& ctx, Field& f, unsigned y) {
    const Line* o = line_at(f, model(ctx, "ours"), y);
    const Line* x = line_at(f, model(ctx, "xroar"), y);
    const Line* m = line_at(f, model(ctx, "mame"), y);
    std::string s;
    if (o && !o->note.empty()) s = "ours: " + o->note;
    for (const std::string& d : { first_diff("ours", o, "xroar", x), first_diff("mame", m, "xroar", x) })
        if (!d.empty()) s += (s.empty() ? "" : "; ") + d;
    return s;
}

// Observations on the field after the setup already applied to ctx:
//   <pre>.line<y>_hash  hash of display lines 0, 1, 7, 8, 100 and the
//                               last; absent for a model with no valid line y
//   <pre>.active_lines  how many lines the model reported as active
//                               over the field (catches short/long fields and
//                               missing lines, which the per-line checks skip)
//   <pre>.first_active_px  the left border width (first active pixel
//                               from HS fall); absent for a model with no
//                               valid line 0 or no horizontal position (ours
//                               keeps none)
//   <pre>.border_colour  (with_border) the border colour of display line
//                               BORDER_Y: ours' border_colour as run_line
//                               computes it, the pixel left of the active
//                               area in XRoar and MAME; absent for a model
//                               with no border pixel (MAME on a wide line)
void render_observe(Ctx& ctx, const std::string& pre, bool with_border) {
    Field f = trace_field(ctx);
    std::vector<unsigned> ys(std::begin(YS), std::end(YS));
    ys.push_back(LAST_Y);
    for (unsigned y : ys) {
        char id[96];
        snprintf(id, sizeof id, "%s.line%u_hash", pre.c_str(), y);
        ctx.check_opt(id, [&](GimeModel& m) -> std::optional<int64_t> {
            const Line* l = line_at(f, &m, y);
            if (!l || !l->ok) return std::nullopt;
            return line_hash(l->c); }, CAP_RENDER, 0, diff_note(ctx, f, y));
    }
    ctx.check(pre + ".active_lines", [&](GimeModel& m) {
        auto it = f.find(&m);
        int64_t n = 0;
        if (it != f.end())
            for (const Line& l : it->second) n += l.ok;
        return n; }, CAP_RENDER);
    ctx.check_opt(pre + ".first_active_px",
                  [&](GimeModel& m) -> std::optional<int64_t> {
        const Line* l = line_at(f, &m, 0);
        if (!l || !l->ok || l->first_px < 0) return std::nullopt;
        return l->first_px; }, CAP_RENDER);
    if (with_border)
        ctx.check_opt(pre + ".border_colour", [&](GimeModel& m) -> std::optional<int64_t> {
            const Line* l = line_at(f, &m, BORDER_Y);
            if (!l || !l->ok || l->border < 0) return std::nullopt;
            return l->border; }, CAP_RENDER);
}

} // namespace

// For scn_random.cpp: the same observations, ids prefixed with `pre`.
void render_observe_after_setup(Ctx& ctx, const std::string& pre) { render_observe(ctx, pre, false); }

SCENARIO(render, modes) {
    for (const Mode& md : modes()) {
        setup(ctx, md);
        render_observe(ctx, "render." + md.name, true);
    }
}

// Review focus 5: a top-border line (FS fall + 7 blanking + 20 of the 36
// border lines) has no active area. The check is 1 if the model reports an
// active line there, 0 if not: every model must say 0 (ours renders nothing
// for a border line, and it is never compared as an empty line against
// border colour).
SCENARIO(render, border_line) {
    setup(ctx, { "border", false, 0x80, 0x1E, 0x00, 0, 0 });   // 160 bytes, 16 colours
    ctx.sync_after(EvType::FS_FALL, LINE);
    ctx.drain();
    ctx.sync_after(EvType::FS_FALL, 27 * LINE);
    ctx.check("render.border_line_has_active_line", [](GimeModel& m) {
        std::vector<uint8_t> c;
        int px;
        return (int64_t)m.last_line(c, px); }, CAP_RENDER);
}
