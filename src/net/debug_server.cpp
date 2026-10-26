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
#include <esp_heap_caps.h>

#include "debug_rpc.h"
#include "wifi_mgr.h"
#include "dw_bus.h"
#include "dw_client.h"
#include "png_writer.h"
#include "../hal/hal.h"             // hal_video_capture_*
#include "../core/machine.h"        // g_machine_type, machine types
#include "../core/speed_meter.h"
#include "../supervisor/supervisor.h" // supervisor_set_machine_type
#include "../supervisor/sv_disk.h"
#include "../core/ssc/ssc.h"
#include "../core/vhd.h"
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

static const char* cpu_name(uint8_t variant) {
    return (variant == CPU_VARIANT_HD6309) ? "6309" : "6809";
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
    j += ",\"cpu\":\"" + String(cpu_name(g_cpu_variant)) + "\"";
    j += ",\"coco3_ram_kb\":" + String(g_coco3_ram_kb);
    j += ",\"paused\":" + String(debug_rpc_is_paused() ? "true" : "false");
    j += ",\"firmware\":\"" FIRMWARE_VERSION "\"";
    j += ",\"api\":" + String(DEBUG_API_VERSION);
    j += ",\"bus_mode\":\"" + String(dw_bus_mode_str(dw_bus_mode())) + "\"";
    j += ",\"bus_link\":\"" + String(dw_bus_link_str()) + "\"";
    {
        int idx = (g_machine_type == 4) ? 1 : 0;
        j += ",\"cart_rom\":\"" + String(g_cart_rom_loaded[idx] ? g_cart_rom_loaded[idx] : "none") + "\"";
        if (g_cart_rom_fallback[idx]) j += ",\"cart_rom_warning\":\"HDB-DOS ROM missing\"";
    }
    j += ",\"bus_to_coco\":" + String(becker_bytes_to_coco());
    j += ",\"bus_from_coco\":" + String(becker_bytes_from_coco());
    // Internal RAM headroom (free now / lowest since boot / largest block).
    j += ",\"int_free\":" + String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    j += ",\"int_min\":" + String(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    j += ",\"int_largest\":" + String(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    // Byte-addressable part only (int_* also counts the 32-bit-only IRAM heap,
    // which malloc/calloc/new cannot use).
    j += ",\"int8_free\":" + String(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    j += ",\"int8_min\":" + String(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    j += ",\"int8_largest\":" + String(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    j += ",\"fps\":" + String(hal_video_get_fps(), 1);
    // Emulated CPU speed over the last ~1 s of running frames (see speed_meter.h).
    {
        SpeedStats sp;
        speed_meter_get(&sp);
        j += ",\"speed\":{\"mhz\":" + String(sp.mhz, 4);
        j += ",\"target_mhz\":" + String(sp.target_mhz, 4);
        j += ",\"ratio\":" + String(sp.target_mhz > 0 ? sp.mhz / sp.target_mhz : 0.0f, 3);
        j += ",\"fps\":" + String(sp.fps, 2);
        j += ",\"rate\":" + String(sp.rate);
        j += ",\"cycles_per_frame\":" + String(sp.cycles_per_frame);
        j += ",\"frame_us_avg\":" + String(sp.frame_us_avg);
        j += ",\"frame_us_max\":" + String(sp.frame_us_max);
        j += ",\"windows\":" + String(sp.windows) + "}";
    }
    // GIME timer: expiries since power-on (rate = difference between two
    // reads), source (true = 3.58 MHz, false = scanline) and reload.
    {
        uint32_t fires; bool tins; unsigned reload;
        machine_gime_timer_info(&fires, &tins, &reload);
        j += ",\"gime_timer\":{\"fires\":" + String(fires);
        j += ",\"tins\":" + String(tins ? "true" : "false");
        j += ",\"reload\":" + String(reload) + "}";
    }
    // Output layout (640x200, or 512x240 for tall GIME screens)
    {
        int w, h;
        uint32_t sw, us;
        hal_video_layout_info(&w, &h, &sw, &us);
        j += ",\"video\":{\"w\":" + String(w) + ",\"h\":" + String(h);
        j += ",\"switches\":" + String(sw) + ",\"switch_us\":" + String(us) + "}";
    }
    // GIME video registers ($FF90-$FF9F as last written) and line counts
    {
        uint8_t r[16];
        unsigned lAA, lTB;
        if (machine_gime_video_info(r, &lAA, &lTB)) {
            char hex[33];
            for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02X", r[i]);
            j += ",\"gime\":{\"regs\":\"" + String(hex) + "\"";
            j += ",\"lAA\":" + String(lAA);
            j += ",\"lTB\":" + String(lTB) + "}";
        }
    }
    // Audio queue diagnostics; ?audio_reset=1 clears them after this read.
    {
        HalAudioStats a;
        hal_audio_get_stats(&a);
        float mhz = (float)getCpuFrequencyMhz();
        j += ",\"audio\":{\"commits\":" + String(a.commits);
        j += ",\"underruns\":" + String(a.underruns);
        j += ",\"hold_ticks\":" + String(a.hold_ticks);
        j += ",\"catchup_ticks\":" + String(a.catchup_ticks);
        j += ",\"overruns\":" + String(a.overruns);
        j += ",\"fill\":" + String(a.fill);
        j += ",\"fill_min\":" + String(a.fill_min);
        j += ",\"fill_max\":" + String(a.fill_max);
        j += ",\"stride_adj\":" + String(a.stride_adj);
        j += ",\"commit_us_min\":" + String(a.commit_us_min);
        j += ",\"commit_us_max\":" + String(a.commit_us_max);
        j += ",\"isr_us_max\":" + String(a.isr_cyc_max / mhz, 2);
        j += ",\"isr_gap_us_min\":" + String(a.isr_gap_cyc_min / mhz, 2);
        j += ",\"isr_gap_us_max\":" + String(a.isr_gap_cyc_max / mhz, 2) + "}";
        if (s_server.arg("audio_reset") == "1") hal_audio_reset_stats();
    }
    // Speech/Sound Cartridge: off, or its counters (see docs/ssc.md).
    j += ",\"ssc\":{\"enabled\":" + String(ssc_enabled() ? "true" : "false");
    if (ssc_enabled()) {
        j += ",\"busy\":" + String(ssc_busy() ? "true" : "false");
        j += ",\"commands\":" + String(ssc_command_count());
        j += ",\"allophones\":" + String(ssc_ald_speech_count());
        j += ",\"ay_writes\":" + String(ssc_ay_write_count());
        j += ",\"underruns\":" + String(ssc_underruns());
        j += ",\"queued\":" + String(ssc_queued());
        j += ",\"stack_free\":" + String(ssc_stack_free());
    }
    j += "}";
    // Unused stack of this server task, lowest since boot (bytes).
    j += ",\"srv_stack_free\":" + String(uxTaskGetStackHighWaterMark(nullptr));
    // Emulator/OSD task (Arduino loopTask), lowest unused stack since boot.
    j += ",\"osd_state\":" + String(supervisor_state());
    extern TaskHandle_t loopTaskHandle;
    if (loopTaskHandle) j += ",\"loop_stack_free\":" + String(uxTaskGetStackHighWaterMark(loopTaskHandle));
    j += "}";
    send_json(200, j);
}

// DriveWire bus (Becker port) configuration. Read-only view of the running
// state plus the saved settings; POST saves and restarts (mode is boot-time).
static void h_get_bus() {
    String j = "{";
    j += "\"mode\":" + String((int)dw_bus_mode());
    j += ",\"mode_name\":\"" + String(dw_bus_mode_str(dw_bus_mode())) + "\"";
    j += ",\"host\":\"" + dw_bus_host() + "\"";
    j += ",\"port\":" + String(dw_bus_port());
    j += ",\"rom_timeout\":" + String(dw_bus_rom_timeout() ? "true" : "false");
    int idx = (g_machine_type == 4) ? 1 : 0;
    j += ",\"cart_rom\":\"" + String(g_cart_rom_loaded[idx] ? g_cart_rom_loaded[idx] : "none") + "\"";
    j += ",\"cart_rom_fallback\":" + String(g_cart_rom_fallback[idx] ? "true" : "false");
    j += ",\"link\":\"" + String(dw_bus_link_str()) + "\"";
    j += ",\"link_up\":" + String(becker_link_up() ? "true" : "false");
    j += ",\"to_coco\":" + String(becker_bytes_to_coco());
    j += ",\"from_coco\":" + String(becker_bytes_from_coco());
    j += ",\"connects\":" + String(dw_client_connects());
    j += ",\"max_reply_ms\":" + String(dw_client_max_reply_ms());
    j += ",\"slow_replies\":" + String(dw_client_slow_replies());
    j += "}";
    send_json(200, j);
}

static void h_post_bus() {
    if (!s_server.hasArg("mode")) { send_err(400, "missing mode (0=Off,1=External)"); return; }
    uint32_t m = arg_u32("mode", 0);
    if (m >= BUS_MODE_COUNT || !dw_bus_mode_supported((BusMode)m)) {
        send_err(400, "mode not supported in this build");
        return;
    }
    String host = s_server.hasArg("host") ? s_server.arg("host") : dw_bus_host();
    uint32_t port = arg_u32("port", dw_bus_port());
    if (port == 0 || port > 65535) { send_err(400, "bad port"); return; }
    bool rom_to = s_server.hasArg("rom_to") ? (arg_u32("rom_to", 0) != 0)
                                            : dw_bus_rom_timeout();
    if (m == BUS_MODE_EXTERNAL && host.length() == 0) { send_err(400, "External mode needs host"); return; }

    dw_bus_save_config((BusMode)m, host, (uint16_t)port, rom_to);
    // Respond BEFORE restarting — see h_post_machine.
    send_json(200, String("{\"rebooting\":true,\"mode\":") + m + "}");
    delay(200);
    debug_rpc_set_paused(true);
    delay(50);
    supervisor_save_and_restart();
}

// Disk Manager drives 0-3 (the WD1793 drives). Mount/eject run on core 1 and
// are remembered in NVS.
static void h_get_disk() {
    Machine* m = debug_rpc_machine();
    if (!m) { send_err(400, "no machine"); return; }
    String j = "{\"drives\":[";
    sv_disk_lock();   // a mount on core 1 may be rewriting the entry
    for (int d = 0; d < SV_DISK_MAX_DRIVES; d++) {
        SV_DiskImage* img = &m->fdc.drives[d];
        if (d) j += ",";
        j += "{\"drive\":" + String(d) + ",\"mounted\":" + (img->mounted ? "true" : "false");
        if (img->mounted) {
            j += ",\"path\":\"" + String(img->path) + "\"";
            j += ",\"bytes\":" + String(img->cache_size);
            j += ",\"read_only\":" + String(img->read_only ? "true" : "false");
            j += ",\"dirty\":" + String(img->dirty ? "true" : "false");
        }
        j += "}";
    }
    sv_disk_unlock();
    // Hard disks: plain fields, changed only by mounts on core 1.
    j += "],\"hd_enabled\":" + String(vhd_enabled() ? "true" : "false") + ",\"hard_disks\":[";
    for (int d = 0; d < VHD_NUM_DRIVES; d++) {
        bool mounted = vhd_is_mounted(d);
        if (d) j += ",";
        j += "{\"drive\":\"hd" + String(d) + "\",\"mounted\":" + (mounted ? "true" : "false");
        if (mounted) {
            j += ",\"path\":\"" + String(vhd_get_path(d)) + "\"";
            j += ",\"read_only\":" + String(vhd_is_read_only(d) ? "true" : "false");
        }
        j += "}";
    }
    j += "]}";
    send_json(200, j);
}

static void h_post_disk() {
    String op = s_server.arg("op");
    DebugCmd c = {};
    c.type = DBG_CMD_DISK;
    // drive=0-3 for the floppies, drive=hd0 / hd1 for the hard disks
    String drive = s_server.arg("drive");
    if (drive.startsWith("hd")) c.addr = SV_DISK_MAX_DRIVES + drive.substring(2).toInt();
    else                        c.addr = arg_u32("drive", 0);
    String path = s_server.arg("path");
    if (op == "mount") {
        if (path.length() == 0) { send_err(400, "mount needs path"); return; }
        c.disk_op = DBG_DISK_MOUNT;
        c.path = path.c_str();
    } else if (op == "eject") {
        c.disk_op = DBG_DISK_EJECT;
    } else if (op == "flush") {
        c.disk_op = DBG_DISK_FLUSH;
    } else {
        send_err(400, "op must be mount|eject|flush");
        return;
    }
    // Loading a large image from SD takes a while — longer than other RPCs.
    if (!debug_rpc_submit(&c, 10000)) { send_err(504, "rpc timeout"); return; }
    if (c.result != DBG_OK) { send_err(400, op == "mount" ? "mount failed" : "bad drive"); return; }
    send_json(200, String("{\"ok\":true,\"op\":\"") + op + "\",\"drive\":" + c.addr + "}");
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
    j += ",\"cpu\":\"" + String(c.cpu == CPU_VARIANT_HD6309 ? "6309" : "6809") + "\"";
    if (c.cpu == CPU_VARIANT_HD6309) {
        // E/F are the halves of W; MD bits: 7 div-by-zero, 6 illegal op, 1 FIRQ mode, 0 native
        j += ",\"e\":" + String(c.w >> 8) + ",\"f\":" + String(c.w & 0xFF) + ",\"w\":" + String(c.w);
        j += ",\"v\":" + String(c.v) + ",\"md\":" + String(c.md);
        j += ",\"native\":" + String((c.md & HD6309_MD_NM) ? 1 : 0);
    }
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
    if (s_server.hasArg("w"))  { c.w  = (uint16_t)arg_u32("w",  c.w);  mask |= DBG_REG_W; }
    if (s_server.hasArg("e"))  { c.w  = (uint16_t)((arg_u32("e", c.w >> 8) << 8) | (c.w & 0xFF)); mask |= DBG_REG_W; }
    if (s_server.hasArg("f"))  { c.w  = (uint16_t)((c.w & 0xFF00) | (arg_u32("f", c.w & 0xFF) & 0xFF)); mask |= DBG_REG_W; }
    if (s_server.hasArg("v"))  { c.v  = (uint16_t)arg_u32("v",  c.v);  mask |= DBG_REG_V; }
    if (s_server.hasArg("md")) { c.md = (uint8_t) arg_u32("md", c.md); mask |= DBG_REG_MD; }

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
               ",\"machine\":\"" + machine_name(g_machine_type) +
               "\",\"cpu\":\"" + cpu_name(g_cpu_variant) +
               "\",\"coco3_ram_kb\":" + g_coco3_ram_kb + "}";
    send_json(200, j);
}

static void h_post_machine() {
    if (!s_server.hasArg("type")) { send_err(400, "missing type (3=CoCo2,4=CoCo3)"); return; }
    uint8_t t = (uint8_t)arg_u32("type", g_machine_type);
    if (t != 3 && t != 4) { send_err(400, "type must be 3 or 4"); return; }

    // Optional cpu=6809|6309; like the machine type it takes effect at boot.
    uint8_t cpu = g_cpu_variant;
    if (s_server.hasArg("cpu")) {
        uint32_t c = arg_u32("cpu", 0);
        if (c != 6809 && c != 6309) { send_err(400, "cpu must be 6809 or 6309"); return; }
        cpu = (c == 6309) ? CPU_VARIANT_HD6309 : CPU_VARIANT_MC6809;
    }
    // Optional ram=128|512|1024|2048: CoCo 3 memory in KB, also applied at boot.
    uint16_t ram = g_coco3_ram_kb;
    if (s_server.hasArg("ram")) {
        uint32_t kb = arg_u32("ram", 0);
        if (!coco3_ram_kb_valid(kb)) { send_err(400, "ram must be 128, 512, 1024 or 2048"); return; }
        ram = (uint16_t)kb;
    }
    if (t == g_machine_type && cpu == g_cpu_variant && ram == g_coco3_ram_kb) { h_get_machine(); return; }

    // Respond BEFORE switching: supervisor_set_machine_type() reboots and
    // never returns. Freeze core 1 first to avoid touching emulator state
    // mid-frame during the disk-cache flush + state save.
    send_json(200, String("{\"rebooting\":true,\"machine_type\":") + t +
                   ",\"cpu\":\"" + cpu_name(cpu) + "\",\"coco3_ram_kb\":" + ram + "}");
    delay(200);
    debug_rpc_set_paused(true);
    delay(50);
    supervisor_save_cpu_variant(cpu);
    supervisor_save_coco3_ram(ram);
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
// With fb=1 it instead returns the live display framebuffer (post-HAL), read
// while the emulator is held paused after the stepped frame.
static void h_screenshot() {
    bool was_paused = debug_rpc_is_paused();
    const bool fb = s_server.hasArg("fb");

    if (fb) debug_rpc_set_paused(true);
    else    hal_video_capture_arm();

    DebugCmd c = {};
    c.type = DBG_CMD_STEP_FRAME;
    if (!debug_rpc_submit(&c, 2000)) {
        debug_rpc_set_paused(was_paused);
        send_err(504, "frame step timeout");
        return;
    }
    if (fb) hal_video_capture_framebuffer();

    // Restore the prior pause state (STEP_FRAME does not change it).
    debug_rpc_set_paused(was_paused);

    int w = 0, h = 0;
    const uint16_t* frame = hal_video_capture_frame(&w, &h);
    if (!frame || w <= 0 || h <= 0) { send_err(500, "capture failed"); return; }

    size_t png_len = 0;
    uint8_t* png = png_encode_rgb565(frame, w, h, HAL_CAPTURE_STRIDE, &png_len);
    if (!png) { send_err(500, "png encode failed"); return; }

    // Stream in 4 KB chunks, yielding between them: one ~150 KB write let lwIP
    // queue far more internal-RAM pbufs than the heap can spare, dropping WiFi.
    s_server.setContentLength(png_len);
    s_server.send(200, "image/png", "");
    const size_t CHUNK = 4096;
    for (size_t off = 0; off < png_len; off += CHUNK) {
        size_t n = (png_len - off < CHUNK) ? png_len - off : CHUNK;
        s_server.sendContent((const char*)png + off, n);
        if (!s_server.client().connected()) break;
        vTaskDelay(1);
    }
    free(png);
}

// OSD key injection: POST /api/key  hid=<USB HID usage>  or  toggle=1 (F3).
static void h_key() {
    uint8_t hid = s_server.hasArg("toggle") ? DBG_KEY_TOGGLE : (uint8_t)s_server.arg("hid").toInt();
    if (hid == 0) { send_err(400, "hid=<usage> or toggle=1"); return; }
    if (!debug_rpc_key_push(hid)) { send_err(503, "key queue full"); return; }
    send_json(200, String("{\"ok\":true,\"hid\":") + hid + "}");
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
    s_server.on("/api/key",           HTTP_POST, h_key);
    s_server.on("/api/reset",         HTTP_POST, h_reset);
    s_server.on("/api/machine",       HTTP_GET,  h_get_machine);
    s_server.on("/api/machine",       HTTP_POST, h_post_machine);
    s_server.on("/api/nvram",         HTTP_GET,  h_nvram);
    s_server.on("/api/bus",           HTTP_GET,  h_get_bus);
    s_server.on("/api/bus",           HTTP_POST, h_post_bus);
    s_server.on("/api/disk",          HTTP_GET,  h_get_disk);
    s_server.on("/api/disk",          HTTP_POST, h_post_disk);
    s_server.on("/api/screenshot.png",HTTP_GET,  h_screenshot);

    s_server.onNotFound([]() { send_err(404, "not found"); });
}

static void server_task(void* arg) {
    (void)arg;
    register_routes();
    for (;;) {
        WifiMgrState st = wifi_mgr_state();
        bool net_up = (st == WIFI_MGR_STA_RUNNING);
        if (net_up && !s_begun) {
            s_server.begin();
            s_begun = true;
            DEBUG_PRINT("debug_server: WebServer started on port 80");
        }
        if (s_begun && s_enabled) s_server.handleClient();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

// Persisted On/Off setting ("sv" namespace, key "dbg_srv", default On).
static bool load_enabled(void) {
    nvs_handle_t h;
    uint8_t v = 1;
    if (nvs_open("sv", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "dbg_srv", &v);
        nvs_close(h);
    }
    return v != 0;
}

static void save_enabled(bool on) {
    nvs_handle_t h;
    if (nvs_open("sv", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "dbg_srv", on ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void ensure_task(void) {
    if (s_task) return;
    // Measured peak 1952 B (screenshots, 4 KB mem reads, NVS dump); see srv_stack_free.
    xTaskCreatePinnedToCore(server_task, "dbg_srv", 4096, nullptr, 1, &s_task, 0);
}

void debug_server_begin(void) {
    s_enabled = load_enabled();
    if (!s_enabled) {
        // Off: no task, routes or socket at all (~4.4 KB+ of internal RAM kept
        // free). Turning it on creates it.
        DEBUG_PRINT("debug_server: off (setting) - not started");
        return;
    }
    ensure_task();
}

void debug_server_set_enabled(bool on) {
    s_enabled = on;
    save_enabled(on);
    if (on) ensure_task();   // off: stops serving now, memory freed at next boot
}

bool debug_server_enabled(void) { return s_enabled; }
