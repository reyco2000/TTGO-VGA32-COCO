/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : sv_disk.cpp
 *  Module : WD1793-compatible FDC emulation — PSRAM disk cache with INTRQ/NMI/HALT mechanism
 * ============================================================
*/

/*
 * sv_disk.cpp - Virtual floppy disk controller (WD1793-compatible)
 *
 * Command-level FDC emulation with INTRQ tracking.
 * Entire disk images are cached in PSRAM at mount time — all sector
 * reads/writes go to the in-memory cache, eliminating SD card timing
 * issues during emulation.  Dirty images are flushed back to SD on
 * eject or explicit flush.
 *
 * DSKCON sequence: issue command → enable NMI in DSKREG → wait for NMI.
 * INTRQ is latched when a command completes. NMI fires when BOTH
 * INTRQ is asserted AND NMI is enabled in DSKREG ($FF40 bit 7).
 * INTRQ is cleared on status register read (matching XRoar behavior).
 *
 * CRITICAL NOTES:
 * - Sectors are 1-based (1-18), NOT 0-based
 * - NMI fires via INTRQ + DSKREG NMI enable, NOT immediately
 * - Disk images cached in PSRAM (~161KB each, up to 4 drives)
 */

#include "sv_disk.h"
#include "../utils/debug.h"
#include <SD.h>
#include <esp_heap_caps.h>

// FDC debug traces (disabled — disk I/O confirmed working)
#define FDC_TRACE(fmt, ...)
#define FDC_CMD_TRACE(fmt, ...)

// WRITE TRACK state machine constants
#define WT_IDLE        0
#define WT_ID_FIELD    1
#define WT_WAIT_DAM    2
#define WT_DATA_FIELD  3
#define WT_TRACK_BYTES 6250  // Standard DD track length in bytes

// Signal HALT to CPU (matching XRoar: signal_halt)
static void signal_halt(SV_DiskController* fdc, bool halted) {
    if (fdc->halt_callback) {
        fdc->halt_callback(fdc->callback_context, halted);
    }
}

// Signal NMI to CPU (matching XRoar: signal_nmi, now edge-aware)
static void signal_nmi(SV_DiskController* fdc, bool active) {
    if (fdc->nmi_callback) {
        fdc->nmi_callback(fdc->callback_context, active);
    }
}

// INTRQ callback — matches XRoar rsdos.c set_intrq()
// When INTRQ asserts: disable HALT, release CPU, fire NMI via density gating
// When INTRQ clears: deassert NMI line
static void set_intrq(SV_DiskController* fdc, bool value) {
    if (value && !fdc->intrq) {
        FDC_CMD_TRACE("INTRQ→1 density=%d NMI=%d", fdc->density, !fdc->density);
    }
    fdc->intrq = value;
    if (value) {
        // INTRQ asserted: disable HALT and release CPU
        fdc->halt_enable = false;
        signal_halt(fdc, false);
    }
    // NMI gating: matches XRoar rsdos.c set_intrq() exactly
    // NMI fires when !density AND intrq; otherwise NMI deasserted
    if (!fdc->density && fdc->intrq) {
        signal_nmi(fdc, true);
    } else {
        signal_nmi(fdc, false);
    }
}

// DRQ callback — matches XRoar rsdos.c set_drq()
// DRQ high = data ready → release HALT
// DRQ low = waiting → engage HALT if enabled
static void set_drq(SV_DiskController* fdc, bool value) {
    fdc->drq = value;
    if (value) {
        // Data ready → release CPU
        signal_halt(fdc, false);
    } else {
        // No data → halt CPU if halt is enabled
        if (fdc->halt_enable) {
            signal_halt(fdc, true);
        }
    }
}

