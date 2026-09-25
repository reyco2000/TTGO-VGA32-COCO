/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : dw_server.cpp
 *  Module : Internal DriveWire back end — disk-only DW3/DW4 server on core 0
 * ============================================================
 *
 * Written as straight-line blocking code (like tools/dw_test_server.py, the
 * reference for the opcode set): rx() waits on the Becker TX ring, tx() fills
 * the RX ring. A CoCo reset aborts the transaction in progress; everything is
 * then resynchronised at the next opcode, exactly as a TCP back end does by
 * reconnecting (see becker_reset_requested).
 */

#include "dw_server.h"

#include <time.h>

#include "wifi_mgr.h"
#include "../core/becker.h"
#include "../supervisor/sv_disk.h"
#include "../../config.h"
#include "../utils/debug.h"

// DriveWire opcodes
#define OP_NOP          0x00
#define OP_NAMEOBJ_MOUNT  0x01
#define OP_NAMEOBJ_CREATE 0x02
#define OP_TIME         0x23
#define OP_SERREAD      0x43
#define OP_SERGETSTAT   0x44
#define OP_SERINIT      0x45
#define OP_PRINTFLUSH   0x46
#define OP_GETSTAT      0x47
#define OP_INIT         0x49
#define OP_PRINT        0x50
#define OP_READ         0x52
#define OP_SETSTAT      0x53
#define OP_TERM         0x54
#define OP_WRITE        0x57
#define OP_DWINIT       0x5A
#define OP_SERREADM     0x63
#define OP_SERWRITEM    0x64
#define OP_REREAD       0x72
#define OP_REWRITE      0x77
#define OP_FASTWRITE_FIRST 0x80
#define OP_FASTWRITE_LAST  0x8E
#define OP_SERWRITE     0xC3
#define OP_SERSETSTAT   0xC4
#define OP_SERTERM      0xC5
#define OP_READEX       0xD2
#define OP_REREADEX     0xF2
#define OP_RESET3       0xF8
#define OP_RESET1       0xFE
#define OP_RESET2       0xFF

#define SS_COMST        0x28   // SERSETSTAT code carrying a 26-byte block

// DriveWire status codes
#define E_OK            0x00
#define E_WP            0xF2   // write protected
#define E_CRC           0xF3   // checksum mismatch
#define E_READ          0xF4
#define E_WRITE         0xF5
#define E_NOTRDY        0xF6

#define SECTOR          256
#define DW_DRIVES       SV_DISK_MAX_DRIVES

// Flush dirty drives this long after the last DriveWire write.
#define DW_FLUSH_IDLE_MS 2000
// Wait between checks when idle. The CPU side wakes us as soon as the CoCo
// writes, so this only paces the background flush and shutdown checks.
#define DW_IDLE_WAIT_MS  50
// FreeRTOS stack (internal RAM). Measured peak ~1.7 KB including a
// background flush to SD; see dw_server_stack_free() in /api/bus.
#define DW_SERVER_STACK  4096

static TaskHandle_t       s_task = nullptr;
static SV_DiskController* s_fdc  = nullptr;

static volatile uint32_t  s_reads = 0, s_writes = 0, s_errors = 0, s_flushes = 0;
static volatile uint8_t   s_last_op = 0;
static volatile bool      s_shutdown_req = false;
static volatile bool      s_shutdown_done = false;

static bool     s_flush_pending = false;
static uint32_t s_last_write_ms = 0;
static bool     s_sntp_started = false;

// Bytes popped from the TX ring but not consumed yet.
static uint8_t  s_in[64];
static size_t   s_in_len = 0, s_in_pos = 0;

static uint8_t  s_sector[SECTOR];

// ------------------------------------------------------------
// Byte transport
// ------------------------------------------------------------

static void flush_dirty_drives(void) {
    for (int d = 0; d < DW_DRIVES; d++) sv_disk_flush(s_fdc, d);
    s_flush_pending = false;
    s_flushes++;
}

