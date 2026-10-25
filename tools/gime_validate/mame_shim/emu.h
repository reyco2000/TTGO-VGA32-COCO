/*
 * GIME validation suite: stand-in for MAME's emu.h, just enough of the MAME
 * device framework to compile src/mame/trs/gime.cpp and
 * src/devices/video/mc6847.cpp unmodified (see mame_gime.cpp). Plumbing only:
 * devices, timers and the scheduler, callbacks, finders, save states (no-op),
 * logging (compiled out), I/O ports (absent), bitmaps.
 *
 * Time: attotime counts periods of the GIME's 28.636363 MHz master clock
 * (MAME_SHIM_CLOCK, coco3.cpp:268 XTAL(28'636'363)) as an exact integer. The
 * suite's 14.31818 MHz tick is two of them. MAME's real attotime truncates
 * the clock period to whole attoseconds (< 29 ps of drift per second), far
 * below one tick; this stand-in has no such drift.
 *
 * Everything here is included inside the per-variant namespace that
 * mame_gime.cpp opens, so the standard headers it uses are included there
 * first (an include here is then a no-op).
 */
#ifndef MAME_SHIM_EMU_H
#define MAME_SHIM_EMU_H
#define __EMU_H__

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

// ---- basic types and attribute macros (emucore.h, osdcomm.h) ----
typedef uint8_t u8;   typedef uint16_t u16;  typedef uint32_t u32;  typedef uint64_t u64;
typedef int8_t s8;    typedef int16_t s16;   typedef int32_t s32;   typedef int64_t s64;
typedef uint32_t offs_t;
#define ATTR_COLD
#define ATTR_FORCE_INLINE inline
#define RESTRICT __restrict__
#define UNEXPECTED(x) (x)
#define EXPECTED(x) (x)
#define NAME(x) x, #x
#define FUNC(x) &x, #x
#define TIMER_CALLBACK_MEMBER(name) void name(s32 param)
template <typename T> constexpr T BIT(T x, int n) { return (x >> n) & 1; }

[[noreturn]] inline void fatalerror(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); abort();
}

namespace util {
inline const char* fmt_arg(const std::string& s) { return s.c_str(); }
template <typename T> T fmt_arg(T v) { return v; }
template <typename... A> std::string string_format(const char* fmt, A&&... a) {
    char b[512];
    snprintf(b, sizeof b, fmt, fmt_arg(a)...);
    return b;
}
}
using util::string_format;

// ---- rgb_t (lib/util/palette.h) ----
class rgb_t {
public:
    constexpr rgb_t(uint32_t v = 0) : m_v(v) {}
    constexpr rgb_t(uint8_t r, uint8_t g, uint8_t b) : m_v(0xff000000u | (r << 16) | (g << 8) | b) {}
    constexpr operator uint32_t() const { return m_v; }
    constexpr uint8_t r() const { return (m_v >> 16) & 0xff; }
    constexpr uint8_t g() const { return (m_v >> 8) & 0xff; }
    constexpr uint8_t b() const { return m_v & 0xff; }
private:
    uint32_t m_v;
};

// ---- bitmaps (lib/util/bitmap.h) ----
struct rectangle {
    int min_x = 0, max_x = -1, min_y = 0, max_y = -1;
    rectangle() {}
    rectangle(int x0, int x1, int y0, int y1) : min_x(x0), max_x(x1), min_y(y0), max_y(y1) {}
};
class bitmap_rgb32 {
public:
    bitmap_rgb32(int w = 0, int h = 0) : m_w(w), m_h(h), m_p((size_t)w * h) {}
    uint32_t& pix(int y, int x) { return m_p[(size_t)y * m_w + x]; }
    int width() const { return m_w; }
    int height() const { return m_h; }
private:
    int m_w, m_h;
    std::vector<uint32_t> m_p;
};