// Calculate byte offset into disk data for given track/side/sector
// JVC double-sided layout: T0S0, T0S1, T1S0, T1S1, ...
// Returns offset relative to start of data (after header), or UINT32_MAX on error
static uint32_t sector_offset(SV_DiskImage* img, uint8_t track, uint8_t side, uint8_t sector) {
    // CoCo BASIC uses sectors 1-18 (1-based!)
    if (sector < 1 || sector > img->sectors_per_track) return UINT32_MAX;
    if (track >= img->tracks) return UINT32_MAX;
    if (side >= img->sides) return UINT32_MAX;

    // For DS images: linear_track = track * sides + side (interleaved layout)
    // For SS images: sides=1, side=0, so linear_track = track
    uint32_t linear_track = (uint32_t)track * img->sides + side;
    uint32_t offset = linear_track * img->sectors_per_track * img->sector_size;
    offset += (uint32_t)(sector - 1) * img->sector_size;
    return offset;
}

// Execute FDC command
static void fdc_execute_command(SV_DiskController* fdc, uint8_t cmd) {
    SV_DiskImage* disk = &fdc->drives[fdc->drive_select];
    uint8_t cmd_type = cmd & 0xF0;
    FDC_CMD_TRACE("CMD=$%02X drv=%d trk=%d side=%d sec=%d mounted=%d",
              cmd, fdc->drive_select, fdc->track, fdc->side, fdc->sector, disk->mounted);

    // Clear any in-progress transfer state from previous command
    fdc->reading = false;
    fdc->read_pending = false;
    fdc->writing = false;
    fdc->write_track = false;
    fdc->drq = false;
    fdc->intrq_defer = 0;

    fdc->command = cmd;
    fdc->busy = true;
    fdc->status = 0x01;  // BUSY

    switch (cmd_type) {
        case 0x00:  // RESTORE
            fdc->track = 0;
            fdc->step_direction = -1;
            fdc->status = 0x04;  // TRACK 0
            if (!disk->mounted) fdc->status |= 0x80;
            fdc->busy = false;
            break;

        case 0x10:  // SEEK
            fdc->track = fdc->data;
            fdc->status = (fdc->track == 0) ? 0x04 : 0x00;
            if (!disk->mounted) fdc->status |= 0x80;
            fdc->busy = false;
            break;

        case 0x80: case 0x90:  // READ SECTOR
            if (!disk->mounted || !disk->cache) {
                fdc->status = 0x80;  // NOT READY
                fdc->busy = false;
                break;
            }
            // Defer actual sector load until first $FF4B read.
            // On real hardware the FDC takes time to find the sector;
            // the CoCo DSKCON writes DSKREG (with side select) AFTER
            // the command register.  Deferring ensures we use the
            // correct side value that DSKREG will set momentarily.
            fdc->buf_pos = 0;
            fdc->buf_len = disk->sector_size;
            fdc->reading = true;
            fdc->read_pending = true;
            fdc->drq = true;
            fdc->status = 0x03;  // BUSY + DRQ
            break;

        case 0xA0: case 0xB0:  // WRITE SECTOR
            if (!disk->mounted || !disk->cache) {
                fdc->status = 0x80;
                fdc->busy = false;
                break;
            }
            if (disk->read_only) {
                fdc->status = 0x40;  // WRITE PROTECT
                fdc->busy = false;
                break;
            }
            FDC_CMD_TRACE("WRITE trk=%d side=%d sec=%d", fdc->track, fdc->side, fdc->sector);
            fdc->buf_pos = 0;
            fdc->buf_len = disk->sector_size;
            fdc->writing = true;
            fdc->drq = true;
            fdc->status = 0x03;  // BUSY + DRQ
            break;

        case 0xC0:  // READ ADDRESS
            // Returns 6 bytes: track, side, sector, sector_length_code, CRC1, CRC2
            // NitrOS-9 may use this to verify disk geometry
            fdc->sector_buf[0] = fdc->track;
            fdc->sector_buf[1] = fdc->side;
            fdc->sector_buf[2] = fdc->sector;
            fdc->sector_buf[3] = 0x01;  // Sector length code 1 = 256 bytes
            fdc->sector_buf[4] = 0x00;  // CRC (dummy)
            fdc->sector_buf[5] = 0x00;  // CRC (dummy)
            fdc->buf_pos = 0;
            fdc->buf_len = 6;
            fdc->reading = true;
            fdc->drq = true;
            fdc->data = fdc->sector_buf[0];
            fdc->status = 0x03;  // BUSY + DRQ
            // Per WD1793 datasheet: READ ADDRESS updates the sector register
            // with the track field from the ID (byte 0 of the result)
            fdc->sector = fdc->track;  // WD1793 quirk: sector reg ← ID track field
            break;

        case 0xE0:  // READ TRACK (not needed, return empty)
            fdc->status = 0x00;
            fdc->busy = false;
            break;

        case 0xF0:  // WRITE TRACK (format)
            if (!disk->mounted || !disk->cache) {
                fdc->status = 0x80;
                fdc->busy = false;
                break;
            }
            if (disk->read_only) {
                fdc->status = 0x40;  // WRITE PROTECT
                fdc->busy = false;
                break;
            }
            // Initialize write-track state machine
            fdc->write_track = true;
            fdc->wt_state = WT_IDLE;
            fdc->wt_f5_count = 0;
            fdc->wt_id_pos = 0;
            fdc->wt_id_sector = 1;
            fdc->wt_data_pos = 0;
            fdc->wt_byte_count = 0;
            fdc->drq = true;
            fdc->status = 0x03;  // BUSY + DRQ
            break;

        case 0xD0:  // FORCE INTERRUPT
            fdc->busy = false;
            fdc->reading = false;
            fdc->writing = false;
            fdc->write_track = false;
            fdc->drq = false;
            fdc->status = 0x00;
            if (fdc->track == 0) fdc->status = 0x04;
            if (!disk->mounted) fdc->status |= 0x80;
            // Bit 3 = immediate interrupt (like XRoar)
            if (cmd & 0x08) {
                set_intrq(fdc, true);
            }
            return;  // Don't fall through to set_intrq below

        default:
            // STEP / STEP IN / STEP OUT
            if (cmd_type >= 0x20 && cmd_type <= 0x7F) {
                if (cmd_type >= 0x40 && cmd_type <= 0x5F) fdc->step_direction = 1;
                if (cmd_type >= 0x60 && cmd_type <= 0x7F) fdc->step_direction = -1;
                int new_track = (int)fdc->track + fdc->step_direction;
                if (new_track < 0) new_track = 0;
                if (new_track > 79) new_track = 79;
                if (cmd & 0x10) fdc->track = (uint8_t)new_track;
                fdc->status = 0x00;
                if (fdc->track == 0) fdc->status = 0x04;
                if (!disk->mounted) fdc->status |= 0x80;
                fdc->busy = false;
            } else {
                FDC_CMD_TRACE("UNHANDLED CMD=$%02X (type=$%02X)", cmd, cmd_type);
                fdc->status = 0x00;
                fdc->busy = false;
            }
            break;
    }

    // Assert INTRQ when command completes (not mid-transfer)
    if (!fdc->busy) {
        set_intrq(fdc, true);
    }
}

