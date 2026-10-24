/*
 * GIME validation suite: scenario context and registry. A scenario applies
 * the same operations to every model and records one value per model for
 * each check.
 */
#ifndef GIME_SCENARIO_H
#define GIME_SCENARIO_H

#include "compare.h"
#include "gime_model.h"
#include <functional>
#include <map>
#include <optional>
#include <string>

struct Ctx {
    std::vector<GimeModel*> models;
    Variant variant = Variant::G1987;

    void reset();                                  // reset every model
    void write(uint16_t addr, uint8_t val);
    void advance(uint64_t ticks);
    void lines(unsigned n) { advance((uint64_t)n * 912); }
    void drain();                                  // forget events seen so far

    void check(const std::string& id, std::function<int64_t(GimeModel&)> f,
               Cap cap, uint32_t tol_ticks = 0);
    void check_read(const std::string& id, uint16_t addr);            // CAP_REGS
    void check_event(const std::string& id, EvType t, unsigned nth);  // nth since drain

    std::vector<CheckRecord>& records() { return records_; }

private:
    std::vector<CheckRecord> records_;
    std::map<GimeModel*, std::vector<Event>> events_;   // since the last drain()
};

struct Scenario { std::string id; void (*fn)(Ctx&); };
std::vector<Scenario>& scenarios();
struct ScenarioReg { ScenarioReg(const char* area, const char* name, void (*fn)(Ctx&)); };

#define SCENARIO(area, name) \
    static void scn_##area##_##name(Ctx&); \
    static ScenarioReg reg_##area##_##name(#area, #name, scn_##area##_##name); \
    static void scn_##area##_##name(Ctx& ctx)

#endif
