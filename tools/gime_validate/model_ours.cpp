/*
 * GimeModel adapter over our GIME (src/core/tcc1014.cpp). Every access goes
 * through tcc1014_mem_cycle(), which mirrors XRoar's decode and register
 * paths. advance() runs the per-line GIME sequence of
 * machine_run_scanline_coco3() (src/core/machine.cpp), minus CPU and PIA.
 */
#define TCC1014_RENDER_TEST 1
#include "../../src/core/tcc1014.cpp"

#include "gime_model.h"

namespace {

class Ours : public GimeModel {
public:
    const char* name() const override { return "ours"; }
    unsigned caps() const override { return CAP_REGS | CAP_DECODE | CAP_IRQ | CAP_TIMER_EV | CAP_SYNC_EV; }
    uint32_t resolution(Cap c) const override {
        return (c == CAP_TIMER_EV || c == CAP_SYNC_EV || c == CAP_IRQ) ? TCC1014_tSL : 1;
    }

    void reset(Variant, uint8_t* ram, uint32_t ram_size) override {
        tcc1014_init(&g_);
        tcc1014_reset(&g_);
        tcc1014_set_ram(&g_, ram, ram_size);
        ram_ = ram;
        ram_mask_ = ram_size - 1;
        now_ = 0;
        line_ticks_ = 0;
        scanline_ = 0;
        ev_.clear();
        fires_ = g_.timer_fires;
        irq_prev_ = g_.IRQ;
        firq_prev_ = g_.FIRQ;
    }

    void write(uint16_t addr, uint8_t val) override {
        bool is_reg = false;
        tcc1014_mem_cycle(&g_, addr, false, val, nullptr, &is_reg);
        if (!is_reg && g_.RAS) ram_[g_.Z & ram_mask_] = val;
        sync_edges();
    }

    uint8_t read(uint16_t addr) override {
        uint8_t d = 0xFF;
        bool is_reg = false;
        tcc1014_mem_cycle(&g_, addr, true, 0, &d, &is_reg);
        sync_edges();
        if (is_reg) return d;
        return g_.RAS ? ram_[g_.Z & ram_mask_] : 0xFF;   // no ROM/PIA modelled
    }

    // Decode on a copy of the GIME so reads of register addresses (which
    // acknowledge interrupts) and the external-interrupt sampling leave no trace.
    Decode decode(uint16_t addr) override {
        TCC1014 saved = g_;
        uint8_t d;
        bool is_reg;
        tcc1014_mem_cycle(&g_, addr, true, 0, &d, &is_reg);
        // Z is only meaningful below $FF00 (XRoar never sets it for I/O);
        // the suite's convention in every adapter is Z = 0 for $FF00-$FFFF.
        Decode r = { g_.S, addr >= 0xFF00 ? 0u : g_.Z, g_.RAS };
        g_ = saved;
        return r;
    }

    void advance(uint64_t ticks) override {
        while (ticks) {
            uint64_t step = TCC1014_tSL - line_ticks_;
            if (step > ticks) step = ticks;
            line_ticks_ += (uint32_t)step;
            now_ += step;
            ticks -= step;
            if (line_ticks_ == TCC1014_tSL) {
                line_ticks_ = 0;
                run_line();
            }
        }
    }

    uint64_t now() const override { return now_; }
    bool irq() const override { return g_.IRQ; }
    bool firq() const override { return g_.FIRQ; }

    void set_il(unsigned line, bool level) override {
        if (line == 0) g_.IL0 = level;
        else if (line == 1) g_.IL1 = level;
        else if (line == 2) g_.IL2 = level;
        sync_edges();
    }

    std::vector<Event> drain_events() override {
        std::vector<Event> r;
        r.swap(ev_);
        return r;
    }