// Data register read ($FF4B) during READ SECTOR
//
// DSKCON read loop: LDA $FF4B / STA ,X+ / BRA LOOP  (broken by NMI)
//
// We can't model cycle-accurate HALT (no propagation delay), so bytes
// are delivered instantly — the CPU reads all 256 bytes at full speed.
//
// For the last byte (256th read): we can't fire INTRQ immediately
// because NMI would preempt STA ,X+ (the CPU hasn't stored the byte
// yet).  Instead, we defer: on the NEXT $FF4B read (byte 257 attempt,
// after STA has completed), we fire INTRQ.  NMI preempts the stale
// byte 257's STA — which is exactly what the DSKCON NMI handler
// expects (it manipulates the stack to skip the read loop).
//
// sv_disk_tick provides a safety-net fallback (intrq_defer countdown)
// in case the CPU doesn't loop back to $FF4B for some reason.
//
static uint8_t fdc_read_data(SV_DiskController* fdc) {
    // Deferred sector load: on the first $FF4B read after a READ SECTOR
    // command, the side select from DSKREG is now current.
    if (fdc->read_pending) {
        fdc->read_pending = false;
        SV_DiskImage* disk = &fdc->drives[fdc->drive_select];
        uint32_t off = sector_offset(disk, fdc->track, fdc->side, fdc->sector);
        if (off == UINT32_MAX || (off + disk->sector_size) > disk->cache_size) {
            FDC_CMD_TRACE("READ RNF! trk=%d side=%d sec=%d off=%lu cache=%lu",
                          fdc->track, fdc->side, fdc->sector,
                          (unsigned long)off, (unsigned long)disk->cache_size);
            fdc->reading = false;
            fdc->drq = false;
            fdc->busy = false;
            fdc->status = 0x10;  // RECORD NOT FOUND
            set_intrq(fdc, true);
            return 0xFF;
        }
        FDC_CMD_TRACE("READ trk=%d side=%d sec=%d off=%lu",
                      fdc->track, fdc->side, fdc->sector, (unsigned long)off);
        memcpy(fdc->sector_buf, disk->cache + off, disk->sector_size);
        fdc->data = fdc->sector_buf[0];
    }

    uint8_t val = fdc->data;

    if (fdc->reading && fdc->buf_pos < fdc->buf_len) {
        fdc->buf_pos++;
        if (fdc->buf_pos < fdc->buf_len) {
            fdc->data = fdc->sector_buf[fdc->buf_pos];
            fdc->drq = true;
        } else {
            // Transfer complete.  Defer INTRQ until the byte-257 read.
            // Use a large defer count so sv_disk_tick doesn't fire
            // before the byte-257 path (which fires in ~14 CPU cycles).
            fdc->reading = false;
            fdc->drq = false;
            fdc->busy = false;
            fdc->status = 0x00;
            fdc->intrq_defer = 5;  // safety net: 5 scanlines (~285 cycles)
        }
    } else if (fdc->intrq_defer > 0 && !fdc->intrq) {
        // Post-transfer: CPU looped back to LDA $FF4B (byte 257).
        // STA ,X+ for byte 256 has already executed.  Fire INTRQ now
        // so NMI breaks out before this stale read gets stored.
        fdc->intrq_defer = 0;
        set_intrq(fdc, true);
    }

    return val;
}

