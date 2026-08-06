/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : hal_rs232.h
 *  Module : RS-232 HAL interface — bridges MC6551 ACIA to UART0 (Serial)
 * ============================================================
*/

#ifndef HAL_RS232_H
#define HAL_RS232_H

#include <stdint.h>
#include <stddef.h>

// Wire the MC6551 callbacks to UART0 and clear the RX ring. Call once from hal_init().
void hal_rs232_begin(void);

// Drain UART0 into the RX ring. Call from the main loop while in RS-232 mode.
void hal_rs232_poll(void);

// Discard any buffered host input (RX ring + UART0 RX FIFO). Called on mode change.
void hal_rs232_flush(void);

// Number of bytes currently buffered in the RX ring (for the debug page).
size_t hal_rs232_ring_fill(void);

// RX ring capacity (for the debug page).
size_t hal_rs232_ring_capacity(void);

#endif // HAL_RS232_H
