/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : mc6551.cpp
 *  Module : MC6551 ACIA — Tandy Deluxe RS-232 Pak chip emulation
 * ============================================================
*/

#include "mc6551.h"

// CoCo CPU clock used to convert programmed baud into a cycle countdown.
// Mirrors CPU_CLOCK_HZ in config.h; duplicated as a plain constant so this
// translation unit stays free of Arduino/config includes.
static const uint32_t MC6551_CPU_CLOCK_HZ = 895000;

// 6551 baud select (control bits 0..3) → bits/sec. Index 0 = 16x external
// clock, which has no source in this emulator → treated as "no throttle".
static const uint16_t baud_table[16] = {
    /* 0 */ 0,    /* 1 */ 50,   /* 2 */ 75,   /* 3 */ 110,
    /* 4 */ 134,  /* 5 */ 150,  /* 6 */ 300,  /* 7 */ 600,
    /* 8 */ 1200, /* 9 */ 1800, /*10 */ 2400, /*11 */ 3600,
    /*12 */ 4800, /*13 */ 7200, /*14 */ 9600, /*15 */ 19200
};

// Callback pointers (wired by hal_rs232).
void (*mc6551_tx_byte_out)(uint8_t b) = nullptr;
bool (*mc6551_rx_byte_in)(uint8_t* out) = nullptr;

// ------------------------------------------------------------
// Chip state
// ------------------------------------------------------------
namespace {

uint8_t  reg_command = 0;
uint8_t  reg_control = 0;
uint8_t  rx_data     = 0;

bool     tdre        = true;   // TX data register empty
bool     rdrf        = false;  // RX data register full
bool     overrun     = false;
bool     irq_flag    = false;  // status bit 7 — cleared by reading status

// Derived from command/control (recomputed on write).
bool     rx_irq_en   = false;  // command bit 1 == 0
bool     tx_irq_en   = false;  // command bits[3:2] == 01
bool     echo_en     = false;  // command bit 4
uint16_t baud        = 0;
int32_t  cycles_per_byte = 1;

// TX pacing.
bool     tx_busy     = false;
int32_t  tx_countdown = 0;
bool     tx_pending  = false;
uint8_t  tx_pending_byte = 0;

// RX pacing.
int32_t  rx_countdown = 0;

// Debug counters.
uint32_t tx_count = 0, rx_count = 0, overrun_count = 0, firq_count = 0;

void recompute_command() {
    rx_irq_en = (reg_command & 0x02) == 0;            // bit1 low = RX IRQ enabled
    tx_irq_en = ((reg_command >> 2) & 0x03) == 0x01;  // bits[3:2]=01 = TX IRQ enabled
    echo_en   = (reg_command & 0x10) != 0;            // bit4 = receiver echo
}

void recompute_control() {
    baud = baud_table[reg_control & 0x0F];
    if (baud == 0) {
        cycles_per_byte = 1;  // no throttle (treat as effectively infinite)
    } else {
        // 8N1 = 10 bit-times per byte.
        cycles_per_byte = (int32_t)((MC6551_CPU_CLOCK_HZ * 10UL) / baud);
        if (cycles_per_byte < 1) cycles_per_byte = 1;
    }
}

void set_irq() {
    if (!irq_flag) {
        irq_flag = true;
        firq_count++;
    }
}

// Submit a byte to the TX path. Delivers immediately if the line is idle,
// otherwise holds it in the 1-deep pending slot (newer overwrites older —
// the CoCo is expected to honor TDRE, and a stale echo is the one we drop).
void tx_submit(uint8_t b) {
    if (!tx_busy) {
        tx_busy = true;
        tx_countdown = cycles_per_byte;
        tdre = false;
        if (mc6551_tx_byte_out) mc6551_tx_byte_out(b);
        tx_count++;
    } else {
        tx_pending = true;
        tx_pending_byte = b;
    }
}

uint8_t assemble_status() {
    uint8_t s = 0;
    if (irq_flag) s |= 0x80;
    // bit6 DSR, bit5 DCD hardwired to 0 (modem always ready)
    if (tdre)     s |= 0x10;
    if (rdrf)     s |= 0x08;
    if (overrun)  s |= 0x04;
    // bit1 framing, bit0 parity always 0 (lossless transport)
    return s;
}

} // namespace

