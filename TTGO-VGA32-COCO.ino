/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : ESP32_CoCo3_XRoar_Port.ino
 *  Module : Main Arduino sketch — setup/loop entry point for the CoCo 2/3 emulator
 * ============================================================
*/

#include "config.h"
#include "esp_timer.h"
#include "src/core/machine.h"
#include "src/core/speed_meter.h"
#include "src/hal/hal.h"
#include "src/hal/osd_canvas.h"
#include "src/supervisor/supervisor.h"
#include "src/utils/debug.h"
#include "src/net/debug_rpc.h"
#include "src/net/wifi_mgr.h"
#include "src/net/debug_server.h"
#include "src/net/dw_bus.h"
#include "src/utils/perf_bench.h"

// Uncomment to enable integration tests (serial command 'R' to run)
// CoCo2: LOADM verify, VRAM dump    CoCo3: GIME, video, audio tests
//#define RUN_INTEGRATION_TESTS 1

#ifdef RUN_INTEGRATION_TESTS
#include "src/tests/integration_test.h"
#endif

// Boot halt screen types. Declared ahead of every function in this file:
// arduino-cli inserts its generated prototypes before the first function, and
// in the bootloader build that is bootloader_release_otadata() below, so types
// declared later would not exist yet for boot_halt_screen()'s prototype.

// One centred text line of a boot halt screen.
struct HaltLine {
    const char* text;
    uint8_t     y;
    uint16_t    color;
};

enum HaltIcon : uint8_t { HALT_ICON_SD, HALT_ICON_CHIP };

#if BUILD_TARGET == BUILD_TARGET_BOOTLOADER
#include "esp_partition.h"

// Hand the device back to ESP32_Bootloader on the next power-up.
//
// The bootloader flashes this app into `ota_0` and points `otadata` at it.
// Left alone, the ROM would boot straight here every time and the SD-card menu
// would be unreachable. Blanking `otadata` makes the ROM fall back to the
// `factory` partition — the bootloader — while this run continues normally.
//
// Must be the FIRST thing setup() does: anything that can hang or halt (SD
// probe, missing-ROM screen) would otherwise strand the user in this app.
static void bootloader_release_otadata(void) {
    const esp_partition_t* otadata = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_OTA, NULL);
    if (!otadata) {
        return;   // standalone layout (huge_app) has no otadata — nothing to do
    }
    esp_partition_erase_range(otadata, 0, otadata->size);
}
#endif

Machine coco;

// hal_video.cpp exposes the OSD canvas used by the boot halt screen and supervisor.
extern OSDCanvas* hal_video_get_canvas(void);

// Paint a full-screen fatal message on the VGA display and never return.
// Used at boot when the SD card or required ROMs are missing. Everything is
// centred: a red warning triangle and an SD card or chip icon above the text.
// 640x200 pixels are ~2.4x taller than wide, so the icons are stretched
// horizontally to look right.
static void boot_halt_screen(HaltIcon icon, const HaltLine* lines, int n) {
    hal_video_init();
    OSDCanvas* canvas = hal_video_get_canvas();
    if (canvas) {
        const int ICON_H = 44, TRI_W = 104, GAP = 40;
        const int ICON_W = (icon == HALT_ICON_SD) ? 72 : 96;
        const int cx = DISPLAY_WIDTH / 2;
        const int tx = cx - (TRI_W + GAP + ICON_W) / 2;     // triangle left edge
        const int ix = tx + TRI_W + GAP;                    // icon left edge
        const int y0 = 14;

        canvas->startWrite();
        canvas->fillScreen(OSD_BLACK);
        canvas->setTextFont(2);        // 8x14, readable on 640x200
        canvas->setTextDatum(TC_DATUM);

        // Warning triangle with a white exclamation mark
        for (int r = 0; r < ICON_H; r++) {
            int half = 2 + r * (TRI_W / 2 - 2) / (ICON_H - 1);
            canvas->fillRect(tx + TRI_W / 2 - half, y0 + r, half * 2, 1, OSD_RED);
        }
        canvas->fillRect(tx + TRI_W / 2 - 5, y0 + 13, 10, 17, OSD_WHITE);
        canvas->fillRect(tx + TRI_W / 2 - 5, y0 + 34, 10, 5, OSD_WHITE);

        if (icon == HALT_ICON_SD) {
            // SD card: body with a cut corner, gold contacts, label
            canvas->fillRect(ix, y0, ICON_W, ICON_H, OSD_BLUE);
            for (int r = 0; r < 10; r++) {
                int cut = (10 - r) * 24 / 10;
                canvas->fillRect(ix + ICON_W - cut, y0 + r, cut, 1, OSD_BLACK);
            }
            for (int c = 0; c < 5; c++) {
                canvas->fillRect(ix + 6 + c * 9, y0 + 4, 5, 11, OSD_YELLOW);
            }
            canvas->fillRect(ix + 8, y0 + 22, ICON_W - 16, 16, OSD_WHITE);
            canvas->setTextColor(OSD_BLUE, OSD_WHITE);
            canvas->drawString("SD", ix + ICON_W / 2, y0 + 23);
        } else {
            // ROM chip: pins top and bottom, body with a notch, label
            for (int p = 0; p < 8; p++) {
                canvas->fillRect(ix + 9 + p * 10, y0, 6, ICON_H, 0xAD55);
            }
            canvas->fillRect(ix, y0 + 8, ICON_W, ICON_H - 16, 0x52AA);
            canvas->fillEllipse(ix + 2, y0 + ICON_H / 2, 16, 8, OSD_BLACK);
            canvas->setTextColor(OSD_WHITE, 0x52AA);
            canvas->drawString("ROM", ix + ICON_W / 2, y0 + 15);
        }

        for (int i = 0; i < n; i++) {
            canvas->setTextColor(lines[i].color, OSD_BLACK);
            canvas->drawString(lines[i].text, cx, lines[i].y);
        }
        canvas->setTextDatum(TL_DATUM);
        canvas->endWrite();
    }
    for (int i = 0; i < n; i++) DEBUG_PRINT(lines[i].text);
    while (true) { delay(1000); }
}

