#!/usr/bin/env python3
"""
Minimal DriveWire 3/4 server for testing the emulator's Becker port.

Listens on TCP (default 65504) — the same endpoint XRoar's Becker port and
FujiNet-PC use — and serves .DSK images as DriveWire drives 0-3. Implements
what HDB-DOS and a basic NitrOS-9 boot need; serial/vport ops are answered
with "no data".

    tools/dw_test_server.py [-p PORT] [-v] [drive0.dsk [drive1.dsk ...]]

Every opcode is logged with -v, which makes it a protocol trace for testing.
"""

import argparse
import datetime
import os
import socket
import sys

OP_NOP       = 0x00
OP_TIME      = 0x23
OP_INIT      = 0x49
OP_TERM      = 0x54
OP_DWINIT    = 0x5A
OP_READ      = 0x52
OP_READEX    = 0xD2
OP_REREAD    = 0x72
OP_REREADEX  = 0xF2
OP_WRITE     = 0x57
OP_REWRITE   = 0x77
OP_GETSTAT   = 0x47
OP_SETSTAT   = 0x53
OP_SERREAD   = 0x43
OP_SERREADM  = 0x63
OP_SERWRITE  = 0xC3
OP_SERWRITEM = 0x64
OP_SERGETSTAT = 0x44
OP_SERSETSTAT = 0xC4
OP_SERINIT   = 0x45
OP_SERTERM   = 0xC5
OP_PRINT     = 0x50
OP_PRINTFLUSH = 0x46
OP_NAMEOBJ_MOUNT  = 0x01
OP_NAMEOBJ_CREATE = 0x02
OP_RESET1    = 0xFE
OP_RESET2    = 0xFF
OP_RESET3    = 0xF8
OP_FASTWRITE_FIRST = 0x80
OP_FASTWRITE_LAST  = 0x8E

E_OK     = 0x00
E_WP     = 0xF2   # write protect
E_CRC    = 0xF3   # checksum error
E_READ   = 0xF4
E_WRITE  = 0xF5
E_NOTRDY = 0xF6

SECTOR = 256

NAMES = {v: k for k, v in globals().items() if k.startswith("OP_") and isinstance(v, int)}


class Drive:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "r+b") if os.access(path, os.W_OK) else open(path, "rb")
        self.readonly = not os.access(path, os.W_OK)

    def read(self, lsn):
        self.f.seek(lsn * SECTOR)
        data = self.f.read(SECTOR)
        return data + bytes(SECTOR - len(data))

    def write(self, lsn, data):
        self.f.seek(lsn * SECTOR)
        self.f.write(data)
        self.f.flush()


