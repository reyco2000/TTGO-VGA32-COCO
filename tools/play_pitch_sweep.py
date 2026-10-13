#!/usr/bin/env python3
# ============================================================
#        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
#   (C) 2026 Reinaldo Torres / CoCo Byte Club
#   https://github.com/reyco2000/TTGO-VGA32-COCO
#   Co-developed with Claude Code
#   GPL-3.0-or-later License
# ============================================================
#  File   : tools/play_pitch_sweep.py
#  Module : Audio test — plays every PLAY note on the board and
#           measures each one at the PC, with nobody at the keyboard
# ============================================================
"""
play_pitch_sweep.py — measure the pitch of every BASIC PLAY note.

For each of the 60 notes (O1 C to O5 B) this reads the note's delay value from
the Extended BASIC ROM on the running board, starts the ROM's own tone loop
with it through the WiFi debug API, and asks frequency_meter_mcp.py (running
on the PC wired to the board's audio jack) what it hears. It prints the heard
frequency against the loop's calculated one, with the board's frame rate and
audio rate correction.

Interrupts are masked while a note plays, so the reference is the tone loop
alone. A real PLAY sits about 0.8 % lower, because the 60 Hz interrupt takes
time from the loop.

    python play_pitch_sweep.py --board http://192.168.50.104 \\
        --audio http://lapreyco:8765/mcp --machines coco3,coco2 --json out.json

Needs the MCP SDK 1.x here too:  pip install "mcp>=1.2,<2"

What a healthy run looks like: every note within a few tenths of a percent,
60.0 FPS, rate correction near 0. A machine that cannot hold 60 FPS shows it
as flat notes with a matching negative rate correction.

Notes on method:
- Each note pauses the board briefly to load its delay value, which disturbs
  the audio queue; --settle is the wait before measuring. Offsets of 0.3 % or
  less are within what that settling can cause.
- Switching machines reboots the board; the original machine and CPU are
  restored at the end. CoCo 2 is always run with the 6809.
- RAM at $4000-$410F, $DF/$E0 and PIA1 CRB are saved and restored.
"""

import argparse
import asyncio
import json
import sys
import time
import urllib.parse
import urllib.request

try:
    from mcp import ClientSession
    from mcp.client.streamable_http import streamablehttp_client
except ModuleNotFoundError:
    sys.exit('Needs the MCP SDK 1.x:  pip install "mcp>=1.2,<2"')

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
CPU_HZ = 14318180 / 16                 # 0.894886 MHz

# Extended BASIC 1.1 / Super Extended BASIC PLAY internals (same in both ROMs).
TABLE16 = 0x9C62                       # O1-O2: 24 delay words
TABLE8 = 0x9C92                        # O3-O5: 36 delay bytes
LOOP16 = 0x9B49                        # tone loop using a 16-bit delay at ,Y
LOOP8 = 0x9B72                         # tone loop using an 8-bit delay at ,X
CODE_AT = 0x4000
DELAY_AT = 0x4100                      # byte delay here, word delay at +1

# ORCC #$50 ; LDA $FF23 ; ORA #8 ; STA $FF23 (sound enable) ; CLRA ; TFR A,DP
PREAMBLE = "1A50" + "B6FF23" + "8A08" + "B7FF23" + "4F1F8B"
CODE16 = PREAMBLE + "108E%04X" % (DELAY_AT + 1) + "7E%04X" % LOOP16   # LDY # ; JMP
CODE8 = PREAMBLE + "8E%04X" % DELAY_AT + "7E%04X" % LOOP8             # LDX # ; JMP

STRIDE_NOMINAL = 46362                 # hal_audio.cpp STRIDE_NOMINAL_Q16


class Board:
    def __init__(self, base):
        self.base = base.rstrip("/")

    def call(self, path, data=None, tries=15):
        body = urllib.parse.urlencode(data).encode() if data is not None else None
        err = None
        for _ in range(tries):
            try:
                req = urllib.request.Request(self.base + path, data=body)
                return json.loads(urllib.request.urlopen(req, timeout=8).read())
            except Exception as e:      # the board drops a request now and then
                err = e
                time.sleep(2)
        raise err

    def mem(self, addr, length):
        return bytes.fromhex(self.call(f"/api/mem?addr={addr}&len={length}")["data"])

    def switch(self, machine_type, cpu):
        self.call("/api/machine", {"type": machine_type, "cpu": cpu})
        time.sleep(14)                  # reboot
        self.call("/api/status")
        time.sleep(6)


def measure(url, **args):
    async def go():
        async with streamablehttp_client(url) as (r, w, _):
            async with ClientSession(r, w) as s:
                await s.initialize()
                res = await s.call_tool("measure_pitch", args)
                return json.loads(next(c.text for c in res.content if hasattr(c, "text")))
    return asyncio.run(go())


def loop_hz(octave, delay):
    """Frequency of the ROM tone loop alone, from its cycle counts."""
    if octave <= 2:
        return CPU_HZ / (4 * (26 + 8 * delay) + 3)
    return CPU_HZ / (4 * (25 + 5 * delay) + 3)


