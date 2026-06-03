/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : mc6551.h
 *  Module : MC6551 ACIA — Tandy Deluxe RS-232 Pak chip emulation ($FF68-$FF6B)
 * ============================================================
*/

#ifndef MC6551_H
#define MC6551_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Host transport callbacks, wired by hal_rs232_begin().
//   tx_byte_out: deliver one byte to the host (Serial.write).
//   rx_byte_in:  pull one pending byte from the host ring; returns false if none.
extern void (*mc6551_tx_byte_out)(uint8_t b);
extern bool (*mc6551_rx_byte_in)(uint8_t* out);

// Hard reset — all registers cleared, TDRE=1, RDRF=0, IRQ deasserted, counters zeroed.
void mc6551_reset(void);

// Register access (reg = 0..3, i.e. addr & 0x03).
uint8_t mc6551_read(uint8_t reg);
void    mc6551_write(uint8_t reg, uint8_t val);

// Advance the chip by the given number of CPU cycles (call once per scanline).
void mc6551_tick(uint32_t cpu_cycles);

// True while the chip is asserting its IRQ line (routed to FIRQ on the Deluxe Pak).
bool mc6551_irq_active(void);

// Snapshot of internal state for the OSD debug page.
struct MC6551Debug {
    uint8_t  status;
    uint8_t  command;
    uint8_t  control;
    uint16_t baud;          // bits/sec from the baud table (0 = "ext clock", no throttle)
    bool     tdre;
    bool     rdrf;
    bool     overrun;
    bool     tx_irq_en;
    bool     rx_irq_en;
    bool     echo;
    uint32_t tx_count;
    uint32_t rx_count;
    uint32_t overrun_count;
    uint32_t firq_count;
};
void mc6551_get_debug(MC6551Debug* d);

#ifdef __cplusplus
}
#endif

#endif // MC6551_H
