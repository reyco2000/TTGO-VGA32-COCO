/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : integration_test.cpp
 *  Module : Integration test framework — LOADM disk verification and VRAM screen inspection
 * ============================================================
*/

// Integration tests are CoCo 2-specific (direct RAM access, RS-DOS LOADM).
// Disabled for CoCo 3 builds — will need adaptation for GIME MMU addressing.
#include "../../config.h"
#if MACHINE_TYPE != 4

/*
 * CoCo ESP32 - Integration Test Framework Implementation
 *
 * LOADM disk verification test: injects LOADM "filename" via keyboard,
 * then compares CoCo RAM with the raw file data read from the disk cache.
 *
 * KEYBOARD MATRIX (verified from BASIC ROM KEYIN routine):
 * hal_keyboard_press(row, col) where row=PA bit, col=PB bit.
 * key_matrix[col] bit row = 0 when pressed (active LOW).
 * BASIC computes key_code = PA_row * 8 + PB_column.
 *
 *         PB0  PB1  PB2  PB3  PB4  PB5  PB6  PB7
 * PA0:     @    A    B    C    D    E    F    G
 * PA1:     H    I    J    K    L    M    N    O
 * PA2:     P    Q    R    S    T    U    V    W
 * PA3:     X    Y    Z   UP  DOWN LEFT RIGHT SPACE
 * PA4:     0    1    2    3    4    5    6    7
 * PA5:     8    9    :    ;    ,    -    .    /
 * PA6:   ENTER CLEAR BREAK ---  ---  ---  ---  SHIFT
 *
 * VRAM text screen: 32x16 chars at $0400, MC6847 internal charset.
 * VRAM byte encoding:
 *   $00-$1F -> characters '@' through '_' (uppercase + symbols)
 *   $20-$3F -> space through '?' (ASCII 0x20-0x3F directly)
 *   $40-$5F -> inverse '@' through '_'
 *   $60-$7F -> inverse space through '?'
 *   $80-$FF -> semigraphics-4 blocks
 */

#include "integration_test.h"
#include "../../config.h"
#include "../hal/hal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <Arduino.h>
#include <SD.h>

// CoCo text screen constants
static const uint16_t TEXT_SCREEN_ADDR = 0x0400;
static const int SCREEN_COLS = 32;
static const int SCREEN_ROWS = 16;
static const int SCREEN_SIZE = SCREEN_COLS * SCREEN_ROWS;

// CoCo SHIFT key: PA6, PB7
static const uint8_t SHIFT_ROW = 6;
static const uint8_t SHIFT_COL = 7;

// ============================================================================
// ASCII -> CoCo matrix position (verified from BASIC ROM KEYIN)
// ============================================================================

bool IntegrationTest::ascii_to_matrix(char c, uint8_t& row, uint8_t& col, bool& shift_out) {
    shift_out = false;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';

    if (c == '@')              { row = 0; col = 0; return true; }
    if (c >= 'A' && c <= 'G')  { row = 0; col = 1 + (c - 'A'); return true; }
    if (c >= 'H' && c <= 'O')  { row = 1; col = c - 'H'; return true; }
    if (c >= 'P' && c <= 'W')  { row = 2; col = c - 'P'; return true; }
    if (c >= 'X' && c <= 'Z')  { row = 3; col = c - 'X'; return true; }
    if (c == ' ')              { row = 3; col = 7; return true; }
    if (c == '0')              { row = 4; col = 0; return true; }
    if (c >= '1' && c <= '7')  { row = 4; col = 1 + (c - '1'); return true; }
    if (c == '8')              { row = 5; col = 0; return true; }
    if (c == '9')              { row = 5; col = 1; return true; }
    if (c == ':')              { row = 5; col = 2; return true; }
    if (c == ';')              { row = 5; col = 3; return true; }
    if (c == ',')              { row = 5; col = 4; return true; }
    if (c == '-')              { row = 5; col = 5; return true; }
    if (c == '.')              { row = 5; col = 6; return true; }
    if (c == '/')              { row = 5; col = 7; return true; }
    if (c == '\n' || c == '\r') { row = 6; col = 0; return true; }

    // Shifted punctuation
    if (c == '!') { row = 4; col = 1; shift_out = true; return true; }
    if (c == '"') { row = 4; col = 2; shift_out = true; return true; }
    if (c == '#') { row = 4; col = 3; shift_out = true; return true; }
    if (c == '$') { row = 4; col = 4; shift_out = true; return true; }
    if (c == '%') { row = 4; col = 5; shift_out = true; return true; }
    if (c == '&') { row = 4; col = 6; shift_out = true; return true; }
    if (c == '\'') { row = 4; col = 7; shift_out = true; return true; }
    if (c == '(') { row = 5; col = 0; shift_out = true; return true; }
    if (c == ')') { row = 5; col = 1; shift_out = true; return true; }
    if (c == '*') { row = 5; col = 2; shift_out = true; return true; }
    if (c == '+') { row = 5; col = 3; shift_out = true; return true; }
    if (c == '<') { row = 5; col = 4; shift_out = true; return true; }
    if (c == '=') { row = 5; col = 5; shift_out = true; return true; }
    if (c == '>') { row = 5; col = 6; shift_out = true; return true; }
    if (c == '?') { row = 5; col = 7; shift_out = true; return true; }

    return false;
}

// ============================================================================
// VRAM byte -> ASCII
// ============================================================================

char IntegrationTest::vram_to_ascii(uint8_t vram_byte) {
    if (vram_byte & 0x80) return '#';
    uint8_t ch = vram_byte & 0x3F;
    if (ch < 0x20) return (char)(ch + 0x40);
    return (char)ch;
}

// ============================================================================
// Constructor
// ============================================================================

IntegrationTest::IntegrationTest(Machine* m) : machine(m) {
    memset(results, 0, sizeof(results));
    memset(key_queue, 0, sizeof(key_queue));
}

// ============================================================================
// Frame execution
// ============================================================================

void IntegrationTest::run_frames(int count) {
    for (int i = 0; i < count; i++) {
        process_key_queue();
        machine_run_frame(machine);
        frame_counter++;
    }
}

// ============================================================================
// Keyboard injection
// ============================================================================

void IntegrationTest::queue_key_event(uint8_t row, uint8_t col, bool shift, bool pressed) {
    if (kq_count >= KEY_QUEUE_SIZE) return;
    key_queue[kq_tail].row = row;
    key_queue[kq_tail].col = col;
    key_queue[kq_tail].shift = shift;
    key_queue[kq_tail].pressed = pressed;
    kq_tail = (kq_tail + 1) % KEY_QUEUE_SIZE;
    kq_count++;
}

void IntegrationTest::release_all_keys() {
    hal_keyboard_release_all();
    key_active = false;
    key_timer = 0;
}