// ------------------------------------------------------------
// Public API
// ------------------------------------------------------------

void mc6551_reset(void) {
    reg_command = 0;
    reg_control = 0;
    rx_data     = 0;
    tdre        = true;
    rdrf        = false;
    overrun     = false;
    irq_flag    = false;
    tx_busy     = false;
    tx_pending  = false;
    tx_pending_byte = 0;
    tx_count = rx_count = overrun_count = firq_count = 0;
    recompute_command();
    recompute_control();
    tx_countdown = cycles_per_byte;
    rx_countdown = cycles_per_byte;
}

uint8_t mc6551_read(uint8_t reg) {
    switch (reg & 0x03) {
    case 0:  // RX data — reading clears RDRF and the error flags
        rdrf = false;
        overrun = false;
        return rx_data;
    case 1:  // Status — reading clears the IRQ latch (bit 7)
    {
        uint8_t s = assemble_status();
        irq_flag = false;
        return s;
    }
    case 2:  // Command
        return reg_command;
    default: // 3: Control
        return reg_control;
    }
}

void mc6551_write(uint8_t reg, uint8_t val) {
    switch (reg & 0x03) {
    case 0:  // TX data
        tx_submit(val);
        break;
    case 1:  // Programmed reset: clears overrun + command bits 0-4, TDRE=1,
             // RDRF=0, IRQ deasserted; preserves the control register.
        overrun  = false;
        rdrf     = false;
        tdre     = true;
        irq_flag = false;
        reg_command &= ~0x1F;
        recompute_command();
        break;
    case 2:  // Command
        reg_command = val;
        recompute_command();
        break;
    case 3:  // Control
        reg_control = val;
        recompute_control();
        break;
    }
}

void mc6551_tick(uint32_t cpu_cycles) {
    int32_t cyc = (int32_t)cpu_cycles;

    // --- TX completion ---
    if (tx_busy) {
        tx_countdown -= cyc;
        if (tx_countdown <= 0) {
            tx_busy = false;
            tdre = true;
            if (tx_irq_en) set_irq();
            if (tx_pending) {
                tx_pending = false;
                tx_submit(tx_pending_byte);
            }
        }
    }

    // --- RX intake (paced) ---
    rx_countdown -= cyc;
    if (rx_countdown <= 0) {
        uint8_t b;
        if (!rdrf) {
            if (mc6551_rx_byte_in && mc6551_rx_byte_in(&b)) {
                rx_data = b;
                rdrf = true;
                rx_count++;
                if (rx_irq_en) set_irq();
                rx_countdown = cycles_per_byte;
                if (echo_en) tx_submit(b);
            }
            // ring empty: leave countdown <=0 so the next byte is picked up promptly
        } else {
            // Previous byte unread — a new arrival is an overrun (dropped).
            if (mc6551_rx_byte_in && mc6551_rx_byte_in(&b)) {
                overrun = true;
                overrun_count++;
                rx_countdown = cycles_per_byte;
            }
        }
    }
}

bool mc6551_irq_active(void) {
    return irq_flag;
}

void mc6551_get_debug(MC6551Debug* d) {
    if (!d) return;
    d->status        = assemble_status();
    d->command       = reg_command;
    d->control       = reg_control;
    d->baud          = baud;
    d->tdre          = tdre;
    d->rdrf          = rdrf;
    d->overrun       = overrun;
    d->tx_irq_en     = tx_irq_en;
    d->rx_irq_en     = rx_irq_en;
    d->echo          = echo_en;
    d->tx_count      = tx_count;
    d->rx_count      = rx_count;
    d->overrun_count = overrun_count;
    d->firq_count    = firq_count;
}
