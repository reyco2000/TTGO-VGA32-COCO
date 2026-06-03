/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : integration_test.h
 *  Module : Integration test framework — CoCo 2 and CoCo 3 tests
 * ============================================================
*/

#ifndef INTEGRATION_TEST_H
#define INTEGRATION_TEST_H

#include "../../config.h"
#include <stdint.h>
#include <stdbool.h>
#include "../core/machine.h"

// ============================================================
// CoCo 2 Tests (MACHINE_TYPE <= 3)
//
// LOADM disk verification, VRAM inspection, keyboard injection.
// These tests use direct RAM access with flat 16-bit addresses.
// ============================================================

#if MACHINE_TYPE != 4

// RS-DOS constants
#define RSDOS_DIR_TRACK      17
#define RSDOS_DIR_FIRST_SEC  3
#define RSDOS_DIR_LAST_SEC   11
#define RSDOS_FAT_SECTOR     2
#define RSDOS_GRANULE_SECTORS 9
#define RSDOS_DIR_ENTRY_SIZE 32
#define RSDOS_ENTRIES_PER_SEC (256 / RSDOS_DIR_ENTRY_SIZE)

#define LOADM_PREAMBLE  0x00
#define LOADM_POSTAMBLE 0xFF
#define MAX_LOADM_SEGMENTS 16

class IntegrationTest {
public:
    explicit IntegrationTest(Machine* m);

    bool test_loadm_verify(const char* filename, int iterations = 5);

    void inject_keystrokes(const char* text, int delay_frames = 3);
    bool wait_for_screen_text(const char* text, int timeout_frames = 360);
    void capture_vram_snapshot(uint8_t* out, size_t max_bytes);
    bool compare_vram_region(uint16_t offset, const uint8_t* expected, size_t len);

    void dump_vram_hex(int rows = 16);
    void dump_screen_text();

    void run_all(bool stop_on_failure = false);
    void print_report();
    void process_serial_command(char cmd);

    void set_headless(bool enabled) { headless = enabled; }
    bool is_headless() const { return headless; }

private:
    Machine* machine;
    int pass_count = 0;
    int fail_count = 0;
    bool headless = true;

    void run_frames(int count);

    struct TestResult {
        const char* name;
        bool passed;
        uint32_t elapsed_frames;
        uint32_t elapsed_ms;
    };
    static const int MAX_TESTS = 16;
    TestResult results[MAX_TESTS];
    int result_count = 0;

    void record(const char* name, bool passed, uint32_t frames, uint32_t ms);

    struct KeyEvent {
        uint8_t row;
        uint8_t col;
        bool shift;
        bool pressed;
    };
    static const int KEY_QUEUE_SIZE = 512;
    KeyEvent key_queue[KEY_QUEUE_SIZE];
    int kq_head = 0;
    int kq_tail = 0;
    int kq_count = 0;

    int key_hold_frames = 3;
    int key_gap_frames = 2;
    int key_timer = 0;
    bool key_active = false;

    void queue_key_event(uint8_t row, uint8_t col, bool shift, bool pressed);
    void process_key_queue();
    void release_all_keys();
    void drain_key_queue(int settle_frames = 30);

    static bool ascii_to_matrix(char c, uint8_t& row, uint8_t& col, bool& shift_out);
    static char vram_to_ascii(uint8_t vram_byte);

    void capture_screen_text(char* buf, size_t buf_size);
    bool screen_contains(const char* text);
    bool ensure_booted();

    struct LoadmSegment {
        uint16_t addr;
        uint16_t length;
        uint32_t file_off;
    };

    int read_rsdos_file(const char* filename, uint8_t** file_buf, int drive = 0);
    int read_sd_file(const char* path, uint8_t** out_buf);
    void compare_buffers(const char* label, const uint8_t* expected,
                         const uint8_t* actual, int len, int base_addr);
    int parse_loadm_segments(const uint8_t* file_data, int file_size,
                             LoadmSegment* segs, int max_segs,
                             uint16_t* exec_addr);

    uint32_t frame_counter = 0;
    bool boot_verified = false;
};

#endif // MACHINE_TYPE != 4

// ============================================================
// CoCo 3 Tests (MACHINE_TYPE == 4)
//
// GIME subsystem tests (MMU, palette, timer, interrupts) plus
// video mode and audio tests that exercise the full CoCo3 stack.
// ============================================================

#if MACHINE_TYPE == 4

#include "../core/tcc1014.h"

class CoCo3IntegrationTest {
public:
    explicit CoCo3IntegrationTest(Machine* m);

    void run_all();
    void print_report();
    void process_serial_command(char cmd);

private:
    Machine* machine;
    int pass_count;
    int fail_count;

    void record(const char* name, bool passed);

    // --- GIME subsystem tests ---
    bool test_mmu_identity_map();
    bool test_mmu_bank_isolation();
    bool test_mmu_task_switch();
    bool test_mmu_fast_path_cache();
    bool test_gime_register_rw();
    bool test_palette_rw();
    bool test_rom_mapping();
    bool test_timer_fire();
    bool test_interrupt_acknowledge();
    bool test_sam_compat();

    // --- Video mode tests ---
    bool test_vdg_compat_text_rendering();
    bool test_palette_rgb565_mapping();
    bool test_scanline_output_width();

    // --- Audio test ---
    bool test_pia_dac_audio_path();
};

#endif // MACHINE_TYPE == 4

#endif // INTEGRATION_TEST_H
