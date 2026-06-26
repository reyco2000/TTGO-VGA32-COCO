#!/usr/bin/env python3
"""
CoCo 3 ESP32 emulator — MCP bridge.

Translates the device's small HTTP/JSON debug API (see
TTGO-VGA32-COCO/docs/wifi-debug.md) into MCP tools so an agent (Claude Desktop /
Claude Code) can inspect and control the running 6809.

The device exposes form/query-parameter endpoints; each tool here is a thin
httpx call. Configure the device address with the COCO_DEBUG_HOST environment
variable, e.g.:

    COCO_DEBUG_HOST=http://192.168.1.50

Run:  python coco_mcp_bridge.py
"""

import os
import time

import httpx
from mcp.server.fastmcp import FastMCP, Image

# --- configuration ---
HOST = os.environ.get("COCO_DEBUG_HOST", "http://coco3.local").rstrip("/")
TIMEOUT = float(os.environ.get("COCO_DEBUG_TIMEOUT", "5.0"))

mcp = FastMCP("coco3-debug")


# --- HTTP helpers ---

def _get(path: str, **params):
    r = httpx.get(f"{HOST}{path}", params=params or None, timeout=TIMEOUT)
    r.raise_for_status()
    return r


def _post(path: str, **data):
    # The device parses application/x-www-form-urlencoded parameters.
    r = httpx.post(f"{HOST}{path}", data=data or None, timeout=TIMEOUT)
    r.raise_for_status()
    return r


# --- tools ---

@mcp.tool()
def status() -> dict:
    """Get emulator status: machine type, paused flag, firmware and API version."""
    return _get("/api/status").json()


@mcp.tool()
def read_registers() -> dict:
    """Read the 6809 CPU registers (A, B, D, X, Y, U, S, PC, DP, CC),
    decoded condition-code flags (E F H I N Z V C), and the cycle count."""
    return _get("/api/registers").json()


@mcp.tool()
def write_registers(
    pc: int | None = None,
    a: int | None = None,
    b: int | None = None,
    d: int | None = None,
    x: int | None = None,
    y: int | None = None,
    u: int | None = None,
    s: int | None = None,
    dp: int | None = None,
    cc: int | None = None,
) -> dict:
    """Set any subset of the 6809 registers. Only the values you pass are
    changed. Returns the full register set after the write."""
    fields = {k: v for k, v in dict(
        pc=pc, a=a, b=b, d=d, x=x, y=y, u=u, s=s, dp=dp, cc=cc
    ).items() if v is not None}
    if not fields:
        raise ValueError("provide at least one register to write")
    return _post("/api/registers", **fields).json()


@mcp.tool()
def read_memory(addr: int, length: int = 16, space: str = "cpu") -> dict:
    """Read `length` bytes starting at `addr`.

    space="cpu"  -> MMU-correct view (what the 6809 sees, ROM/I-O mapped in).
    space="phys" -> raw physical RAM (CoCo 3 512KB linear).

    Returns {addr, len, space, data(hex), bytes(list of ints)}.
    """
    j = _get("/api/mem", addr=addr, len=length, space=space).json()
    hexstr = j.get("data", "")
    j["bytes"] = list(bytes.fromhex(hexstr)) if hexstr else []
    return j


@mcp.tool()
def write_memory(addr: int, data_hex: str, space: str = "cpu") -> dict:
    """Write bytes (given as a hex string, e.g. "86ff39") at `addr`.
    space="cpu" (MMU-correct) or "phys" (raw physical RAM)."""
    return _post("/api/mem", addr=addr, data=data_hex.replace(" ", ""), space=space).json()


@mcp.tool()
def pause() -> dict:
    """Pause emulation at the next frame boundary (CPU frozen, server responsive)."""
    return _post("/api/pause").json()


@mcp.tool()
def resume() -> dict:
    """Resume emulation after a pause."""
    return _post("/api/resume").json()


@mcp.tool()
def inject_code(addr: int, data_hex: str, pc: int | None = None,
                resume_after: bool = False) -> dict:
    """Write a blob of 6809 code/data (hex string) into CPU memory at `addr`.
    Optionally set PC to begin executing it, and optionally resume the CPU."""
    args = dict(addr=addr, data=data_hex.replace(" ", ""))
    if pc is not None:
        args["pc"] = pc
    if resume_after:
        args["resume"] = 1
    return _post("/api/inject", **args).json()


@mcp.tool()
def reset() -> dict:
    """Cold-reset the emulated CoCo in place (machine_reset). The device stays
    up and WiFi/server are unaffected."""
    return _post("/api/reset").json()


@mcp.tool()
def get_machine() -> dict:
    """Get the current machine type (3 = CoCo 2, 4 = CoCo 3)."""
    return _get("/api/machine").json()


@mcp.tool()
def set_machine(machine_type: int, wait: bool = True) -> dict:
    """Switch machine type (3 = CoCo 2, 4 = CoCo 3).

    IMPORTANT: this reboots the device — the current connection drops. If
    `wait` is true, the tool polls /api/status until the device comes back
    (requires auto-connect enabled so it rejoins WiFi automatically).
    """
    resp = _post("/api/machine", type=machine_type).json()
    if not wait:
        return resp
    # Device reboots; wait for it to rejoin and report the new type.
    deadline = time.time() + 60
    time.sleep(3)
    while time.time() < deadline:
        try:
            st = _get("/api/status").json()
            if int(st.get("machine_type", -1)) == machine_type:
                return {"rebooted": True, "status": st}
        except Exception:
            pass
        time.sleep(2)
    return {"rebooted": True, "warning": "device did not report new type within timeout",
            "initial_response": resp}


@mcp.tool()
def read_nvram() -> dict:
    """Dump all persisted settings in the device's NVS "sv" namespace as JSON
    (machine_type, serial_mode, kbd_layout, joyLevel, joyInv, WiFi creds, ...)."""
    return _get("/api/nvram").json()


@mcp.tool()
def screenshot() -> Image:
    """Capture the current screen as a PNG. Note: this advances the emulation by
    exactly one frame, then restores the prior pause state."""
    r = _get("/api/screenshot.png")
    return Image(data=r.content, format="png")


if __name__ == "__main__":
    mcp.run()
