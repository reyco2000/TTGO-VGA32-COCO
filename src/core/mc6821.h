/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : mc6821.h
 *  Module : Motorola 6821 PIA interface — two PIAs for keyboard/audio/VDG I/O
 * ============================================================
*/

#ifndef MC6821_H
#define MC6821_H

#include <Arduino.h>

// PIA register offsets
#define PIA_REG_DA   0    // Data/Direction register A (selected by CRA bit 2)
#define PIA_REG_CRA  1    // Control register A
#define PIA_REG_DB   2    // Data/Direction register B (selected by CRB bit 2)
#define PIA_REG_CRB  3    // Control register B

// Control register bits
#define PIA_CR_IRQ1      0x80   // IRQA1/IRQB1 flag (read-only)
#define PIA_CR_IRQ2      0x40   // IRQA2/IRQB2 flag (read-only)
#define PIA_CR_DDR_SEL   0x04   // 0=DDR selected, 1=data register selected

typedef struct MC6821 {
    // Port A
    uint8_t data_a;       // Output data register A
    uint8_t ddr_a;        // Data direction register A (0=input, 1=output)
    uint8_t ctrl_a;       // Control register A
    uint8_t input_a;      // External input on port A lines

    // Port B
    uint8_t data_b;       // Output data register B
    uint8_t ddr_b;        // Data direction register B
    uint8_t ctrl_b;       // Control register B
    uint8_t input_b;      // External input on port B lines

    // IRQ output callbacks
    void (*irq_a_callback)(bool active);
    void (*irq_b_callback)(bool active);
} MC6821;

// Initialize PIA to power-on state
void mc6821_init(MC6821* pia);

// Reset PIA
void mc6821_reset(MC6821* pia);

// CPU interface: read/write PIA registers
uint8_t mc6821_read(MC6821* pia, uint8_t reg);
void mc6821_write(MC6821* pia, uint8_t reg, uint8_t val);

// External signal inputs (directly set port input lines)
void mc6821_set_input_a(MC6821* pia, uint8_t val);
void mc6821_set_input_b(MC6821* pia, uint8_t val);

// CA1/CA2/CB1/CB2 control line transitions
void mc6821_ca1_transition(MC6821* pia, bool rising);
void mc6821_cb1_transition(MC6821* pia, bool rising);

#endif // MC6821_H