void IntegrationTest::process_key_queue() {
    if (key_timer > 0) { key_timer--; return; }
    if (key_active) { release_all_keys(); key_timer = key_gap_frames; return; }
    if (kq_count == 0) return;

    KeyEvent ev = key_queue[kq_head];
    kq_head = (kq_head + 1) % KEY_QUEUE_SIZE;
    kq_count--;

    if (ev.pressed) {
        if (ev.shift) hal_keyboard_press(SHIFT_ROW, SHIFT_COL);
        hal_keyboard_press(ev.row, ev.col);
        key_active = true;
        key_timer = key_hold_frames;
    } else {
        hal_keyboard_release(ev.row, ev.col);
        if (ev.shift) hal_keyboard_release(SHIFT_ROW, SHIFT_COL);
    }
}

void IntegrationTest::drain_key_queue(int settle_frames) {
    while (kq_count > 0 || key_active || key_timer > 0) run_frames(1);
    if (settle_frames > 0) run_frames(settle_frames);
}

void IntegrationTest::inject_keystrokes(const char* text, int delay_frames) {
    key_hold_frames = delay_frames > 0 ? delay_frames : 3;

    Serial.printf("  [kbd] inject: \"");
    for (const char* p = text; *p; p++)
        Serial.printf("%c", (*p >= 0x20 && *p < 0x7F) ? *p : '.');
    Serial.printf("\"\n");

    while (*text) {
        uint8_t row, col;
        bool shift;
        if (ascii_to_matrix(*text, row, col, shift)) {
            queue_key_event(row, col, shift, true);
            queue_key_event(row, col, shift, false);
        }
        text++;
    }
}

// ============================================================================
// Screen text helpers
// ============================================================================

void IntegrationTest::capture_screen_text(char* buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    int count = SCREEN_SIZE;
    if (count >= (int)buf_size) count = (int)buf_size - 1;
    for (int i = 0; i < count; i++)
        buf[i] = vram_to_ascii(machine->ram[TEXT_SCREEN_ADDR + i]);
    buf[count] = '\0';
}

bool IntegrationTest::screen_contains(const char* text) {
    char screen[SCREEN_SIZE + 1];
    capture_screen_text(screen, sizeof(screen));
    return strstr(screen, text) != nullptr;
}

bool IntegrationTest::wait_for_screen_text(const char* text, int timeout_frames) {
    for (int f = 0; f < timeout_frames; f++) {
        if (screen_contains(text)) return true;
        run_frames(1);
    }
    return false;
}

void IntegrationTest::capture_vram_snapshot(uint8_t* out, size_t max_bytes) {
    if (!out || max_bytes == 0) return;
    size_t len = (max_bytes < (size_t)SCREEN_SIZE) ? max_bytes : (size_t)SCREEN_SIZE;
    memcpy(out, &machine->ram[TEXT_SCREEN_ADDR], len);
}

bool IntegrationTest::compare_vram_region(uint16_t offset, const uint8_t* expected, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (machine->ram[TEXT_SCREEN_ADDR + offset + i] != expected[i])
            return false;
    }
    return true;
}

// ============================================================================
// VRAM diagnostics
// ============================================================================

void IntegrationTest::dump_vram_hex(int rows) {
    if (rows > SCREEN_ROWS) rows = SCREEN_ROWS;
    Serial.println("\n--- VRAM Hex Dump ($0400) ---");
    Serial.println("       +0 +1 +2 +3 +4 +5 +6 +7 +8 +9 +A +B +C +D +E +F  ASCII");
    for (int r = 0; r < rows; r++) {
        uint16_t addr = TEXT_SCREEN_ADDR + r * SCREEN_COLS;
        for (int half = 0; half < 2; half++) {
            uint16_t la = addr + half * 16;
            Serial.printf("$%04X: ", la);
            char ascii[17];
            for (int i = 0; i < 16; i++) {
                uint8_t b = machine->ram[la + i];
                Serial.printf("%02X ", b);
                ascii[i] = vram_to_ascii(b);
                if (ascii[i] < ' ' || ascii[i] > '~') ascii[i] = '.';
            }
            ascii[16] = '\0';
            Serial.printf(" %s\n", ascii);
        }
    }
    Serial.println("----------------------------");
}

void IntegrationTest::dump_screen_text() {
    Serial.println("\n--- Screen Text ---");
    Serial.println("+--------------------------------+");
    char line[SCREEN_COLS + 1];
    for (int r = 0; r < SCREEN_ROWS; r++) {
        for (int c = 0; c < SCREEN_COLS; c++) {
            line[c] = vram_to_ascii(machine->ram[TEXT_SCREEN_ADDR + r * SCREEN_COLS + c]);
            if (line[c] < ' ' || line[c] > '~') line[c] = '.';
        }
        line[SCREEN_COLS] = '\0';
        Serial.printf("|%s|\n", line);
    }
    Serial.println("+--------------------------------+");
}

// ============================================================================
// Ensure booted helper
// ============================================================================

bool IntegrationTest::ensure_booted() {
    if (boot_verified && screen_contains("OK")) {
        release_all_keys();
        for (int i = 0; i < 8; i++)
            machine->ram[0x0152 + i] = 0xFF;
        run_frames(15);
        return true;
    }
    machine_reset(machine);
    release_all_keys();
    frame_counter = 0;
    if (wait_for_screen_text("OK", 360)) {
        boot_verified = true;
        run_frames(15);
        return true;
    }
    return false;
}

// ============================================================================
// Result recording
// ============================================================================

void IntegrationTest::record(const char* name, bool passed, uint32_t frames, uint32_t ms) {
    if (result_count < MAX_TESTS) {
        results[result_count].name = name;
        results[result_count].passed = passed;
        results[result_count].elapsed_frames = frames;
        results[result_count].elapsed_ms = ms;
        result_count++;
    }
    if (passed) pass_count++; else fail_count++;
}

// ============================================================================
// RS-DOS disk helpers: read a file from disk cache
// ============================================================================