#define HALT_DOC_URL "https://github.com/reyco2000/TTGO-VGA32-COCO"

// Emulator + OSD task stack. Measured peak 3,348 B (2026-09-27: full OSD tour,
// disk mounts from OSD and API, RAM dump to SD, screenshots, emulation); the
// default 8 KB wasted ~2.5 KB of internal RAM. See loop_stack_free in /api/status.
SET_LOOP_TASK_STACK_SIZE(5632);

void setup() {
#if BUILD_TARGET == BUILD_TARGET_BOOTLOADER
    bootloader_release_otadata();   // before Serial/video/SD — see note above
#endif

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
    g_cpu_variant  = supervisor_load_cpu_variant();
    g_coco3_ram_kb = supervisor_load_coco3_ram();
    tcc1014_set_artifacts(supervisor_load_artifacts());

    // DriveWire bus mode (Becker port). Read before ROM loading so later
    // phases can pick the HDB-DOS Becker ROM instead of disk11.rom.
    dw_bus_load_config();

    // Orchestra-90 setting. After the DriveWire config so its cartridge
    // ROM replaces the HDB-DOS ROM.
    supervisor_load_cart_config();
    DEBUG_PRINTF("g_machine_type = %u (compile-time default %u)", g_machine_type, (uint8_t)MACHINE_TYPE);
    DEBUG_PRINTF("CPU: %s", g_cpu_variant == CPU_VARIANT_HD6309 ? "HD6309" : "MC6809");
    DEBUG_PRINTF("CoCo 3 RAM: %u KB", (unsigned)g_coco3_ram_kb);

    // Initialize emulated machine
    machine_init(&coco);

    // Load ROM images BEFORE video init
    machine_load_roms(&coco);

    // Validate the SD card and the required ROMs for the active machine. If
    // anything essential is missing, paint a message and halt instead of
    // booting into a broken emulator (garbage reset vector -> blank/crash).
    if (!hal_storage_is_ready()) {
        static const HaltLine msg[] = {
            { "SD card not found or it has an incorrect format.", 70,  OSD_RED    },
            { "Please insert an SD card formatted \"FAT\" (FAT32)", 94,  OSD_WHITE  },
            { "and restart the emulator.",                        110, OSD_WHITE  },
            { "See the documentation at",                         134, OSD_WHITE  },
            { HALT_DOC_URL,                                       150, OSD_YELLOW },
            { "Emulator halted.",                                 178, OSD_CYAN   },
        };
        boot_halt_screen(HALT_ICON_SD, msg, sizeof(msg) / sizeof(msg[0]));  // never returns
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
        const HaltLine msg[] = {
            { "Required ROM files are missing.",                      68,  OSD_RED    },
            { missline,                                               86,  OSD_YELLOW },
            { "Copy them into the /roms folder on the SD card",       108, OSD_WHITE  },
            { "and restart the emulator.",                            124, OSD_WHITE  },
            { (g_machine_type == 4) ? "CoCo 3 needs: coco3.rom, disk11.rom"
                                    : "CoCo 2 needs: bas13.rom, extbas11.rom, disk11.rom",
                                                                      144, OSD_WHITE  },
            { HALT_DOC_URL,                                           164, OSD_YELLOW },
            { "Emulator halted.",                                     184, OSD_CYAN   },
        };
        boot_halt_screen(HALT_ICON_CHIP, msg, sizeof(msg) / sizeof(msg[0]));  // never returns
    }

    // Initialize VGA display
    hal_video_init();

    // Cold reset
    machine_reset(&coco);

    // Initialize supervisor (OSD menu, disk controller, NVS)
    supervisor_init(&coco);
    supervisor_load_state();  // Auto-mount last disks if enabled
    hal_keyboard_set_machine(&coco);

    // WiFi debug server (core 0). The cross-core RPC bridge must be bound to the
    // active machine before the server task can service any command. If the user
    // enabled auto-connect, kick off a non-blocking STA connect with the saved
    // credentials; otherwise the radio stays off until the supervisor starts it.
    debug_rpc_init(&coco);
    wifi_mgr_init();
    if (wifi_mgr_autoconnect()) {
        wifi_mgr_connect_saved();
    }
    debug_server_begin();  // creates the core-0 WebServer task (idle until WiFi up)

    // Becker port + DriveWire back end (no-op when the bus mode is Off).
    dw_bus_begin(&coco);

    DEBUG_PRINT("=== Post-Init Memory Report ===");
    DEBUG_PRINTF("SRAM  free:  %d bytes (used: %d)", ESP.getFreeHeap(), ESP.getHeapSize() - ESP.getFreeHeap());
    DEBUG_PRINTF("PSRAM free:  %d bytes (used: %d)", ESP.getFreePsram(), ESP.getPsramSize() - ESP.getFreePsram());
    DEBUG_PRINT("===============================");

    perf_bench_run();      // -DPERF_BENCH=1 only: CPU interpreter micro-benchmark

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

// Hold emulation to real time: wait until the next frame slot. When more
// than a frame behind (slow modes, SD access, debugger pause), resync
// instead of running a catch-up burst.
static void pace_frame(void) {
#if FRAME_LIMIT_ENABLED
    static int64_t next_us = 0;
    const int64_t period = 1000000 / TARGET_FPS;
    int64_t now = esp_timer_get_time();
    if (next_us == 0 || now - next_us > period) {
        next_us = now + period;
        return;
    }
    while (next_us - now > 1500) {      // coarse: sleep in 1 ms ticks
        vTaskDelay(1);
        now = esp_timer_get_time();
    }
    while (esp_timer_get_time() < next_us) { }   // fine: spin the last ms
    next_us += period;
#endif
}

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
    debug_rpc_keys_poll();   // OSD keys injected over the debug API
    wifi_mgr_tick();         // advance CONNECTING (no longer tied to the server task)

    // Check if supervisor is handling this frame
    static bool osd_was_active = false;
    if (supervisor_is_active()) hal_video_standard_layout();   // OSD draws for 640x200
    if (supervisor_update_and_render()) {
        // Supervisor is active — emulation paused
        osd_was_active = true;
        speed_meter_gap();
        yield();
        return;
    }
    if (osd_was_active) {
        // The OSD painted over the emulator picture: redraw every line,
        // whichever way the menu was left.
        osd_was_active = false;
        hal_video_force_repaint();
    }

    // Service one pending debug-server command (core 0 -> core 1) between frames.
    debug_rpc_poll();

    // If a remote debugger paused the CPU, freeze emulation but keep draining
    // RPC commands so inspect/resume/screenshot still respond promptly.
    if (debug_rpc_is_paused()) {
        speed_meter_gap();
        delay(2);
        return;
    }

    // Run one video frame worth of emulation
    int64_t frame_t0 = esp_timer_get_time();
    machine_run_frame(&coco);
    uint32_t frame_work_us = (uint32_t)(esp_timer_get_time() - frame_t0);
    pace_frame();

    // Push framebuffer to display
    hal_render_frame();
    speed_meter_frame(&coco, frame_work_us);

    // Sound frequency debug — detect end-of-sound and report
    hal_audio_debug_tick();
}
