#include "scenario.h"
#include <algorithm>
#include <stdio.h>

static const uint32_t RAM_SIZE = 512 * 1024;

std::vector<Scenario>& scenarios() {
    static std::vector<Scenario> v;
    return v;
}

ScenarioReg::ScenarioReg(const char* area, const char* name, void (*fn)(Ctx&)) {
    scenarios().push_back({ std::string(area) + "." + name, fn });
}

static std::map<GimeModel*, std::vector<uint8_t>>& rams() {   // one RAM per model
    static std::map<GimeModel*, std::vector<uint8_t>> r;
    return r;
}

void Ctx::reset() {
    events_.clear();
    anchors_.clear();
    for (GimeModel* m : models) {
        std::vector<uint8_t>& r = rams()[m];
        r.assign(RAM_SIZE, 0);
        m->reset(variant, r.data(), RAM_SIZE);
    }
}

// Events can also come from a CPU access (a register write that raises an
// interrupt output), not just from advance().
void Ctx::collect(GimeModel* m) {
    std::vector<Event> e = m->drain_events();
    events_[m].insert(events_[m].end(), e.begin(), e.end());
}

void Ctx::write(uint16_t addr, uint8_t val) {
    for (GimeModel* m : models) { m->write(addr, val); collect(m); }
}

void Ctx::read(uint16_t addr) {
    for (GimeModel* m : models) { m->read(addr); collect(m); }
}

void Ctx::set_il(unsigned line, bool level) {
    for (GimeModel* m : models) { m->set_il(line, level); collect(m); }
}

void Ctx::advance(uint64_t ticks) {
    for (GimeModel* m : models) {
        m->advance(ticks);
        collect(m);
    }
}

void Ctx::drain() { events_.clear(); }

void Ctx::fill_ram(uint8_t (*f)(uint32_t)) {
    for (GimeModel* m : models) {
        std::vector<uint8_t>& r = rams()[m];
        for (uint32_t a = 0; a < r.size(); a++) r[a] = f(a);
    }
}

int64_t line_hash(const std::vector<uint8_t>& c) {
    uint64_t h = 1469598103934665603ull;             // FNV-1a 64
    for (uint8_t v : c) { h ^= v; h *= 1099511628211ull; }
    return (int64_t)(h & 0x7FFFFFFFFFFFFFFFull);
}

void Ctx::sync_after(EvType t, uint64_t offset) {
    const unsigned max_lines = 1000;                 // more than three fields
    for (GimeModel* m : models) {
        if (!(m->caps() & CAP_SYNC_EV)) { m->advance(offset); collect(m); continue; }
        int64_t e = event_tick(m, t, 0);
        for (unsigned n = 0; n < max_lines && e < 0; n++) {
            m->advance(912);
            collect(m);
            e = event_tick(m, t, 0);                 // includes the final advance
        }
        if (e < 0) {                                 // never seen: leave it there
            fprintf(stderr, "warning: %s: %s: sync_after: event %d not seen within %u lines\n",
                    scenario.c_str(), m->name(), (int)t, max_lines);
            continue;
        }
        uint64_t target = (uint64_t)e + anchors_[m] + offset;
        if (target > m->now()) m->advance(target - m->now());
        collect(m);
    }
}

void Ctx::anchor() {
    for (GimeModel* m : models) anchors_[m] = m->now();
}

int64_t Ctx::event_tick(GimeModel* m, EvType t, unsigned nth) {
    unsigned n = 0;
    for (const Event& e : events_[m])
        if (e.type == t && n++ == nth) return (int64_t)(e.tick - anchors_[m]);
    return -1;                                       // no such event
}

void Ctx::check(const std::string& id, std::function<int64_t(GimeModel&)> f,
                Cap cap, uint32_t tol_ticks) {
    CheckRecord r;
    r.id = prefix + id;
    r.cap = cap;
    r.tol = tol_ticks;
    for (GimeModel* m : models)
    {
        r.v[m->name()] = (m->caps() & cap) ? std::optional<int64_t>(f(*m))
                                                : std::nullopt;
        collect(m);                                  // f may have read a register
    }
    records_.push_back(r);
}

void Ctx::check_opt(const std::string& id,
                    std::function<std::optional<int64_t>(GimeModel&)> f,
                    Cap cap, uint32_t tol_ticks, const std::string& note) {
    CheckRecord r;
    r.id = prefix + id;
    r.cap = cap;
    r.tol = tol_ticks;
    r.note = note;
    for (GimeModel* m : models) {
        r.v[m->name()] = (m->caps() & cap) ? f(*m) : std::nullopt;
        collect(m);
    }
    records_.push_back(r);
}

void Ctx::check_read(const std::string& id, uint16_t addr, uint8_t mask) {
    check(id, [addr, mask](GimeModel& m) { return (int64_t)(m.read(addr) & mask); },
          CAP_REGS);
}

void Ctx::check_event(const std::string& id, EvType t, unsigned nth) {
    Cap cap = CAP_SYNC_EV;
    if (t == EvType::TIMER) cap = CAP_TIMER_EV;
    else if (t == EvType::IRQ_RISE || t == EvType::FIRQ_RISE) cap = CAP_IRQ;

    uint32_t tol = 0;
    for (GimeModel* m : models)
        if (m->caps() & cap) tol = std::max(tol, m->resolution(cap));

    check(id, [&](GimeModel& m) { return event_tick(&m, t, nth); }, cap, tol);
}
