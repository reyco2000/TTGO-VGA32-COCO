# WiFi Debug Server + MCP Bridge

Remote inspection and control of the running 6809 from an external agent/LLM.
The device exposes a small, stable **HTTP/JSON debug API** over WiFi; a tiny
host-side **MCP bridge** (`mcp-bridge/`, Python) translates that API into MCP
tools for Claude.

## Architecture

```
  ESP32 (core 1)                 ESP32 (core 0)              Host
  ┌──────────────┐  DebugCmd     ┌──────────────┐  HTTP      ┌────────────┐  MCP
  │ emulator      │ <─ queue ──  │ WebServer task │ <───────  │ MCP bridge  │ <──  Claude
  │ machine_run_  │  ─ sem ──>   │ (debug_server) │  ───────> │ (httpx)     │
  │ frame() loop  │              │  wifi_mgr      │           └────────────┘
  └──────────────┘              └──────────────┘
       debug_rpc_poll()               port 80
```

- The emulator runs single-threaded in `loop()` on **core 1** (with FabGL VGA +
  the audio timer ISR). **Core 0 is free**, so the WiFi stack and the WebServer
  task live there and never preempt emulation timing. `WiFi.setSleep(false)`
  keeps latency low.

## Concurrency model (the core safety mechanism)

All emulator state lives on core 1. The web server (core 0) **never touches it
directly**. Instead, a single-slot RPC handshake (`src/net/debug_rpc.{h,cpp}`):

1. The web handler (core 0) fills a `DebugCmd`, calls `debug_rpc_submit()`, and
   blocks on a completion semaphore (1 s timeout).
2. Core 1 calls `debug_rpc_poll()` **between frames** in `loop()`. It executes
   the pending command against `coco`/`cpu`, writes the result, and signals the
   semaphore.
3. **Pause** is a flag. While paused, `loop()` stops calling
   `machine_run_frame()` and instead spins on `debug_rpc_poll()` + a short
   `delay`, so the CPU is frozen but commands are serviced promptly. Resume
   clears the flag.

Commands routed through the queue (core 1): register read/write, memory
read/write (CPU or physical), inject, reset, and a one-frame step (screenshots).
Machine-switch and the NVS dump run on core 0 (machine-switch reboots; NVS read
is core-independent).

## WiFi flow

`src/net/wifi_mgr.{h,cpp}` — state machine
`OFF → AP_CONFIG → CONNECTING → STA_RUNNING / FAILED`.

1. From the supervisor (**Settings → WiFi / Debug**) the user picks **Start
   Config Portal**. The device brings up an open SoftAP `CoCo3-Setup`.
2. The user joins that AP from a phone/PC and opens `http://192.168.4.1/`. The
   served page scans networks, lets them pick an SSID and type the password
   **in the browser** (no on-screen password entry).
3. `POST /save` stores SSID+password to NVS (namespace `"sv"`: `wifi_ssid`,
   `wifi_pass`, `wifi_auto`), switches to STA, and connects.
4. The supervisor WiFi screen then shows **STA connected + the LAN IP** — the
   debug-server address.

On boot, if `wifi_auto` is set, `setup()` kicks off a non-blocking STA connect
with the saved credentials, so the API is reachable automatically (also required
for the machine-switch reboot to re-expose the API).

## Device HTTP/JSON API (STA mode)

Inputs are **form/query parameters** (the device has no JSON-body parser).
Numbers accept decimal or `0x` hex; byte payloads are lowercase hex strings.
Responses are JSON. Transfers are capped at 4 KB per request.

| Method / Path | Description |
|---|---|
| `GET /api/status` | machine type, paused flag, firmware/API version |
| `POST /api/pause`, `POST /api/resume` | freeze / un-freeze at frame boundary |
| `GET /api/registers` | A,B,D,X,Y,U,S,PC,DP,CC + decoded flags + cycles |
| `POST /api/registers` | set any subset: `pc=`, `a=`, `b=`, `d=`, `x=`, `y=`, `u=`, `s=`, `dp=`, `cc=` |
| `GET /api/mem?addr=&len=&space=cpu\|phys` | read bytes → `{..,"data":"hex"}` |
| `POST /api/mem` | `addr=`, `data=`hex, `space=` — write bytes |
| `POST /api/inject` | `addr=`, `data=`hex, optional `pc=`, `resume=1` |
| `GET /api/screenshot.png` | arm capture, advance one frame, return PNG |
| `POST /api/reset` | clean in-place `machine_reset()` (device stays up) |
| `GET /api/machine` | current machine type (3 = CoCo 2, 4 = CoCo 3) |
| `POST /api/machine` | `type=3\|4` — **reboots the device** (see below) |
| `GET /api/nvram` | dump all `"sv"` NVS settings as JSON |