class Conn:
    def __init__(self, sock, drives, verbose):
        self.s = sock
        self.drives = drives
        self.verbose = verbose
        self.last_read = None    # (drive, lsn) for REREAD*

    def log(self, msg):
        if self.verbose:
            print(msg, flush=True)

    def recv(self, n):
        buf = b""
        while len(buf) < n:
            chunk = self.s.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("peer closed")
            buf += chunk
        return buf

    def send(self, data):
        self.s.sendall(bytes(data))

    def drive_lsn(self):
        hdr = self.recv(4)
        return hdr[0], (hdr[1] << 16) | (hdr[2] << 8) | hdr[3]

    def do_readex(self, drive, lsn):
        d = self.drives.get(drive)
        data = d.read(lsn) if d else bytes(SECTOR)
        self.send(data)
        cs = int.from_bytes(self.recv(2), "big")
        if d is None:
            err = E_NOTRDY
        elif cs != (sum(data) & 0xFFFF):
            err = E_CRC
        else:
            err = E_OK
        self.send([err])
        self.last_read = (drive, lsn)
        self.log(f"  drive {drive} lsn {lsn} -> err ${err:02X}")

    def do_write(self, drive, lsn):
        data = self.recv(SECTOR)
        cs = int.from_bytes(self.recv(2), "big")
        d = self.drives.get(drive)
        if d is None:
            err = E_NOTRDY
        elif cs != (sum(data) & 0xFFFF):
            err = E_CRC
        elif d.readonly:
            err = E_WP
        else:
            d.write(lsn, data)
            err = E_OK
        self.send([err])
        self.log(f"  drive {drive} lsn {lsn} <- err ${err:02X}")

    def serve(self):
        while True:
            op = self.recv(1)[0]
            name = NAMES.get(op, f"${op:02X}")
            if OP_FASTWRITE_FIRST <= op <= OP_FASTWRITE_LAST:
                self.recv(1)
                continue
            if op not in (OP_SERREAD, OP_NOP):
                self.log(f"{name}")

            if op in (OP_NOP, OP_INIT, OP_TERM, OP_RESET1, OP_RESET2, OP_RESET3, OP_PRINTFLUSH):
                pass
            elif op == OP_DWINIT:
                ver = self.recv(1)[0]
                self.log(f"  client driver version ${ver:02X}")
                self.send([0x00])
            elif op == OP_TIME:
                t = datetime.datetime.now()
                self.send([t.year - 1900, t.month, t.day, t.hour, t.minute, t.second])
            elif op in (OP_READEX, OP_REREADEX):
                drive, lsn = self.drive_lsn()
                self.do_readex(drive, lsn)
            elif op in (OP_READ, OP_REREAD):
                # Legacy DW3 read: data then 2-byte checksum, error first.
                drive, lsn = self.drive_lsn()
                d = self.drives.get(drive)
                if d is None:
                    self.send([E_NOTRDY])
                else:
                    data = d.read(lsn)
                    self.send([E_OK])
                    self.send(data)
                    self.send((sum(data) & 0xFFFF).to_bytes(2, "big"))
            elif op in (OP_WRITE, OP_REWRITE):
                drive, lsn = self.drive_lsn()
                self.do_write(drive, lsn)
            elif op in (OP_GETSTAT, OP_SETSTAT):
                drive, code = self.recv(2)
                self.log(f"  drive {drive} code ${code:02X}")
            elif op == OP_SERREAD:
                self.send([0x00, 0x00])        # no vport data waiting
            elif op == OP_SERREADM:
                self.recv(2)
            elif op in (OP_SERWRITE,):
                self.recv(2)
            elif op == OP_SERWRITEM:
                _chan, n = self.recv(2)
                self.recv(n)
            elif op == OP_SERGETSTAT:
                self.recv(2)
            elif op == OP_SERSETSTAT:
                _chan, code = self.recv(2)
                if code == 0x28:               # SS.ComSt carries a 26-byte block
                    self.recv(26)
            elif op in (OP_SERINIT, OP_SERTERM, OP_PRINT):
                self.recv(1)
            elif op in (OP_NAMEOBJ_MOUNT, OP_NAMEOBJ_CREATE):
                n = self.recv(1)[0]
                self.recv(n)
                self.send([0x00])              # not found
            else:
                self.log(f"  unknown opcode ${op:02X} — ignored")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[1])
    ap.add_argument("-p", "--port", type=int, default=65504)
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("disks", nargs="*", help="images for drives 0-3")
    args = ap.parse_args()

    drives = {i: Drive(p) for i, p in enumerate(args.disks[:4])}
    for i, d in drives.items():
        print(f"drive {i}: {d.path}{' (read-only)' if d.readonly else ''}")

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("", args.port))
    srv.listen(1)
    print(f"listening on TCP {args.port}", flush=True)
    while True:
        sock, addr = srv.accept()
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        print(f"client connected: {addr[0]}:{addr[1]}", flush=True)
        try:
            Conn(sock, drives, args.verbose).serve()
        except (ConnectionError, OSError) as e:
            print(f"client gone: {e}", flush=True)
        finally:
            sock.close()


if __name__ == "__main__":
    sys.exit(main())