// Read a named file from disk cache by following RS-DOS directory + granule chain.
// Allocates *file_buf with malloc (caller must free). Returns file size or -1.
int IntegrationTest::read_rsdos_file(const char* filename, uint8_t** file_buf, int drive) {
    SV_DiskImage* disk = &machine->fdc.drives[drive];
    if (!disk->mounted || !disk->cache) {
        Serial.printf("  [disk] Drive %d not mounted or no cache\n", drive);
        return -1;
    }

    uint16_t sec_size = disk->sector_size;
    uint8_t spt = disk->sectors_per_track;

    // Helper lambda: get pointer to sector data in cache
    // Track/sector are 0-based track, 1-based sector
    auto sector_ptr = [&](int track, int sector) -> uint8_t* {
        if (sector < 1 || sector > spt || track < 0 || track >= disk->tracks)
            return nullptr;
        uint32_t off = (uint32_t)track * spt * sec_size + (uint32_t)(sector - 1) * sec_size;
        if (off + sec_size > disk->cache_size) return nullptr;
        return disk->cache + off;
    };

    // Read FAT (track 17, sector 2)
    uint8_t* fat = sector_ptr(RSDOS_DIR_TRACK, RSDOS_FAT_SECTOR);
    if (!fat) {
        Serial.println("  [disk] Cannot read FAT sector");
        return -1;
    }

    // Build padded 8.3 filename for comparison (RS-DOS: 8-char name + 3-char ext, space-padded)
    char name83[12]; // 8 + 3 + NUL
    memset(name83, ' ', 11);
    name83[11] = '\0';

    // Parse "FILENAME" or "FILENAME.EXT"
    const char* dot = strchr(filename, '.');
    int name_len = dot ? (int)(dot - filename) : (int)strlen(filename);
    if (name_len > 8) name_len = 8;
    for (int i = 0; i < name_len; i++)
        name83[i] = toupper(filename[i]);

    if (dot) {
        const char* ext = dot + 1;
        int ext_len = strlen(ext);
        if (ext_len > 3) ext_len = 3;
        for (int i = 0; i < ext_len; i++)
            name83[8 + i] = toupper(ext[i]);
    }

    Serial.printf("  [disk] Searching for '%.8s.%.3s' in directory\n", name83, name83 + 8);

    // Scan directory (track 17, sectors 3-11)
    int first_granule = -1;
    int file_type = -1;
    int ascii_flag = -1;

    for (int sec = RSDOS_DIR_FIRST_SEC; sec <= RSDOS_DIR_LAST_SEC; sec++) {
        uint8_t* dir = sector_ptr(RSDOS_DIR_TRACK, sec);
        if (!dir) continue;

        for (int e = 0; e < RSDOS_ENTRIES_PER_SEC; e++) {
            uint8_t* entry = dir + e * RSDOS_DIR_ENTRY_SIZE;

            // Byte 0: $00 = deleted, $FF = end of directory
            if (entry[0] == 0xFF) goto dir_done;
            if (entry[0] == 0x00) continue;

            // Compare 8+3 name (bytes 0-7 = name, 8-10 = extension)
            if (memcmp(entry, name83, 11) == 0) {
                file_type = entry[11];     // 0=BASIC, 1=data, 2=ML
                ascii_flag = entry[12];    // 0=binary, $FF=ASCII
                first_granule = entry[13]; // First granule number
                Serial.printf("  [disk] Found: type=%d ascii=%d first_gran=%d\n",
                              file_type, ascii_flag, first_granule);
                goto dir_done;
            }
        }
    }
dir_done:

    if (first_granule < 0) {
        Serial.printf("  [disk] File '%s' not found in directory\n", filename);
        return -1;
    }

    // Follow granule chain to collect all sectors
    // Granule N maps to: track = N/2, starting sector = (N%2)*9 + 1
    // FAT[N]: $C0-$C9 = last granule, value & 0x0F = number of sectors used in last granule
    //         $00-$43 = next granule number

    // First pass: count total bytes
    int total_bytes = 0;
    int gran = first_granule;
    int chain_len = 0;
    const int MAX_CHAIN = 128; // safety limit

    // Temporary chain storage
    struct GranInfo { int granule; int sectors; };
    GranInfo chain[MAX_CHAIN];

    while (chain_len < MAX_CHAIN) {
        if (gran < 0 || gran > 67) {
            Serial.printf("  [disk] Invalid granule %d in chain\n", gran);
            return -1;
        }

        uint8_t fat_entry = fat[gran];

        if (fat_entry >= 0xC0 && fat_entry <= 0xC9) {
            // Last granule: low nibble = sectors used (1-9)
            int secs = fat_entry & 0x0F;
            if (secs == 0) secs = 9; // $C0 means 9 sectors? Actually 0 means read last sector bytes from dir
            // RS-DOS: bytes in last sector from directory entry bytes 16-17
            chain[chain_len].granule = gran;
            chain[chain_len].sectors = secs;
            chain_len++;
            total_bytes += secs * sec_size;
            break;
        } else {
            // Full granule (9 sectors)
            chain[chain_len].granule = gran;
            chain[chain_len].sectors = RSDOS_GRANULE_SECTORS;
            chain_len++;
            total_bytes += RSDOS_GRANULE_SECTORS * sec_size;
            gran = fat_entry;
        }
    }

    // The last granule's last sector may not be fully used.
    // RS-DOS directory entry bytes 14-15 (big-endian) = bytes used in last sector of last granule
    // We need to re-scan directory for this — but for LOADM comparison we'll
    // use the LOADM preamble to determine exact lengths. So read full sectors.

    Serial.printf("  [disk] Granule chain: %d granules, %d bytes (full sectors)\n",
                  chain_len, total_bytes);

    if (total_bytes == 0) {
        Serial.println("  [disk] File is empty");
        return -1;
    }

    // Allocate buffer and read sectors
    uint8_t* buf = (uint8_t*)malloc(total_bytes);
    if (!buf) {
        Serial.printf("  [disk] malloc(%d) failed\n", total_bytes);
        return -1;
    }

    int offset = 0;
    for (int ci = 0; ci < chain_len; ci++) {
        int g = chain[ci].granule;
        int nsecs = chain[ci].sectors;
        int track = g / 2;
        if (track >= RSDOS_DIR_TRACK) track++;  // skip directory track 17
        int start_sector = (g % 2) * RSDOS_GRANULE_SECTORS + 1;

        for (int s = 0; s < nsecs; s++) {
            uint8_t* sp = sector_ptr(track, start_sector + s);
            if (!sp) {
                Serial.printf("  [disk] Bad sector: T%d S%d\n", track, start_sector + s);
                free(buf);
                return -1;
            }
            memcpy(buf + offset, sp, sec_size);
            offset += sec_size;
        }
    }

    Serial.printf("  [disk] Read %d bytes from disk cache\n", offset);
    *file_buf = buf;
    return offset;
}

// ============================================================================
// Parse LOADM preamble/postamble
// ============================================================================

int IntegrationTest::parse_loadm_segments(const uint8_t* data, int size,
                                           LoadmSegment* segs, int max_segs,
                                           uint16_t* exec_addr) {
    int pos = 0;
    int seg_count = 0;
    *exec_addr = 0;

    while (pos < size && seg_count < max_segs) {
        if (pos + 5 > size) break;

        uint8_t type = data[pos];

        if (type == LOADM_PREAMBLE) {
            // Preamble: $00 len_hi len_lo addr_hi addr_lo data[len]
            uint16_t len  = ((uint16_t)data[pos + 1] << 8) | data[pos + 2];
            uint16_t addr = ((uint16_t)data[pos + 3] << 8) | data[pos + 4];
            pos += 5;

            if (len == 0) {
                // Zero-length preamble = skip (sometimes used as padding)
                continue;
            }

            segs[seg_count].addr = addr;
            segs[seg_count].length = len;
            segs[seg_count].file_off = pos;
            seg_count++;

            Serial.printf("  [loadm] Segment %d: $%04X len=%d (file offset %d)\n",
                          seg_count, addr, len, pos);
            pos += len;
        } else if (type == LOADM_POSTAMBLE) {
            // Postamble: $FF $00 $00 exec_hi exec_lo
            *exec_addr = ((uint16_t)data[pos + 3] << 8) | data[pos + 4];
            Serial.printf("  [loadm] Exec addr: $%04X\n", *exec_addr);
            break;
        } else {
            Serial.printf("  [loadm] Unexpected byte $%02X at offset %d\n", type, pos);
            break;
        }
    }

    return seg_count;
}

