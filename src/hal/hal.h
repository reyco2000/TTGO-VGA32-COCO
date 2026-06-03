/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : hal.h
 *  Module : Hardware Abstraction Layer interface — all platform I/O declarations
 * ============================================================
*/

/*
 * hal.h - Hardware Abstraction Layer interface
 *
 * All platform-specific I/O goes through these functions.
 * Implementations are in the individual hal_*.cpp files.
 */

#ifndef HAL_H
#define HAL_H

#include <Arduino.h>
#include "../../config.h"
#include "../utils/debug.h"   // SerialPortMode + g_serial_mode

// ============================================================
// Serial-port ownership (single UART0 — see debug.h)
// ============================================================

// True when the RS-232 Pak owns UART0 — gates the $FF68 decode/tick/FIRQ.
static inline bool rs232_pak_enabled(void) {
    return g_serial_mode == SERIAL_MODE_RS232;
}

// Switch serial-port ownership at runtime. Resets the ACIA and, when leaving
// debug mode, flushes buffered host input. Does NOT persist — callers that
// want the choice remembered also call supervisor_save_serial_mode().
void serial_mode_apply(SerialPortMode mode);

// ============================================================
// Top-level HAL control
// ============================================================

// Initialize all HAL subsystems
void hal_init(void);

// Process all pending input
void hal_process_input(void);

// Render the current frame to the display
void hal_render_frame(void);

// ============================================================
// Video subsystem
// ============================================================

// Initialize the display hardware
void hal_video_init(void);

// Set the MC6847 display mode
//   mode bits: CSS, GM2, GM1, GM0, A/G, A/S, INT/EXT, INV
void hal_video_set_mode(uint8_t mode);

// Render one scanline of video data
//   line: scanline number (0-191 for active area)
//   pixels: pointer to pixel data for this line
//   width: number of pixels in the line
void hal_video_render_scanline(int line, const uint8_t* pixels, int width);

// Present the completed frame to the display
// Performs VRAM shadow compare: skips SPI push if screen unchanged (OPT-16)
//   ram:      pointer to CoCo RAM (64KB)
//   vdg_base: SAM F0-F6 display base address
//   vdg_mode: VDG mode byte (AG|GM|CSS bits)
void hal_video_present(const uint8_t* ram, uint16_t vdg_base, uint8_t vdg_mode);

// Render one GIME scanline (CoCo3 mode)
//   line:     display line number (0-based within active area)
//   pixels:   pre-converted RGB565 pixels (byte-swapped, OPT-C4 format) from
//             tcc1014_render_scanline — OPT-C4
//   width:    pixel width of the line
//   palette:  unused (retained for ABI stability; pass nullptr)
void hal_video_render_scanline_gime(int line, int total_lines,
                                     uint8_t border_colour,
                                     const uint16_t* pixels,
                                     int width, const uint16_t* palette);

// Present the completed CoCo3 frame to the display
// Phase 5: dirty flag — if non-null, skip SPI push when *dirty==false, clear after push
void hal_video_present_gime(bool* dirty = nullptr);

// ============================================================
// Audio subsystem
// ============================================================

// Initialize audio output hardware
void hal_audio_init(void);

// Write a single audio sample (mono or stereo depending on config)
void hal_audio_write_sample(int16_t left, int16_t right);

// Set audio volume (0-255)
void hal_audio_set_volume(uint8_t volume);

// Write single-bit audio (PIA1 port B bit 1)
void hal_audio_write_bit(bool value);

// Write 6-bit DAC audio (PIA1 port A bits 2-7, value 0-63)
void hal_audio_write_dac(uint8_t dac6);

// Call once per frame from main loop — detects end-of-sound and prints frequency debug
void hal_audio_debug_tick(void);

// Capture current audio level for the current scanline (call once per scanline)
void hal_audio_capture_scanline(void);

// Commit the scanline audio buffer for ISR playback (call at frame end)
void hal_audio_commit_frame(void);

// ============================================================
// Keyboard subsystem
// ============================================================

// Initialize keyboard input
void hal_keyboard_init(void);

// Scan keyboard matrix for a given column
//   column: 0-7 (active low)
//   Returns: row data (active low, bits 0-6)
uint8_t hal_keyboard_scan(uint8_t column);

// Tick deferred key releases (call once per frame)
void hal_keyboard_tick(void);

// ============================================================
// Joystick subsystem
// ============================================================

// Initialize joystick input
void hal_joystick_init(void);

// Read joystick axis value
//   port: 0 or 1
//   Returns: 6-bit DAC value (0-63) representing position
uint8_t hal_joystick_read_axis(int port, int axis);

// Read joystick button state
//   port: 0 or 1
//   Returns: 1 if pressed, 0 if not
uint8_t hal_joystick_read_button(int port);

// DAC comparator for PIA0 PA7 (matches XRoar's joystick_update)
//   port: 0=right, 1=left (from PIA0 CRB bit 3)
//   axis: 0=X, 1=Y (from PIA0 CRA bit 3)
//   dac_value: 6-bit threshold (from PIA1 DA bits 2-7)
//   Returns: true if joystick axis value >= dac_value
bool hal_joystick_compare(int port, int axis, uint8_t dac_value);

// Update joystick ADC readings (call once per frame)
void hal_joystick_update(void);

// Set machine pointer for keyboard hotkeys (F2 reset, etc.)
struct Machine;
void hal_keyboard_set_machine(Machine* m);


// Force full display repaint (invalidates VRAM shadow compare)
void hal_video_force_repaint(void);

// Toggle FPS overlay
void hal_video_toggle_fps_overlay(void);

// ============================================================
// Keyboard injection (for integration tests)
// ============================================================

// Press a CoCo key by matrix position (row, col)
void hal_keyboard_press(uint8_t row, uint8_t col);

// Release a CoCo key by matrix position
void hal_keyboard_release(uint8_t row, uint8_t col);

// Release all keys
void hal_keyboard_release_all(void);

// ============================================================
// Storage subsystem
// ============================================================

// Initialize storage (SD card or flash filesystem)
bool hal_storage_init(void);

// True if the SD card mounted successfully during hal_storage_init().
// Lets boot-time validation tell "SD missing" apart from "ROM missing".
bool hal_storage_is_ready(void);

// Load a file into a buffer
//   path: file path (relative to storage root)
//   buffer: destination buffer
//   size: max bytes to read
//   Returns: true on success
bool hal_storage_load_file(const char* path, uint8_t* buffer, size_t size);

// Check if a file exists
bool hal_storage_file_exists(const char* path);

#endif // HAL_H
