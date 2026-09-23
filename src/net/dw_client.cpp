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

// Graceful-close handshake for restarts: esp_restart() sends no FIN, and
// FujiNet-PC accepts only one connection, so it would sit on the dead
// socket (until TCP keepalive, ~2 h) and ignore our next boot.
static volatile bool          s_shutdown_req = false;
static volatile bool          s_shutdown_done = false;

static void run_link(WiFiClient& c) {
    static uint8_t buf[512];
    while (c.connected()) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(DW_POLL_MS));

        // CoCo was reset: start a fresh session (see becker_reset_requested).
        if (becker_reset_requested()) return;
        if (s_shutdown_req) return;

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
        if (s_shutdown_req) {
            s_state = DW_CLIENT_IDLE;
            s_shutdown_done = true;
            vTaskDelay(portMAX_DELAY);   // device is about to restart
        }
        if (wifi_mgr_state() != WIFI_MGR_STA_RUNNING) {
            s_state = DW_CLIENT_WAIT_WIFI;
            vTaskDelay(pdMS_TO_TICKS(250));
            continue;
        }

        s_state = DW_CLIENT_CONNECTING;
        WiFiClient c;
        if (c.connect(s_host.c_str(), s_port, DW_CONNECT_TIMEOUT_MS)) {
            c.setNoDelay(true);
            // A reset before the link came up needs no reconnect — just drop
            // the pre-reset bytes.
            becker_clear_reset();
            s_connects++;
            s_state = DW_CLIENT_CONNECTED;
            DEBUG_PRINTF("dw_client: connected to %s:%u", s_host.c_str(), s_port);
            becker_set_link_up(true);
            backoff = DW_BACKOFF_MIN_MS;

            run_link(c);

            bool was_reset = becker_reset_requested();
            c.stop();   // sends FIN — the server frees its single slot
            if (s_shutdown_req) continue;
            becker_set_link_up(false);
            if (was_reset) {
                DEBUG_PRINT("dw_client: CoCo reset — reconnecting");
                continue;   // reconnect at once, no backoff
            }
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

void dw_client_shutdown(uint32_t timeout_ms) {
    if (!s_task) return;
    s_shutdown_req = true;
    xTaskNotifyGive(s_task);
    uint32_t t0 = millis();
    while (!s_shutdown_done && millis() - t0 < timeout_ms) delay(5);
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