// ============================================================================
// Read a raw file from SD card. Allocates buffer with malloc (caller frees).
// Returns file size or -1 on error.
// ============================================================================

int IntegrationTest::read_sd_file(const char* path, uint8_t** out_buf) {
    if (!SD.exists(path)) {
        Serial.printf("  [sd] File '%s' not found on SD card\n", path);
        return -1;
    }
    File f = SD.open(path, FILE_READ);
    if (!f) {
        Serial.printf("  [sd] Failed to open '%s'\n", path);
        return -1;
    }
    int sz = (int)f.size();
    if (sz <= 0) {
        f.close();
        Serial.printf("  [sd] File '%s' is empty\n", path);
        return -1;
    }
    uint8_t* buf = (uint8_t*)malloc(sz);
    if (!buf) {
        f.close();
        Serial.printf("  [sd] malloc(%d) failed\n", sz);
        return -1;
    }
    int rd = (int)f.read(buf, sz);
    f.close();
    if (rd != sz) {
        Serial.printf("  [sd] Read %d of %d bytes\n", rd, sz);
        free(buf);
        return -1;
    }
    Serial.printf("  [sd] Read %d bytes from '%s'\n", sz, path);
    *out_buf = buf;
    return sz;
}

// ============================================================================
// Compare two buffers, report matches/mismatches with hex dump
// ============================================================================

void IntegrationTest::compare_buffers(const char* label,
                                       const uint8_t* expected, const uint8_t* actual,
                                       int len, int base_addr) {
    int matches = 0, mismatches = 0;
    int first_mismatch = -1;

    for (int i = 0; i < len; i++) {
        if (expected[i] == actual[i]) {
            matches++;
        } else {
            if (mismatches < 16) {
                Serial.printf("    MISMATCH @$%04X: expected=$%02X got=$%02X (offset %d)\n",
                              base_addr + i, expected[i], actual[i], i);
            }
            if (first_mismatch < 0) first_mismatch = i;
            mismatches++;
        }
    }

    Serial.printf("  [%s] %d bytes compared: %d match, %d mismatch",
                  label, len, matches, mismatches);
    if (base_addr >= 0)
        Serial.printf(" (base addr $%04X)", base_addr);
    Serial.println();

    // Hex dump around first mismatch
    if (first_mismatch >= 0) {
        int dstart = (first_mismatch > 16) ? first_mismatch - 16 : 0;
        int dend = first_mismatch + 32;
        if (dend > len) dend = len;

        Serial.printf("    Expected (offset %d):\n    ", dstart);
        for (int i = dstart; i < dend; i++)
            Serial.printf("%02X ", expected[i]);
        Serial.println();

        Serial.printf("    Actual   (offset %d):\n    ", dstart);
        for (int i = dstart; i < dend; i++)
            Serial.printf("%02X ", actual[i]);
        Serial.println();
    }
}

// ============================================================================
// Test: LOADM verification
// ============================================================================

// User-triggered verification: user types LOADM on the CoCo keyboard,
// then sends 'R' on serial to verify RAM against disk cache.
//
// Three-way comparison:
// 1. SD card file vs disk image extraction (validates our RS-DOS reader)
// 2. File data (after LOADM headers) vs CoCo RAM (validates the load)
bool IntegrationTest::test_loadm_verify(const char* filename, int iterations) {
    Serial.println("\n========================================");
    Serial.println("=== LOADM Verify Test ===");
    Serial.println("========================================");
    Serial.printf("  File: %s\n", filename);

    // --- Step 1: Read raw file from SD card ---
    char sd_path[64];
    snprintf(sd_path, sizeof(sd_path), "/%s", filename);
    uint8_t* sd_data = nullptr;
    int sd_size = read_sd_file(sd_path, &sd_data);

    // --- Step 2: Read file from disk image cache ---
    uint8_t* disk_data = nullptr;
    int disk_size = read_rsdos_file(filename, &disk_data, 0);

    // --- Step 3: Compare SD vs disk extraction ---
    if (sd_data && sd_size > 0 && disk_data && disk_size > 0) {
        Serial.println("\n--- SD Card vs Disk Image Extraction ---");
        Serial.printf("  SD card file:   %d bytes\n", sd_size);
        Serial.printf("  Disk extracted: %d bytes\n", disk_size);

        int cmp_len = (sd_size < disk_size) ? sd_size : disk_size;
        compare_buffers("SD vs Disk", sd_data, disk_data, cmp_len, -1);

        if (sd_size != disk_size) {
            Serial.printf("  WARNING: Size mismatch! SD=%d, Disk=%d (diff=%d)\n",
                          sd_size, disk_size, disk_size - sd_size);
            Serial.println("  Note: Disk extraction reads full sectors, SD file is exact size.");
            Serial.println("  The disk may have extra padding bytes at the end.");
        }
    } else {
        if (!sd_data || sd_size <= 0)
            Serial.printf("  [INFO] No SD card file at '%s' — skipping SD comparison\n", sd_path);
        if (!disk_data || disk_size <= 0) {
            Serial.println("  [FAIL] Could not read file from disk cache");
            if (sd_data) free(sd_data);
            return false;
        }
    }

    // --- Step 4: Parse LOADM segments from the file ---
    // Use SD file if available (exact size), otherwise disk extraction
    uint8_t* file_data = sd_data ? sd_data : disk_data;
    int file_size = sd_data ? sd_size : disk_size;

    Serial.println("\n--- LOADM Header Parsing ---");
    Serial.printf("  First 16 bytes of file: ");
    for (int i = 0; i < 16 && i < file_size; i++)
        Serial.printf("%02X ", file_data[i]);
    Serial.println();

    LoadmSegment segs[MAX_LOADM_SEGMENTS];
    uint16_t exec_addr = 0;
    int seg_count = parse_loadm_segments(file_data, file_size, segs, MAX_LOADM_SEGMENTS, &exec_addr);

    if (seg_count == 0) {
        Serial.println("  [FAIL] No valid LOADM segments found");
        if (sd_data) free(sd_data);
        if (disk_data) free(disk_data);
        return false;
    }

    uint32_t total_data_bytes = 0;
    for (int s = 0; s < seg_count; s++)
        total_data_bytes += segs[s].length;

    Serial.printf("  Segments: %d, total data payload: %lu bytes\n",
                  seg_count, (unsigned long)total_data_bytes);
    Serial.printf("  Exec address: $%04X\n", exec_addr);

    // --- Step 5: Compare each LOADM segment data vs CoCo RAM ---
    Serial.println("\n--- CoCo RAM Verification ---");
    Serial.println("  (Comparing LOADM data payload vs CoCo RAM at load addresses)");
    dump_screen_text();

    bool all_pass = true;
    int total_matches = 0;
    int total_mismatches = 0;

    for (int s = 0; s < seg_count; s++) {
        uint16_t addr = segs[s].addr;
        uint16_t len  = segs[s].length;
        uint32_t foff = segs[s].file_off;  // offset past the 5-byte header

        Serial.printf("\n  Segment %d: load addr=$%04X, length=%d, file_offset=%d\n",
                      s, addr, len, (int)foff);
        Serial.printf("  Header skipped: bytes 0-%d (type=$%02X size=$%04X addr=$%04X)\n",
                      (int)(foff - 1), file_data[foff - 5],
                      (uint16_t)((file_data[foff - 4] << 8) | file_data[foff - 3]),
                      (uint16_t)((file_data[foff - 2] << 8) | file_data[foff - 1]));

        if (addr + len > 0x8000) {
            Serial.printf("  [seg%d] $%04X+%d overlaps ROM area, skipping\n", s, addr, len);
            continue;
        }

        int seg_matches = 0, seg_mismatches = 0;
        int first_mismatch = -1;

        for (int i = 0; i < len; i++) {
            uint8_t expected = file_data[foff + i];
            uint8_t actual   = machine->ram[addr + i];
            if (expected == actual) {
                seg_matches++;
            } else {
                if (seg_mismatches < 16) {
                    Serial.printf("    MISMATCH @$%04X: expected=$%02X got=$%02X (file+%d)\n",
                                  addr + i, expected, actual, (int)(foff + i));
                }
                if (first_mismatch < 0) first_mismatch = i;
                seg_mismatches++;
            }
        }

        Serial.printf("  [seg%d] CoCo addr $%04X-%04X: %d match, %d mismatch out of %d bytes\n",
                      s, addr, addr + len - 1, seg_matches, seg_mismatches, len);

        total_matches += seg_matches;
        total_mismatches += seg_mismatches;

        if (seg_mismatches > 0) {
            all_pass = false;

            // Hex dump around first mismatch
            if (first_mismatch >= 0) {
                int dstart = (first_mismatch > 16) ? first_mismatch - 16 : 0;
                int dend = first_mismatch + 32;
                if (dend > len) dend = len;

                Serial.printf("    Expected (file offset %d):\n    ", (int)(foff + dstart));
                for (int i = dstart; i < dend; i++)
                    Serial.printf("%02X ", file_data[foff + i]);
                Serial.println();

                Serial.printf("    Actual   (RAM $%04X):\n    ", addr + dstart);
                for (int i = dstart; i < dend; i++)
                    Serial.printf("%02X ", machine->ram[addr + i]);
                Serial.println();
            }
        }
    }

    // --- Summary ---
    Serial.println("\n========================================");
    Serial.printf("  TOTAL: %d match + %d mismatch = %d bytes checked\n",
                  total_matches, total_mismatches, total_matches + total_mismatches);
    if (all_pass) {
        Serial.printf("  RESULT: PASS — all %d bytes verified OK\n", total_matches);
    } else {
        Serial.printf("  RESULT: FAIL — %d mismatches found\n", total_mismatches);
    }
    Serial.println("========================================\n");

    if (sd_data) free(sd_data);
    if (disk_data) free(disk_data);

    return all_pass;
}