// ---- attotime (emu/attotime.h): exact master-clock periods ----
static constexpr uint32_t MAME_SHIM_CLOCK = 28636363;
class attotime {
public:
    constexpr attotime() : m_u(0) {}
    constexpr explicit attotime(int64_t clocks) : m_u(clocks) {}
    static attotime from_hz(uint32_t hz) {
        if (!hz || MAME_SHIM_CLOCK % hz) fatalerror("mame_shim: from_hz(%u) is not a whole number of clocks\n", hz);
        return attotime(MAME_SHIM_CLOCK / hz);
    }
    static const attotime never;
    static const attotime zero;
    bool is_never() const { return m_u == INT64_MAX; }
    bool is_zero() const { return m_u == 0; }
    int64_t seconds() const { return is_never() ? INT64_MAX : m_u / MAME_SHIM_CLOCK; }
    int64_t clocks() const { return m_u; }
    attotime operator+(const attotime& o) const {
        return (is_never() || o.is_never()) ? never : attotime(m_u + o.m_u);
    }
    attotime& operator+=(const attotime& o) { *this = *this + o; return *this; }
    attotime operator*(uint32_t f) const { return is_never() ? never : attotime(m_u * (int64_t)f); }
    attotime operator/(uint32_t d) const { return is_never() ? never : attotime(m_u / (int64_t)d); }
    bool operator<(const attotime& o) const { return m_u < o.m_u; }
    bool operator<=(const attotime& o) const { return m_u <= o.m_u; }
    bool operator>(const attotime& o) const { return m_u > o.m_u; }
    bool operator==(const attotime& o) const { return m_u == o.m_u; }
private:
    int64_t m_u;
};
inline const attotime attotime::never{INT64_MAX};
inline const attotime attotime::zero{0};

// ---- scheduler and timers (emu/schedule.h, schedule.cpp semantics) ----
class running_machine;
class emu_timer {
public:
    // schedule.cpp:137-160
    void adjust(attotime start_delay, s32 param = 0, const attotime& period = attotime::never);
    attotime start() const { return m_start; }
    attotime expire() const { return m_expire; }
    bool enabled() const { return m_enabled; }
    const char* name() const { return m_name; }
private:
    friend class running_machine;
    running_machine* m_machine = nullptr;
    std::function<void(s32)> m_cb;
    const char* m_name = "";
    attotime m_start, m_expire = attotime::never, m_period = attotime::never;
    s32 m_param = 0;
    bool m_enabled = false;
    uint64_t m_seq = 0;     // insertion order: equal expiry fires in list order
};

class device_t;
class memory_bank {
public:
    void set_base(void* p) { m_base = (uint8_t*)p; }
    uint8_t* base() const { return m_base; }
private:
    uint8_t* m_base = nullptr;
};

class running_machine {
public:
    // Scheduler time: the firing timer's expiry inside a callback, else the
    // base time (schedule.cpp:329-334).
    attotime time() const { return m_callback ? m_callback_expire : m_base; }
    std::string describe_context() const { return "mame_shim"; }
    device_t& root_device() { return *m_root; }
    memory_bank* bank(const std::string& tag) {
        auto& p = m_banks[tag];
        if (!p) p.reset(new memory_bank());
        return p.get();
    }
    emu_timer* timer_alloc(std::function<void(s32)> cb, const char* name) {
        m_timers.emplace_back(new emu_timer());
        emu_timer* t = m_timers.back().get();
        t->m_machine = this;
        t->m_cb = std::move(cb);
        t->m_name = name;
        return t;
    }
    // Called with each timer just before its callback runs (time() = expiry).
    std::function<void(const emu_timer&)> on_timer_fire;

    // Run every timer due at or before `target` in order, then set the base
    // time to target (schedule.cpp:933-982 execute_timers).
    void run_until(attotime target) {
        for (;;) {
            emu_timer* t = nullptr;
            for (auto& p : m_timers) {
                emu_timer* c = p.get();
                if (!c->m_enabled || c->m_expire.is_never() || target < c->m_expire) continue;
                if (!t || c->m_expire < t->m_expire ||
                    (c->m_expire == t->m_expire && c->m_seq < t->m_seq)) t = c;
            }
            if (!t) break;
            m_base = t->m_expire;
            bool was_enabled = t->m_enabled;
            if (t->m_period.is_zero() || t->m_period.is_never()) t->m_enabled = false;
            m_callback = t;
            m_callback_modified = false;
            m_callback_expire = t->m_expire;
            if (was_enabled) {
                if (on_timer_fire) on_timer_fire(*t);
                t->m_cb(t->m_param);
            }
            if (!m_callback_modified) {             // schedule_next_period (schedule.cpp:242-253)
                t->m_start = t->m_expire;
                t->m_expire += t->m_period;
                t->m_seq = ++m_seq;
            }
            m_callback = nullptr;
        }
        m_base = target;
    }

private:
    friend class emu_timer;
    friend class device_t;
    device_t* m_root = nullptr;
    attotime m_base;
    emu_timer* m_callback = nullptr;
    bool m_callback_modified = false;
    attotime m_callback_expire;
    uint64_t m_seq = 0;
    std::vector<std::unique_ptr<emu_timer>> m_timers;
    std::map<std::string, std::unique_ptr<memory_bank>> m_banks;
};

