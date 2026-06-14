/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   MIT License
 * ============================================================
 *  File   : ESP32_CoCo3_XRoar_Port.ino
 *  Module : Main Arduino sketch — setup/loop entry point for the CoCo 2/3 emulator
 * ============================================================
*/

#include "config.h"
#include "src/core/machine.h"
#include "src/hal/hal.h"
#include "src/hal/osd_canvas.h"
#include "src/supervisor/supervisor.h"
#include "src/utils/debug.h"

// Uncomment to enable integration tests (serial command 'R' to run)
// CoCo2: LOADM verify, VRAM dump    CoCo3: GIME, video, audio tests
//#define RUN_INTEGRATION_TESTS 1

#ifdef RUN_INTEGRATION_TESTS
#include "src/tests/integration_test.h"
#endif

Machine coco;

// hal_video.cpp exposes the OSD canvas used by the boot halt screen and supervisor.
extern OSDCanvas* hal_video_get_canvas(void);

// Paint a full-screen fatal message on the VGA display and never return.
// Used at boot when the SD card or required ROMs are missing.
static void boot_halt_screen(const char* const* lines, int n) {
    hal_video_init();
    OSDCanvas* canvas = hal_video_get_canvas();
    if (canvas) {
        canvas->startWrite();
        canvas->fillScreen(OSD_BLACK);
        canvas->setTextFont(2);        // 8x14, readable on 640x200
        canvas->setTextDatum(TL_DATUM);
        int y = 16;
        for (int i = 0; i < n; i++) {
            canvas->setTextColor(i == 0 ? OSD_RED : OSD_WHITE, OSD_BLACK);
            canvas->drawString(lines[i], 16, y);
            y += 16;
        }
        canvas->endWrite();
    }
    for (int i = 0; i < n; i++) DEBUG_PRINT(lines[i]);
    while (true) { delay(1000); }
}