// WRITE TRACK byte-level state machine
// Parses the raw format stream to extract sector data and writes to cache.
// CoCo DSKINI format stream per sector:
//   ... $F5 $F5 $F5 $FE [track] [side] [sector] [size] $F7 ...
//   ... $F5 $F5 $F5 $FB [256 bytes data] $F7 ...
// $F5 = address mark prefix, $FE = ID AM, $FB = Data AM, $F7 = CRC

static void fdc_write_track_byte(SV_DiskController* fdc, uint8_t value) {
    fdc->wt_byte_count++;

    uint8_t f5 = fdc->wt_f5_count;

    // Track consecutive $F5 bytes (address mark prefix)
    if (value == 0xF5) {
        fdc->wt_f5_count++;
        // $F5 is consumed by address mark logic, not sector data
        goto check_done;
    }

    fdc->wt_f5_count = 0;

    switch (fdc->wt_state) {
        case WT_IDLE:
        case WT_WAIT_DAM:
            if (f5 >= 3 && value == 0xFE) {
                // ID Address Mark — next 4 bytes are track/side/sector/size
                fdc->wt_state = WT_ID_FIELD;
                fdc->wt_id_pos = 0;
            } else if (f5 >= 3 && value == 0xFB) {
                // Data Address Mark — next sector_size bytes are data
                fdc->wt_state = WT_DATA_FIELD;
                fdc->wt_data_pos = 0;
            }
            // $F7 (CRC) and gap bytes are ignored
            break;

        case WT_ID_FIELD:
            // 4 bytes: track, side, sector, size_code
            if (fdc->wt_id_pos == 2) {
                fdc->wt_id_sector = value;  // Capture sector number
            }
            fdc->wt_id_pos++;
            if (fdc->wt_id_pos >= 4) {
                fdc->wt_state = WT_WAIT_DAM;
            }
            break;

        case WT_DATA_FIELD: {
            SV_DiskImage* disk = &fdc->drives[fdc->drive_select];
            fdc->sector_buf[fdc->wt_data_pos] = value;
            fdc->wt_data_pos++;
            if (fdc->wt_data_pos >= disk->sector_size) {
                // Full sector received — write to PSRAM cache
                uint32_t off = sector_offset(disk, fdc->track, fdc->side, fdc->wt_id_sector);
                if (off != UINT32_MAX && disk->cache &&
                    (off + disk->sector_size) <= disk->cache_size) {
                    memcpy(disk->cache + off, fdc->sector_buf, disk->sector_size);
                    disk->dirty = true;
                }
                fdc->wt_state = WT_IDLE;
            }
            break;
        }
    }

check_done:
    // Track complete after enough bytes received
    if (fdc->wt_byte_count >= WT_TRACK_BYTES) {
        fdc->write_track = false;
        fdc->drq = false;
        fdc->busy = false;
        fdc->status = 0x00;
        set_intrq(fdc, true);  // Fire NMI immediately (see read path comment)
        return;
    }

    // Request next byte
    set_drq(fdc, true);
}