inline void emu_timer::adjust(attotime start_delay, s32 param, const attotime& period) {
    if (m_machine->m_callback == this) m_machine->m_callback_modified = true;
    m_param = param;
    m_enabled = true;
    if (start_delay.seconds() < 0) start_delay = attotime::zero;
    m_start = m_machine->time();
    m_expire = m_start + start_delay;
    m_period = period;
    m_seq = ++m_machine->m_seq;
}

// ---- profiler (emu/profiler.h): no-op ----
enum profile_type { PROFILER_USER1, PROFILER_USER2, PROFILER_TIMER_CALLBACK };
struct profiler_shim { int start(profile_type) { return 0; } };
inline profiler_shim g_profiler;

// ---- device types (emu/devfind.h, device.h) ----
struct device_type_impl_base { const char* shortname; const char* fullname; };
typedef device_type_impl_base const& device_type;
inline bool operator==(device_type_impl_base const& a, device_type_impl_base const& b) { return &a == &b; }
#define DECLARE_DEVICE_TYPE(Type, Class) class Class; extern device_type_impl_base const& Type;
#define DEFINE_DEVICE_TYPE(Type, Class, Short, Full) \
    static device_type_impl_base const Type##_shim_impl{ Short, Full }; \
    device_type_impl_base const& Type = Type##_shim_impl;

// The suite builds one machine per model; devices find it through the config.
class machine_config {
public:
    explicit machine_config(running_machine& m) : m_machine(m) {}
    running_machine& machine() const { return m_machine; }
private:
    running_machine& m_machine;
};

// ---- I/O ports (emu/ioport.h): none exist here ----
typedef uint32_t ioport_value;
typedef void (*ioport_constructor)();
class ioport_port { public: ioport_value read() { return 0; } };
#define INPUT_PORTS_NAME(name) construct_ioport_##name
#define INPUT_PORTS_EXTERN(name) extern void INPUT_PORTS_NAME(name)()
#define INPUT_PORTS_START(name) void INPUT_PORTS_NAME(name)() {
#define INPUT_PORTS_END }
#define PORT_START(tag)
#define PORT_CONFNAME(mask, def, name)
#define PORT_CONFSETTING(val, name)
#define DEF_STR(x)

class device_missing_dependencies {};

// ---- device_t (emu/device.h) ----
class device_t {
public:
    device_t(const machine_config& mconfig, device_type type, const char* tag, device_t* owner, uint32_t clock)
        : m_machine(mconfig.machine()), m_type(type), m_tag(tag), m_owner(owner), m_clock(clock) {
        if (!m_machine.m_root) m_machine.m_root = this;
    }
    virtual ~device_t() {}
    running_machine& machine() const { return m_machine; }
    device_type type() const { return m_type; }
    const char* tag() const { return m_tag; }
    device_t* owner() const { return m_owner; }
    uint32_t clock() const { return m_clock; }
    bool started() const { return true; }
    memory_bank* membank(const std::string& tag) { return m_machine.bank(tag); }
    ioport_port* ioport(const std::string&) { return nullptr; }
    attotime clocks_to_attotime(uint64_t n) const { return attotime((int64_t)n * attotime::from_hz(m_clock).clocks()); }
    uint64_t attotime_to_clocks(const attotime& t) const { return (uint64_t)(t.clocks() / attotime::from_hz(m_clock).clocks()); }
    template <typename T> emu_timer* timer_alloc(void (T::*cb)(s32), const char* name, T* obj) {
        return m_machine.timer_alloc([obj, cb](s32 p) { (obj->*cb)(p); }, name);
    }
    template <typename T> void save_item(T&, const char*) {}
    template <typename T> void save_pointer(T*, const char*, size_t) {}
    template <typename... A> void logerror(const char*, A&&...) const {}

