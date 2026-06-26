# CoCo 3 ESP32 — MCP Bridge

A tiny host-side [MCP](https://modelcontextprotocol.io) server that exposes the
ESP32 CoCo 3 emulator's WiFi debug API as MCP tools, so Claude (Desktop or Code)
can inspect and control the running 6809.

The device itself speaks a small HTTP/JSON debug API (see
`../docs/wifi-debug.md`). This bridge is just a thin translator —
each MCP tool is one `httpx` call to the device.

## Tools

| Tool | Description |
|------|-------------|
| `status` | Machine type, paused flag, firmware/API version |
| `read_registers` | A,B,D,X,Y,U,S,PC,DP,CC + decoded flags + cycle count |
| `write_registers` | Set any subset of registers (incl. PC) |
| `read_memory` | Read bytes (`space="cpu"` MMU view, or `"phys"` raw RAM) |
| `write_memory` | Write bytes (hex) at an address |
| `pause` / `resume` | Freeze / un-freeze emulation at frame boundary |
| `inject_code` | Write a code blob, optionally set PC and resume |
| `reset` | Cold-reset the CoCo in place (device stays up) |
| `get_machine` / `set_machine` | Read / switch CoCo 2 ↔ CoCo 3 (switch **reboots** the device) |
| `read_nvram` | Dump the persisted `"sv"` NVS settings |
| `screenshot` | Capture the screen as a PNG (advances emulation one frame) |

## Setup

```bash
cd mcp-bridge
python -m venv .venv && source .venv/bin/activate
pip install -r requirements.txt
```

Point the bridge at your device (find the IP in the supervisor's
**Settings → WiFi / Debug** screen):

```bash
export COCO_DEBUG_HOST=http://192.168.1.50
python coco_mcp_bridge.py
```

## Register with Claude

### Claude Code

```bash
claude mcp add coco3-debug \
  --env COCO_DEBUG_HOST=http://192.168.1.50 \
  -- python /absolute/path/to/mcp-bridge/coco_mcp_bridge.py
```

### Claude Desktop

Add to `claude_desktop_config.json` (Settings → Developer → Edit Config):

```json
{
  "mcpServers": {
    "coco3-debug": {
      "command": "python",
      "args": ["/absolute/path/to/mcp-bridge/coco_mcp_bridge.py"],
      "env": { "COCO_DEBUG_HOST": "http://192.168.1.50" }
    }
  }
}
```

Use the venv's Python (`.venv/bin/python`) if you installed into a venv.

## Notes

- **No authentication / TLS** — the device API is intended for a trusted LAN.
- **`set_machine` reboots the device.** The tool polls `/api/status` until it
  returns; this requires **auto-connect** to be enabled (supervisor WiFi screen)
  so the device rejoins WiFi automatically after the reboot.
- **`screenshot` advances emulation by one frame** (a documented side effect of
  the on-device capture), then restores the prior pause state.
- Inputs are sent as form/query parameters (the device has no JSON-body parser).
  Addresses/values are plain integers; byte payloads are hex strings (`"86ff39"`).
