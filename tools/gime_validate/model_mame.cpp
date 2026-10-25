/*
 * GimeModel adapter over MAME's GIME, through mame_gime.h.
 *
 * Form: STAND-IN. gime.cpp and mc6847.cpp are compiled whole and unmodified
 * against mame_shim/ (474 lines: emu.h, logmacro.h, screen.h, machine/ram.h,
 * bus/coco/cococart.h), which needs nothing from MAME's emu/ core. That is
 * inside the Task 9 switch rule (shim under 1,000 lines, no emu/ headers), so
 * the extract fallback (mame_extract.cpp) was not needed.
 *
 * Two builds, one per GIME_TYPE_1987 value (gime.cpp:99): MAME86 is
 * $MAME_SRC's gime.cpp as is, MAME87 a copy with line 99 rewritten to 1 by
 * run.sh. reset() picks the build for the variant it is given.
 *
 * Time: MAME runs the GIME from the 28.636363 MHz master clock
 * (coco3.cpp:268); one suite tick is two clocks. Every edge MAME schedules is
 * a whole number of ticks (hsync 1824/1696 clocks, fsync 1696, timer
 * 1824*n or 8*n), so every capability is exact: resolution 1.
 */
#include "gime_model.h"
#include "mame_gime.h"

namespace {

class Mame : public GimeModel {
public:
    const char* name() const override { return "mame"; }
    unsigned caps() const override {
        return CAP_REGS | CAP_DECODE | CAP_IRQ | CAP_TIMER_EV | CAP_SYNC_EV | CAP_RENDER;
    }
    uint32_t resolution(Cap) const override { return 1; }

    void reset(Variant v, uint8_t* ram, uint32_t ram_size) override {
        g_.reset();     // free the old GIME first: its RAM is the caller's
        g_ = v == Variant::G1987 ? mame87::create(ram, ram_size) : mame86::create(ram, ram_size);
    }

    void write(uint16_t addr, uint8_t val) override { g_->write(addr, val); }
    uint8_t read(uint16_t addr) override { return g_->read(addr); }

    Decode decode(uint16_t addr) override {
        Decode d;
        g_->decode(addr, &d.S, &d.Z, &d.RAS, &d.z_valid);
        return d;
    }

    void advance(uint64_t ticks) override { g_->advance(ticks); }
    uint64_t now() const override { return g_->now(); }
    bool irq() const override { return g_->irq(); }
    bool firq() const override { return g_->firq(); }
    void set_il(unsigned line, bool level) override { g_->set_il(line, level); }
    std::vector<Event> drain_events() override { return g_->drain_events(); }

    // MAME's bitmap columns have no fixed relation to HS fall, so
    // first_active_px is reported as absent.
    bool last_line(std::vector<uint8_t>& colours, int& first_active_px) override {
        first_active_px = -1;
        return g_->last_line(colours);
    }

private:
    std::unique_ptr<MameGime> g_;
};

} // namespace

// The variant is chosen at each reset() (Ctx passes its variant); the
// argument documents which one the caller runs first.
std::unique_ptr<GimeModel> make_mame(Variant) {
    return std::unique_ptr<GimeModel>(new Mame());
}
