/*
 * GIME validation suite: MAME's GIME (gime_ntsc_device, src/mame/trs/gime.cpp)
 * and its mc6847_friend_device base (src/devices/video/mc6847.cpp), both
 * compiled unmodified against the stand-in framework in mame_shim/, behind the
 * MameGime API (mame_gime.h).
 *
 * run.sh compiles this file twice, once per GIME_TYPE_1987 value, with
 * MAME_GIME_NS naming the namespace (mame86 / mame87) and the include path
 * choosing which gime.cpp is found: $MAME_SRC's own (GIME_TYPE_1987 0) or a
 * copy in the build dir with gime.cpp:99 rewritten to 1. Everything MAME is
 * #included inside that namespace, so the two builds don't collide.
 *
 * Plumbing here, not GIME logic:
 *  - the sam6883_friend_device_interface constructor (as 6883sam.cpp:107-113)
 *    and update_cpu_clock() (6883sam.cpp:339, CPU clock rate: a no-op here);
 *  - the bus around the GIME, as coco3.cpp:36-58 maps it: RAM/ROM through the
 *    GIME's read/write banks, $FF20-$FF2F also to pia_write()
 *    (coco3_m.cpp:53-59), $FF90-$FFDF to read()/write(); no PIA, ROM or
 *    cartridge contents (reads there give $FF, the suite's floating bus);
 *  - floating bus reads give $FF (coco3.cpp:274 wires a CPU-state reader);
 *  - observation: the hsync/fsync/irq/firq callbacks (coco3.cpp:270-273
 *    wire them to the PIA and CPU), the timer_elapsed timer firing, and a
 *    record_full_body_scanline override that calls through and then renders
 *    the line with MAME's own update_rgb() into an identity palette.
 */
#include <algorithm>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "mame_gime.h"

#ifndef MAME_GIME_NS
#error "build with -DMAME_GIME_NS=mame86 or mame87 (see run.sh)"
#endif

namespace MAME_GIME_NS {

#include "emu.h"
#include "video/mc6847.cpp"
#include "gime.cpp"

// 6883sam.cpp:107-113
sam6883_friend_device_interface::sam6883_friend_device_interface(const machine_config &mconfig, device_t &device, int divider)
    : device_interface(device, "sam6883")
    , m_cpu(device, finder_base::DUMMY_TAG)
    , m_sam_state(0x0000)
    , m_divider(divider)
{
}

// 6883sam.cpp:339: sets the CPU clock from SAM R1/R0; there is no CPU here.
void sam6883_friend_device_interface::update_cpu_clock() {}

namespace {

class Probe : public gime_ntsc_device {
public:
    Probe(const machine_config& mc, uint8_t* ram, uint32_t ram_size)
        : gime_ntsc_device(mc, "gime", nullptr, MAME_SHIM_CLOCK),
          ram_dev_(ram, ram_size), rom_region_(rom_, sizeof rom_), bm_(640, 300) {
        memset(rom_, 0xFF, sizeof rom_);
    }

    void start() {
        m_maincpu.set_target(&cpu_);
        m_cpu.set_target(&cpu_);
        m_ram.set_target(&ram_dev_);
        m_cart_device.set_target(&cart_);
        m_rom_region.set_target(&rom_region_);
        device_start();
        // Identity "RGB" palette: a rendered pixel is its 6-bit colour.
        for (int c = 0; c < 64; c++) m_rgb_palette[c] = (pixel_t)c;
        device_reset();
    }

    bool legacy_video() const { return m_legacy_video; }
    bool line_ready = false;            // a body line was recorded on this HS rise
    std::vector<uint8_t> line;          // its colours
    int border = -1;                    // its border colour (-1: wide line, none)

    // Pointer the read/write bank gives for CPU address addr (< $FF00).
    uint8_t* bank_ptr(uint16_t addr, bool write) {
        int b = addr >= 0xFE00 ? 8 : addr >> 13;
        uint8_t* base = (write ? m_write_banks[b] : m_read_banks[b])->base();
        return base + (b == 8 ? addr - 0xFE00 : addr & 0x1FFF);
    }
    bool is_ram(const uint8_t* p) const {
        return p >= ram_dev_.pointer() && p < ram_dev_.pointer() + ram_dev_.size();
    }
    bool is_rom(const uint8_t* p) const { return p >= rom_ && p < rom_ + sizeof rom_; }
    const uint8_t* ram_base() const { return ram_dev_.pointer(); }

protected:
    void record_full_body_scanline(uint16_t physical_scanline, uint16_t logical_scanline) override {
        gime_ntsc_device::record_full_body_scanline(physical_scanline, logical_scanline);
        capture(physical_scanline);
    }

private:
    // Render the recorded line as MAME's screen update would (update_rgb
    // after update_composite, so has_video_changed() is always true), then
    // keep its active area: [0,640) when wide, else [64,576), as
    // render_scanline() places the borders (gime.cpp:1781-1808).
    void capture(uint16_t y) {
        update_composite(bm_, rectangle(0, 639, 1, 0));
        update_rgb(bm_, rectangle(0, 639, y, y));
        bool wide = !m_legacy_video && (m_scanlines[y].m_ff99_value & 0x04);
        int a = wide ? 0 : 64, n = wide ? 640 : 512;
        line.resize(n);
        for (int i = 0; i < n; i++) line[i] = (uint8_t)(bm_.pix(y, a + i) & 0x3F);
        border = a > 0 ? (int)(bm_.pix(y, a - 1) & 0x3F) : -1;
        line_ready = true;
    }