// Data register write ($FF4B) during WRITE SECTOR
static void fdc_write_data(SV_DiskController* fdc, uint8_t value) {
    // WRITE TRACK (format) data handling
    if (fdc->write_track) {
        fdc_write_track_byte(fdc, value);
        return;
    }

    if (fdc->writing && fdc->buf_pos < fdc->buf_len) {
        fdc->sector_buf[fdc->buf_pos] = value;
        fdc->buf_pos++;

        if (fdc->buf_pos >= fdc->buf_len) {
            // All bytes received — write to PSRAM cache (instant, no SD)
            SV_DiskImage* disk = &fdc->drives[fdc->drive_select];
            uint32_t off = sector_offset(disk, fdc->track, fdc->side, fdc->sector);
            if (off != UINT32_MAX && disk->cache &&
                (off + disk->sector_size) <= disk->cache_size) {
                memcpy(disk->cache + off, fdc->sector_buf, disk->sector_size);
                disk->dirty = true;
            }
            fdc->writing = false;
            fdc->drq = false;
            fdc->busy = false;
            fdc->status = (off == UINT32_MAX) ? 0x10 : 0x00;
            set_intrq(fdc, true);
        } else {
            fdc->drq = true;
        }
    }
}

// DSKREG write ($FF40) — matches XRoar rsdos.c ff40_write()
static void fdc_write_drive_select(SV_DiskController* fdc, uint8_t value) {
    // FDC_TRACE for DSKREG is intentionally disabled — fires every CPU cycle in wait loops
    // Drive select: bits 0-2 are individual drive select lines
    if (value & 0x01)      fdc->drive_select = 0;
    else if (value & 0x02) fdc->drive_select = 1;
    else if (value & 0x04) fdc->drive_select = 2;
    else                   fdc->drive_select = 0;

    if (fdc->drive_select >= SV_DISK_MAX_DRIVES)
        fdc->drive_select = 0;

    fdc->motor_on = (value & 0x08) != 0;

    // Side select: bit 6 of DSKREG
    fdc->side = (value >> 6) & 0x01;

    // Density bit with XOR (matching XRoar: octet ^= 0x20)
    // This inverts bit 5 so that the "normal" CoCo setting (bit5=1)
    // results in density=false, which enables NMI via set_intrq path.
    uint8_t xored = value ^ 0x20;
    fdc->density = (xored & 0x20) != 0;

    // NMI gating: matches XRoar rsdos.c ff40_write()
    // NMI fires when !ic1_density AND intrq_flag; otherwise NMI deasserted
    if (!fdc->density && fdc->intrq) {
        signal_nmi(fdc, true);
    } else {
        signal_nmi(fdc, false);
    }

    // Bit 7 = HALT enable (NOT NMI!) — matches XRoar: halt_enable = octet & 0x80
    fdc->halt_enable = (value & 0x80) != 0;

    // INTRQ disables HALT (XRoar: if (intrq_flag) halt_enable = 0)
    if (fdc->intrq) fdc->halt_enable = false;

    // Apply HALT state to CPU
    // CRITICAL: Only halt when an FDC command is in progress (busy).
    // DSKCON writes DSKREG (enabling HALT) BEFORE issuing the FDC command.
    // On real hardware, the CPU completes the next instruction (command write)
    // before HALT takes effect. In our model, halting here with no command
    // active would deadlock: DRQ stays false, command never issued, CPU stuck.
    signal_halt(fdc, fdc->halt_enable && !fdc->drq && fdc->busy);
}