    virtual void device_config_complete() {}
    virtual void device_start() {}
    virtual void device_reset() {}
    virtual void device_pre_save() {}
    virtual void device_post_load() {}
    virtual ioport_constructor device_input_ports() const { return nullptr; }

private:
    running_machine& m_machine;
    device_type m_type;
    const char* m_tag;
    device_t* m_owner;
    uint32_t m_clock;
};

class device_interface {
public:
    device_interface(device_t& device, const char*) : m_device(device) {}
    virtual ~device_interface() {}
    device_t& device() { return m_device; }
private:
    device_t& m_device;
};

// ---- screen (emu/screen.h): only what mc6847.cpp / gime.cpp call ----
constexpr uint32_t UPDATE_HAS_NOT_CHANGED = 0x0001;
class screen_device {
public:
    int64_t refresh_attoseconds() const { return 1; }
    void set_raw(uint32_t, uint16_t, uint16_t, uint16_t, double, uint16_t, uint16_t) {}
    bool has_screen_update() const { return true; }
    template <typename... A> void set_screen_update(A&&...) {}
    void reset_origin(int, int) {}
    int vpos() const { return 0; }
    int hpos() const { return 0; }
};
class device_video_interface {
public:
    device_video_interface(const machine_config&, device_t&, bool = true) {}
    virtual ~device_video_interface() {}
    screen_device& screen() const { static screen_device s; return s; }
    bool has_screen() const { return false; }
};

// ---- finders (emu/devfind.h): the adapter sets each target directly ----
struct finder_base { static constexpr const char* DUMMY_TAG = "finder_dummy_tag"; };
template <class T, bool Required> class device_finder {
public:
    device_finder(device_t&, const char*) {}
    template <typename U> void set_tag(U&&) {}
    void set_target(T* t) { m_target = t; }
    T* target() const { return m_target; }
    T* operator->() const { return m_target; }
    operator T*() const { return m_target; }
private:
    T* m_target = nullptr;
};
template <class T> using required_device = device_finder<T, true>;
template <class T> using optional_device = device_finder<T, false>;

class memory_region {
public:
    memory_region(uint8_t* base, uint32_t size) : m_base(base), m_size(size) {}
    uint8_t* base() const { return m_base; }
    uint32_t bytes() const { return m_size; }
private:
    uint8_t* m_base;
    uint32_t m_size;
};
typedef device_finder<memory_region, true> required_memory_region;

class cpu_device { public: bool started() const { return true; } };

// ---- callbacks (emu/devcb.h, devdelegate.h) ----
class devcb_write_line {
public:
    struct binder {
        devcb_write_line* p;
        template <class F> void set(F f) { p->m_fn = f; }
    };
    explicit devcb_write_line(device_t&) {}
    binder bind() { return binder{ this }; }
    void operator()(int state) { if (m_fn) m_fn(state); }
private:
    std::function<void(int)> m_fn;
};
class devcb_read8 {
public:
    struct binder {
        devcb_read8* p;
        template <class F> void set(F f) { p->m_fn = f; }
    };
    devcb_read8(device_t&, uint8_t dflt) : m_dflt(dflt) {}
    binder bind() { return binder{ this }; }
    uint8_t operator()(offs_t offset) { return m_fn ? m_fn(offset) : m_dflt; }
private:
    std::function<uint8_t(offs_t)> m_fn;
    uint8_t m_dflt;
};
template <typename Signature> class device_delegate;
template <typename R, typename... A> class device_delegate<R (A...)> {
public:
    explicit device_delegate(device_t&) {}
    template <typename... B> void set(B&&...) {}
    void resolve() {}
    bool isnull() const { return !m_fn; }
    R operator()(A... a) const { return m_fn(a...); }
private:
    std::function<R (A...)> m_fn;
};

// ---- memory spaces (emu/emumem.h): declarations for 6883sam.h only ----
enum endianness_t { ENDIANNESS_LITTLE, ENDIANNESS_BIG };
class address_space_config {
public:
    address_space_config() {}
    address_space_config(const char*, endianness_t, u8, u8, s8) {}
};
template <int Width, int AddrShift, int Level, endianness_t Endian> struct memory_access {
    struct cache { u8 read_byte(offs_t) { return 0; } };
    struct specific { u8 read_byte(offs_t) { return 0; } };
};
class device_memory_interface {
public:
    typedef std::vector<std::pair<int, const address_space_config*>> space_config_vector;
    virtual ~device_memory_interface() {}
    virtual space_config_vector memory_space_config() const = 0;
};

#endif
