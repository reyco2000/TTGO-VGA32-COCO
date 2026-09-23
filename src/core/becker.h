/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : becker.h
 *  Module : Becker port — DriveWire byte pipe at $FF41/$FF42
 * ============================================================
 *
 * The Becker port is the emulator-only DriveWire transport used by XRoar,
 * MAME and VCC together with the HDB-DOS Becker ROMs (hdbdw3bck/hdbdw3bc3):
 *
 *   $FF41 read   status — bit 1 set when a byte is waiting for the CoCo
 *   $FF42 read   data   — pop the next byte sent by the DriveWire back end
 *   $FF42 write  data   — push a byte toward the DriveWire back end
 *
 * The CPU side (core 1) and the back end (core 0, see src/net/dw_client.cpp)
 * talk through two lock-free single-producer/single-consumer rings in PSRAM:
 *
 *   RX ring  back end → CoCo   (producer core 0, consumer core 1)
 *   TX ring  CoCo → back end   (producer core 1, consumer core 0)
 *
 * Nothing on the CPU side ever blocks. When the TX ring goes from empty to
 * non-empty a "pending" flag is set, and becker_scanline_tick() wakes the
 * back-end task at most once per scanline.
 *
 * HDB-DOS DWRead has NO timeout: if the back end dies mid-transaction the
 * CoCo spins forever with interrupts masked. The back end reports that with
 * becker_set_link_up(false), which flushes both rings; status is exposed for
 * the OSD and /api/status.
 */

#ifndef CORE_BECKER_H
#define CORE_BECKER_H

#include <Arduino.h>

// DriveWire bus mode (NVS "sv" key "bus_mode"). A change needs a restart.
typedef enum {
    BUS_MODE_OFF         = 0,  // no Becker port; $FF41/$FF42 are DSKREG mirrors
    BUS_MODE_EXTERNAL    = 1,  // TCP client to pyDriveWire / DW4 / FujiNet-PC
    BUS_MODE_INTERNAL_DW = 2,  // native disk-only DriveWire server (Phase 3)
    BUS_MODE_FUJINET     = 3,  // embedded FujiNet (Phase 4+)
    BUS_MODE_COUNT
} BusMode;

#define BECKER_STATUS_ADDR 0xFF41
#define BECKER_DATA_ADDR   0xFF42

// Set once at boot by becker_init(); gates the address decode.
extern bool g_becker_enabled;

static inline bool becker_enabled(void) { return g_becker_enabled; }

// Allocate the rings and enable the port. Returns false if allocation fails
// (the port then stays disabled).
bool becker_init(void);

// ---- CPU side (core 1) ----
uint8_t becker_read_status(void);
uint8_t becker_read_data(void);
void    becker_write_data(uint8_t b);
void    becker_reset(void);          // CoCo reset: drop any half-done transaction

// Wake the back-end task if the CoCo has sent bytes since the last call.
// Called once per emulated scanline; a single bool test when idle.
extern volatile bool g_becker_tx_pending;
void becker_notify_backend(void);
static inline void becker_scanline_tick(void) {
    if (g_becker_tx_pending) becker_notify_backend();
}

// ---- Back-end side (core 0) ----
void   becker_set_backend_task(TaskHandle_t task);
size_t becker_tx_pop(uint8_t* buf, size_t max);        // bytes the CoCo sent
size_t becker_rx_push(const uint8_t* buf, size_t len); // bytes toward the CoCo
size_t becker_rx_space(void);
void   becker_set_link_up(bool up);                    // false flushes both rings

// ---- Status ----
bool     becker_link_up(void);
uint32_t becker_bytes_to_coco(void);    // total bytes delivered to the CoCo
uint32_t becker_bytes_from_coco(void);  // total bytes written by the CoCo

#endif // CORE_BECKER_H