// ============================================================================
// Run all (dispatches single verify)
// ============================================================================

void IntegrationTest::run_all(bool stop_on_failure) {
    pass_count = 0;
    fail_count = 0;
    result_count = 0;

    uint32_t sm = millis();
    bool p = test_loadm_verify("ZAXXON.BIN", 1);
    uint32_t em = millis() - sm;

    record("LOADM ZAXXON.BIN", p, 0, em);

    Serial.printf("=== %s (%u ms) ===\n", p ? "PASS" : "FAIL", em);
}

// ============================================================================
// Print report
// ============================================================================

void IntegrationTest::print_report() {
    if (result_count == 0) {
        Serial.println("No test results. Run tests first (R).");
        return;
    }
    Serial.println("\n=== Last Test Report ===");
    for (int i = 0; i < result_count; i++) {
        Serial.printf("  [%s] %-22s %5u frames  %5u ms\n",
                      results[i].passed ? "PASS" : "FAIL",
                      results[i].name,
                      results[i].elapsed_frames,
                      results[i].elapsed_ms);
    }
    Serial.printf("Total: %d PASS  %d FAIL\n", pass_count, fail_count);
}

// ============================================================================
// Serial command interface
// ============================================================================

void IntegrationTest::process_serial_command(char cmd) {
    switch (cmd) {
        case 'R': case 'r':
            run_all(false);
            break;

        case 'S': case 's':
            print_report();
            break;

        case 'D': case 'd':
            dump_vram_hex(16);
            break;

        case 'T': case 't':
            dump_screen_text();
            break;
    }
}

/* ==========================================================================
 * Commented-out old tests (kept for reference):
 *
 * bool IntegrationTest::test_boot_sequence()   — boot to OK prompt
 * bool IntegrationTest::test_basic_print()     — PRINT "ABCDEFGH"
 * bool IntegrationTest::test_basic_for_loop()  — FOR I=1 TO 10
 * bool IntegrationTest::test_graphics_pmode4() — PMODE 3 page flip
 * bool IntegrationTest::test_sound_output()    — SOUND 100,10
 *
 * These were in the original integration_test.cpp and can be restored
 * by uncommenting their declarations in integration_test.h and re-adding
 * the implementations from git history.
 * ========================================================================== */

#endif // MACHINE_TYPE != 4

// ############################################################
// ##  CoCo 3 Integration Tests (Phase 5)
// ############################################################

#if MACHINE_TYPE == 4

#include "integration_test.h"
#include "../core/tcc1014.h"
#include <string.h>
#include <stdio.h>
#include <Arduino.h>

CoCo3IntegrationTest::CoCo3IntegrationTest(Machine* m)
    : machine(m), pass_count(0), fail_count(0) {}

void CoCo3IntegrationTest::record(const char* name, bool passed) {
    if (passed) {
        pass_count++;
        Serial.printf("  PASS: %s\n", name);
    } else {
        fail_count++;
        Serial.printf("  FAIL: %s\n", name);
    }
}

// ============================================================
// Test: MMU identity map after reset
// After reset, banks should be 0x38-0x3F (identity map to top 64KB)
// ============================================================
bool CoCo3IntegrationTest::test_mmu_identity_map() {
    TCC1014* g = &machine->gime;
    tcc1014_reset(g);
    g->ram = machine->ram_physical;
    g->ram_size = COCO3_PHYSICAL_RAM;

    for (int i = 0; i < 8; i++) {
        if (g->mmu_bank[i] != (0x38 | i)) return false;
        if (g->mmu_bank[8 + i] != (0x38 | i)) return false;
        // Phase 5: verify active_banks cache matches
        if (g->active_banks[i] != (0x38 | i)) return false;
    }
    return true;
}

