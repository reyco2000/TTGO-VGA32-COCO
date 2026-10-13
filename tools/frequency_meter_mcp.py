#!/usr/bin/env python3
# ============================================================
#        CoCo 2&3 Emulator for ESP32-TTGO-VGA32-COCO
#   (C) 2026 Reinaldo Torres / CoCo Byte Club
#   https://github.com/reyco2000/TTGO-VGA32-COCO
#   Co-developed with Claude Code
#   GPL-3.0-or-later License
# ============================================================
#  File   : tools/frequency_meter_mcp.py
#  Module : Audio test tool — frequency_meter.py as an MCP server,
#           so an agent can measure the emulator's sound output
# ============================================================
"""
frequency_meter_mcp.py — the pitch meter as an MCP server.

Runs on the PC that hears the board (the TTGO's 3.5 mm audio output wired to
one of its audio inputs) and serves measurements over HTTP, so an agent on
another machine can ask "what is playing right now?" instead of having someone
read frequency_meter.py's screen and paste it.

Setup, on the PC with the audio input:
    pip install "mcp>=1.2" sounddevice numpy
    python frequency_meter_mcp.py --list-devices      # find the input
    python frequency_meter_mcp.py --device 0          # serve on port 8765

Then register it where the agent runs, for example with Claude Code:
    claude mcp add --transport http coco-audio http://<pc-address>:8765/mcp

Tools:
    list_inputs      capture devices on this PC
    measure_pitch    dominant frequency, note, cents, level, strongest peaks
    measure_level    RMS and peak level, DC offset, clipping, flat stretches

Only numbers leave this PC: no tool returns recorded audio. The server still
listens on the network, so run it on a trusted LAN and stop it when done.
"""

import argparse
import sys
import threading

import numpy as np
import sounddevice as sd
from mcp.server.fastmcp import FastMCP

from frequency_meter import freq_to_note, refine_peak

DEFAULT_DEVICE = None          # set from --device in main()
SILENCE_DBFS = -55.0           # below this RMS level a capture is reported as silent
WINDOW_S = 0.75                # analysis window, same as frequency_meter.py
HOP_S = 0.25                   # step between windows when checking stability
MAX_SECONDS = 20.0

# The server is reached by its LAN address, so the SDK's localhost-only Host
# header check (meant for servers bound to 127.0.0.1) has to be off.
try:
    from mcp.server.transport_security import TransportSecuritySettings
    mcp = FastMCP("coco-audio", transport_security=TransportSecuritySettings(
        enable_dns_rebinding_protection=False))
except ImportError:                  # older SDK without the check
    mcp = FastMCP("coco-audio")
_capture_lock = threading.Lock()     # one recording at a time on the input


def _capture(seconds, device):
    """Record mono float32 audio. Returns (samples, samplerate)."""
    seconds = float(min(max(seconds, 0.1), MAX_SECONDS))
    dev = DEFAULT_DEVICE if device is None else device
    srate = int(sd.query_devices(dev, "input")["default_samplerate"])
    with _capture_lock:
        data = sd.rec(int(seconds * srate), samplerate=srate, channels=1,
                      dtype="float32", device=dev)
        sd.wait()
    return data[:, 0], srate


def _dbfs(x):
    return float(20.0 * np.log10(x + 1e-12))


def _spectrum(samples, srate):
    n = len(samples)
    mag = np.abs(np.fft.rfft(samples * np.hanning(n)))
    freqs = np.fft.rfftfreq(n, 1.0 / srate)
    return freqs[1:], mag[1:]                      # drop the DC bin


def _dominant(samples, srate, fmin, fmax):
    """Interpolated frequency of the strongest spectral peak in [fmin, fmax]."""
    freqs, mag = _spectrum(samples, srate)
    band = (freqs >= fmin) & (freqs <= fmax)
    if not band.any():
        return 0.0
    idx = int(np.argmax(np.where(band, mag, 0.0)))
    return refine_peak(freqs, mag, idx)


def _top_peaks(samples, srate, fmin, fmax, count):
    """Strongest local maxima of the spectrum, as Hz and dB below the top one."""
    freqs, mag = _spectrum(samples, srate)
    band = (freqs >= fmin) & (freqs <= fmax)
    is_peak = np.zeros(len(mag), dtype=bool)
    is_peak[1:-1] = (mag[1:-1] > mag[:-2]) & (mag[1:-1] >= mag[2:])
    cand = np.flatnonzero(is_peak & band)
    if len(cand) == 0:
        return []
    cand = cand[np.argsort(-mag[cand])][:count]
    top = mag[cand[0]]
    return [{"hz": round(refine_peak(freqs, mag, int(i)), 2),
             "db": round(_dbfs(mag[i] / top), 1)} for i in cand]