// Housekeeping between transactions (never mid-transaction).
static void idle_work(void) {
    if (s_flush_pending && millis() - s_last_write_ms >= DW_FLUSH_IDLE_MS) {
        flush_dirty_drives();
    }
    // Clock for OP_TIME: start SNTP once STA is up (WiFi is optional in
    // Internal mode — without it TIME reports the unsynced system clock).
    if (!s_sntp_started && wifi_mgr_state() == WIFI_MGR_STA_RUNNING) {
        configTzTime(DW_SERVER_TZ, "pool.ntp.org", "time.google.com");
        s_sntp_started = true;
    }
}

// Read exactly n bytes from the CoCo. Returns false if the CoCo was reset
// (or a shutdown was requested) first. `idle` = waiting for an opcode.
static bool rx(uint8_t* dst, size_t n, bool idle = false) {
    while (n > 0) {
        if (s_in_pos < s_in_len) {
            size_t k = s_in_len - s_in_pos;
            if (k > n) k = n;
            memcpy(dst, s_in + s_in_pos, k);
            s_in_pos += k;
            dst += k;
            n -= k;
            continue;
        }
        // Check for a reset BEFORE popping, so that what is popped next can
        // only be post-reset bytes if the reset raced this check (the CoCo
        // takes milliseconds after a reset before it writes again).
        if (becker_reset_requested() || s_shutdown_req) return false;
        s_in_len = becker_tx_pop(s_in, sizeof(s_in));
        s_in_pos = 0;
        if (s_in_len == 0) {
            if (idle) idle_work();
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DW_IDLE_WAIT_MS));
        }
    }
    return true;
}

static inline bool rx_byte(uint8_t* b, bool idle = false) { return rx(b, 1, idle); }

// Send n bytes to the CoCo. The RX ring (4 KB) holds any single reply, but
// wait for space rather than drop if the CoCo is slow to read.
static bool tx(const uint8_t* src, size_t n) {
    while (n > 0) {
        if (becker_reset_requested() || s_shutdown_req) return false;
        size_t k = becker_rx_push(src, n);
        src += k;
        n -= k;
        if (n) vTaskDelay(1);
    }
    return true;
}

static inline bool tx_byte(uint8_t b) { return tx(&b, 1); }

static bool skip(size_t n) {
    uint8_t b;
    while (n--) if (!rx_byte(&b)) return false;
    return true;
}

// ------------------------------------------------------------
// Disk access (PSRAM caches shared with the Disk Manager / WD1793)
// ------------------------------------------------------------

static uint16_t checksum(const uint8_t* p, size_t n) {
    uint16_t s = 0;
    while (n--) s += *p++;
    return s;
}

// HDB-DOS addresses its DRIVE n as LSN n*630 (35 tracks x 18 sectors) on
// DriveWire drive 0 — one "hard drive" of stacked floppies. The Disk Manager
// has one image per drive, so an LSN past the end of the addressed image is
// mapped to Disk Manager drive (drive + lsn/630), sector lsn%630: DW4's
// "HDB-DOS mode". An LSN inside the image is used as-is, so NitrOS-9 and
// HDB-DOS hard-drive images work unchanged. (Only an HDB-DOS DRIVE >= 1
// behind a lower drive holding a > 35-track image resolves wrongly.)
#define HDB_DISK_SECTORS 630

// Resolve (drive, lsn) to a mounted image and byte offset. Call locked.
static SV_DiskImage* resolve(uint8_t drive, uint32_t lsn, uint32_t* off) {
    if (drive >= DW_DRIVES) return nullptr;
    SV_DiskImage* img = &s_fdc->drives[drive];
    if (img->mounted && img->cache && (uint64_t)(lsn + 1) * SECTOR <= img->cache_size) {
        *off = lsn * SECTOR;
        return img;
    }
    uint32_t vd = drive + lsn / HDB_DISK_SECTORS;
    if (lsn < HDB_DISK_SECTORS || vd >= DW_DRIVES) return img;   // out of range
    img = &s_fdc->drives[vd];
    *off = (lsn % HDB_DISK_SECTORS) * SECTOR;
    return img;
}

