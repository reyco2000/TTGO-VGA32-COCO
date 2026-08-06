/*
 * ============================================================
 *        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
 *   (C) 2026 Reinaldo Torres / CoCo Byte Club
 *   https://github.com/reyco2000/TTGO-VGA32-COCO
 *   Based on XRoar , co-developed with Claude Code
 *   GPL-3.0-or-later License
 * ============================================================
 *  File   : debug_server.cpp
 *  Module : WiFi debug server (core-0 WebServer task)
 * ============================================================
 *
 * Input convention: POST endpoints take form/query parameters (parsed by
 * WebServer::arg), NOT raw JSON bodies — robust and dependency-free. Numbers
 * accept decimal or 0x-hex. Byte payloads are lowercase hex strings.
 * Responses are hand-built JSON (no ArduinoJson dependency).
 */

#include "debug_server.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "debug_rpc.h"
#include "wifi_mgr.h"
#include "png_writer.h"
#include "../hal/hal.h"             // hal_video_capture_*
#include "../core/machine.h"        // g_machine_type, machine types
#include "../supervisor/supervisor.h" // supervisor_set_machine_type
#include "../utils/debug.h"
#include "../../config.h"           // MACHINE_NAME_COCO2/3, FW version

#ifndef FIRMWARE_VERSION   // normally provided by config.h
#define FIRMWARE_VERSION "0.8"
#endif

// Cap per-request memory transfer to bound temp buffers.
#define DBG_MAX_XFER 4096
// Default per-command core-1 RPC timeout.
#define DBG_RPC_TIMEOUT_MS 1000

static WebServer  s_server(80);
static TaskHandle_t s_task   = nullptr;
static bool       s_begun    = false;
static volatile bool s_enabled = true;

// --- small parse helpers ---