// ============================================================
// Public API
// ============================================================

void sv_disk_init(SV_DiskController* fdc) {
    memset(fdc, 0, sizeof(SV_DiskController));
    fdc->step_direction = -1;
    fdc->status = 0x04;  // Track 0
    for (int i = 0; i < SV_DISK_MAX_DRIVES; i++) {
        fdc->drives[i].mounted = false;
        fdc->drives[i].dirty = false;
        fdc->drives[i].cache = nullptr;
        fdc->drives[i].cache_size = 0;
        fdc->drives[i].sectors_per_track = DISK_SECTORS;
        fdc->drives[i].sector_size = DISK_SECTOR_SIZE;
        fdc->drives[i].tracks = DISK_TRACKS;
        fdc->drives[i].sides = 1;
    }
}

void sv_disk_reset(SV_DiskController* fdc) {
    fdc->command = 0;
    fdc->status = 0x04;
    fdc->track = 0;
    fdc->sector = 1;
    fdc->data = 0;
    fdc->busy = false;
    fdc->drq = false;
    fdc->intrq = false;
    fdc->intrq_defer = 0;
    fdc->reading = false;
    fdc->read_pending = false;
    fdc->writing = false;
    fdc->write_track = false;
    fdc->step_direction = -1;
    fdc->side = 0;
    fdc->drive_select = 0;
    fdc->motor_on = false;
    fdc->halt_enable = false;
    fdc->density = false;
}

uint8_t sv_disk_read(SV_DiskController* fdc, uint16_t address) {
    // $FF40-$FF47: DSKREG (drive select latch, write-only — reads return 0)
    if (address < 0xFF48) {
        return 0x00;
    }

    // $FF48-$FF4F: WD1793 FDC registers (mirrored every 4)
    switch (address & 0x03) {
        case 0: {  // Status ($FF48)
            uint8_t s = fdc->status;
            // Dynamically update NOT READY based on current mount state
            SV_DiskImage* disk = &fdc->drives[fdc->drive_select];
            if (disk->mounted) {
                s &= ~0x80;  // Clear NOT READY
            } else {
                s |= 0x80;   // Set NOT READY
            }
            // DRQ in bit 1 for Type II/III commands
            if (fdc->reading || fdc->writing || fdc->write_track) {
                if (fdc->drq) s |= 0x02;
                else s &= ~0x02;
            }
            // Clear INTRQ on status read (per WD279x datasheet / XRoar)
            set_intrq(fdc, false);
            return s;
        }
        case 1: return fdc->track;     // $FF49
        case 2: return fdc->sector;    // $FF4A
        case 3: return fdc_read_data(fdc);  // $FF4B
    }

    return 0x00;
}

void sv_disk_write(SV_DiskController* fdc, uint16_t address, uint8_t value) {
    // $FF40-$FF47: DSKREG (drive select latch)
    if (address < 0xFF48) {
        fdc_write_drive_select(fdc, value);
        return;
    }

    // $FF48-$FF4F: WD1793 FDC registers (mirrored every 4)
    switch (address & 0x03) {
        case 0:  // Command ($FF48)
            // Clear INTRQ when a new command is issued (per XRoar/datasheet)
            set_intrq(fdc, false);
            fdc_execute_command(fdc, value);
            break;
        case 1:  // Track ($FF49)
            fdc->track = value;
            break;
        case 2:  // Sector ($FF4A)
            fdc->sector = value;
            break;
        case 3:  // Data ($FF4B)
            fdc->data = value;
            fdc_write_data(fdc, value);
            break;
    }
}

