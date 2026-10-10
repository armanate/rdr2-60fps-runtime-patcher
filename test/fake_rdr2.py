"""Fake RDR2 v1.32 for the host-side tests: the PS5 side of
test/stub/game_stub.c plus the game's startup, on timers that start when the
payload first sees it:
  - defaults (eboot 0x783e20) store the TV's video mode height (0x7f90c08);
  - apply (0x7861e0, r12 = the live settings node on the heap) copies the
    node's screen size to the video mode (0x7865e8) and its scalingMode to
    the index global (0x786846); patched sites are followed into the caves,
    whose bytes run through a tiny x86 interpreter;
  - sizing: the fill + mode snap (0xa24ca0/0xa25126), then 0x5840e0: base =
    TV mode (W halved when checkerboarding), scaled = base x ratio[index],
    final = base (W doubled when checkerboarding). The renderer's view of the
    node is recorded too.
"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

from run_tests import ROOT, app_info, make_sfo

PORT = 17444
BASE = 0x400000
SITE = 0x786846
RESUME = 0x786854
CAVE = 0x794bf80
TAIL = 0x7948000
SCREENH = 0x7f90c08
NODE = 0x200100000
INDEX = 0x87ec408
SIZES = 0x87ec40c
NO_GAME = (0, "", "")
RDR2 = (4242, "CUSA08519", "01.32")

SITE_CTX = bytes.fromhex(
    "e8f5190601 4183bc24a401000000 8b154e4e0c08 be01000000 0f94055acf3a09"
    "418b84243c010000 8905b45b0608 418b8424d4010000 89056e97e208 b8030000".replace(" ", ""))
ORIG = SITE_CTX[32:46]
COND = 0x1eaffa0
COND_CTX = bytes.fromhex(
    "554889e5415653813e255cf3b2410f94"
    "c6813dedff6f07800700000f97c0813d"
    "e4ff6f07380400000f97c308c3e82ecd"
    "0a0585c00f95c020d84430f034015b41")
COND_SITE = COND + 0x2b
FAKE = 0x7865e8
FAKE_RESUME = 0x7865f7
FAKE_CAVE = 0x794bf00
VIDEO = 0x95affa8
FAKE_ORIG = bytes.fromhex("498b8424bc010000488905b199e208")
FAKE_CTX = bytes.fromhex(
    "abe308ff901801000084c07413488b05"
    "04abe308488d3dfdaae30831f6ff5048") + FAKE_ORIG + bytes.fromhex(
    "e8d4b2ffffc5fa1185c0feffffe827b7ff")
SNAP = 0xa2512e
SNAP_CTX = bytes.fromhex(
    "984c010000488db550ffffff488d8408"
    "50010000480f47f8e8353fe3044885c0"
    "7419c5fa6f00c5f97ec1c4e37916c001"
    "c4c17a7f8694000000eb0e418b8e9400")
NUM = [1, 2, 3, 4, 5, 7, 9, 11, 14, 29, 5, 3, 7, 2, 5]
DEN = [2, 3, 4, 5, 6, 8, 10, 12, 15, 30, 4, 2, 4, 1, 2]


def u32(b):
    return struct.unpack("<I", b)[0]


def s32(b):
    return struct.unpack("<i", b)[0]


class FakeConsole:
    """The PS5 side of test/stub/game_stub.c, plus a fake game."""

    def __init__(self, tmp, fg_seq, mode=(1920, 1080), node_scale=0,
                 t_defaults=0.2, t_apply=0.4, t_sizing=0.8, ctx=SITE_CTX,
                 cond=COND_CTX, fake_ctx=FAKE_CTX, tail_junk=False, ver="01.32"):
        self.fg_seq = list(fg_seq)
        self.mode = mode
        self.times = [(t_defaults, self.defaults), (t_apply, self.apply),
                      (t_sizing, self.sizing)]
        self.regions = {
            SITE - 0x1000: bytearray(0x2000),
            TAIL: bytearray(0x4000),
            0x7f90000: bytearray(0x1000),
            NODE: bytearray(0x1000),
            0x87ec000: bytearray(0x1000),
            COND - 0x100: bytearray(0x200),
            FAKE - 0x100: bytearray(0x200),
            SNAP - 0x100: bytearray(0x200),
            0x95af000: bytearray(0x1000),
        }
        self._put(SITE - 32, ctx)
        self._put(COND, cond)
        self._put(FAKE - 32, fake_ctx)
        self._put(SNAP - 32, SNAP_CTX)
        # the live settings node: the TV's mode, checkerboard on for 4K
        self._put(NODE + 0x1bc, struct.pack("<II", *mode))
        self._put(NODE + 0xe9, bytes([1 if mode[1] > 2159 else 0]))
        self._put(NODE + 0x13c, struct.pack("<I", node_scale))
        if tail_junk:
            self._put(TAIL + 0x100, b"\x01")
        self.writes = []
        self.errors = []
        self.render_scale = None
        self.applied = 0
        self.lock = threading.Lock()
        self.started = False
        for t, fn in self.times:
            if t == "pre":
                fn()
        for _, tid, _ in self.fg_seq:
            if not tid:
                continue
            d = os.path.join(tmp, "appmeta", tid)
            os.makedirs(d, exist_ok=True)
            with open(os.path.join(d, "param.sfo"), "wb") as f:
                f.write(make_sfo([("APP_VER", ver), ("TITLE_ID", tid),
                                  ("VERSION", "01.00")]))
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", PORT))
        self.srv.listen(16)
        self.alive = True
        threading.Thread(target=self._accept, daemon=True).start()

    def _find(self, addr, n):
        for start, m in self.regions.items():
            off = addr - start
            if 0 <= off and off + n <= len(m):
                return m, off
        return None, None

    def _put(self, addr, data):
        m, off = self._find(addr, len(data))
        m[off:off + len(data)] = data

    def get(self, addr, n):
        m, off = self._find(addr, n)
        if m is None:
            raise ValueError(f"fake game read outside memory {addr:#x}")
        return bytes(m[off:off + n])

    def sizes(self):
        return struct.unpack("<6I", self.get(SIZES, 24))

    def node_scale(self):
        return u32(self.get(NODE + 0x13c, 4))

    def run_cave(self, addr, r12, resume):
        """Interpret a cave: only the instructions the payload may emit.
        Must end in a jmp to `resume`."""
        rax, flags = 0, None
        r12d = lambda b: r12 + s32(b[4:8])
        for _ in range(40):
            b = self.get(addr, 13)
            if b[:2] == b"\x8b\x05":  # mov eax, [rip+d]
                rax = u32(self.get(addr + 6 + s32(b[2:6]), 4))
                addr += 6
            elif b[:4] == b"\x41\x8b\x84\x24":  # mov eax, [r12+d]
                rax = u32(self.get(r12d(b), 4))
                addr += 8
            elif b[:4] == b"\x49\x8b\x84\x24":  # mov rax, [r12+d]
                rax = struct.unpack("<Q", self.get(r12d(b), 8))[0]
                addr += 8
            elif b[0] == 0x3d:  # cmp eax, imm32
                flags = (rax & 0xffffffff, u32(b[1:5]))
                addr += 5
            elif b[:4] == b"\x41\x81\xbc\x24":  # cmp dword [r12+d], imm32
                flags = (u32(self.get(r12d(b), 4)), u32(b[8:12]))
                addr += 12
            elif b[0] in (0x75, 0x76, 0xeb):  # jne / jbe / jmp rel8
                take = {0x75: flags[0] != flags[1], 0x76: flags[0] <= flags[1],
                        0xeb: True}[b[0]]
                addr += 2 + (struct.unpack("<b", b[1:2])[0] if take else 0)
            elif b[:4] == b"\x41\xc7\x84\x24":  # mov dword [r12+d], imm32
                self._put(r12d(b), b[8:12])
                addr += 12
            elif b[:4] == b"\x41\xc6\x84\x24":  # mov byte [r12+d], imm8
                self._put(r12d(b), b[8:9])
                addr += 9
            elif b[:2] == b"\x89\x05":  # mov [rip+d], eax
                tgt = addr + 6 + s32(b[2:6])
                if tgt != INDEX:
                    raise ValueError(f"cave stores eax to {tgt:#x}")
                self._put(tgt, struct.pack("<I", rax & 0xffffffff))
                addr += 6
            elif b[:3] == b"\x48\x89\x05":  # mov [rip+d], rax
                tgt = addr + 7 + s32(b[3:7])
                if tgt != VIDEO:
                    raise ValueError(f"cave stores rax to {tgt:#x}")
                self._put(tgt, struct.pack("<Q", rax))
                addr += 7
            elif b[0] == 0xe9:  # jmp rel32
                tgt = addr + 5 + s32(b[1:5])
                if tgt != resume:
                    raise ValueError(f"cave jumps to {tgt:#x}, not {resume:#x}")
                return
            else:
                raise ValueError(f"unexpected cave bytes {b[:4].hex()} at {addr:#x}")
        raise ValueError("cave runs away")

    def follow(self, site, n, orig, resume):
        """True if the site is clean; runs the cave if it's our jmp."""
        cur = self.get(site, n)
        if cur == orig:
            return True
        if cur[0] == 0xe9 and cur[5:] == b"\x90" * (n - 5):
            self.run_cave(site + 5 + s32(cur[1:5]), NODE, resume)
            return False
        raise ValueError(f"site {site:#x} is {cur.hex()}")

    def defaults(self):
        self._put(SCREENH, struct.pack("<I", self.mode[1]))

    def apply(self):
        """0x7861e0: copy the node's screen size to the video mode
        (0x7865e8), later scalingMode to the index (0x786846)."""
        self.applied += 1
        if self.follow(FAKE, 15, FAKE_ORIG, FAKE_RESUME):
            self._put(VIDEO, self.get(NODE + 0x1bc, 8))
        if self.follow(SITE, 14, ORIG, RESUME):
            self._put(INDEX, self.get(NODE + 0x13c, 4))

    def sizing(self):
        """0xa24ca0 fill + 0xa25126 snap, then 0x5840e0."""
        w, h = struct.unpack("<II", self.get(NODE + 0x1bc, 8))
        tv_modes = [self.mode] + ([(1920, 1080)] if self.mode[1] > 1080 else [])
        if self.get(SNAP, 2) == b"\x74\x19":  # snap to the TV's own modes
            if (w, h) not in tv_modes:
                w, h = self.mode
        elif self.get(SNAP, 2) != b"\xeb\x19":
            raise ValueError("snap site")
        cb = self.get(NODE + 0xe9, 1) == b"\x01"
        if cb:
            w >>= 1
        idx = u32(self.get(INDEX, 4))
        num = den = 1
        if 1 <= idx <= 15:
            num, den = NUM[idx - 1], DEN[idx - 1]
        self.render_scale = (idx, self.node_scale())
        self.video = struct.unpack("<II", self.get(VIDEO, 8))
        self._put(SIZES, struct.pack("<6I", w, h, w * num // den, h * num // den,
                                     w * 2 if cb else w, h))

    def _timeline(self):
        t0 = time.monotonic()
        for when, fn in sorted(((t, f) for t, f in self.times
                                if isinstance(t, float)), key=lambda x: x[0]):
            time.sleep(max(0.0, t0 + when - time.monotonic()))
            with self.lock:
                try:
                    fn()
                except Exception as e:  # surfaced by the checks
                    self.errors.append(repr(e))

    def close(self):
        self.alive = False
        self.srv.close()

    def _accept(self):
        while self.alive:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            threading.Thread(target=self._serve, args=(c,), daemon=True).start()

    @staticmethod
    def _recvn(c, n):
        b = b""
        while len(b) < n:
            chunk = c.recv(n - len(b))
            if not chunk:
                raise ConnectionError
            b += chunk
        return b

    def _serve(self, c):
        try:
            op = self._recvn(c, 1)
            if op == b"F":
                pid, tid, _ = self.fg_seq.pop(0) if len(self.fg_seq) > 1 else self.fg_seq[0]
                if tid == RDR2[1] and not self.started:
                    self.started = True
                    threading.Thread(target=self._timeline, daemon=True).start()
                c.sendall(struct.pack("<I", pid) + app_info(tid, 0x10))
                return
            pid, addr, n = struct.unpack("<IQI", self._recvn(c, 16))
            if op == b"R":
                with self.lock:
                    m, off = self._find(addr, n)
                    data = None if m is None else bytes(m[off:off + n])
                c.sendall(struct.pack("<I", 1) if data is None
                          else struct.pack("<I", 0) + data)
            elif op == b"W":
                data = self._recvn(c, n)
                with self.lock:
                    m, _ = self._find(addr, n)
                    if m is not None:
                        self._put(addr, data)
                        self.writes.append((addr, data))
                c.sendall(struct.pack("<I", 0 if m is not None else 1))
            elif op == b"B":
                c.sendall(struct.pack("<Q", BASE))
        except (ConnectionError, OSError, struct.error):
            pass
        finally:
            c.close()
