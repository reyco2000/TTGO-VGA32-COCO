/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : mc6809.h
 *  Module : Motorola MC6809 CPU emulation interface
 * ============================================================
*/

#ifndef MC6809_H
#define MC6809_H

#include <Arduino.h>

// Condition code register flags
#define MC6809_FLAG_E   0x80   // Entire state saved
#define MC6809_FLAG_F   0x40   // FIRQ mask
#define MC6809_FLAG_H   0x20   // Half carry (from bit 3)
#define MC6809_FLAG_I   0x10   // IRQ mask
#define MC6809_FLAG_N   0x08   // Negative
#define MC6809_FLAG_Z   0x04   // Zero
#define MC6809_FLAG_V   0x02   // Overflow
#define MC6809_FLAG_C   0x01   // Carry

// Interrupt vectors
#define MC6809_VEC_RESET    0xFFFE
#define MC6809_VEC_NMI      0xFFFC
#define MC6809_VEC_SWI      0xFFFA
#define MC6809_VEC_IRQ      0xFFF8
#define MC6809_VEC_FIRQ     0xFFF6
#define MC6809_VEC_SWI2     0xFFF4
#define MC6809_VEC_SWI3     0xFFF2

// CPU state
typedef struct MC6809 {
    // Registers
    uint16_t pc;        // Program counter
    uint16_t d;         // D register (A = high byte, B = low byte)
    uint16_t x;         // Index register X
    uint16_t y;         // Index register Y
    uint16_t u;         // User stack pointer
    uint16_t s;         // Hardware stack pointer
    uint8_t  dp;        // Direct page register
    uint8_t  cc;        // Condition codes

    // Cycle counter
    unsigned long cycles;   // Total cycles executed this call

    // Internal state
    bool     halted;                // CPU halted
    bool     wait_for_interrupt;    // CWAI/SYNC state
    bool     cwai_state;            // true if CWAI pushed state (skip push on interrupt)
    bool     nmi_armed;
    bool     nmi_line;              // NMI pin level (for edge detection)
    bool     nmi_pending;           // Latched on falling edge of nmi_line
    bool     firq_pending;
    bool     irq_pending;

    // Memory access callbacks (set by machine)
    uint8_t  (*read)(uint16_t addr);
    void     (*write)(uint16_t addr, uint8_t val);
} MC6809;

// Initialize CPU state (zero all registers, mask interrupts)
void mc6809_init(MC6809* cpu);

// Reset CPU (reads reset vector, sets initial state)
void mc6809_reset(MC6809* cpu);

// Execute instructions for up to 'budget' cycles.
// Returns actual cycles consumed.
int mc6809_run(MC6809* cpu, int budget);

// Debug: dump CPU trace buffer to serial
void mc6809_dump_trace(void);

// Signal interrupts
// NMI is edge-triggered: only latches on inactive→active transition
void mc6809_nmi(MC6809* cpu, bool active);
void mc6809_firq(MC6809* cpu, bool active);
void mc6809_irq(MC6809* cpu, bool active);

// Inline register helpers
static inline uint8_t mc6809_get_a(const MC6809* cpu) {
    return (uint8_t)(cpu->d >> 8);
}

static inline uint8_t mc6809_get_b(const MC6809* cpu) {
    return (uint8_t)(cpu->d & 0xFF);
}

static inline void mc6809_set_a(MC6809* cpu, uint8_t val) {
    cpu->d = (cpu->d & 0x00FF) | ((uint16_t)val << 8);
}

static inline void mc6809_set_b(MC6809* cpu, uint8_t val) {
    cpu->d = (cpu->d & 0xFF00) | val;
}

#endif // MC6809_H