    bool last_line(std::vector<uint8_t>&, int&) override { return false; }

private:
    // One scanline, in machine_run_scanline_coco3() order. Everything that
    // happens at the line's end in machine.cpp is stamped at now_ (the line
    // boundary): ours resolves sync events to a whole line (912 ticks).
    void run_line() {
        // mirrors machine.cpp:675-682 (timer tick, HS edges to PIA0 CA1)
        tcc1014_tick_scanline(&g_);
        for (; fires_ < g_.timer_fires; fires_++)
            ev_.push_back({ EvType::TIMER, now_ });
        ev_.push_back({ EvType::HS_FALL, now_ });  // mc6821_ca1_transition(false)

        // mirrors machine.cpp:717-848 (vertical timing), except the render
        // and HAL calls at machine.cpp:796-816; PIA0 CB1 transitions become
        // FS_FALL / FS_RISE events. Diff this block against those lines.
        TCC1014* g = &g_;
        unsigned scanline = scanline_;  // 0-261 absolute

        if (scanline == 0) {
            ev_.push_back({ EvType::FS_FALL, now_ });  // mc6821_cb1_transition(false)
            g->vertical.lAA = g->COCO ? 192 :
                (g->LPF == 0 ? 192 : g->LPF == 1 ? 199 : g->LPF == 3 ? 225 : 0xFFFF);
            if (g->COCO) {
                g->vertical.lTB = g->H50 ? 63 : 36;
            }
        }

        if (scanline == 4) {
            ev_.push_back({ EvType::FS_RISE, now_ });  // mc6821_cb1_transition(true)
        }

        if (scanline == 7) {
            if (g->COCO) {
                g->B = (g->Y & 0x701FF) | ((uint32_t)g->SAM_F << 9);
            } else {
                g->B = g->Y;
            }
            g->vertical.lcount = 0;
            g->vertical.active_area = false;
        }

        if (scanline >= 7) {
            unsigned vline = g->vertical.lcount;
            unsigned top_border_end = g->vertical.lTB;

            if (!g->vertical.active_area && vline == top_border_end) {
                g->vertical.active_area = true;
                if (!g->COCO) {
                    g->row = g->VSC;
                    if ((g->row & g->rowmask) == g->rowmask) {
                        g->row = 0;
                    }
                } else {
                    g->row = 0;
                }
            }

            if (g->vertical.active_area) {
                unsigned display_line = vline - top_border_end;

                g->Xoff = g->X;
                if (g->COCO || g->BP) {
                    g->row_stride = g->HVEN ? 256 : g->BPR;
                } else {
                    g->row_stride = g->HVEN ? 256 : (g->BPR << (g->CRES & 1));
                }
                if (g->COCO) {
                    if (!g->VDG.GnA) {
                        bool GM2 = g->PIA1B_shadow.pdr & 0x40;
                        bool text_border = !g->VDG.GM1 && GM2;
                        unsigned text_border_colour = g->VDG.CSS ? 0x26 : 0x12;
                        g->border_colour = text_border ? text_border_colour : 0;
                    } else {
                        unsigned c = g->VDG.CSS ? TCC1014_RGCSS1_1 : TCC1014_RGCSS0_1;
                        g->border_colour = g->palette_reg[c];
                    }
                } else {
                    g->border_colour = g->BRDR;
                }

                // machine.cpp:796-816 renders display_line here (not modelled yet).

                g->row = (g->row + 1) & 15;
                bool row_advance = false;
                if ((g->row & g->rowmask) == g->rowmask) {
                    row_advance = true;
                    g->row = 0;
                }
                if (!g->COCO && g->LPR == 7) {
                    row_advance = false;
                }
                if (row_advance) {
                    g->B += g->row_stride;
                }
                tcc1014_set_interrupt(g, TCC1014_INT_HBORD);

                if (display_line + 1 >= g->vertical.lAA) {
                    g->vertical.active_area = false;
                    tcc1014_set_interrupt(g, TCC1014_INT_VBORD);
                }
            }

            g->vertical.lcount++;
        }

        scanline_++;
        if (scanline_ >= 262) {  // SCANLINES_PER_FRAME, config.h:256
            scanline_ = 0;
        }

        sync_edges();
    }

    // Emit IRQ_RISE / FIRQ_RISE for any rising edge of the outputs since the
    // last call, stamped at now_. Called after every CPU access and set_il
    // too, so an edge caused by a register write (enabling an already-pending
    // source) is reported like XRoar's adapter does (xg_check_edges). Edges
    // from line-driven sources still carry the line-boundary stamp.
    void sync_edges() {
        if (g_.IRQ && !irq_prev_) ev_.push_back({ EvType::IRQ_RISE, now_ });
        if (g_.FIRQ && !firq_prev_) ev_.push_back({ EvType::FIRQ_RISE, now_ });
        irq_prev_ = g_.IRQ;
        firq_prev_ = g_.FIRQ;
    }

    TCC1014 g_;
    uint8_t* ram_ = nullptr;
    uint32_t ram_mask_ = 0;
    uint64_t now_ = 0;
    uint32_t line_ticks_ = 0;
    unsigned scanline_ = 0;
    uint32_t fires_ = 0;
    bool irq_prev_ = false, firq_prev_ = false;
    std::vector<Event> ev_;
};

} // namespace

std::unique_ptr<GimeModel> make_ours() { return std::unique_ptr<GimeModel>(new Ours()); }
