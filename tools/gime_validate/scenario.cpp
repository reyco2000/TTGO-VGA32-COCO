#include "scenario.h"
#include <algorithm>

static const uint32_t RAM_SIZE = 2048 * 1024;

std::vector<Scenario>& scenarios() {
    static std::vector<Scenario> v;
    return v;
}

ScenarioReg::ScenarioReg(const char* area, const char* name, void (*fn)(Ctx&)) {
    scenarios().push_back({ std::string(area) + "." + name, fn });
}

void Ctx::reset() {
    static std::map<GimeModel*, std::vector<uint8_t>> rams;   // one RAM per model
    events_.clear();
    for (GimeModel* m : models) {
        std::vector<uint8_t>& r = rams[m];
        r.assign(RAM_SIZE, 0);
        m->reset(variant, r.data(), RAM_SIZE);
    }
}

void Ctx::write(uint16_t addr, uint8_t val) {
    for (GimeModel* m : models) m->write(addr, val);
}

void Ctx::advance(uint64_t ticks) {
    for (GimeModel* m : models) {
        m->advance(ticks);
        std::vector<Event> e = m->drain_events();
        events_[m].insert(events_[m].end(), e.begin(), e.end());
    }
}

void Ctx::drain() { events_.clear(); }

void Ctx::check(const std::string& id, std::function<int64_t(GimeModel&)> f,
                Cap cap, uint32_t tol_ticks) {
    CheckRecord r;
    r.id = id;
    r.cap = cap;
    r.tol = tol_ticks;
    for (GimeModel* m : models)
        r.v[m->name()] = (m->caps() & cap) ? std::optional<int64_t>(f(*m))
                                                : std::nullopt;
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

    check(id, [&](GimeModel& m) -> int64_t {
        unsigned n = 0;
        for (const Event& e : events_[&m])
            if (e.type == t && n++ == nth) return (int64_t)e.tick;
        return -1;                                   // no such event
    }, cap, tol);
}