void sv_disk_tick(SV_DiskController* fdc) {
    // Safety-net fallback: if the byte-257 $FF4B read doesn't fire
    // INTRQ (e.g., CPU stuck for some reason), the countdown fires
    // it after N scanlines.  Normally the byte-257 path fires in
    // ~14 CPU cycles (well within a single scanline), so this never
    // triggers in practice.
    if (fdc->intrq_defer > 0) {
        fdc->intrq_defer--;
        if (fdc->intrq_defer == 0) {
            set_intrq(fdc, true);
        }
    }
}

bool sv_disk_detect_geometry(SV_DiskImage* img) {
    uint32_t size = img->image_size;

    // Check for VDK header (12 bytes)
    const char* ext = strrchr(img->path, '.');
    if (ext && (strcasecmp(ext, ".vdk") == 0)) {
        img->header_size = 12;
        size -= 12;
    } else {
        // JVC: check if file size has remainder when divided by 256
        img->header_size = size % DISK_SECTOR_SIZE;
    }

    uint32_t data_size = size - img->header_size;
    img->sector_size = DISK_SECTOR_SIZE;
    img->sectors_per_track = DISK_SECTORS;

    // Determine track count from data size
    uint32_t track_size = (uint32_t)img->sectors_per_track * img->sector_size;
    if (track_size == 0) return false;

    img->tracks = data_size / track_size;
    if (img->tracks == 0) return false;

    // Detect double-sided images.
    // JVC stores sides interleaved: T0S0, T0S1, T1S0, T1S1, ...
    // 360K (40T DS) → 80 linear tracks; 720K (80T DS) → 160 linear tracks.
    // Single-sided: 35T → 35, 40T → 40.  Threshold at 40 tracks.
    if (img->tracks > 40) {
        img->sides = 2;
        img->tracks /= 2;
    } else {
        img->sides = 1;
    }

    return true;
}