// ============================================================
// Test: MMU bank isolation
// Write different values through different MMU banks,
// verify they land in different physical RAM locations.
// ============================================================
bool CoCo3IntegrationTest::test_mmu_bank_isolation() {
    TCC1014* g = &machine->gime;
    tcc1014_reset(g);
    g->ram = machine->ram_physical;
    g->ram_size = COCO3_PHYSICAL_RAM;
    memset(machine->ram_physical, 0, COCO3_PHYSICAL_RAM);

    // Enable MMU
    tcc1014_write_register(g, 0, 0x40);  // MMUEN=1

    // Map slot 0 ($0000-$1FFF) to bank 0x00
    tcc1014_write_mmu(g, 0, 0x00);
    // Map slot 1 ($2000-$3FFF) to bank 0x01
    tcc1014_write_mmu(g, 1, 0x01);

    // Write via address decode to slot 0
    uint8_t wr_data;
    bool is_reg;
    wr_data = 0xAA;
    tcc1014_mem_cycle(g, 0x0000, false, wr_data, nullptr, &is_reg);
    if (g->RAS) machine->ram_physical[g->Z] = wr_data;

    // Write via address decode to slot 1
    wr_data = 0x55;
    tcc1014_mem_cycle(g, 0x2000, false, wr_data, nullptr, &is_reg);
    if (g->RAS) machine->ram_physical[g->Z] = wr_data;

    // Verify physical addresses differ
    // Bank 0x00 → physical 0x00000, Bank 0x01 → physical 0x02000
    if (machine->ram_physical[0x00000] != 0xAA) return false;
    if (machine->ram_physical[0x02000] != 0x55) return false;

    return true;
}

// ============================================================
// Test: MMU task switch
// Task 0 and Task 1 should use different bank sets
// ============================================================
bool CoCo3IntegrationTest::test_mmu_task_switch() {
    TCC1014* g = &machine->gime;
    tcc1014_reset(g);
    g->ram = machine->ram_physical;
    g->ram_size = COCO3_PHYSICAL_RAM;

    // Enable MMU
    tcc1014_write_register(g, 0, 0x40);  // MMUEN=1

    // Set Task 0 slot 0 = bank 0x02
    tcc1014_write_mmu(g, 0, 0x02);
    // Set Task 1 slot 0 = bank 0x05
    tcc1014_write_mmu(g, 8, 0x05);

    // Select Task 0 (TR=0)
    tcc1014_write_register(g, 1, 0x00);
    if (g->active_banks[0] != 0x02) return false;

    // Select Task 1 (TR=8)
    tcc1014_write_register(g, 1, 0x01);
    if (g->active_banks[0] != 0x05) return false;

    return true;
}

// ============================================================
// Test: Phase 5 fast-path cache consistency
// Verify active_banks[] stays in sync after various writes
// ============================================================
bool CoCo3IntegrationTest::test_mmu_fast_path_cache() {
    TCC1014* g = &machine->gime;
    tcc1014_reset(g);

    // After reset: MMUEN=0, all slots should be identity (0x38|i)
    for (int i = 0; i < 8; i++) {
        if (g->active_banks[i] != (0x38 | i)) return false;
    }

    // Enable MMU
    tcc1014_write_register(g, 0, 0x40);
    // Now active_banks should reflect mmu_bank[TR|i] (TR=0 after reset)
    for (int i = 0; i < 8; i++) {
        if (g->active_banks[i] != g->mmu_bank[i]) return false;
    }

    // Change a bank and verify cache updates
    tcc1014_write_mmu(g, 3, 0x10);
    if (g->active_banks[3] != 0x10) return false;

    // Disable MMU — should revert to identity
    tcc1014_write_register(g, 0, 0x00);
    if (g->active_banks[3] != (0x38 | 3)) return false;

    return true;
}

// ============================================================
// Test: GIME register write/read
// Only IRQ/FIRQ status registers are readable
// ============================================================
bool CoCo3IntegrationTest::test_gime_register_rw() {
    TCC1014* g = &machine->gime;
    tcc1014_reset(g);

    // Write INIT0 — verify decoded fields
    tcc1014_write_register(g, 0, 0xC8);  // COCO=1, MMUEN=1, MC3=1
    if (!g->COCO) return false;
    if (!g->MMUEN) return false;
    if (!g->MC3) return false;

    // Write VRES — verify decoded fields
    tcc1014_write_register(g, 9, 0x74);  // LPF=3, HRES=5, CRES=0
    if (g->LPF != 3) return false;
    if (g->HRES != 5) return false;
    if (g->CRES != 0) return false;

    // IRQ status: force an interrupt, then read-acknowledge
    g->registers[2] = 0x20;  // Enable TMR in IRQ
    g->registers[0] = 0x20;  // Master IRQ enable
    tcc1014_set_interrupt(g, TCC1014_INT_TMR);
    if (!g->IRQ) return false;

    uint8_t status = tcc1014_read_register(g, 2);
    if (!(status & TCC1014_INT_TMR)) return false;
    // After read-acknowledge, IRQ should be cleared
    // (unless timer is still at 0, which re-asserts)

    return true;
}

// ============================================================
// Test: Palette register write/read
// ============================================================
bool CoCo3IntegrationTest::test_palette_rw() {
    TCC1014* g = &machine->gime;
    tcc1014_reset(g);

    // Write palette entry 5 = 0x3F (brightest white)
    tcc1014_write_palette(g, 5, 0x3F);
    if (g->palette_reg[5] != 0x3F) return false;

    // Read it back
    uint8_t val = tcc1014_read_palette(g, 5);
    if (val != 0x3F) return false;

    // Verify RGB565 pre-computation (white = 0xFFFF)
    // GIME 0x3F = R=3,G=3,B=3 = 255,255,255 → RGB565 = 0xFFFF
    if (g->palette_rgb565[5] != 0xFFFF) return false;

    // Verify dirty flag set
    g->dirty_frame = false;
    tcc1014_write_palette(g, 0, 0x30);  // Red
    if (!g->dirty_frame) return false;

    return true;
}