// Copy LSN `lsn` of `drive` into s_sector. Returns a DriveWire status.
static uint8_t disk_read(uint8_t drive, uint32_t lsn) {
    uint8_t rc = E_OK;
    uint32_t off = UINT32_MAX;
    sv_disk_lock();
    SV_DiskImage* img = resolve(drive, lsn, &off);
    if (!img || !img->mounted || !img->cache) {
        rc = E_NOTRDY;
    } else if (off == UINT32_MAX || off + SECTOR > img->cache_size) {
        rc = E_READ;
    } else {
        memcpy(s_sector, img->cache + off, SECTOR);
    }
    sv_disk_unlock();
    if (rc != E_OK) memset(s_sector, 0, SECTOR);
    return rc;
}

static uint8_t disk_write(uint8_t drive, uint32_t lsn) {
    uint8_t rc = E_OK;
    uint32_t off = UINT32_MAX;
    sv_disk_lock();
    SV_DiskImage* img = resolve(drive, lsn, &off);
    if (!img || !img->mounted || !img->cache) {
        rc = E_NOTRDY;
    } else if (off == UINT32_MAX || off + SECTOR > img->cache_size) {
        rc = E_WRITE;
    } else if (img->read_only) {
        rc = E_WP;
    } else {
        memcpy(img->cache + off, s_sector, SECTOR);
        sv_disk_mark_dirty(img, off, SECTOR);
        s_flush_pending = true;
        s_last_write_ms = millis();
    }
    sv_disk_unlock();
    return rc;
}

// ------------------------------------------------------------
// Opcode handlers — each returns false if interrupted by a reset
// ------------------------------------------------------------

static bool rx_drive_lsn(uint8_t* drive, uint32_t* lsn) {
    uint8_t h[4];
    if (!rx(h, 4)) return false;
    *drive = h[0];
    *lsn = ((uint32_t)h[1] << 16) | ((uint32_t)h[2] << 8) | h[3];
    return true;
}

static void count(uint8_t rc) { if (rc != E_OK) s_errors++; }

// READEX: sector data (zeros on error), CoCo's checksum, then the status.
static bool op_readex(void) {
    uint8_t drive; uint32_t lsn;
    if (!rx_drive_lsn(&drive, &lsn)) return false;
    uint8_t rc = disk_read(drive, lsn);
    if (!tx(s_sector, SECTOR)) return false;
    uint8_t cs[2];
    if (!rx(cs, 2)) return false;
    if (rc == E_OK && (((uint16_t)cs[0] << 8) | cs[1]) != checksum(s_sector, SECTOR)) rc = E_CRC;
    s_reads++;
    count(rc);
    return tx_byte(rc);
}

// Legacy DW3 READ: status first, then data + checksum only on success.
static bool op_read(void) {
    uint8_t drive; uint32_t lsn;
    if (!rx_drive_lsn(&drive, &lsn)) return false;
    uint8_t rc = disk_read(drive, lsn);
    s_reads++;
    count(rc);
    if (!tx_byte(rc)) return false;
    if (rc != E_OK) return true;
    uint16_t c = checksum(s_sector, SECTOR);
    uint8_t cs[2] = { (uint8_t)(c >> 8), (uint8_t)c };
    return tx(s_sector, SECTOR) && tx(cs, 2);
}

static bool op_write(void) {
    uint8_t drive; uint32_t lsn; uint8_t cs[2];
    if (!rx_drive_lsn(&drive, &lsn)) return false;
    if (!rx(s_sector, SECTOR) || !rx(cs, 2)) return false;
    uint8_t rc;
    if ((((uint16_t)cs[0] << 8) | cs[1]) != checksum(s_sector, SECTOR)) rc = E_CRC;
    else rc = disk_write(drive, lsn);
    s_writes++;
    count(rc);
    return tx_byte(rc);
}

static bool op_time(void) {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    uint8_t r[6] = { (uint8_t)t.tm_year, (uint8_t)(t.tm_mon + 1), (uint8_t)t.tm_mday,
                     (uint8_t)t.tm_hour, (uint8_t)t.tm_min, (uint8_t)t.tm_sec };
    return tx(r, sizeof(r));
}

