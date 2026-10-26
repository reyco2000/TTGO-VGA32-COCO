/*
 * GimeModel adapter over our GIME (src/core/tcc1014.cpp). Every access goes
 * through tcc1014_mem_cycle(), which mirrors XRoar's decode and register
 * paths. advance() runs the per-line GIME sequence of
 * machine_run_scanline_coco3() (src/core/machine.cpp), minus CPU and PIA.
 */
#define TCC1014_RENDER_TEST 1
#include "../../src/core/tcc1014.cpp"
#include "../../config.h"            // SCANLINES_PER_FRAME (host stub Arduino.h in stub/)

#include "gime_model.h"
#include <stdio.h>
#include <stdlib.h>

namespace {

class Ours : public GimeModel {
public:
    const char* name() const override { return "ours"; }
    unsigned caps() const override {
        return CAP_REGS | CAP_DECODE | CAP_IRQ | CAP_TIMER_EV | CAP_SYNC_EV | CAP_RENDER;
    }
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
        have_line_ = false;
        line_note_.clear();
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

    // Ours renders only the active area (line_width pixels, no borders) and
    // keeps no horizontal position, so first_active_px is reported as absent.
    bool last_line(std::vector<uint8_t>& colours, int& first_active_px) override {
        if (!have_line_) return false;
        colours = line_;
        first_active_px = -1;
        return true;
    }
    std::string last_line_note() const override { return line_note_; }
    // border_colour as run_line computed it for that line (machine.cpp hands
    // it to the HAL, which draws the border).
    int last_line_border() const override { return border_; }

private:
    // One scanline, in machine_run_scanline_coco3() order. Everything that
    // happens at the line's end in machine.cpp is stamped at now_ (the line
    // boundary): ours resolves sync events to a whole line (912 ticks).
    void run_line() {
        // The two MIRROR blocks below are copies of machine.cpp's
        // machine_run_scanline_coco3(); run.sh diffs them against machine.cpp
        // (normalisation rules in run.sh) and fails the build on any drift.
        // mirrors machine.cpp's timer tick and HS edges to PIA0 CA1
        // MIRROR-BEGIN timer
        tcc1014_tick_scanline(&g_);
        for (; fires_ < g_.timer_fires; fires_++)
            ev_.push_back({ EvType::TIMER, now_ });
        ev_.push_back({ EvType::HS_FALL, now_ });  // mc6821_ca1_transition(false)
        // MIRROR-END timer

        // mirrors machine.cpp's vertical timing, except the render and HAL
        // calls; PIA0 CB1 transitions become FS_FALL / FS_RISE events.
        // MIRROR-BEGIN vertical
        TCC1014* g = &g_;
        unsigned scanline = scanline_;  // 0-261 absolute
        have_line_ = false;             // until this line renders an active area
        line_note_.clear();

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

                // mirrors machine.cpp's render call only (no HAL):
                // raw_output false, so line_buffer holds byte-swapped RGB565.
                // line_clean (OPT-G5) can only skip a line when raw_output is
                // set and a raw LUT is registered, neither of which holds
                // here; the signatures are cleared anyway so that line_buffer
                // always holds this line, whatever that skip rule becomes.
                // MIRROR-RENDER-BEGIN
                tcc1014_invalidate_lines();
                g->raw_output = false;
                tcc1014_render_scanline(g, display_line);
                capture_line();
                // MIRROR-RENDER-END

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
        if (scanline_ >= SCANLINES_PER_FRAME) {
            scanline_ = 0;
        }
        // MIRROR-END vertical

        sync_edges();
    }

    // Decode line_buffer (byte-swapped RGB565) back to 6-bit colours: RGB565
    // -> palette slot (palette_rgb565) -> palette_reg. The map must be
    // injective: two slots holding different colours with the same RGB565
    // value would make the decode ambiguous, so that aborts the run.
    void capture_line() {
        const TCC1014* g = &g_;
        for (unsigned i = 0; i < 16; i++)
            for (unsigned j = 0; j < i; j++)
                if (g->palette_rgb565[i] == g->palette_rgb565[j] &&
                    (g->palette_reg[i] & 0x3F) != (g->palette_reg[j] & 0x3F)) {
                    fprintf(stderr, "model_ours: palette slots %u ($%02X) and %u ($%02X) "
                            "share RGB565 $%04X: line decode is ambiguous\n", j,
                            g->palette_reg[j], i, g->palette_reg[i], g->palette_rgb565[i]);
                    abort();
                }
        if (g->line_clean) { line_note_ = "line_clean: line_buffer not rendered"; return; }
        unsigned w = g->line_width;
        line_.resize(w);
        for (unsigned k = 0; k < w; k++) {
            unsigned i = 0;
            while (i < 16 && g->palette_rgb565[i] != g->line_buffer[k]) i++;
            if (i == 16) { line_note_ = "undecodable px " + std::to_string(k); return; }
            line_[k] = g->palette_reg[i] & 0x3F;
        }
        border_ = g->border_colour & 0x3F;
        have_line_ = true;
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
    std::vector<uint8_t> line_;     // colours of the last finished line, if have_line_
    bool have_line_ = false;
    int border_ = -1;
    std::string line_note_;
};


} // namespace

std::unique_ptr<GimeModel> make_ours() { return std::unique_ptr<GimeModel>(new Ours()); }