// ============================================================
// Test: ROM mapping
// With default reset state (MC1=0, MC0=0, TY=0),
// banks 0x3C-0x3F should produce S=0 (ROM) or S=1 (CTS)
// ============================================================
bool CoCo3IntegrationTest::test_rom_mapping() {
    TCC1014* g = &machine->gime;
    tcc1014_reset(g);
    g->ram = machine->ram_physical;
    g->ram_size = COCO3_PHYSICAL_RAM;

    // After reset: INIT0=0 → COCO=0, MMUEN=0, MC1=0, MC0=0, TY=0
    // Default banks: 0x38-0x3F (identity), so $8000-$9FFF → bank 0x3C
    // With MC1=0: bank >= 0x3E → CTS (S=1), else ROM (S=0)
    bool is_reg = false;
    uint8_t rd = 0;
    tcc1014_mem_cycle(g, 0x8000, true, 0, &rd, &is_reg);
    if (g->S != 0) return false;  // Should be ROM

    tcc1014_mem_cycle(g, 0xC000, true, 0, &rd, &is_reg);
    if (g->S != 1) return false;  // Should be CTS (bank 0x3E)

    // Enable all-RAM mode (TY=1 via SAM)
    // SAM bit 15 = TY. SAM register $FFD0/$FFD1 sets/clears bit 8
    // Actually, TY is SAM register bit 15
    g->TY = true;
    tcc1014_mem_cycle(g, 0x8000, true, 0, &rd, &is_reg);
    if (!g->RAS) return false;  // Should be RAM access now

    return true;
}

// ============================================================
// Test: Timer fires and generates TMR interrupt
// ============================================================
bool CoCo3IntegrationTest::test_timer_fire() {
    TCC1014* g = &machine->gime;
    tcc1014_reset(g);

    // Enable TMR interrupt on IRQ line
    g->registers[0] = 0x20;  // Master IRQ enable
    tcc1014_write_register(g, 2, 0x20);  // Enable TMR in IRQ enable reg
    // Clear after the enable-write clears state
    g->irq_state = 0;
    g->IRQ = false;

    // Set timer to 3 (TINS=0 = scanline rate)
    tcc1014_write_register(g, 5, 3);   // LSB = 3
    tcc1014_write_register(g, 4, 0);   // MSB = 0 (arms timer: counter = 3+1=4)

    // Re-enable after timer arm clears
    g->registers[2] = 0x20;
    g->irq_state = 0;
    g->IRQ = false;

    // Tick 3 scanlines — timer should not have fired yet
    tcc1014_tick_scanline(g);  // counter: 4→3
    tcc1014_tick_scanline(g);  // counter: 3→2
    tcc1014_tick_scanline(g);  // counter: 2→1
    if (g->IRQ) return false;  // Should not fire yet

    // 4th tick — timer reaches 0, should fire
    tcc1014_tick_scanline(g);  // counter: 1→0
    if (!g->IRQ) return false;  // TMR interrupt should be asserted

    return true;
}

// ============================================================
// Test: Interrupt read-acknowledge clears IRQ/FIRQ
// ============================================================
bool CoCo3IntegrationTest::test_interrupt_acknowledge() {
    TCC1014* g = &machine->gime;
    tcc1014_reset(g);

    // Enable VBORD on IRQ
    g->registers[0] = 0x20;  // Master IRQ enable
    g->registers[2] = TCC1014_INT_VBORD;  // Enable VBORD

    // Generate VBORD interrupt
    tcc1014_set_interrupt(g, TCC1014_INT_VBORD);
    if (!g->IRQ) return false;
    if (!(g->irq_state & TCC1014_INT_VBORD)) return false;

    // Read-acknowledge IRQ status register
    uint8_t status = tcc1014_read_register(g, 2);
    if (!(status & TCC1014_INT_VBORD)) return false;  // Should report VBORD

    // After acknowledge, IRQ should be deasserted
    if (g->IRQ) return false;
    if (g->irq_state != 0) return false;

    return true;
}

// ============================================================
// Test: SAM compatibility writes
// ============================================================
bool CoCo3IntegrationTest::test_sam_compat() {
    TCC1014* g = &machine->gime;
    tcc1014_reset(g);

    // SAM register is 16 bits, set/clear via pairs at $FFC0-$FFDF
    // Offset 0 = clear bit 0, offset 1 = set bit 0
    // Offset 2 = clear bit 1, offset 3 = set bit 1
    tcc1014_write_sam(g, 1);   // Set bit 0 (V0)
    tcc1014_write_sam(g, 3);   // Set bit 1 (V1)
    if (g->SAM_V != 3) return false;  // V0=1, V1=1 → SAM_V=3

    tcc1014_write_sam(g, 0);   // Clear bit 0
    if (g->SAM_V != 2) return false;  // V0=0, V1=1 → SAM_V=2

    // Test TY bit (bit 15 → offset 30/31)
    tcc1014_write_sam(g, 31);  // Set bit 15 (TY)
    if (!g->TY) return false;

    tcc1014_write_sam(g, 30);  // Clear bit 15
    if (g->TY) return false;

    return true;
}

// ============================================================
// Test: VDG compat text rendering
// In COCO=1 text mode, verify render produces correct line width
// and non-zero pixel data from a known character in VRAM.
// ============================================================
bool CoCo3IntegrationTest::test_vdg_compat_text_rendering() {
    TCC1014* g = &machine->gime;
    tcc1014_reset(g);
    g->ram = machine->ram_physical;
    g->ram_size = COCO3_PHYSICAL_RAM;
    memset(machine->ram_physical, 0, COCO3_PHYSICAL_RAM);

    // Set VDG compat text mode: COCO=1, MMUEN=0
    tcc1014_write_register(g, 0, 0x80);  // COCO=1
    // Set PIA1B shadow for text mode (GnA=0)
    g->PIA1B_shadow.ddr = true;
    g->PIA1B_shadow.pdr = 0x00;  // GnA=0, CSS=0
    tcc1014_update_mode(g);

    // Set palette entries for VDG text colors (reset clears to 0 = black)
    tcc1014_write_palette(g, TCC1014_BRIGHT_GREEN, 0x12);  // fg
    tcc1014_write_palette(g, TCC1014_DARK_GREEN, 0x00);    // bg

    // Write 'A' (0x41) at VRAM start (physical 0x70000 for bank 0x38)
    // Identity map: $0000 → bank 0x38 → physical 0x70000
    uint32_t vram_phys = (0x38 << 13);
    machine->ram_physical[vram_phys] = 0x41;  // 'A'

    // Set B to point at VRAM
    g->B = vram_phys;
    g->row = 0;
    g->vertical.active_area = true;

    // Render one scanline
    tcc1014_render_scanline(g, 0);

    // VDG text: BPR=32, resolution=1 → 32 bytes × 16 pixels/byte = 512 pixels
    if (g->line_width != 512) return false;

    // First character 'A' should produce non-zero pixels (font data)
    // Check that at least some pixels in the first 16 positions are non-zero
    bool found_nonzero = false;
    for (int i = 0; i < 16; i++) {
        if (g->line_buffer[i] != 0) { found_nonzero = true; break; }
    }
    if (!found_nonzero) return false;

    return true;
}

