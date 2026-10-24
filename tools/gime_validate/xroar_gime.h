/*
 * GIME validation suite: C API over XRoar's tcc1014.c (compiled unmodified,
 * see xroar_gime.c). One GIME instance at a time: xg_init() replaces it.
 * Time is in 14.31818 MHz ticks and only moves through xg_advance().
 */
#ifndef XROAR_GIME_H
#define XROAR_GIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     xg_init(int is_1987, uint8_t* ram, uint32_t ram_size);  // allocate, finish, reset
void     xg_cycle(uint16_t addr, int rnw, uint8_t* data);        // one tcc1014_mem_cycle at now
void     xg_bus(unsigned* S, uint32_t* Z, int* RAS);            // S/Z/RAS left by the last xg_cycle
void     xg_decode(uint16_t addr, unsigned* S, uint32_t* Z, int* RAS); // tcc1014_decode + Z
void     xg_advance(uint32_t ticks);                             // dispatch events up to now+ticks
uint64_t xg_now(void);
int      xg_irq(void), xg_firq(void);
void     xg_set_il(unsigned line, int level);

// Callbacks the adapter registers: HS/FS edges (level), finished line.
// They run inside xg_advance(); xg_now() is the tick of the edge.
typedef void (*xg_line_fn)(unsigned burst, unsigned npixels, const uint8_t* data);
void     xg_set_hooks(void (*hs)(int), void (*fs)(int), xg_line_fn line);

// Timer expiry: called once per pass through XRoar's update_timer() (the
// counter reaching zero at HS fall when TINS=0, or the TINS=1 timer event),
// whatever $FF92/$FF93 enable. Seen as the private 'blink' toggle, which only
// update_timer() changes. irq_rise/firq_rise: rising edge of public IRQ /
// FIRQ, after an event or after an xg_cycle() (then stamped at now).
void     xg_set_int_hooks(void (*timer)(void), void (*irq_rise)(void),
                          void (*firq_rise)(void));

// Active area of the line being handed to the line hook: pixel offsets from
// HS fall, [start, end). Valid inside the line hook.
void     xg_active_area(unsigned* start, unsigned* end);

#ifdef __cplusplus
}
#endif

#endif