void setup() {
    Serial.begin(115200);
    delay(500);

    // Apply the persisted serial-port mode BEFORE any debug output: on this
    // single-UART board the RS-232 Pak and the debug log share UART0, so if the
    // user left the pak enabled the boot banner must not corrupt the host link.
    serial_mode_apply(supervisor_load_serial_mode());

    DEBUG_PRINT("=================================");
    DEBUG_PRINT("CoCo_ESP32 - Starting up...");
    DEBUG_PRINTF("CPU freq: %d MHz", ESP.getCpuFreqMHz());
    DEBUG_PRINT("----- Memory Report -----");
    DEBUG_PRINTF("SRAM  total: %d bytes", ESP.getHeapSize());
    DEBUG_PRINTF("SRAM  free:  %d bytes", ESP.getFreeHeap());
    DEBUG_PRINTF("SRAM  used:  %d bytes", ESP.getHeapSize() - ESP.getFreeHeap());
    DEBUG_PRINTF("SRAM  min free ever: %d bytes", ESP.getMinFreeHeap());
    DEBUG_PRINTF("PSRAM total: %d bytes", ESP.getPsramSize());
    DEBUG_PRINTF("PSRAM free:  %d bytes", ESP.getFreePsram());
    DEBUG_PRINTF("PSRAM used:  %d bytes", ESP.getPsramSize() - ESP.getFreePsram());
    DEBUG_PRINTF("PSRAM min free ever: %d bytes", ESP.getMinFreePsram());
    DEBUG_PRINT("-------------------------");
    DEBUG_PRINT("=================================");

    // Seed the PS/2 keyboard layout from NVS BEFORE hal_init() so
    // hal_keyboard_init() applies the user's chosen layout.
    g_kbd_layout = supervisor_load_kbd_layout();

    // Load the user's custom key mappings (Key Mapper, supervisor Settings).
    supervisor_load_keymap();

    // Apply the saved mouse sensitivity / invert before the joystick HAL is used.
    supervisor_load_joystick();

    // Initialize HAL (storage, audio, keyboard, joystick — but NOT video yet)
    hal_init();

    // Seed the runtime machine type from NVS (falls back to MACHINE_TYPE).
    // Must happen before machine_init() so later steps can branch on it.
    g_machine_type = supervisor_load_machine_type();
    DEBUG_PRINTF("g_machine_type = %u (compile-time default %u)", g_machine_type, (uint8_t)MACHINE_TYPE);

    // Initialize emulated machine
    machine_init(&coco);

    // Load ROM images BEFORE video init
    machine_load_roms(&coco);

    // Validate the SD card and the required ROMs for the active machine. If
    // anything essential is missing, paint a message and halt instead of
    // booting into a broken emulator (garbage reset vector -> blank/crash).
    if (!hal_storage_is_ready()) {
        const char* msg[] = {
            "CoCo Emulator - Cannot Start",
            "",
            "SD card not found.",
            "Please check the SD card is inserted",
            "and properly connected, then reboot.",
            "",
            "Emulator halted.",
        };
        boot_halt_screen(msg, sizeof(msg) / sizeof(msg[0]));  // never returns
    }

    // Collect the required ROMs that failed to load for the active machine.
    // Disk BASIC (disk11.rom) is required alongside the BASIC ROM(s).
    const char* missing[3];
    int nmiss = 0;
    if (g_machine_type == 4) {                  // CoCo 3
        if (!coco.rom_coco3_loaded) missing[nmiss++] = ROM_COCO3_FILE;
        if (!coco.rom_disk_loaded)  missing[nmiss++] = ROM_DISK_FILE;
    } else {                                    // CoCo 2
        if (!coco.rom_basic_loaded)  missing[nmiss++] = ROM_BASIC_FILE;
        if (!coco.rom_extbas_loaded) missing[nmiss++] = ROM_EXT_BASIC_FILE;
        if (!coco.rom_cart_loaded)   missing[nmiss++] = ROM_DISK_FILE;
    }

    if (nmiss > 0) {
        char missline[64];
        int off = snprintf(missline, sizeof(missline), "Missing: ");
        for (int i = 0; i < nmiss && off < (int)sizeof(missline); i++) {
            off += snprintf(missline + off, sizeof(missline) - off, "%s%s",
                            missing[i], (i + 1 < nmiss) ? ", " : "");
        }
        const char* msg[] = {
            "CoCo Emulator - Cannot Start",
            "",
            "No valid Basic & Disk Color Basic ROMs.",
            missline,
            "",
            "Place ROMs in the /roms directory of the SD:",
            (g_machine_type == 4) ? "  coco3.rom, disk11.rom"
                                  : "  bas13.rom, extbas11.rom, disk11.rom",
            "",
            "Emulator halted.",
        };
        boot_halt_screen(msg, sizeof(msg) / sizeof(msg[0]));  // never returns
    }

    // Initialize VGA display
    hal_video_init();

    // Cold reset
    machine_reset(&coco);

    // Initialize supervisor (OSD menu, disk controller, NVS)
    supervisor_init(&coco);
    supervisor_load_state();  // Auto-mount last disks if enabled
    hal_keyboard_set_machine(&coco);

    DEBUG_PRINT("=== Post-Init Memory Report ===");
    DEBUG_PRINTF("SRAM  free:  %d bytes (used: %d)", ESP.getFreeHeap(), ESP.getHeapSize() - ESP.getFreeHeap());
    DEBUG_PRINTF("PSRAM free:  %d bytes (used: %d)", ESP.getFreePsram(), ESP.getPsramSize() - ESP.getFreePsram());
    DEBUG_PRINT("===============================");

    DEBUG_PRINT("Entering main loop...");

#ifdef RUN_INTEGRATION_TESTS
    Serial.println("\n*** LOADM Verify Test Ready ***");
    Serial.println("1. Mount disk with ZAXXON.BIN");
    Serial.println("2. Type LOADM\"ZAXXON\" on CoCo keyboard + ENTER");
    Serial.println("3. Wait for OK prompt, then send 'R' via serial to verify RAM");
    Serial.println("Commands: R=verify, S=report, D=VRAM hex, T=screen text");
#endif
}

#ifdef RUN_INTEGRATION_TESTS
  #if MACHINE_TYPE == 4
    static CoCo3IntegrationTest itest(&coco);
  #else
    static IntegrationTest itest(&coco);
  #endif
#endif

void loop() {
#ifdef RUN_INTEGRATION_TESTS
    // Check for serial test commands
    if (Serial.available()) {
        char c = Serial.read();
        if (c == 'R' || c == 'r' || c == 'S' || c == 's' ||
            c == 'D' || c == 'd' || c == 'T' || c == 't') {
            itest.process_serial_command(c);
        }
    }
#endif

    // Process host input (keyboard, joystick — includes F1 intercept)
    hal_process_input();

    // Check if supervisor is handling this frame
    if (supervisor_update_and_render()) {
        // Supervisor is active — emulation paused
        yield();
        return;
    }

    // Run one video frame worth of emulation
    machine_run_frame(&coco);

    // Push framebuffer to display
    hal_render_frame();

    // Sound frequency debug — detect end-of-sound and report
    hal_audio_debug_tick();
}