// ============================================================
// Test: Palette RGB565 mapping (interleaved R1G1B1R0G0B0)
// Verify the GIME palette correctly maps to RGB565 colors.
// ============================================================
bool CoCo3IntegrationTest::test_palette_rgb565_mapping() {
    TCC1014* g = &machine->gime;
    tcc1014_reset(g);

    // GIME $00 = black (R=0,G=0,B=0) → RGB565 = 0x0000
    tcc1014_write_palette(g, 0, 0x00);
    if (g->palette_rgb565[0] != 0x0000) return false;

    // GIME $3F = white (R1G1B1R0G0B0 = 111111 → R=3,G=3,B=3) → RGB565 = 0xFFFF
    tcc1014_write_palette(g, 1, 0x3F);
    if (g->palette_rgb565[1] != 0xFFFF) return false;

    // GIME $12 = green (R1=0,G1=1,B1=0,R0=0,G0=1,B0=0)
    //   R = (0<<1)|0 = 0, G = (1<<1)|1 = 3, B = (0<<1)|0 = 0
    //   RGB = (0, 255, 0) → RGB565 = 0x07E0
    tcc1014_write_palette(g, 2, 0x12);
    if (g->palette_rgb565[2] != 0x07E0) return false;

    // GIME $24 = red (R1=1,G1=0,B1=1,R0=0,G0=0,B0=0)
    // Wait: $24 = 100100. R=(1<<1)|0=2, G=(0<<1)|0=0, B=(1<<1)|0=2
    // That's purple, not red. Pure red is R=3,G=0,B=0.
    // R=3 → R1=1,R0=1 → bits 5,2 set → 0b100100 | 0b000100 = need bits 5=1,2=1
    // $24 = 0b100100 → bit5=1,bit2=1,rest=0 → R=(1<<1)|1=3, G=0, B=0
    // Wait: $24 = 36 = 0b100100
    // bit5=1, bit4=0, bit3=0, bit2=1, bit1=0, bit0=0
    // R = (bit5<<1)|bit2 = (1<<1)|1 = 3 → 255
    // G = (bit4<<1)|bit1 = (0<<1)|0 = 0
    // B = (bit3<<1)|bit0 = (0<<1)|0 = 0
    // RGB = (255, 0, 0) → RGB565 = 0xF800
    tcc1014_write_palette(g, 3, 0x24);
    if (g->palette_rgb565[3] != 0xF800) return false;

    return true;
}

// ============================================================
// Test: Scanline output width for different modes
// Verify BPR and resolution produce correct line_width values.
// ============================================================
bool CoCo3IntegrationTest::test_scanline_output_width() {
    TCC1014* g = &machine->gime;
    tcc1014_reset(g);
    g->ram = machine->ram_physical;
    g->ram_size = COCO3_PHYSICAL_RAM;
    memset(machine->ram_physical, 0, COCO3_PHYSICAL_RAM);

    // Set up minimal state for rendering
    g->B = (0x38 << 13);
    g->vertical.active_area = true;
    g->row = 0;

    // VDG compat text: BPR=32, res=1 → 512 pixels
    tcc1014_write_register(g, 0, 0x80);  // COCO=1
    g->PIA1B_shadow.ddr = true;
    g->PIA1B_shadow.pdr = 0x00;
    tcc1014_update_mode(g);
    tcc1014_render_scanline(g, 0);
    if (g->line_width != 512) return false;

    // CoCo3 native 40-col text: COCO=0, BP=0, HRES=1
    // HRES=1 text → BPR=40, resolution=1 → 40 bytes × 16 px/byte = 640
    tcc1014_write_register(g, 0, 0x40);  // COCO=0, MMUEN=1
    tcc1014_write_register(g, 8, 0x00);  // BP=0 (text)
    tcc1014_write_register(g, 9, 0x04);  // HRES=1, CRES=0
    g->B = (0x38 << 13);
    g->row = 0;
    tcc1014_render_scanline(g, 0);
    // 40 bytes × 2 nibbles × 4 pixels × 2x expansion = 640
    if (g->line_width != 640) return false;

    return true;
}

// ============================================================
// Test: PIA DAC audio path
// Verify PIA1 port A DAC bits reach the expected value range.
// This is a wiring test — actual audio output depends on HAL.
// ============================================================
bool CoCo3IntegrationTest::test_pia_dac_audio_path() {
    Machine* m = machine;

    // PIA1 port A drives 6-bit DAC (bits 7-2)
    // Configure PIA1 port A: DDR = $FC (bits 7-2 output), data = $FC
    mc6821_reset(&m->pia1);
    // Select DDR (ctrl_a bit 2 = 0)
    mc6821_write(&m->pia1, 1, 0x00);  // CRA: DDR select
    mc6821_write(&m->pia1, 0, 0xFC);  // DDR: bits 7-2 as output
    mc6821_write(&m->pia1, 1, 0x04);  // CRA: data select
    mc6821_write(&m->pia1, 0, 0xFC);  // Write max DAC value

    // Read back — output bits should match
    uint8_t pa = m->pia1.data_a & m->pia1.ddr_a;
    uint8_t dac_val = (pa >> 2) & 0x3F;
    if (dac_val != 0x3F) return false;

    // Write zero
    mc6821_write(&m->pia1, 0, 0x00);
    pa = m->pia1.data_a & m->pia1.ddr_a;
    dac_val = (pa >> 2) & 0x3F;
    if (dac_val != 0x00) return false;

    return true;
}

// ============================================================
// Run all CoCo3 tests
// ============================================================

void CoCo3IntegrationTest::run_all() {
    pass_count = 0;
    fail_count = 0;

    Serial.println("=== CoCo3 Integration Tests ===");
    Serial.println("--- GIME Subsystem ---");
    record("MMU identity map",        test_mmu_identity_map());
    record("MMU bank isolation",      test_mmu_bank_isolation());
    record("MMU task switch",         test_mmu_task_switch());
    record("MMU fast-path cache",     test_mmu_fast_path_cache());
    record("GIME register r/w",       test_gime_register_rw());
    record("Palette r/w",             test_palette_rw());
    record("ROM mapping",             test_rom_mapping());
    record("Timer fire",              test_timer_fire());
    record("Interrupt acknowledge",   test_interrupt_acknowledge());
    record("SAM compat",              test_sam_compat());

    Serial.println("--- Video Modes ---");
    record("VDG compat text render",  test_vdg_compat_text_rendering());
    record("Palette RGB565 mapping",  test_palette_rgb565_mapping());
    record("Scanline output width",   test_scanline_output_width());

    Serial.println("--- Audio ---");
    record("PIA DAC audio path",      test_pia_dac_audio_path());

    print_report();

    // Restore machine state — tests modify GIME/PIA registers
    Serial.println("Resetting machine after tests...");
    machine_reset(machine);
}

void CoCo3IntegrationTest::print_report() {
    Serial.println("==========================================");
    Serial.printf("  Results: %d passed, %d failed, %d total\n",
                  pass_count, fail_count, pass_count + fail_count);
    Serial.println("==========================================");
}

void CoCo3IntegrationTest::process_serial_command(char cmd) {
    switch (cmd) {
    case 'R': case 'r':
        run_all();
        break;
    case 'S': case 's':
        print_report();
        break;
    default:
        Serial.printf("CoCo3 test commands: R=run all, S=show report\n");
        break;
    }
}

#endif // MACHINE_TYPE == 4