    cpu_device cpu_;
    ram_device ram_dev_;
    cococart_slot_device cart_;
    uint8_t rom_[0x8000];
    memory_region rom_region_;
    bitmap_rgb32 bm_;
};

class Model : public MameGime {
public:
    Model(uint8_t* ram, uint32_t ram_size) : mc_(machine_), gime_(mc_, ram, ram_size) {
        gime_.irq_wr_callback().set([this](int s) { edge(irq_, s != 0, EvType::IRQ_RISE); });
        gime_.firq_wr_callback().set([this](int s) { edge(firq_, s != 0, EvType::FIRQ_RISE); });
        gime_.hsync_wr_callback().set([this](int s) {
            if (!s) { push(EvType::HS_FALL); return; }
            // HS rise ends a line: the record_*_scanline call for it came first
            // (mc6847.cpp:328-383); retrace and blanking lines record nothing.
            have_line_ = gime_.line_ready;
            if (have_line_) { line_.swap(gime_.line); border_ = gime_.border; }
            gime_.line_ready = false;
        });
        gime_.fsync_wr_callback().set([this](int s) { push(s ? EvType::FS_RISE : EvType::FS_FALL); });
        gime_.floating_bus_rd_callback().set([](offs_t) { return (uint8_t)0xFF; });
        // The GIME timer's only timer: timer_elapsed (gime.cpp:182, 440-451)
        // fires whatever $FF92/$FF93 enable.
        machine_.on_timer_fire = [this](const emu_timer& t) {
            if (!strcmp(t.name(), "gime_device::timer_elapsed")) push(EvType::TIMER);
        };
        gime_.start();
    }

    void write(uint16_t addr, uint8_t val) override {
        if (addr < 0xFF00) *gime_.bank_ptr(addr, true) = val;
        else if (addr >= 0xFF20 && addr <= 0xFF2F) gime_.pia_write(addr - 0xFF20, val);
        else if (addr >= 0xFF90 && addr <= 0xFFDF) gime_.write(addr - 0xFF90, val);
    }

    uint8_t read(uint16_t addr) override {
        if (addr < 0xFF00) {
            uint8_t* p = gime_.bank_ptr(addr, false);
            return gime_.is_ram(p) ? *p : 0xFF;
        }
        if (addr >= 0xFF90 && addr <= 0xFFDF) return gime_.read(addr - 0xFF90);
        return 0xFF;
    }

    void decode(uint16_t addr, unsigned* S, uint32_t* Z, bool* RAS, bool* z_valid) override {
        *Z = 0; *RAS = false; *z_valid = true;
        if (addr < 0xFF00) {
            uint8_t* p = gime_.bank_ptr(addr, false);
            if (gime_.is_ram(p)) { *S = 7; *Z = (uint32_t)(p - gime_.ram_base()); *RAS = true; }
            else { *S = gime_.is_rom(p) ? 0 : 1; *z_valid = false; }   // else empty cartridge
        } else if (addr < 0xFF40) {
            *S = (addr & 0x10) ? 7 : 2;                                // PIA0 / PIA1
        } else if (addr < 0xFF60) {
            *S = gime_.spare_chip_select_enabled() ? 6 : 7;            // coco3_m.cpp:67-80
        } else {
            *S = addr >= 0xFFE0 ? 0 : 7;                               // vectors: ROM
        }
    }

    void advance(uint64_t ticks) override {
        machine_.run_until(machine_.time() + attotime((int64_t)(ticks * 2)));
    }
    uint64_t now() const override { return (uint64_t)machine_.time().clocks() / 2; }
    bool irq() const override { return irq_; }
    bool firq() const override { return firq_; }

    void set_il(unsigned line, bool level) override {
        if (line == 0) gime_.set_il0(level);
        else if (line == 1) gime_.set_il1(level);
        else if (line == 2) gime_.set_il2(level);
    }

    std::vector<Event> drain_events() override {
        std::vector<Event> r;
        r.swap(ev_);
        return r;
    }

    bool last_line(std::vector<uint8_t>& colours) override {
        if (!have_line_) return false;
        colours = line_;
        return true;
    }
    int last_line_border() const override { return border_; }

private:
    void push(EvType t) { ev_.push_back({ t, now() }); }
    void edge(bool& prev, bool v, EvType t) {
        if (v && !prev) push(t);
        prev = v;
    }

    running_machine machine_;
    machine_config mc_;
    Probe gime_;
    bool irq_ = false, firq_ = false;
    std::vector<Event> ev_;
    std::vector<uint8_t> line_;
    int border_ = -1;
    bool have_line_ = false;
};

} // namespace

std::unique_ptr<MameGime> create(uint8_t* ram, uint32_t ram_size) {
    return std::unique_ptr<MameGime>(new Model(ram, ram_size));
}

} // namespace MAME_GIME_NS
