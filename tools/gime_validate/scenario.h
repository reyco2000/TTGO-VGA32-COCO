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

// FNV-1a 64 of a line's colours, masked to a non-negative int64_t.
int64_t line_hash(const std::vector<uint8_t>& colours);

struct Ctx {
    std::vector<GimeModel*> models;
    Variant variant = Variant::G1987;
    std::string scenario;                          // id of the running scenario (for warnings)
    std::string prefix;                            // prepended to every check id ("v86.")

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
    // A model that doesn't see the event within the cap is left where it is,
    // with a warning on stderr naming the scenario and the model.
    // Afterwards the models sit at the same point relative to their own
    // event, not at the same absolute tick.
    void sync_after(EvType t, uint64_t offset);

    // Tick of the nth event of type t since the last drain(), relative to
    // the model's anchor (0 until anchor() is called); -1 if there is none.
    int64_t event_tick(GimeModel* m, EvType t, unsigned nth);
    // Line index of the nth event of type t since the last drain(), in the
    // model's own line grid: the number of HS_FALL events after anchor + from
    // and at or before that event's tick (an event on an HS fall belongs to
    // the line that fall starts); -1 if there is no such event. `from` moves
    // the start off a line boundary, where the models' HS falls straddle it.
    int64_t event_line(GimeModel* m, EvType t, unsigned nth, uint64_t from = 0);

    void check(const std::string& id, std::function<int64_t(GimeModel&)> f,
               Cap cap, uint32_t tol_ticks = 0);
    // As check(), but f may report no value (nullopt: SKIP for that model).
    void check_opt(const std::string& id, std::function<std::optional<int64_t>(GimeModel&)> f,
                   Cap cap, uint32_t tol_ticks = 0, const std::string& note = std::string());
    void check_read(const std::string& id, uint16_t addr, uint8_t mask = 0xFF); // CAP_REGS
    void check_event(const std::string& id, EvType t, unsigned nth);  // event_tick()
    // For line-rate events (TINS=0 timer, interrupts from line-rate sources):
    // event_line() with tolerance 0, id + "_line". Absent for a model without
    // CAP_SYNC_EV (no HS_FALL to count). A whole-line shift that the tick
    // check's 912-tick tolerance would pass is a mismatch here (R10/R11).
    void check_event_line(const std::string& id, EvType t, unsigned nth, uint64_t from = 0);

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

// Randomized runs (scn_random.cpp): n runs per area, seeded from `seed`.
void run_random(Ctx& ctx, uint32_t seed, unsigned n);
// Shared with scn_random.cpp.
void render_observe_after_setup(Ctx& ctx, const std::string& pre);   // scn_render.cpp
uint8_t regs_read_mask(unsigned addr);                                // scn_reset_regs.cpp

#define SCENARIO(area, name) \
    static void scn_##area##_##name(Ctx&); \
    static ScenarioReg reg_##area##_##name(#area, #name, scn_##area##_##name); \
    static void scn_##area##_##name(Ctx& ctx)

#endif
