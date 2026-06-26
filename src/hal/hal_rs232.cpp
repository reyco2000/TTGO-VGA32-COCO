/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : hal_rs232.cpp
 *  Module : RS-232 HAL — MC6551 ACIA host transport over UART0 (Serial)
 * ============================================================
*/

#include "hal_rs232.h"
#include "../core/mc6551.h"
#include <Arduino.h>

// 256-byte static RX ring. UART0's driver FIFO provides backpressure once
// this is full (hal_rs232_poll stops reading), so no dynamic allocation.
static const size_t RING_SIZE = 256;
static uint8_t  rx_ring[RING_SIZE];
static volatile size_t ring_head = 0;  // write index
static volatile size_t ring_tail = 0;  // read index

static inline size_t ring_count() {
    return (ring_head - ring_tail) & (RING_SIZE - 1);
}

static inline bool ring_full() {
    return ((ring_head + 1) & (RING_SIZE - 1)) == ring_tail;
}

static void ring_clear() {
    ring_head = ring_tail = 0;
}

// --- MC6551 callbacks ---

// CoCo TX → host: byte hits UART0 immediately.
static void rs232_tx(uint8_t b) {
    Serial.write(b);
}

// host → CoCo RX: pop one byte from the ring.
static bool rs232_rx(uint8_t* out) {
    if (ring_head == ring_tail) return false;  // empty
    *out = rx_ring[ring_tail];
    ring_tail = (ring_tail + 1) & (RING_SIZE - 1);
    return true;
}

void hal_rs232_begin(void) {
    ring_clear();
    mc6551_tx_byte_out = rs232_tx;
    mc6551_rx_byte_in  = rs232_rx;
}

void hal_rs232_poll(void) {
    while (Serial.available() > 0 && !ring_full()) {
        rx_ring[ring_head] = (uint8_t)Serial.read();
        ring_head = (ring_head + 1) & (RING_SIZE - 1);
    }
}

void hal_rs232_flush(void) {
    ring_clear();
    while (Serial.available() > 0) Serial.read();
}

size_t hal_rs232_ring_fill(void) {
    return ring_count();
}

size_t hal_rs232_ring_capacity(void) {
    return RING_SIZE;
}