@mcp.tool()
def list_inputs() -> list:
    """List this PC's audio capture devices: index, name, channels, sample
    rate, and which one measurements use by default."""
    out = []
    default_in = sd.default.device[0] if DEFAULT_DEVICE is None else DEFAULT_DEVICE
    for i, d in enumerate(sd.query_devices()):
        if d["max_input_channels"] > 0:
            out.append({"index": i, "name": d["name"],
                        "channels": d["max_input_channels"],
                        "samplerate": int(d["default_samplerate"]),
                        "default": i == default_in})
    return out


@mcp.tool()
def measure_pitch(seconds: float = 1.5, fmin: float = 40.0, fmax: float = 5000.0,
                  device: int | None = None) -> dict:
    """Listen for `seconds` and report the dominant frequency.

    Returns frequency_hz (interpolated, about 0.01 Hz on a steady tone), the
    nearest note with its offset in cents, level_dbfs, and the strongest
    spectral peaks. `windows` gives the frequency in each 0.75 s window across
    the capture (0.25 s apart) with its min, max and spread, so a pitch that
    moves during the capture is visible. `silent` is true when the level is
    below -55 dBFS; the frequency is then meaningless.

    The tone must already be playing when this is called and last for the
    whole capture. Search only between fmin and fmax (Hz).
    """
    samples, srate = _capture(seconds, device)
    level = _dbfs(np.sqrt(np.mean(samples ** 2)))
    result = {"samplerate": srate, "seconds": round(len(samples) / srate, 3),
              "level_dbfs": round(level, 1), "silent": level < SILENCE_DBFS}

    freq = _dominant(samples, srate, fmin, fmax)
    note, octave, _, cents = freq_to_note(freq)
    result.update({"frequency_hz": round(freq, 2), "note": f"{note}{octave}",
                   "cents": round(float(cents), 1),
                   "peaks": _top_peaks(samples, srate, fmin, fmax, 6)})

    win, hop = int(WINDOW_S * srate), int(HOP_S * srate)
    if len(samples) >= win:
        per = [round(_dominant(samples[s:s + win], srate, fmin, fmax), 2)
               for s in range(0, len(samples) - win + 1, hop)]
        result["windows"] = {"hz": per, "min": min(per), "max": max(per),
                             "spread_hz": round(max(per) - min(per), 2)}
    return result


@mcp.tool()
def measure_level(seconds: float = 1.0, device: int | None = None) -> dict:
    """Listen for `seconds` and report signal level, without pitch analysis.

    Returns rms_dbfs and peak_dbfs, dc_offset, the fraction of samples at
    full scale (clipping), and `flat_ms`: the longest stretch where the signal
    stayed within 0.1 % of full scale of one value. A long flat stretch in the
    middle of a tone is a dropout; in silence the whole capture is flat.
    """
    samples, srate = _capture(seconds, device)
    step = np.abs(np.diff(samples)) < 0.001
    longest = run = 0
    for flat in step:
        run = run + 1 if flat else 0
        if run > longest:
            longest = run
    rms = _dbfs(np.sqrt(np.mean(samples ** 2)))
    return {"samplerate": srate, "seconds": round(len(samples) / srate, 3),
            "rms_dbfs": round(rms, 1),
            "peak_dbfs": round(_dbfs(np.max(np.abs(samples))), 1),
            "dc_offset": round(float(np.mean(samples)), 4),
            "clipped_fraction": round(float(np.mean(np.abs(samples) >= 0.999)), 5),
            "flat_ms": round(1000.0 * longest / srate, 1),
            "silent": rms < SILENCE_DBFS}


def main():
    global DEFAULT_DEVICE
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--device", type=int, default=None,
                    help="input device index (default: system default input)")
    ap.add_argument("--list-devices", action="store_true", help="show devices and exit")
    ap.add_argument("--host", default="0.0.0.0",
                    help="address to listen on (default: all interfaces)")
    ap.add_argument("--port", type=int, default=8765, help="TCP port (default 8765)")
    args = ap.parse_args()

    if args.list_devices:
        for d in list_inputs():
            print(f"  {d['index']:3d}  {d['name']}  ({d['samplerate']} Hz)")
        return

    DEFAULT_DEVICE = args.device
    try:
        name = sd.query_devices(DEFAULT_DEVICE, "input")["name"]
    except Exception as e:
        print(f"Could not open audio input {args.device}: {e}")
        print("Run with --list-devices to see valid inputs.")
        sys.exit(1)

    mcp.settings.host = args.host
    mcp.settings.port = args.port
    print(f"coco-audio MCP: input '{name}', http://{args.host}:{args.port}/mcp")
    mcp.run(transport="streamable-http")


if __name__ == "__main__":
    main()
