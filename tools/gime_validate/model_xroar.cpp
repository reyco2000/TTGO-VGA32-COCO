/*
 * GimeModel adapter over XRoar's tcc1014.c, through the C API in
 * xroar_gime.h. XRoar is event-driven and exact to the tick, so every
 * capability has resolution 1. Only one XRoar GIME exists at a time (its
 * event list and tick are globals), so only one Xroar model may be live.
 */
#include "gime_model.h"
#include "xroar_gime.h"

namespace {

class Xroar;
Xroar* g_live = nullptr;     // target of the C hooks

class Xroar : public GimeModel {
public:
    Xroar() {}
    ~Xroar() override {
        if (g_live == this) {
            xg_set_hooks(nullptr, nullptr, nullptr);
            xg_set_int_hooks(nullptr, nullptr, nullptr);
            g_live = nullptr;
        }
    }

    const char* name() const override { return "xroar"; }
    unsigned caps() const override {
        return CAP_REGS | CAP_DECODE | CAP_IRQ | CAP_TIMER_EV | CAP_SYNC_EV | CAP_RENDER;
    }
    uint32_t resolution(Cap) const override { return 1; }

    void reset(Variant v, uint8_t* ram, uint32_t ram_size) override {
        ram_ = ram;
        ram_mask_ = ram_size - 1;
        ev_.clear();
        line_.clear();
        have_line_ = false;
        g_live = this;
        xg_set_hooks(on_hs, on_fs, on_line);
        xg_set_int_hooks(on_timer, on_irq_rise, on_firq_rise);
        xg_init(v == Variant::G1987, ram, ram_size);
    }

    void write(uint16_t addr, uint8_t val) override {
        uint8_t d = val;
        xg_cycle(addr, 0, &d);
        unsigned S; uint32_t Z; int RAS;
        xg_bus(&S, &Z, &RAS);
        if (RAS) ram_[Z & ram_mask_] = val;
    }

    // The bus floats high: the GIME drives only the low six bits of its
    // readable registers, so start from $FF as ours does. No ROM/PIA modelled.
    uint8_t read(uint16_t addr) override {
        uint8_t d = 0xFF;
        xg_cycle(addr, 1, &d);
        unsigned S; uint32_t Z; int RAS;
        xg_bus(&S, &Z, &RAS);
        return RAS ? ram_[Z & ram_mask_] : d;
    }

    Decode decode(uint16_t addr) override {
        unsigned S; uint32_t Z; int RAS;
        xg_decode(addr, &S, &Z, &RAS);
        return { S, Z, RAS != 0 };
    }

    void advance(uint64_t ticks) override {
        while (ticks) {
            uint32_t step = ticks > 0x40000000u ? 0x40000000u : (uint32_t)ticks;
            xg_advance(step);
            ticks -= step;
        }
    }

    uint64_t now() const override { return xg_now(); }
    bool irq() const override { return xg_irq() != 0; }
    bool firq() const override { return xg_firq() != 0; }
    void set_il(unsigned line, bool level) override { xg_set_il(line, level); }

    std::vector<Event> drain_events() override {
        std::vector<Event> r;
        r.swap(ev_);
        return r;
    }

    bool last_line(std::vector<uint8_t>& colours, int& first_active_px) override {
        if (!have_line_) return false;
        colours = line_;
        first_active_px = (int)line_first_;
        return true;
    }
    int last_line_border() const override { return border_; }

private:
    static void push(EvType t) { g_live->ev_.push_back({ t, xg_now() }); }
    static void on_hs(int level) { if (!level) push(EvType::HS_FALL); }
    static void on_fs(int level) { push(level ? EvType::FS_RISE : EvType::FS_FALL); }
    static void on_timer() { push(EvType::TIMER); }
    static void on_irq_rise() { push(EvType::IRQ_RISE); }
    static void on_firq_rise() { push(EvType::FIRQ_RISE); }
    static void on_line(unsigned, unsigned npixels, const uint8_t* data) {
        Xroar* m = g_live;
        unsigned a, b;
        xg_active_area(&a, &b);
        // Border and blanking lines have no active area (GimeModel::last_line).
        if (!data || !xg_line_active() || b > npixels || a >= b) {
            m->have_line_ = false;
            return;
        }
        m->line_.assign(data + a, data + b);
        m->line_first_ = a;
        m->border_ = a > 0 ? (int)(data[a - 1] & 0x3F) : -1;   // the pixel left of the active area
        m->have_line_ = true;
    }

    uint8_t* ram_ = nullptr;
    uint32_t ram_mask_ = 0;
    std::vector<Event> ev_;
    std::vector<uint8_t> line_;
    unsigned line_first_ = 0;
    int border_ = -1;
    bool have_line_ = false;
};

} // namespace

// The variant is chosen at each reset() (Ctx passes its variant); the
// argument documents which one the caller runs first.
std::unique_ptr<GimeModel> make_xroar(Variant) {
    return std::unique_ptr<GimeModel>(new Xroar());
}
