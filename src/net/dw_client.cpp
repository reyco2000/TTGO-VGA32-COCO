/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : dw_client.cpp
 *  Module : External DriveWire back end — TCP client on core 0
 * ============================================================
 */

#include "dw_client.h"

#include <WiFi.h>

#include "wifi_mgr.h"
#include "../core/becker.h"
#include "../utils/debug.h"

#define DW_CONNECT_TIMEOUT_MS  3000
#define DW_BACKOFF_MIN_MS      1000
#define DW_BACKOFF_MAX_MS      8000
// Idle wait between polls of the socket. The CPU side also wakes us as soon
// as the CoCo writes, so this only bounds reply latency when the server
// sends on its own.
#define DW_POLL_MS             2

static TaskHandle_t           s_task  = nullptr;
static volatile DwClientState s_state = DW_CLIENT_IDLE;
static volatile uint32_t      s_connects = 0;
static String                 s_host;
static uint16_t               s_port = 0;

static void run_link(WiFiClient& c) {
    static uint8_t buf[512];
    while (c.connected()) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DW_POLL_MS));

        // CoCo → server
        size_t n;
        while ((n = becker_tx_pop(buf, sizeof(buf))) > 0) {
            if (c.write(buf, n) != n) return;
        }

        // server → CoCo. Only read what the RX ring can take; the rest stays
        // in the socket so TCP flow control applies.
        int avail;
        while ((avail = c.available()) > 0) {
            size_t space = becker_rx_space();
            if (space == 0) break;
            size_t want = (size_t)avail;
            if (want > space) want = space;
            if (want > sizeof(buf)) want = sizeof(buf);
            int got = c.read(buf, want);
            if (got <= 0) break;
            becker_rx_push(buf, (size_t)got);
        }

        if (WiFi.status() != WL_CONNECTED) return;
    }
}

static void client_task(void* arg) {
    (void)arg;
    uint32_t backoff = DW_BACKOFF_MIN_MS;
    for (;;) {
        if (wifi_mgr_state() != WIFI_MGR_STA_RUNNING) {
            s_state = DW_CLIENT_WAIT_WIFI;
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        s_state = DW_CLIENT_CONNECTING;
        WiFiClient c;
        if (c.connect(s_host.c_str(), s_port, DW_CONNECT_TIMEOUT_MS)) {
            c.setNoDelay(true);
            s_connects++;
            s_state = DW_CLIENT_CONNECTED;
            DEBUG_PRINTF("dw_client: connected to %s:%u", s_host.c_str(), s_port);
            becker_set_link_up(true);
            backoff = DW_BACKOFF_MIN_MS;

            run_link(c);

            becker_set_link_up(false);
            c.stop();
            DEBUG_PRINT("dw_client: link lost");
        } else {
            DEBUG_PRINTF("dw_client: connect to %s:%u failed", s_host.c_str(), s_port);
        }

        s_state = DW_CLIENT_BACKOFF;
        vTaskDelay(pdMS_TO_TICKS(backoff));
        backoff = (backoff * 2 > DW_BACKOFF_MAX_MS) ? DW_BACKOFF_MAX_MS : backoff * 2;
    }
}

void dw_client_begin(const String& host, uint16_t port) {
    if (s_task) return;
    s_host = host;
    s_port = port;
    s_state = DW_CLIENT_WAIT_WIFI;
    xTaskCreatePinnedToCore(client_task, "dw_client", 4096, nullptr, 1, &s_task, 0);
    becker_set_backend_task(s_task);
}

DwClientState dw_client_state(void) { return s_state; }

const char* dw_client_state_str(void) {
    switch (s_state) {
        case DW_CLIENT_IDLE:       return "Idle";
        case DW_CLIENT_WAIT_WIFI:  return "Waiting for WiFi";
        case DW_CLIENT_CONNECTING: return "Connecting";
        case DW_CLIENT_CONNECTED:  return "Connected";
        case DW_CLIENT_BACKOFF:    return "Link down (retrying)";
    }
    return "?";
}

uint32_t dw_client_connects(void) { return s_connects; }