### Examples (curl)

```bash
IP=192.168.1.50
curl http://$IP/api/status
curl http://$IP/api/registers
curl -XPOST http://$IP/api/pause
curl -XPOST http://$IP/api/mem  -d 'addr=1024&data=48454c4c4f'
curl       "http://$IP/api/mem?addr=1024&len=5"
curl -XPOST http://$IP/api/registers -d 'pc=0xa027&a=0x41'
curl -XPOST http://$IP/api/inject -d 'addr=0x3f00&data=8641b7ff22&pc=0x3f00&resume=1'
curl       http://$IP/api/screenshot.png -o shot.png
curl -XPOST http://$IP/api/reset
curl -XPOST http://$IP/api/machine -d 'type=4'
curl       http://$IP/api/nvram
```

### Caveats

- **`POST /api/machine` reboots.** It mirrors the supervisor path
  (`supervisor_set_machine_type()`): flush disk caches → save state → write
  `machine_type` to NVS → `esp_restart()`. The handler returns
  `{"rebooting":true}` *before* the reboot; the agent must reconnect after the
  device comes back (auto-connect rejoins STA automatically).
- **A screenshot advances emulation by exactly one frame.** Capture is armed,
  one frame is stepped on core 1 (which renders + fills the PSRAM capture
  buffer), then the prior pause state is restored. The PNG covers the active
  GIME area at its native width (320 or 640) × active lines.
- **No authentication / TLS** — intended for a trusted LAN.

## Config-portal routes (AP mode)

| Method / Path | Description |
|---|---|
| `GET /` | HTML page: Scan button, SSID dropdown, password field |
| `GET /scan` | JSON list of nearby SSIDs (rssi, secure) |
| `POST /save` | `ssid=`, `pass=` — store + connect (STA) |
| `GET /status` | `{state, ip}` — connection result for the page to poll |

## Screenshot capture

`hal_video.cpp` gains a capture hook at the top of
`hal_video_render_scanline_gime()`: when armed, each line's byte-swapped RGB565
`pixels` are copied into a `640×240` PSRAM buffer (`ps_malloc`, lazy). The frame
completes on the last scanline. `src/net/png_writer.{cpp,h}` encodes that buffer
to a 24-bit PNG using deflate **stored** blocks (no compression cost), with
table-free CRC32 + Adler32, allocated in PSRAM.

## Firmware modules (`src/net/`)

| File | Responsibility |
|---|---|
| `wifi_mgr.{cpp,h}` | WiFi state machine, SoftAP, scan, STA connect, NVS creds |
| `debug_server.{cpp,h}` | core-0 WebServer task; config-portal + debug-API routes; JSON ↔ params |
| `debug_rpc.{cpp,h}` | `DebugCmd`, single-slot queue, `debug_rpc_poll()` on core 1, pause flag |
| `png_writer.{cpp,h}` | RGB565 → PNG (stored deflate) |

Boot wiring lives in `TTGO-VGA32-COCO.ino`: `setup()` calls `debug_rpc_init()`,
`wifi_mgr_init()`, optional `wifi_mgr_connect_saved()`, and
`debug_server_begin()`; `loop()` calls `debug_rpc_poll()` each frame and freezes
emulation while paused. The supervisor screen is `src/supervisor/sv_wifi.{cpp,h}`.

### DRAM note

Linking the WiFi/WebServer stack adds enough static data to internal DRAM that
two existing/new buffers were moved off permanent `.bss`: `sv_debug.cpp`'s 4 KB
dump batch buffer is now a transient heap allocation (only during an SD dump),
and `png_writer`'s CRC32 is table-free. This keeps the build within the
`dram0_0_seg` budget.

## Host MCP bridge

See [`../mcp-bridge/README.md`](../mcp-bridge/README.md). Configure `COCO_DEBUG_HOST=http://<device-ip>` and
register `coco_mcp_bridge.py` with Claude Code or Claude Desktop. Tools:
`status`, `read_registers`, `write_registers`, `read_memory`, `write_memory`,
`pause`, `resume`, `inject_code`, `reset`, `get_machine`, `set_machine`,
`read_nvram`, `screenshot`.
