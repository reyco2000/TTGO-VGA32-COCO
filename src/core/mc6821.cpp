/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : mc6821.cpp
 *  Module : Motorola 6821 PIA emulation — keyboard, joystick, audio, and VDG control
 * ============================================================
*/

/*
 * mc6821.cpp - Motorola 6821 PIA emulation
 *
 * Derived from XRoar's mc6821.c by Ciaran Anscomb.
 *
 * Control register bits:
 *   Bit 7: IRQ1 flag (read-only, set by CA1/CB1 transition)
 *   Bit 6: IRQ2 flag (read-only, set by CA2/CB2 transition)
 *   Bit 5: CA2/CB2 direction (1=output)
 *   Bit 4: CA2/CB2 output control (if bit5=1)
 *   Bit 3: CA2/CB2 output control / edge select (if bit5=0)
 *   Bit 2: DDR/Data select (0=DDR, 1=data register)
 *   Bit 1: CA1/CB1 edge select (0=falling, 1=rising)
 *   Bit 0: CA1/CB1 IRQ enable (1=enable interrupt output)
 */

#include "mc6821.h"
#include "../utils/debug.h"

void mc6821_init(MC6821* pia) {
    DEBUG_PRINT("  PIA: MC6821 init");
    memset(pia, 0, sizeof(MC6821));
    pia->input_a = 0xFF;
    pia->input_b = 0xFF;
    pia->irq_a_callback = nullptr;
    pia->irq_b_callback = nullptr;
}

void mc6821_reset(MC6821* pia) {
    DEBUG_PRINT("  PIA: MC6821 reset");
    // Preserve callbacks across reset
    auto irq_a = pia->irq_a_callback;
    auto irq_b = pia->irq_b_callback;

    pia->data_a = 0;
    pia->ddr_a = 0;
    pia->ctrl_a = 0;
    pia->input_a = 0xFF;

    pia->data_b = 0;
    pia->ddr_b = 0;
    pia->ctrl_b = 0;
    pia->input_b = 0xFF;

    pia->irq_a_callback = irq_a;
    pia->irq_b_callback = irq_b;

    // Deassert IRQ outputs
    if (irq_a) irq_a(false);
    if (irq_b) irq_b(false);
}

// Check if IRQA output should be asserted
// IRQA is active if (IRQ1 flag set AND IRQ1 enabled) OR (IRQ2 flag set AND IRQ2 enabled)
static void pia_update_irq_a(MC6821* pia) {
    bool irq1_active = (pia->ctrl_a & PIA_CR_IRQ1) && (pia->ctrl_a & 0x01);
    bool irq2_active = (pia->ctrl_a & PIA_CR_IRQ2) && (pia->ctrl_a & 0x08) && !(pia->ctrl_a & 0x20);
    bool irq = irq1_active || irq2_active;
    if (pia->irq_a_callback) pia->irq_a_callback(irq);
}

static void pia_update_irq_b(MC6821* pia) {
    bool irq1_active = (pia->ctrl_b & PIA_CR_IRQ1) && (pia->ctrl_b & 0x01);
    bool irq2_active = (pia->ctrl_b & PIA_CR_IRQ2) && (pia->ctrl_b & 0x08) && !(pia->ctrl_b & 0x20);
    bool irq = irq1_active || irq2_active;
    if (pia->irq_b_callback) pia->irq_b_callback(irq);
}

uint8_t mc6821_read(MC6821* pia, uint8_t reg) {
    switch (reg & 0x03) {
    case PIA_REG_DA:
        if (pia->ctrl_a & PIA_CR_DDR_SEL) {
            // Read data register: output bits from data_a, input bits from input_a
            // Clear IRQ flags on data register read
            pia->ctrl_a &= ~(PIA_CR_IRQ1 | PIA_CR_IRQ2);
            pia_update_irq_a(pia);
            return (pia->data_a & pia->ddr_a) | (pia->input_a & ~pia->ddr_a);
        } else {
            return pia->ddr_a;
        }

    case PIA_REG_CRA:
        return pia->ctrl_a;

    case PIA_REG_DB:
        if (pia->ctrl_b & PIA_CR_DDR_SEL) {
            // Port B: output bits always from data_b (unlike port A)
            pia->ctrl_b &= ~(PIA_CR_IRQ1 | PIA_CR_IRQ2);
            pia_update_irq_b(pia);
            return (pia->data_b & pia->ddr_b) | (pia->input_b & ~pia->ddr_b);
        } else {
            return pia->ddr_b;
        }

    case PIA_REG_CRB:
        return pia->ctrl_b;
    }
    return 0xFF;
}

void mc6821_write(MC6821* pia, uint8_t reg, uint8_t val) {
    switch (reg & 0x03) {
    case PIA_REG_DA:
        if (pia->ctrl_a & PIA_CR_DDR_SEL) {
            pia->data_a = val;
        } else {
            pia->ddr_a = val;
        }
        break;

    case PIA_REG_CRA:
        // Bits 7,6 are read-only IRQ flags; preserve them
        pia->ctrl_a = (pia->ctrl_a & 0xC0) | (val & 0x3F);
        pia_update_irq_a(pia);
        break;

    case PIA_REG_DB:
        if (pia->ctrl_b & PIA_CR_DDR_SEL) {
            pia->data_b = val;
        } else {
            pia->ddr_b = val;
        }
        break;

    case PIA_REG_CRB:
        pia->ctrl_b = (pia->ctrl_b & 0xC0) | (val & 0x3F);
        pia_update_irq_b(pia);
        break;
    }
}

void mc6821_set_input_a(MC6821* pia, uint8_t val) {
    pia->input_a = val;
}

void mc6821_set_input_b(MC6821* pia, uint8_t val) {
    pia->input_b = val;
}

void mc6821_ca1_transition(MC6821* pia, bool rising) {
    // CA1 edge select: ctrl_a bit 1
    //   bit 1 = 0: IRQ on falling edge
    //   bit 1 = 1: IRQ on rising edge
    bool edge_match = rising == ((pia->ctrl_a & 0x02) != 0);

    if (edge_match) {
        // Set IRQ1 flag
        pia->ctrl_a |= PIA_CR_IRQ1;
        pia_update_irq_a(pia);
    }
}

void mc6821_cb1_transition(MC6821* pia, bool rising) {
    // CB1 edge select: ctrl_b bit 1
    bool edge_match = rising == ((pia->ctrl_b & 0x02) != 0);

    if (edge_match) {
        pia->ctrl_b |= PIA_CR_IRQ1;
        pia_update_irq_b(pia);
    }
}
