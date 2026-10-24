// GIME validation suite: wraps XRoar's tcc1014.c (unmodified, from an XRoar
// source tree) behind the C API in xroar_gime.h. Built only when run.sh finds
// $XROAR_SRC. Pattern: tools/cpu_test/xroar_oracle.c.
//
// tcc1014.c is #included rather than linked so its private state is
// reachable. Linked with it: XRoar's real events.c, tcc1014/font-gime.c and
// portalib delegate.c. Everything else it references (part system, logging,
// serialisation, debug feature types) is stubbed below as plumbing only.

#include "tcc1014/tcc1014.c"

#include "xroar_gime.h"

// ---- stubs for XRoar services unused here ----
struct xroar xroar;     // MACHINE_EVENT_LIST is xroar.machine_events
struct logging logging;
void *part_new(size_t psize) { return calloc(1, psize); }
void *xmalloc(size_t s) { void *p = malloc(s); if (!p) abort(); return p; }
const struct debug_feature_type debug_feature_type_uint8, debug_feature_type_uint16;
void ser_set_error(struct ser_handle *sh, int error) { (void)sh; (void)error; }
void ser_read(struct ser_handle *sh, void *ptr, size_t size) { (void)sh; (void)ptr; (void)size; }
uint8_t ser_read_uint8(struct ser_handle *sh) { (void)sh; return 0; }
void ser_write(struct ser_handle *sh, int tag, const void *ptr, size_t size) {
	(void)sh; (void)tag; (void)ptr; (void)size;
}
void ser_write_tag(struct ser_handle *sh, int tag, size_t length) { (void)sh; (void)tag; (void)length; }
void ser_write_uint8_untagged(struct ser_handle *sh, uint8_t v) { (void)sh; (void)v; }
void ser_write_close_tag(struct ser_handle *sh) { (void)sh; }

// ---- oracle ----
static struct event_list xg_events;
static struct TCC1014_private *xg_gime;
static uint8_t *xg_ram;
static uint32_t xg_ram_mask;
static uint64_t xg_base64;        // xg_now() at event_current_tick == xg_base32
static event_ticks xg_base32;
static bool xg_irq_prev, xg_firq_prev;

static void (*xg_hs)(int);
static void (*xg_fs)(int);
static xg_line_fn xg_line;
static void (*xg_timer)(void);
static void (*xg_irq_rise)(void);
static void (*xg_firq_rise)(void);

static uint16_t xg_fetch_vram(void *sptr, uint32_t A) {
	(void)sptr;
	// 16 bits at once from an even address, as coco3.c fetch_vram() does.
	uint32_t a = A & ~1u;
	return (uint16_t)((xg_ram[a & xg_ram_mask] << 8) | xg_ram[(a + 1) & xg_ram_mask]);
}

static void xg_signal_hs(void *sptr, bool level) {
	(void)sptr;
	if (xg_hs) xg_hs(level);
}

static void xg_signal_fs(void *sptr, bool level) {
	(void)sptr;
	if (xg_fs) xg_fs(level);
}

static void xg_render_line(void *sptr, unsigned burst, unsigned npixels, const uint8_t *data) {
	(void)sptr;
	if (xg_line) xg_line(burst, npixels, data);
}

// Report rising edges of the public interrupt outputs.
static void xg_check_edges(void) {
	bool irq = xg_gime->public.IRQ, firq = xg_gime->public.FIRQ;
	if (irq && !xg_irq_prev && xg_irq_rise) xg_irq_rise();
	if (firq && !xg_firq_prev && xg_firq_rise) xg_firq_rise();
	xg_irq_prev = irq;
	xg_firq_prev = firq;
}

