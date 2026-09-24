#!/usr/bin/env python3
"""
Logging TCP proxy for DriveWire-over-TCP (Becker) traffic.

Sits between the emulator and a DriveWire server and hex-dumps every chunk
in both directions with timestamps, so protocol desyncs can be traced:

    tools/dw_proxy.py [--listen 65505] [--target 127.0.0.1:65504] [--log FILE]

Point the emulator at the proxy port (/api/bus?mode=1&host=<pi>&port=65505).
"""

import argparse
import socket
import sys
import threading
import time

T0 = time.time()


def log(out, lock, tag, data):
    with lock:
        out.write(f"{time.time() - T0:9.3f} {tag} {len(data):4d}  {data.hex(' ')}\n")
        out.flush()


def pump(src, dst, tag, out, lock):
    try:
        while True:
            data = src.recv(4096)
            if not data:
                break
            log(out, lock, tag, data)
            dst.sendall(data)
    except OSError:
        pass
    finally:
        for s in (src, dst):
            try:
                s.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[1])
    ap.add_argument("--listen", type=int, default=65505)
    ap.add_argument("--target", default="127.0.0.1:65504")
    ap.add_argument("--log", default="-")
    args = ap.parse_args()
    host, port = args.target.rsplit(":", 1)

    out = sys.stdout if args.log == "-" else open(args.log, "a")
    lock = threading.Lock()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("", args.listen))
    srv.listen(1)
    print(f"proxy :{args.listen} -> {host}:{port}", file=sys.stderr, flush=True)
    while True:
        c, addr = srv.accept()
        try:
            s = socket.create_connection((host, int(port)))
        except OSError as e:
            print(f"target {host}:{port} unreachable: {e}", file=sys.stderr, flush=True)
            c.close()
            continue
        for x in (c, s):
            x.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        with lock:
            out.write(f"{time.time() - T0:9.3f} ---- connect {addr[0]}:{addr[1]}\n")
            out.flush()
        threading.Thread(target=pump, args=(c, s, "C>S", out, lock), daemon=True).start()
        threading.Thread(target=pump, args=(s, c, "S>C", out, lock), daemon=True).start()


if __name__ == "__main__":
    sys.exit(main())
