/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : config.h
 *  Module : Hardware configuration — pin assignments, compile-time options, and build constants
 *           Target: LilyGo TTGO VGA32 v1.4 (ESP32-WROVER)
 *           FQBN: esp32:esp32:esp32wrover:PartitionScheme=huge_app
 * ============================================================
*/

#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>

// ============================================================
// Build options
// ============================================================

// Firmware version (reported by the WiFi debug API /api/status).
#define FIRMWARE_VERSION        "0.7"

// Machine type: 0 = Dragon 32, 1 = Dragon 64, 2 = CoCo 1, 3 = CoCo 2, 4 = CoCo 3
// Compile-time default only — the active machine is g_machine_type (core/machine.h),
// initialized from NVS at boot and overrideable from the supervisor menu.
#define MACHINE_TYPE            4

// Stable human-readable names for the runtime-switchable machines.
#define MACHINE_NAME_COCO2      "CoCo 2"
#define MACHINE_NAME_COCO3      "CoCo 3"

// Human-readable machine name (derived from MACHINE_TYPE, used in OSD/About)
#if MACHINE_TYPE == 4
  #define MACHINE_NAME MACHINE_NAME_COCO3
#elif MACHINE_TYPE == 3
  #define MACHINE_NAME MACHINE_NAME_COCO2
#elif MACHINE_TYPE == 2
  #define MACHINE_NAME "CoCo 1"
#elif MACHINE_TYPE == 1
  #define MACHINE_NAME "Dragon 64"
#elif MACHINE_TYPE == 0
  #define MACHINE_NAME "Dragon 32"
#else
  #define MACHINE_NAME "Unknown"
#endif

// CPU variant: 0 = MC6809, 1 = HD6309
#define CPU_VARIANT             0

// RAM size in KB (CoCo3 = 512)
#define RAM_SIZE_KB             512

// Enable debug output on Serial
#define DEBUG_ENABLED           1

// Per-keystroke key probe ("[KBD] vk=.. ascii=.."). Verbose during normal
// typing — off by default; enable only to diagnose key-mapping issues.
#define KBD_DEBUG_KEYS          0

// Serial-port ownership at first boot, used when NVS has no stored value.
// Numeric to avoid depending on the SerialMode enum (declared in utils/debug.h):
//   0 = OFF, 1 = DEBUG, 2 = RS-232 Pak.  Default 1 preserves the dev banner.
#define SERIAL_MODE_FIRST_BOOT_DEFAULT  1

// Performance probe — esp_timer_get_time() based hot-path measurement.
#define PERF_PROBE_ENABLED      0

// Target frames per second (NTSC=60, PAL=50)
#define TARGET_FPS              60

// FPS overlay output (toggled at runtime by F5). Bitmask — combine with |.
//   FPS_OVERLAY_SERIAL — print "FPS: NN.N" to Serial once per second
//   FPS_OVERLAY_SCREEN — draw the FPS string in the top-left of the display
//   set FPS_OVERLAY_MODE to 0 to compile both paths out entirely
#define FPS_OVERLAY_SERIAL      0x01
#define FPS_OVERLAY_SCREEN      0x02
#define FPS_OVERLAY_MODE        (FPS_OVERLAY_SCREEN)

// ============================================================
// Display — LilyGo TTGO VGA32 v1.4: FabGL VGA 640x200 @ 70 Hz
// ============================================================

#define DISPLAY_WIDTH           640
#define DISPLAY_HEIGHT          200

// Phase 2 (DISABLED): rendering a true 640-wide scanline at 320 + pixel-double
// drops odd source columns, which makes 80-column text unreadable. The saving
// was marginal (~1 FPS), so this stays off. Set to 1 only if you never use
// 80-column text / hi-res text modes.
#define GIME_VGA_DOWNSCALE      0

// VGA resistor-ladder DAC pins (FabGL configures these in setup)
#define PIN_VGA_R0              21
#define PIN_VGA_R1              22
#define PIN_VGA_G0              18
#define PIN_VGA_G1              19
#define PIN_VGA_B0              4
#define PIN_VGA_B1              5
#define PIN_VGA_HSYNC           23
#define PIN_VGA_VSYNC           15

// ============================================================
// Audio — ESP32 internal DAC1 on GPIO25 via I2S built-in-DAC mode
// ============================================================

#define AUDIO_OUTPUT            0
#define AUDIO_SAMPLE_RATE       22050
#define AUDIO_BUFFER_SIZE       512
#define PIN_DAC_OUT             25

// ============================================================
// PS/2 keyboard
// ============================================================

#define PIN_PS2_KBD_CLK         33
#define PIN_PS2_KBD_DATA        32

// Keyboard layout at first boot, used when NVS has no stored value.
// Numeric to avoid depending on the KbdLayout enum (declared in hal/hal.h):
//   0 = US English, 1 = Spanish Latam
#define KBD_LAYOUT_FIRST_BOOT_DEFAULT   0

// PS/2 mouse (optional — used as CoCo joystick 1)
#define PIN_PS2_MOUSE_CLK       26
#define PIN_PS2_MOUSE_DATA      27

// ============================================================
// Joystick — PS/2 mouse → CoCo joystick 1 (right joystick, port 0)
// Joystick 2 (port 1) is a neutral stub.
// ============================================================

// Each raw mouse count is divided by this before adding to the axis accumulator.
// Lower = more sensitive (traverses 0..63 faster). 4 is a usable starting point.
#define JOYSTICK_MOUSE_SCALE    4

// Set to 1 if the Y axis feels inverted in-game.
#define JOYSTICK_MOUSE_INVERT_Y 0

// Pin sentinels — kept so any stray ADC reference compiles.
#define PIN_JOY0_X              -1
#define PIN_JOY0_Y              -1
#define PIN_JOY0_BTN            -1
#define PIN_JOY1_X              -1
#define PIN_JOY1_Y              -1
#define PIN_JOY1_BTN            -1

// ============================================================
// SD card — HSPI bus. NOTE: GPIO2 is a strapping pin — board
// must keep it floating/high at boot.
// ============================================================

#define PIN_SD_CS               13
#define PIN_SD_MOSI             12
#define PIN_SD_MISO             2
#define PIN_SD_SCLK             14

// ============================================================
// Storage
// ============================================================

#define STORAGE_TYPE            0

#define ROM_BASE_PATH           "/roms"
#define ROM_BASIC_FILE          "bas13.rom"
#define ROM_EXT_BASIC_FILE      "extbas11.rom"
#define ROM_DISK_FILE           "disk11.rom"

#define ROM_COCO3_FILE          "coco3.rom"
#define COCO3_ROM_SIZE          (32 * 1024)
#define COCO3_PHYSICAL_RAM      (512 * 1024)

// ============================================================
// Memory layout
// ============================================================

#define USE_PSRAM               1
#define COCO_RAM_SIZE           (RAM_SIZE_KB * 1024)
#define COCO_ROM_SIZE           (32 * 1024)
#define VRAM_SIZE               6144

// ============================================================
// Timing
// ============================================================

#define CPU_CLOCK_HZ            895000
#define CYCLES_PER_FRAME        (CPU_CLOCK_HZ / TARGET_FPS)
#define SCANLINES_PER_FRAME     262
#define ACTIVE_SCANLINES        192

#endif // CONFIG_H