static uint32_t arg_u32(const String& name, uint32_t def) {
    if (!s_server.hasArg(name)) return def;
    return (uint32_t)strtoul(s_server.arg(name).c_str(), nullptr, 0);
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Decode a hex string into out (max outcap). Returns byte count, or -1 on error.
static int hex_decode(const String& s, uint8_t* out, size_t outcap) {
    size_t n = s.length();
    if (n % 2 != 0) return -1;
    size_t bytes = n / 2;
    if (bytes > outcap) return -1;
    for (size_t i = 0; i < bytes; i++) {
        int hi = hex_nibble(s[2 * i]);
        int lo = hex_nibble(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return -1;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return (int)bytes;
}

static String hex_encode(const uint8_t* in, size_t len) {
    static const char* H = "0123456789abcdef";
    String s;
    s.reserve(len * 2);
    for (size_t i = 0; i < len; i++) {
        s += H[in[i] >> 4];
        s += H[in[i] & 0x0F];
    }
    return s;
}

static const char* machine_name(uint8_t t) {
    return (t == 4) ? MACHINE_NAME_COCO3 : MACHINE_NAME_COCO2;
}

static void send_json(int code, const String& body) {
    s_server.send(code, "application/json", body);
}

static void send_err(int code, const char* msg) {
    send_json(code, String("{\"error\":\"") + msg + "\"}");
}

// Map an RPC submit + result into an HTTP error response; returns false if it
// already sent an error (caller should return).
static bool rpc_ok(DebugCmd& c) {
    if (!debug_rpc_submit(&c, DBG_RPC_TIMEOUT_MS)) { send_err(504, "rpc timeout"); return false; }
    if (c.result != DBG_OK) {
        const char* m = (c.result == DBG_ERR_NOMACHINE) ? "no machine"
                      : (c.result == DBG_ERR_RANGE)      ? "range"
                      : (c.result == DBG_ERR_NOSPACE)    ? "no phys ram"
                                                         : "rpc error";
        send_err(400, m);
        return false;
    }
    return true;
}

// =============================================================
//  Debug API handlers (STA mode)
// =============================================================

static void h_status() {
    String j = "{";
    j += "\"machine_type\":" + String(g_machine_type);
    j += ",\"machine\":\"" + String(machine_name(g_machine_type)) + "\"";
    j += ",\"paused\":" + String(debug_rpc_is_paused() ? "true" : "false");
    j += ",\"firmware\":\"" FIRMWARE_VERSION "\"";
    j += ",\"api\":" + String(DEBUG_API_VERSION);
    j += "}";
    send_json(200, j);
}

static void h_pause()  { debug_rpc_set_paused(true);  send_json(200, "{\"paused\":true}"); }
static void h_resume() { debug_rpc_set_paused(false); send_json(200, "{\"paused\":false}"); }

static void h_get_registers() {
    DebugCmd c = {};
    c.type = DBG_CMD_READ_REGS;
    if (!rpc_ok(c)) return;

    uint8_t a = (uint8_t)(c.d >> 8), b = (uint8_t)(c.d & 0xFF);
    String j = "{";
    j += "\"a\":" + String(a) + ",\"b\":" + String(b) + ",\"d\":" + String(c.d);
    j += ",\"x\":" + String(c.x) + ",\"y\":" + String(c.y);
    j += ",\"u\":" + String(c.u) + ",\"s\":" + String(c.s);
    j += ",\"pc\":" + String(c.pc) + ",\"dp\":" + String(c.dp) + ",\"cc\":" + String(c.cc);
    // Decode CC flags: E F H I N Z V C (bit 7..0)
    j += ",\"flags\":{";
    j += "\"E\":" + String((c.cc & 0x80) ? 1 : 0);
    j += ",\"F\":" + String((c.cc & 0x40) ? 1 : 0);
    j += ",\"H\":" + String((c.cc & 0x20) ? 1 : 0);
    j += ",\"I\":" + String((c.cc & 0x10) ? 1 : 0);
    j += ",\"N\":" + String((c.cc & 0x08) ? 1 : 0);
    j += ",\"Z\":" + String((c.cc & 0x04) ? 1 : 0);
    j += ",\"V\":" + String((c.cc & 0x02) ? 1 : 0);
    j += ",\"C\":" + String((c.cc & 0x01) ? 1 : 0);
    j += "}";
    j += ",\"cycles\":" + String(c.cycles);
    j += "}";
    send_json(200, j);
}

static void h_post_registers() {
    // Read current state first so we only change provided fields.
    DebugCmd c = {};
    c.type = DBG_CMD_READ_REGS;
    if (!rpc_ok(c)) return;

    uint16_t mask = 0;
    if (s_server.hasArg("pc")) { c.pc = (uint16_t)arg_u32("pc", c.pc); mask |= DBG_REG_PC; }
    if (s_server.hasArg("d"))  { c.d  = (uint16_t)arg_u32("d",  c.d);  mask |= DBG_REG_D; }
    if (s_server.hasArg("a"))  { c.d  = (uint16_t)((arg_u32("a", c.d >> 8) << 8) | (c.d & 0xFF)); mask |= DBG_REG_D; }
    if (s_server.hasArg("b"))  { c.d  = (uint16_t)((c.d & 0xFF00) | (arg_u32("b", c.d & 0xFF) & 0xFF)); mask |= DBG_REG_D; }
    if (s_server.hasArg("x"))  { c.x  = (uint16_t)arg_u32("x",  c.x);  mask |= DBG_REG_X; }
    if (s_server.hasArg("y"))  { c.y  = (uint16_t)arg_u32("y",  c.y);  mask |= DBG_REG_Y; }
    if (s_server.hasArg("u"))  { c.u  = (uint16_t)arg_u32("u",  c.u);  mask |= DBG_REG_U; }
    if (s_server.hasArg("s"))  { c.s  = (uint16_t)arg_u32("s",  c.s);  mask |= DBG_REG_S; }
    if (s_server.hasArg("dp")) { c.dp = (uint8_t) arg_u32("dp", c.dp); mask |= DBG_REG_DP; }
    if (s_server.hasArg("cc")) { c.cc = (uint8_t) arg_u32("cc", c.cc); mask |= DBG_REG_CC; }

    if (mask == 0) { send_err(400, "no registers given"); return; }

    DebugCmd w = c;
    w.type = DBG_CMD_WRITE_REGS;
    w.reg_mask = mask;
    if (!rpc_ok(w)) return;
    h_get_registers();
}

static uint8_t space_arg() {
    return (s_server.arg("space") == "phys") ? DBG_SPACE_PHYS : DBG_SPACE_CPU;
}

static void h_get_mem() {
    uint32_t addr = arg_u32("addr", 0);
    uint32_t len  = arg_u32("len", 1);
    uint8_t  space = space_arg();
    if (len == 0 || len > DBG_MAX_XFER) { send_err(400, "len out of range (1..4096)"); return; }

    uint8_t* buf = (uint8_t*)malloc(len);
    if (!buf) { send_err(500, "oom"); return; }

    DebugCmd c = {};
    c.type = DBG_CMD_READ_MEM;
    c.addr = addr; c.len = len; c.buf = buf; c.space = space;
    if (!rpc_ok(c)) { free(buf); return; }

    String j = "{";
    j += "\"addr\":" + String(addr);
    j += ",\"len\":" + String(len);
    j += ",\"space\":\"" + String(space == DBG_SPACE_PHYS ? "phys" : "cpu") + "\"";
    j += ",\"data\":\"" + hex_encode(buf, len) + "\"";
    j += "}";
    free(buf);
    send_json(200, j);
}

static void h_post_mem() {
    uint32_t addr = arg_u32("addr", 0);
    uint8_t  space = space_arg();
    String   data = s_server.arg("data");
    if (data.length() == 0) { send_err(400, "missing data (hex)"); return; }

    uint8_t* buf = (uint8_t*)malloc(DBG_MAX_XFER);
    if (!buf) { send_err(500, "oom"); return; }
    int n = hex_decode(data, buf, DBG_MAX_XFER);
    if (n < 0) { free(buf); send_err(400, "bad hex / too long"); return; }

    DebugCmd c = {};
    c.type = DBG_CMD_WRITE_MEM;
    c.addr = addr; c.len = (uint32_t)n; c.buf = buf; c.space = space;
    if (!rpc_ok(c)) { free(buf); return; }
    free(buf);
    send_json(200, String("{\"addr\":") + addr + ",\"written\":" + n + "}");
}

static void h_inject() {
    uint32_t addr = arg_u32("addr", 0);
    String   data = s_server.arg("data");
    if (data.length() == 0) { send_err(400, "missing data (hex)"); return; }

    uint8_t* buf = (uint8_t*)malloc(DBG_MAX_XFER);
    if (!buf) { send_err(500, "oom"); return; }
    int n = hex_decode(data, buf, DBG_MAX_XFER);
    if (n < 0) { free(buf); send_err(400, "bad hex / too long"); return; }

    DebugCmd c = {};
    c.type = DBG_CMD_INJECT;
    c.addr = addr; c.len = (uint32_t)n; c.buf = buf; c.space = DBG_SPACE_CPU;
    if (s_server.hasArg("pc")) { c.set_pc = true; c.pc = (uint16_t)arg_u32("pc", 0); }
    c.resume_after = (s_server.arg("resume") == "1" || s_server.arg("resume") == "true");
    if (!rpc_ok(c)) { free(buf); return; }
    free(buf);

    String j = String("{\"addr\":") + addr + ",\"written\":" + n;
    j += ",\"pc_set\":" + String(c.set_pc ? "true" : "false");
    j += ",\"resumed\":" + String(c.resume_after ? "true" : "false") + "}";
    send_json(200, j);
}

static void h_reset() {
    DebugCmd c = {};
    c.type = DBG_CMD_RESET;
    if (!rpc_ok(c)) return;
    send_json(200, "{\"reset\":true}");
}

static void h_get_machine() {
    String j = String("{\"machine_type\":") + g_machine_type +
               ",\"machine\":\"" + machine_name(g_machine_type) + "\"}";
    send_json(200, j);
}

static void h_post_machine() {
    if (!s_server.hasArg("type")) { send_err(400, "missing type (3=CoCo2,4=CoCo3)"); return; }
    uint8_t t = (uint8_t)arg_u32("type", g_machine_type);
    if (t != 3 && t != 4) { send_err(400, "type must be 3 or 4"); return; }
    if (t == g_machine_type) { h_get_machine(); return; }

    // Respond BEFORE switching: supervisor_set_machine_type() reboots and
    // never returns. Freeze core 1 first to avoid touching emulator state
    // mid-frame during the disk-cache flush + state save.
    send_json(200, String("{\"rebooting\":true,\"machine_type\":") + t + "}");
    delay(200);
    debug_rpc_set_paused(true);
    delay(50);
    supervisor_set_machine_type(t);   // flushes caches, saves state, esp_restart()
}

// Read-only dump of the "sv" NVS namespace via the ESP-IDF iterator.
static void append_nvs_value(String& j, nvs_handle_t h, const nvs_entry_info_t& info) {
    switch (info.type) {
        case NVS_TYPE_U8:  { uint8_t  v = 0; nvs_get_u8 (h, info.key, &v); j += String(v); break; }
        case NVS_TYPE_I8:  { int8_t   v = 0; nvs_get_i8 (h, info.key, &v); j += String(v); break; }
        case NVS_TYPE_U16: { uint16_t v = 0; nvs_get_u16(h, info.key, &v); j += String(v); break; }
        case NVS_TYPE_I16: { int16_t  v = 0; nvs_get_i16(h, info.key, &v); j += String(v); break; }
        case NVS_TYPE_U32: { uint32_t v = 0; nvs_get_u32(h, info.key, &v); j += String(v); break; }
        case NVS_TYPE_I32: { int32_t  v = 0; nvs_get_i32(h, info.key, &v); j += String(v); break; }
        case NVS_TYPE_STR: {
            size_t len = 0;
            if (nvs_get_str(h, info.key, nullptr, &len) == ESP_OK && len > 0 && len < 512) {
                char* buf = (char*)malloc(len);
                if (buf && nvs_get_str(h, info.key, buf, &len) == ESP_OK) {
                    j += "\""; j += buf; j += "\"";
                } else { j += "null"; }
                free(buf);
            } else { j += "\"\""; }
            break;
        }
        default: j += "null"; break;
    }
}

static void h_nvram() {
    String j = "{";
    bool first = true;
    nvs_handle_t h = 0;
    bool opened = (nvs_open("sv", NVS_READONLY, &h) == ESP_OK);

    nvs_iterator_t it = nvs_entry_find("nvs", "sv", NVS_TYPE_ANY);
    while (it != nullptr) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (!first) j += ",";
        first = false;
        j += "\""; j += info.key; j += "\":";
        if (opened) append_nvs_value(j, h, info);
        else        j += "null";
        it = nvs_entry_next(it);
    }
    if (opened) nvs_close(h);
    j += "}";
    send_json(200, j);
}

// Screenshot: arm capture, advance exactly one frame on core 1, encode PNG.
// Note: this advances the emulation by one frame (documented caveat).
static void h_screenshot() {
    bool was_paused = debug_rpc_is_paused();

    hal_video_capture_arm();

    DebugCmd c = {};
    c.type = DBG_CMD_STEP_FRAME;
    if (!debug_rpc_submit(&c, 2000)) { send_err(504, "frame step timeout"); return; }

    // Restore the prior pause state (STEP_FRAME does not change it).
    debug_rpc_set_paused(was_paused);

    int w = 0, h = 0;
    const uint16_t* frame = hal_video_capture_frame(&w, &h);
    if (!frame || w <= 0 || h <= 0) { send_err(500, "capture failed"); return; }

    size_t png_len = 0;
    uint8_t* png = png_encode_rgb565(frame, w, h, HAL_CAPTURE_STRIDE, &png_len);
    if (!png) { send_err(500, "png encode failed"); return; }

    s_server.setContentLength(png_len);
    s_server.send(200, "image/png", "");
    s_server.sendContent((const char*)png, png_len);
    free(png);
}

// =============================================================
//  Config portal handlers (AP mode)
// =============================================================

static void h_portal_root() {
    String html =
        "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>CoCo3 WiFi Setup</title>"
        "<style>body{font-family:sans-serif;margin:1.5em;max-width:480px}"
        "input,select,button{font-size:1em;padding:.5em;margin:.3em 0;width:100%;box-sizing:border-box}"
        "</style></head><body><h2>CoCo3 Debug — WiFi Setup</h2>"
        "<button onclick='scan()'>Scan networks</button>"
        "<select id=ssid></select>"
        "<input id=pass type=password placeholder='Password'>"
        "<button onclick='save()'>Connect</button>"
        "<p id=msg></p>"
        "<script>"
        "function scan(){msg.textContent='Scanning...';fetch('/scan').then(r=>r.json()).then(d=>{"
        "ssid.innerHTML='';d.networks.forEach(n=>{var o=document.createElement('option');"
        "o.value=n.ssid;o.textContent=n.ssid+' ('+n.rssi+'dBm)'+(n.secure?' 🔒':'');ssid.appendChild(o);});"
        "msg.textContent=d.networks.length+' found';});}"
        "function save(){msg.textContent='Connecting...';"
        "fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
        "body:'ssid='+encodeURIComponent(ssid.value)+'&pass='+encodeURIComponent(pass.value)})"
        ".then(()=>poll());}"
        "function poll(){fetch('/status').then(r=>r.json()).then(d=>{"
        "msg.textContent=d.state+(d.ip!='0.0.0.0'?(' — '+d.ip):'');"
        "if(d.state!='Connected'&&d.state!='Failed')setTimeout(poll,1500);});}"
        "</script></body></html>";
    s_server.send(200, "text/html", html);
}

static void h_portal_scan() {
    int n = wifi_mgr_scan();
    String j = "{\"networks\":[";
    for (int i = 0; i < n; i++) {
        if (i) j += ",";
        String ssid = wifi_mgr_scan_ssid(i);
        ssid.replace("\\", "\\\\");
        ssid.replace("\"", "\\\"");
        j += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + wifi_mgr_scan_rssi(i) +
             ",\"secure\":" + (wifi_mgr_scan_secure(i) ? "true" : "false") + "}";
    }
    j += "]}";
    send_json(200, j);
}

static void h_portal_save() {
    String ssid = s_server.arg("ssid");
    String pass = s_server.arg("pass");
    if (ssid.length() == 0) { send_err(400, "missing ssid"); return; }
    send_json(200, "{\"ok\":true}");
    wifi_mgr_connect(ssid.c_str(), pass.c_str());
}

static void h_portal_status() {
    String j = String("{\"state\":\"") + wifi_mgr_state_str() +
               "\",\"ip\":\"" + wifi_mgr_ip() + "\"}";
    send_json(200, j);
}

// =============================================================
//  Routing / task
// =============================================================

static void register_routes() {
    // Debug API (STA)
    s_server.on("/api/status",        HTTP_GET,  h_status);
    s_server.on("/api/pause",         HTTP_POST, h_pause);
    s_server.on("/api/resume",        HTTP_POST, h_resume);
    s_server.on("/api/registers",     HTTP_GET,  h_get_registers);
    s_server.on("/api/registers",     HTTP_POST, h_post_registers);
    s_server.on("/api/mem",           HTTP_GET,  h_get_mem);
    s_server.on("/api/mem",           HTTP_POST, h_post_mem);
    s_server.on("/api/inject",        HTTP_POST, h_inject);
    s_server.on("/api/reset",         HTTP_POST, h_reset);
    s_server.on("/api/machine",       HTTP_GET,  h_get_machine);
    s_server.on("/api/machine",       HTTP_POST, h_post_machine);
    s_server.on("/api/nvram",         HTTP_GET,  h_nvram);
    s_server.on("/api/screenshot.png",HTTP_GET,  h_screenshot);

    // Config portal (AP)
    s_server.on("/",        HTTP_GET,  h_portal_root);
    s_server.on("/scan",    HTTP_GET,  h_portal_scan);
    s_server.on("/save",    HTTP_POST, h_portal_save);
    s_server.on("/status",  HTTP_GET,  h_portal_status);

    s_server.onNotFound([]() { send_err(404, "not found"); });
}

static void server_task(void* arg) {
    (void)arg;
    register_routes();
    for (;;) {
        wifi_mgr_tick();
        WifiMgrState st = wifi_mgr_state();
        bool net_up = (st == WIFI_MGR_AP_CONFIG || st == WIFI_MGR_STA_RUNNING);
        if (net_up && !s_begun) {
            s_server.begin();
            s_begun = true;
            DEBUG_PRINT("debug_server: WebServer started on port 80");
        }
        if (s_begun && s_enabled) s_server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void debug_server_begin(void) {
    if (s_task) return;
    xTaskCreatePinnedToCore(server_task, "dbg_srv", 8192, nullptr, 1, &s_task, 0);
}

void debug_server_set_enabled(bool on) { s_enabled = on; }
bool debug_server_enabled(void)        { return s_enabled; }