static bool serve_one(void) {
    uint8_t op;
    if (!rx_byte(&op, true)) return false;
    if (op != OP_SERREAD) s_last_op = op;   // NitrOS-9 polls SERREAD constantly

    if (op >= OP_FASTWRITE_FIRST && op <= OP_FASTWRITE_LAST) return skip(1);

    switch (op) {
        case OP_NOP: case OP_INIT: case OP_TERM: case OP_PRINTFLUSH:
        case OP_RESET1: case OP_RESET2: case OP_RESET3:
            return true;
        case OP_DWINIT: {
            uint8_t ver;
            return rx_byte(&ver) && tx_byte(0x00);
        }
        case OP_TIME:      return op_time();
        case OP_READEX:
        case OP_REREADEX:  return op_readex();
        case OP_READ:
        case OP_REREAD:    return op_read();
        case OP_WRITE:
        case OP_REWRITE:   return op_write();
        case OP_GETSTAT:
        case OP_SETSTAT:   return skip(2);              // drive, code
        case OP_SERREAD: {                              // no vport data waiting
            static const uint8_t none[2] = { 0, 0 };
            return tx(none, 2);
        }
        case OP_SERREADM:                               // never invited by SERREAD
        case OP_SERWRITE:
        case OP_SERGETSTAT: return skip(2);
        case OP_SERWRITEM: {
            uint8_t h[2];                               // channel, count, data
            return rx(h, 2) && skip(h[1]);
        }
        case OP_SERSETSTAT: {
            uint8_t h[2];                               // channel, code
            if (!rx(h, 2)) return false;
            return (h[1] == SS_COMST) ? skip(26) : true;
        }
        case OP_SERINIT: case OP_SERTERM: case OP_PRINT:
            return skip(1);
        case OP_NAMEOBJ_MOUNT:
        case OP_NAMEOBJ_CREATE: {
            uint8_t len;
            return rx_byte(&len) && skip(len) && tx_byte(0x00);   // not found
        }
        default:
            // Unknown length: nothing sane to do but carry on at the next
            // byte (the same as the reference servers).
            DEBUG_PRINTF("dw_server: unknown opcode $%02X", op);
            return true;
    }
}

static void server_task(void* arg) {
    (void)arg;
    becker_clear_reset();
    becker_set_link_up(true);
    for (;;) {
        if (s_shutdown_req) {
            if (s_flush_pending) flush_dirty_drives();
            s_shutdown_done = true;
            vTaskDelay(portMAX_DELAY);   // device is about to restart
        }
        if (!serve_one() && becker_reset_requested()) {
            // CoCo reset mid-transaction (or while idle): drop the partial
            // request, and flush anything already queued toward the CoCo —
            // it answers a pre-reset request. Bytes the CoCo wrote after the
            // reset survive (becker_set_link_up keeps them).
            s_in_len = s_in_pos = 0;
            becker_set_link_up(false);
            becker_set_link_up(true);
        }
    }
}

// ------------------------------------------------------------
// Public API
// ------------------------------------------------------------

void dw_server_begin(SV_DiskController* drives) {
    if (s_task || !drives) return;
    s_fdc = drives;
    xTaskCreatePinnedToCore(server_task, "dw_server", DW_SERVER_STACK, nullptr, 1, &s_task, 0);
    becker_set_backend_task(s_task);
}

void dw_server_shutdown(uint32_t timeout_ms) {
    if (!s_task) return;
    s_shutdown_req = true;
    xTaskNotifyGive(s_task);
    uint32_t t0 = millis();
    while (!s_shutdown_done && millis() - t0 < timeout_ms) delay(5);
}

const char* dw_server_state_str(void) {
    if (!s_task) return "Idle";
    for (int d = 0; d < DW_DRIVES; d++) {
        if (s_fdc->drives[d].mounted) return "Serving";
    }
    return "No disks";
}

uint32_t dw_server_reads(void)   { return s_reads; }
uint32_t dw_server_writes(void)  { return s_writes; }
uint32_t dw_server_errors(void)  { return s_errors; }
uint32_t dw_server_flushes(void) { return s_flushes; }
uint8_t  dw_server_last_op(void) { return s_last_op; }

uint32_t dw_server_stack_free(void) {
    return s_task ? uxTaskGetStackHighWaterMark(s_task) : 0;
}