def sweep(board, audio_url, only, settle, seconds, rows):
    st = board.call("/api/status")
    machine, cpu = st["machine"], st["cpu"]
    t16, t8 = board.mem(TABLE16, 48), board.mem(TABLE8, 36)

    board.call("/api/pause", {})
    regs = board.call("/api/registers")
    saved_ram = board.call(f"/api/mem?addr={CODE_AT}&len=272")["data"]
    saved_vol = board.call("/api/mem?addr=223&len=2")["data"]        # $DF/$E0
    saved_crb = board.call("/api/mem?addr=65315&len=1")["data"]      # $FF23
    try:
        board.call("/api/mem", {"addr": 223, "data": "FC00"})        # high / low DAC values
        for octave in range(1, 6):
            for n in range(12):
                name = f"O{octave} {NOTE_NAMES[n]}"
                if only and name not in only:
                    continue
                if octave <= 2:
                    i = (octave - 1) * 24 + 2 * n
                    delay = (t16[i] << 8) | t16[i + 1]
                    data, code = "00%04X" % delay, CODE16
                else:
                    delay = t8[(octave - 3) * 12 + n]
                    data, code = "%02X0000" % delay, CODE8
                expect = loop_hz(octave, delay)

                board.call("/api/pause", {})
                board.call("/api/mem", {"addr": DELAY_AT, "data": data})
                board.call("/api/inject", {"addr": CODE_AT, "data": code,
                                           "pc": CODE_AT, "resume": "1"})
                time.sleep(settle)
                board.call("/api/status?audio_reset=1")
                m = measure(audio_url, seconds=seconds,
                            fmin=max(40.0, expect * 0.6), fmax=expect * 1.6)
                st = board.call("/api/status")
                a = st["audio"]
                row = {"machine": machine, "cpu": cpu, "note": name, "delay": delay,
                       "expect_hz": round(expect, 2), "heard_hz": m["frequency_hz"],
                       "diff_pct": round(100 * (m["frequency_hz"] / expect - 1), 2),
                       "spread_hz": m.get("windows", {}).get("spread_hz"),
                       "level_dbfs": m["level_dbfs"], "fps": st["fps"],
                       "rate_adj_pct": round(100.0 * a["stride_adj"] / STRIDE_NOMINAL, 2),
                       "underruns": a["underruns"]}
                rows.append(row)
                print("%s %-6s expect %8.2f  heard %8.2f  %+6.2f %%  spread %5.2f Hz"
                      "  | %s FPS  rate %+5.2f %%  underruns %d"
                      % (machine, name, expect, row["heard_hz"], row["diff_pct"],
                         row["spread_hz"] or 0.0, st["fps"], row["rate_adj_pct"],
                         row["underruns"]), flush=True)
    finally:
        board.call("/api/pause", {})
        board.call("/api/mem", {"addr": CODE_AT, "data": saved_ram})
        board.call("/api/mem", {"addr": 223, "data": saved_vol})
        board.call("/api/mem", {"addr": 65315, "data": saved_crb})
        board.call("/api/registers",
                   {k: regs[k] for k in ("pc", "d", "x", "y", "u", "s", "dp", "cc")})
        board.call("/api/resume", {})


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", default="http://192.168.50.104", help="debug API base URL")
    ap.add_argument("--audio", default="http://lapreyco:8765/mcp",
                    help="frequency_meter_mcp.py URL")
    ap.add_argument("--machines", default="coco3,coco2",
                    help="comma list of coco3, coco2 (coco2 runs with the 6809)")
    ap.add_argument("--only", default="", help='comma list of notes, e.g. "O2 A,O4 A"')
    ap.add_argument("--settle", type=float, default=0.9,
                    help="seconds to wait after starting a note (default 0.9)")
    ap.add_argument("--seconds", type=float, default=1.5,
                    help="capture length per note (default 1.5)")
    ap.add_argument("--json", default="", help="write all rows to this file")
    args = ap.parse_args()

    board = Board(args.board)
    only = {s.strip() for s in args.only.split(",") if s.strip()}
    start = board.call("/api/status")
    home = (start["machine_type"], int(start["cpu"]))
    print(f"board: {start['firmware']} {start['machine']} {start['cpu']}")

    rows = []
    try:
        for name in [m.strip() for m in args.machines.split(",") if m.strip()]:
            want = {"coco3": 4, "coco2": 3}[name]
            now = board.call("/api/status")
            # CoCo 2 with the 6309 is not a test configuration.
            cpu = 6809 if want == 3 else int(now["cpu"])
            if now["machine_type"] != want or int(now["cpu"]) != cpu:
                board.switch(want, cpu)
            sweep(board, args.audio, only, args.settle, args.seconds, rows)
    finally:
        now = board.call("/api/status")
        if (now["machine_type"], int(now["cpu"])) != home:
            board.switch(*home)
        print("board left as:", board.call("/api/status")["machine"])
        if args.json:
            with open(args.json, "w") as f:
                json.dump(rows, f, indent=1)


if __name__ == "__main__":
    main()
