/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : hal.cpp
 *  Module : HAL top-level — initialization and per-frame dispatch to subsystem drivers (VGA32)
 * ============================================================
*/

/*
 * hal.cpp - Top-level HAL initialization and frame dispatch
 *
 * Calls into individual subsystem init/update functions.
 */

#include "hal.h"
#include "../utils/debug.h"
#include "hal_rs232.h"
#include "../core/mc6551.h"

// Serial-port ownership. Default reflects the first-boot default; overwritten
// from NVS early in setup() via serial_mode_apply(supervisor_load_serial_mode()).
volatile SerialPortMode g_serial_mode = (SerialPortMode)SERIAL_MODE_FIRST_BOOT_DEFAULT;

void serial_mode_apply(SerialPortMode mode) {
    g_serial_mode = mode;
    // The ACIA is reset on every transition so a stale link can't leak across
    // a mode change; buffered host input is dropped unless we're entering debug.
    mc6551_reset();
    if (mode != SERIAL_MODE_DEBUG) {
        hal_rs232_flush();
    }
}

void hal_init(void) {
    DEBUG_PRINT("HAL: initializing subsystems...");
    hal_storage_init();
    // NOTE: hal_video_init() is called AFTER ROM loading in setup()
    hal_audio_init();
    hal_keyboard_init();
    hal_joystick_init();
    hal_rs232_begin();   // wire MC6551 <-> UART0 callbacks
    DEBUG_PRINT("HAL: init complete (video deferred)");
}

void hal_process_input(void) {
    // Tick deferred key releases (ensures keys stay held for min frames)
    hal_keyboard_tick();
    // Update joystick ADC readings
    hal_joystick_update();
    // Drain UART0 into the RS-232 RX ring (only when the pak owns the port).
    if (g_serial_mode == SERIAL_MODE_RS232) {
        hal_rs232_poll();
    }
}

void hal_render_frame(void) {
    // Video present is called by machine_run_frame() after all scanlines
    // Nothing additional needed here
}
