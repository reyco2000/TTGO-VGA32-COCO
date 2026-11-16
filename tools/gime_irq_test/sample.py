#!/usr/bin/env python3
"""Sample a running program: frame time, speed and GIME interrupt counters over N s.
usage: sample.py <label> [seconds]"""
import json, os, sys, time, urllib.request
H = os.environ.get("COCO_HOST", "http://192.168.50.111")

def st():
    for i in range(4):
        try:
            with urllib.request.urlopen(H + "/api/status", timeout=8) as r:
                return json.loads(r.read())
        except Exception:
            time.sleep(1)
    sys.exit("status unreachable")

label = sys.argv[1]; secs = float(sys.argv[2]) if len(sys.argv) > 2 else 10
a = st(); t0 = time.time(); time.sleep(secs); b = st(); dt = time.time() - t0
s = b["speed"]; fr = s["fps"] * dt
ga, gb = a["gime_acks"], b["gime_acks"]
fires = b["gime_timer"]["fires"] - a["gime_timer"]["fires"]
fa = gb["firq"] - ga["firq"]; fz = gb["firq_zero"] - ga["firq_zero"]
ia = gb["irq"] - ga["irq"]; iz = gb["irq_zero"] - ga["irq_zero"]
au = b["audio"]
print(f"{label:18s} {b['cpu']} mhz {s['mhz']:.4f} ratio {s['ratio']:.3f} fps {s['fps']:.2f} "
      f"frame_us {s['frame_us_avg']}/{s['frame_us_max']} | timer fires/f {fires/fr:.1f} "
      f"tins {b['gime_timer']['tins']} reload {b['gime_timer']['reload']} | "
      f"FIRQ acks/f {fa/fr:.1f} zero {100*fz/max(fa,1):.0f}% | IRQ acks/f {ia/fr:.1f} "
      f"zero {100*iz/max(ia,1):.0f}% | underruns {au['underruns']} late {au['late_refills']}")
