#!/usr/bin/env python3
"""Run the GIME FIRQ entry-count guest test on the board.
usage: firqtest.py <label> FAST MASK TINS RELOAD     (values as 6809 asm, e.g. $10)
"""
import json, os, subprocess, sys, tempfile, time, urllib.request, urllib.parse

H = os.environ.get("COCO_HOST", "http://192.168.50.111")
D = os.path.dirname(os.path.abspath(__file__))

def get(path):
    with urllib.request.urlopen(H + path, timeout=15) as r:
        return json.loads(r.read())

def post(path, **kw):
    data = urllib.parse.urlencode(kw).encode()
    with urllib.request.urlopen(H + path, data=data, timeout=15) as r:
        return r.read()

def mem(addr, n):
    return bytes.fromhex(get(f"/api/mem?addr={addr}&len={n}")["data"])

label, fast, mask, tins, reload = sys.argv[1:6]
src = open(os.path.join(D, "firqtest.tpl.asm")).read()
for k, v in (("FAST", fast), ("MASK", mask), ("TINS", tins), ("RELOAD", reload)):
    src = src.replace("@" + k + "@", v)
W = tempfile.mkdtemp(prefix="firqtest-")
asm = os.path.join(W, "firqtest.asm"); out = os.path.join(W, "firqtest.bin")
open(asm, "w").write(src)
subprocess.run(["lwasm", "--raw", "-o", out, asm], check=True)
code = open(out, "rb").read()

post("/api/reset"); time.sleep(4)
s0 = get("/api/status"); t0 = time.time()
post("/api/inject", addr="0x4000", data=code.hex(), pc="0x4000", resume="1")
while mem(0x4206, 1)[0] != 1:
    if time.time() - t0 > 120: sys.exit("timeout")
    time.sleep(1)
el = time.time() - t0
s1 = get("/api/status")
r = mem(0x4200, 6)
ent = int.from_bytes(r[0:4], "big"); frm = int.from_bytes(r[4:6], "big")
a0, a1 = s0["gime_acks"], s1["gime_acks"]
dacks = a1["firq"] - a0["firq"]; dzero = a1["firq_zero"] - a0["firq_zero"]
dfires = s1["gime_timer"]["fires"] - s0["gime_timer"]["fires"]
print(f"{label:22s} frames {frm}  entries {ent}  /frame {ent/frm:7.2f}  "
      f"acks {dacks}  zero {dzero} ({100*dzero/max(dacks,1):.1f}%)  "
      f"real/frame {(dacks-dzero)/frm:7.2f}  timer_fires {dfires}  "
      f"wall {el:.1f}s  frame_us {s1['speed']['frame_us_avg']}")
