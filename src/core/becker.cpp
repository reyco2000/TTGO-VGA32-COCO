/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : becker.cpp
 *  Module : Becker port — DriveWire byte pipe at $FF41/$FF42
 * ============================================================
 */

#include "becker.h"

// Ring sizes (power of two). RX holds a full READEX reply with room to spare;
// TX only ever carries short DriveWire requests plus 256-byte WRITE payloads.
static const uint32_t RX_SIZE = 4096;
static const uint32_t TX_SIZE = 1024;

bool          g_becker_enabled   = false;
volatile bool g_becker_tx_pending = false;

static uint8_t* rx_buf = nullptr;
static uint8_t* tx_buf = nullptr;

// Indices are free-running; masked on access. Each index has one writer.
static volatile uint32_t rx_head = 0;  // written by core 0
static volatile uint32_t rx_tail = 0;  // written by core 1
static volatile uint32_t tx_head = 0;  // written by core 1
static volatile uint32_t tx_tail = 0;  // written by core 0

// A consumer-side index can only be moved by its owner, so cross-core
// flushes are requests serviced by the consumer on its next access. The
// request records the producer index at the time it was made, and the
// consumer only skips up to that mark: bytes produced after the request
// (e.g. HDB-DOS's first command after a reset, queued before the link comes
// up) must survive.
static volatile bool     rx_flush_req  = false;  // set by core 0, serviced by core 1
static volatile uint32_t rx_flush_mark = 0;
static volatile bool     tx_flush_req  = false;  // set by core 1, serviced by core 0
static volatile uint32_t tx_flush_mark = 0;

// CoCo reset while a link is up: the back end must drop the connection so a
// reply still in flight for a pre-reset request can't land in the RX ring
// and desync the next transaction. Set by core 1, cleared by core 0.
static volatile bool     reset_req     = false;

// Advance a consumer index to `mark` unless it is already past it.
static inline uint32_t skip_to(uint32_t tail, uint32_t mark) {
    return ((int32_t)(mark - tail) > 0) ? mark : tail;
}

static volatile bool     link_up     = false;
static volatile uint32_t bytes_to_coco   = 0;
static volatile uint32_t bytes_from_coco = 0;

static TaskHandle_t backend_task = nullptr;

static inline void barrier(void) { __sync_synchronize(); }

bool becker_init(void) {
    if (!rx_buf) rx_buf = (uint8_t*)ps_malloc(RX_SIZE);
    if (!tx_buf) tx_buf = (uint8_t*)ps_malloc(TX_SIZE);
    if (!rx_buf || !tx_buf) {
        g_becker_enabled = false;
        return false;
    }
    rx_head = rx_tail = tx_head = tx_tail = 0;
    g_becker_enabled = true;
    return true;
}

// ------------------------------------------------------------
// CPU side (core 1)
// ------------------------------------------------------------

static inline void service_rx_flush(void) {
    if (rx_flush_req) {
        barrier();
        rx_tail = skip_to(rx_tail, rx_flush_mark);
        barrier();
        rx_flush_req = false;
    }
}

uint8_t becker_read_status(void) {
    service_rx_flush();
    return (rx_head != rx_tail) ? 0x02 : 0x00;
}

uint8_t becker_read_data(void) {
    service_rx_flush();
    uint32_t t = rx_tail;
    if (rx_head == t) return 0x00;
    barrier();
    uint8_t b = rx_buf[t & (RX_SIZE - 1)];
    barrier();
    rx_tail = t + 1;
    bytes_to_coco++;
    return b;
}

void becker_write_data(uint8_t b) {
    uint32_t h = tx_head;
    if (h - tx_tail >= TX_SIZE) return;  // full: back end is not draining — drop
    tx_buf[h & (TX_SIZE - 1)] = b;
    barrier();
    tx_head = h + 1;
    bytes_from_coco++;
    g_becker_tx_pending = true;
}

void becker_reset(void) {
    if (!g_becker_enabled) return;
    rx_tail = rx_head;
    tx_flush_mark = tx_head;
    barrier();
    tx_flush_req = true;
    reset_req = true;
}

void becker_notify_backend(void) {
    g_becker_tx_pending = false;
    if (backend_task) xTaskNotifyGive(backend_task);
}

// ------------------------------------------------------------
// Back-end side (core 0)
// ------------------------------------------------------------

void becker_set_backend_task(TaskHandle_t task) { backend_task = task; }

size_t becker_tx_pop(uint8_t* buf, size_t max) {
    if (tx_flush_req) {
        barrier();
        tx_tail = skip_to(tx_tail, tx_flush_mark);
        barrier();
        tx_flush_req = false;
    }
    uint32_t t = tx_tail;
    uint32_t avail = tx_head - t;
    barrier();
    size_t n = (avail < max) ? avail : max;
    for (size_t i = 0; i < n; i++) buf[i] = tx_buf[(t + i) & (TX_SIZE - 1)];
    barrier();
    tx_tail = t + n;
    return n;
}

size_t becker_rx_space(void) {
    return RX_SIZE - (rx_head - rx_tail);
}

size_t becker_rx_push(const uint8_t* buf, size_t len) {
    uint32_t h = rx_head;
    size_t space = RX_SIZE - (h - rx_tail);
    size_t n = (len < space) ? len : space;
    for (size_t i = 0; i < n; i++) rx_buf[(h + i) & (RX_SIZE - 1)] = buf[i];
    barrier();
    rx_head = h + n;
    return n;
}

bool becker_reset_requested(void) { return reset_req; }

void becker_clear_reset(void) {
    if (!reset_req) return;
    barrier();
    tx_tail = skip_to(tx_tail, tx_flush_mark);   // drop only pre-reset bytes
    barrier();
    tx_flush_req = false;
    reset_req = false;
}

void becker_set_link_up(bool up) {
    if (!up) {
        // Drop everything in flight: a half-delivered reply is useless to
        // the CoCo. For a reset-triggered drop keep the bytes the CoCo wrote
        // after the reset (its first new command); for a real link loss the
        // pending request is stale too.
        if (reset_req) becker_clear_reset();
        else           tx_tail = tx_head;
        rx_flush_mark = rx_head;
        barrier();
        rx_flush_req = true;
    }
    link_up = up;
}

// ------------------------------------------------------------
// Status
// ------------------------------------------------------------

bool     becker_link_up(void)         { return link_up; }
uint32_t becker_bytes_to_coco(void)   { return bytes_to_coco; }
uint32_t becker_bytes_from_coco(void) { return bytes_from_coco; }