void xg_init(int is_1987, uint8_t *ram, uint32_t ram_size) {
	if (xg_gime) {
		tcc1014_free(&xg_gime->public.part);
		free(xg_gime);
		xg_gime = NULL;
	}
	event_list_init(&xg_events);
	xroar.machine_events = &xg_events;
	event_current_tick = 0;
	xg_base64 = 0;
	xg_base32 = 0;
	xg_ram = ram;
	xg_ram_mask = ram_size - 1;

	// What the part system does for part_create("TCC1014-198x").
	struct part *p = tcc1014_allocate();
	p->partdb = is_1987 ? &tcc1014_1987_part : &tcc1014_1986_part;
	tcc1014_initialise(p, NULL);
	xg_gime = (struct TCC1014_private *)p;
	struct TCC1014 *g = &xg_gime->public;

	// Delegates, as coco3.c wires them before finish.
	g->fetch_vram = DELEGATE_AS1(uint16, uint32, xg_fetch_vram, NULL);
	g->signal_hs = DELEGATE_AS1(void, bool, xg_signal_hs, NULL);
	g->signal_fs = DELEGATE_AS1(void, bool, xg_signal_fs, NULL);
	g->render_line = DELEGATE_AS3(void, unsigned, unsigned, uint8cp, xg_render_line, NULL);

	tcc1014_finish(p);
	tcc1014_notify_mode(g);     // coco3.c finish
	tcc1014_reset(g);           // coco3.c reset
	xg_irq_prev = g->IRQ;
	xg_firq_prev = g->FIRQ;
}

void xg_cycle(uint16_t addr, int rnw, uint8_t *data) {
	xg_gime->public.CPUD = data;
	tcc1014_mem_cycle(&xg_gime->public, rnw, addr);
	xg_check_edges();
}

void xg_bus(unsigned *S, uint32_t *Z, int *RAS) {
	*S = xg_gime->public.S;
	*Z = xg_gime->public.Z;
	*RAS = xg_gime->public.RAS;
}

// S from tcc1014_decode(); Z and RAS from a read cycle on a copy of the GIME
// state, restored afterwards so acknowledges and IL sampling leave no trace.
// A read cycle queues no events, so the embedded event nodes are unchanged.
void xg_decode(uint16_t addr, unsigned *S, uint32_t *Z, int *RAS) {
	struct TCC1014_private saved = *xg_gime;
	uint8_t d = 0xff;
	*S = tcc1014_decode(&xg_gime->public, addr);
	xg_gime->public.CPUD = &d;
	tcc1014_mem_cycle(&xg_gime->public, 1, addr);
	// tcc1014_mem_cycle() leaves Z untouched for $FF00-$FFFF; the suite's
	// convention (model_ours.cpp too) is Z = 0 there.
	*Z = addr >= 0xff00 ? 0 : xg_gime->public.Z;
	*RAS = xg_gime->public.RAS;
	*xg_gime = saved;
}

void xg_advance(uint32_t ticks) {
	event_ticks to_time = event_current_tick + ticks;
	while (event_pending(&xg_events, to_time)) {
		bool blink = xg_gime->blink;
		event_dispatch_next(&xg_events);
		if (xg_gime->blink != blink && xg_timer) xg_timer();
		xg_check_edges();
	}
	event_current_tick = to_time;
	xg_base64 += (event_ticks)(event_current_tick - xg_base32);
	xg_base32 = event_current_tick;
}

uint64_t xg_now(void) {
	return xg_base64 + (event_ticks)(event_current_tick - xg_base32);
}

int xg_irq(void) { return xg_gime->public.IRQ; }
int xg_firq(void) { return xg_gime->public.FIRQ; }

void xg_set_il(unsigned line, int level) {
	if (line == 0) xg_gime->public.IL0 = level;
	else if (line == 1) xg_gime->public.IL1 = level;
	else if (line == 2) xg_gime->public.IL2 = level;
}

void xg_set_hooks(void (*hs)(int), void (*fs)(int), xg_line_fn line) {
	xg_hs = hs;
	xg_fs = fs;
	xg_line = line;
}

void xg_set_int_hooks(void (*timer)(void), void (*irq_rise)(void), void (*firq_rise)(void)) {
	xg_timer = timer;
	xg_irq_rise = irq_rise;
	xg_firq_rise = firq_rise;
}

void xg_active_area(unsigned *start, unsigned *end) {
	*start = xg_gime->horizontal.tHS_AA;
	*end = xg_gime->horizontal.tHS_RB;
}