bool sv_disk_mount(SV_DiskController* fdc, uint8_t drive, const char* path) {
    if (drive >= SV_DISK_MAX_DRIVES) return false;

    SV_DiskImage* img = &fdc->drives[drive];

    // Eject any currently mounted image
    if (img->mounted) {
        sv_disk_eject(fdc, drive);
    }

    // Open file to read image
    img->file = SD.open(path, FILE_READ);
    if (!img->file) {
        DEBUG_PRINTF("FDC: Failed to open %s", path);
        return false;
    }

    strncpy(img->path, path, sizeof(img->path) - 1);
    img->path[sizeof(img->path) - 1] = '\0';
    img->image_size = img->file.size();
    img->read_only = false;
    img->dirty = false;

    if (!sv_disk_detect_geometry(img)) {
        DEBUG_PRINTF("FDC: Bad geometry for %s (size=%lu)", path, img->image_size);
        img->file.close();
        return false;
    }

    // Calculate data size (image minus header)
    img->cache_size = (uint32_t)img->tracks * img->sectors_per_track * img->sector_size;
    if (img->sides == 2) img->cache_size *= 2;

    // Allocate PSRAM cache for entire disk image
    img->cache = (uint8_t*)heap_caps_malloc(img->cache_size, MALLOC_CAP_SPIRAM);
    if (!img->cache) {
        // Fall back to regular heap if PSRAM full
        img->cache = (uint8_t*)malloc(img->cache_size);
    }
    if (!img->cache) {
        DEBUG_PRINTF("FDC: Failed to allocate %lu bytes for disk cache", img->cache_size);
        img->file.close();
        return false;
    }

    // Load entire disk image into PSRAM cache.
    // IMPORTANT: ESP32 SPI DMA cannot reliably write directly to PSRAM,
    // so we read into a small internal-RAM bounce buffer first, then
    // memcpy to the PSRAM cache.
    if (img->header_size > 0) {
        img->file.seek(img->header_size);
    }

    // Bounce buffer in internal RAM (DMA-safe)
    const size_t BOUNCE_SIZE = 512;
    uint8_t bounce[BOUNCE_SIZE];

    size_t total_read = 0;
    size_t remaining = img->cache_size;
    while (remaining > 0) {
        size_t chunk = (remaining > BOUNCE_SIZE) ? BOUNCE_SIZE : remaining;
        int got = img->file.read(bounce, chunk);
        if (got <= 0) {
            DEBUG_PRINTF("FDC: Read stalled at %lu/%lu bytes", total_read, img->cache_size);
            memset(img->cache + total_read, 0, remaining);
            break;
        }
        memcpy(img->cache + total_read, bounce, got);
        total_read += got;
        remaining -= got;
    }
    DEBUG_PRINTF("FDC: Loaded %lu/%lu bytes into cache", total_read, img->cache_size);

    // Close read-only handle, reopen as r+ for write-back
    img->file.close();
    img->file = SD.open(path, "r+");
    if (!img->file) {
        // Fall back to read-only (can't write back)
        img->file = SD.open(path, FILE_READ);
        img->read_only = true;
    }

    img->mounted = true;
    DEBUG_PRINTF("FDC: Mounted drive %d: %s (%dT/%dH/%dS/%dB, %lu bytes cached in %s)",
                 drive, path, img->tracks, img->sides, img->sectors_per_track,
                 img->sector_size, img->cache_size,
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM) > 0 ? "PSRAM" : "heap");
    DEBUG_PRINTF("FDC: Free PSRAM: %lu bytes", (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    return true;
}

void sv_disk_eject(SV_DiskController* fdc, uint8_t drive) {
    if (drive >= SV_DISK_MAX_DRIVES) return;

    SV_DiskImage* img = &fdc->drives[drive];
    if (!img->mounted) return;

    // Flush dirty cache back to SD before ejecting
    if (img->dirty && img->cache && img->file) {
        sv_disk_flush(fdc, drive);
    }

    img->file.close();

    // Free PSRAM cache
    if (img->cache) {
        free(img->cache);
        img->cache = nullptr;
        img->cache_size = 0;
    }

    img->mounted = false;
    img->dirty = false;
    img->path[0] = '\0';

    DEBUG_PRINTF("FDC: Ejected drive %d", drive);
}

bool sv_disk_is_mounted(SV_DiskController* fdc, uint8_t drive) {
    if (drive >= SV_DISK_MAX_DRIVES) return false;
    return fdc->drives[drive].mounted;
}

const char* sv_disk_get_path(SV_DiskController* fdc, uint8_t drive) {
    if (drive >= SV_DISK_MAX_DRIVES) return "";
    return fdc->drives[drive].path;
}

void sv_disk_flush(SV_DiskController* fdc, uint8_t drive) {
    if (drive >= SV_DISK_MAX_DRIVES) return;
    SV_DiskImage* img = &fdc->drives[drive];
    if (!img->mounted || !img->dirty || !img->cache) return;
    if (img->read_only || !img->file) return;

    // Write cache back to SD file using bounce buffer (PSRAM→DMA safe)
    DEBUG_PRINTF("FDC: Flushing drive %d (%lu bytes) to SD...", drive, img->cache_size);
    img->file.seek(img->header_size);

    const size_t BOUNCE_SIZE = 512;
    uint8_t bounce[BOUNCE_SIZE];

    size_t total_written = 0;
    size_t remaining = img->cache_size;
    while (remaining > 0) {
        size_t chunk = (remaining > BOUNCE_SIZE) ? BOUNCE_SIZE : remaining;
        memcpy(bounce, img->cache + total_written, chunk);
        size_t wrote = img->file.write(bounce, chunk);
        if (wrote == 0) {
            DEBUG_PRINTF("FDC: Write stalled at %lu/%lu bytes", total_written, img->cache_size);
            break;
        }
        total_written += wrote;
        remaining -= wrote;
    }
    img->file.flush();
    img->dirty = false;
    DEBUG_PRINTF("FDC: Flush complete (%lu bytes written)", total_written);
}

void sv_disk_flush_all(SV_DiskController* fdc) {
    for (int i = 0; i < SV_DISK_MAX_DRIVES; i++) {
        sv_disk_flush(fdc, i);
    }
}
