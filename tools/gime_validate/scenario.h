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
    void read(uint16_t addr);                      // CPU read, result discarded
    void set_il(unsigned line, bool level);        // IL0..IL2 on every model
    void advance(uint64_t ticks);
    void lines(unsigned n) { advance((uint64_t)n * 912); }
    void drain();                                  // forget events seen so far
    void anchor();                                 // event ticks count from each model's now()
    void fill_ram(uint8_t (*f)(uint32_t addr));    // same contents in every model's RAM

    // Per model: advance whole lines until an event of type t has been seen
    // since the last drain(), then on to that event's tick + offset (offset
    // >= 912; t is a sync event). Models without CAP_SYNC_EV just advance offset.
    // Afterwards the models sit at the same point relative to their own
    // event, not at the same absolute tick.
    void sync_after(EvType t, uint64_t offset);

    // Tick of the nth event of type t since the last drain(), relative to
    // the model's anchor (0 until anchor() is called); -1 if there is none.
    int64_t event_tick(GimeModel* m, EvType t, unsigned nth);

    void check(const std::string& id, std::function<int64_t(GimeModel&)> f,
               Cap cap, uint32_t tol_ticks = 0);
    void check_read(const std::string& id, uint16_t addr, uint8_t mask = 0xFF); // CAP_REGS
    void check_event(const std::string& id, EvType t, unsigned nth);  // event_tick()

    std::vector<CheckRecord>& records() { return records_; }

private:
    void collect(GimeModel* m);                          // move m's pending events into events_
    std::vector<CheckRecord> records_;
    std::map<GimeModel*, std::vector<Event>> events_;   // since the last drain()
    std::map<GimeModel*, uint64_t> anchors_;            // since the last reset()
};

struct Scenario { std::string id; void (*fn)(Ctx&); };
std::vector<Scenario>& scenarios();
struct ScenarioReg { ScenarioReg(const char* area, const char* name, void (*fn)(Ctx&)); };

#define SCENARIO(area, name) \
    static void scn_##area##_##name(Ctx&); \
    static ScenarioReg reg_##area##_##name(#area, #name, scn_##area##_##name); \
    static void scn_##area##_##name(Ctx& ctx)

#endif
