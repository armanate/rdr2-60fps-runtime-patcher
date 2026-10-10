#!/usr/bin/env python3
"""Host-side tests for rdr2-60fps.c.

Builds the payload for this machine (clang) with the PS5 calls stubbed
(test/stub), runs it against a fake console that holds the game's memory and
param.sfo, and checks what it wrote, what it showed and how it exited. No PS5 needed; this checks the logic, not the hardware.

    python3 test/run_tests.py
"""
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PORT = 17440

RVA = 0x5453029
CTX = bytes.fromhex(
    "8904e949ffc54d39f50f82e8fdffff8b"
    "05b2cfd5038b3dccd9d6038d70ff85c0"
    "0f44f0e82fad700185c00f88ac100000"
    "448b0568cfd503448b0d65cfd503488d")
ORIG, NEW = bytes.fromhex("0f44f0"), bytes.fromhex("31f690")
NO_GAME = (0, "", "")


def make_sfo(entries):
    """Minimal param.sfo with UTF-8 string values."""
    keys = b"".join(k.encode() + b"\0" for k, _ in entries)
    keys += b"\0" * (-len(keys) % 4)
    index, data, koff = b"", b"", 0
    for k, v in entries:
        val = v.encode() + b"\0"
        size = len(val) + (-len(val) % 4)
        index += struct.pack("<HHIII", koff, 0x0204, len(val), size, len(data))
        data += val.ljust(size, b"\0")
        koff += len(k) + 1
    key_start = 20 + len(index)
    return (b"\0PSF" + struct.pack("<IIII", 0x101, key_start,
                                     key_start + len(keys), len(entries))
            + index + keys + data)


def app_info(tid, title_off):
    """Raw sceKernelGetAppInfo bytes: app id, then junk, then the title id."""
    if not tid:
        return bytes(0x100)
    b = bytearray(0x100)
    b[0:4] = struct.pack("<I", 0x60000123)
    b[4:title_off] = bytes(range(1, title_off - 3))  # no capitals/digits run
    b[title_off:title_off + len(tid)] = tid.encode()
    b[0x40:0x50] = b"CUSA00000-APP000"[:16]  # a decoy after the real id
    return bytes(b)


class FakeConsole:
    """The PS5 side of test/stub/game_stub.c: running game, memory, param.sfo."""

    def __init__(self, tmp, fg_seq, base=0x400000, ctx=CTX, fail_reads=0,
                 reject_writes=False, title_off=0x10):
        self.fg_seq = list(fg_seq)
        self.title_off = title_off
        self.base = base
        self.target = base + RVA
        self.mem_start = self.target - 0x80
        self.mem = bytearray(0x100)
        self.mem[0x80 - 32:0x80 + 32] = ctx
        self.fail_reads = fail_reads
        self.reject_writes = reject_writes
        self.writes = []
        for _, tid, ver in self.fg_seq:
            if not tid:
                continue
            d = os.path.join(tmp, "appmeta", tid)
            os.makedirs(d, exist_ok=True)
            path = os.path.join(d, "param.sfo")
            if ver is None:
                if os.path.exists(path):
                    os.remove(path)
                continue
            with open(path, "wb") as f:
                f.write(make_sfo([("APP_VER", ver), ("TITLE_ID", tid),
                                  ("VERSION", "01.00")]))
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", PORT))
        self.srv.listen(8)
        self.alive = True
        threading.Thread(target=self._accept, daemon=True).start()

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

    def _read(self, addr, n):
        off = addr - self.mem_start
        if off < 0 or off + n > len(self.mem):
            return None
        if self.fail_reads > 0:
            self.fail_reads -= 1
            return None
        return bytes(self.mem[off:off + n])

    def _serve(self, c):
        st = lambda v: c.sendall(struct.pack("<I", v))
        try:
            op = self._recvn(c, 1)
            if op == b"F":
                pid, tid, _ = self.fg_seq.pop(0) if len(self.fg_seq) > 1 else self.fg_seq[0]
                c.sendall(struct.pack("<I", pid) + app_info(tid, self.title_off))
                return
            pid, addr, n = struct.unpack("<IQI", self._recvn(c, 16))
            if op == b"R":
                data = self._read(addr, n)
                if data is None:
                    st(1)
                else:
                    st(0)
                    c.sendall(data)
            elif op == b"W":
                data = self._recvn(c, n)
                if self.reject_writes:
                    st(1)
                    return
                off = addr - self.mem_start
                self.mem[off:off + n] = data
                self.writes.append((addr, data))
                st(0)
            elif op == b"B":
                c.sendall(struct.pack("<Q", self.base))
        except (ConnectionError, OSError, struct.error):
            pass
        finally:
            c.close()


def build(tmp):
    exe = os.path.join(tmp, "rdr2-60fps-host")
    subprocess.run(
        ["clang", "-Wall", "-Wextra", "-Werror", "-O1",
         "-I", os.path.join(ROOT, "test", "stub"),
         "-DHOST_TEST", f'-DLOG_DIR="{tmp}"', f"-DFAKE_PORT={PORT}",
         f'-DAPPMETA_DIR="{tmp}/appmeta"',
         os.path.join(ROOT, "src", "rdr2-60fps.c"),
         os.path.join(ROOT, "test", "stub", "notify_stub.c"),
         os.path.join(ROOT, "test", "stub", "game_stub.c"),
         "-o", exe], check=True)
    return exe


def run(exe, tmp, dbg, timeout=30):
    p = subprocess.run([exe], capture_output=True, text=True, timeout=timeout)
    dbg.close()
    with open(os.path.join(tmp, "rdr2-60fps.log")) as f:
        log = f.read()
    toasts = [l[6:] for l in p.stdout.splitlines() if l.startswith("TOAST ")]
    return p.returncode, toasts, log


RDR2_EU = (4242, "CUSA08519", "01.32")
results = []


def check(name, cond, detail=""):
    results.append(cond)
    print(f"  {'PASS' if cond else 'FAIL'}  {name}" + (f"  -- {detail}" if not cond else ""))


def scenario(title):
    print(f"\n[{title}]")


def main():
    tmp = tempfile.mkdtemp(prefix="rdr2-60fps-test-")
    exe = build(tmp)
    # these checks cover the 60fps patch alone; run_tests_combined.py covers
    # the resolution part
    with open(os.path.join(tmp, "scale.txt"), "w") as f:
        f.write("fake4k=0\n2160p=0\n")

    scenario("CUSA08519 v01.32 starts after the payload")
    d = FakeConsole(tmp, [NO_GAME, NO_GAME, RDR2_EU])
    rc, toasts, log = run(exe, tmp, d)
    check("exit 0", rc == 0, rc)
    check("one write of 31 f6 90 at 0x5853029", d.writes == [(0x5853029, NEW)], d.writes)
    check("'60FPS IS ON' toast, no prefix", any(t.startswith("60FPS IS ON") for t in toasts), toasts)
    check("log has firmware 13.60", "firmware 13.60" in log, log)
    check("log has version from param.sfo", "v01.32 pid 4242" in log, log)
    print("  toasts:", *toasts, sep="\n    ")

    scenario("title id at the SDK sample's offset (other firmwares)")
    d = FakeConsole(tmp, [NO_GAME, RDR2_EU], title_off=0x14)
    rc, toasts, _ = run(exe, tmp, d)
    check("exit 0", rc == 0, rc)
    check("written", d.writes == [(0x5853029, NEW)], d.writes)

    scenario("wrong game version (01.29)")
    d = FakeConsole(tmp, [NO_GAME, (4242, "CUSA08519", "01.29")])
    rc, toasts, _ = run(exe, tmp, d)
    check("exit 4", rc == 4, rc)
    check("nothing written", not d.writes, d.writes)
    check("version toast", any("01.29 is not supported" in t for t in toasts), toasts)

    scenario("different code around the patch site")
    bad = bytearray(CTX); bad[5] ^= 0xFF
    d = FakeConsole(tmp, [NO_GAME, RDR2_EU], ctx=bytes(bad))
    rc, toasts, _ = run(exe, tmp, d)
    check("exit 5", rc == 5, rc)
    check("nothing written", not d.writes, d.writes)
    check("mismatch toast", any("doesn't match" in t for t in toasts), toasts)

    scenario("already patched (original tool reported 'not found' here)")
    pre = bytearray(CTX); pre[32:35] = NEW
    d = FakeConsole(tmp, [NO_GAME, RDR2_EU], ctx=bytes(pre))
    rc, toasts, _ = run(exe, tmp, d)
    check("exit 0", rc == 0, rc)
    check("nothing written", not d.writes, d.writes)
    check("'already ON' toast", any("already ON" in t for t in toasts), toasts)

    scenario("unexpected target bytes")
    odd = bytearray(CTX); odd[32:35] = b"\xaa\xbb\xcc"
    d = FakeConsole(tmp, [NO_GAME, RDR2_EU], ctx=bytes(odd))
    rc, toasts, _ = run(exe, tmp, d)
    check("exit 6", rc == 6, rc)
    check("nothing written", not d.writes, d.writes)

    scenario("eboot at a non-default base (memory-map fallback)")
    d = FakeConsole(tmp, [NO_GAME, RDR2_EU], base=0x1000000)
    rc, toasts, log = run(exe, tmp, d)
    check("exit 0", rc == 0, rc)
    check("write at base+RVA", d.writes == [(0x1000000 + RVA, NEW)], d.writes)

    scenario("game memory not readable for the first reads (still loading)")
    d = FakeConsole(tmp, [NO_GAME, RDR2_EU], fail_reads=3)
    rc, toasts, log = run(exe, tmp, d)
    check("exit 0 after retries", rc == 0, rc)
    check("one write", d.writes == [(0x5853029, NEW)], d.writes)
    check("retries logged", "try 1/10" in log, log)

    scenario("RDR2 already open when the payload starts")
    d = FakeConsole(tmp, [RDR2_EU])
    rc, toasts, _ = run(exe, tmp, d)
    check("exit 0", rc == 0, rc)
    check("written", d.writes == [(0x5853029, NEW)], d.writes)
    check("'already open' warning toast", any("already open" in t for t in toasts), toasts)

    scenario("another game first, then RDR2")
    d = FakeConsole(tmp, [NO_GAME, (777, "CUSA00001", "01.00"), (777, "CUSA00001", "01.00"), RDR2_EU])
    rc, toasts, _ = run(exe, tmp, d)
    check("exit 0", rc == 0, rc)
    check("one 'not RDR2' toast", sum("not RDR2" in t for t in toasts) == 1, toasts)
    check("only RDR2 pid patched", d.writes == [(0x5853029, NEW)], d.writes)

    scenario("param.sfo missing: version unknown, byte check decides")
    d = FakeConsole(tmp, [NO_GAME, (4242, "CUSA08519", None)])
    rc, toasts, log = run(exe, tmp, d)
    check("exit 0", rc == 0, rc)
    check("written", d.writes == [(0x5853029, NEW)], d.writes)
    check("'version unknown' logged", "version unknown" in log, log)

    scenario("process list unavailable")
    d = FakeConsole(tmp, [NO_GAME])
    d.close()
    p = subprocess.run([exe], capture_output=True, text=True, timeout=30)
    toasts = [l[6:] for l in p.stdout.splitlines() if l.startswith("TOAST ")]
    check("exit 8", p.returncode == 8, p.returncode)
    check("'couldn't list' toast", any("couldn't list running apps" in t for t in toasts), toasts)

    scenario("write rejected")
    d = FakeConsole(tmp, [NO_GAME, RDR2_EU], reject_writes=True)
    rc, toasts, _ = run(exe, tmp, d)
    check("exit 7", rc == 7, rc)
    check("memory unchanged", d.mem[0x80:0x83] == ORIG, d.mem[0x80:0x83].hex())
    check("'write failed' toast", any("write failed after 3 tries" in t for t in toasts), toasts)

    scenario("payload loaded twice")
    d = FakeConsole(tmp, [NO_GAME])
    first = subprocess.Popen([exe], stdout=subprocess.PIPE, text=True)
    time.sleep(1.5)
    second = subprocess.run([exe], capture_output=True, text=True, timeout=10)
    log_ok = "started" in open(os.path.join(tmp, "rdr2-60fps.log")).read()
    first.kill(); first.wait(); d.close()
    check("second copy exits 1", second.returncode == 1, second.returncode)
    check("second copy didn't wipe the first one's log", log_ok)

    scenario("log is cleared on every start")
    with open(os.path.join(tmp, "rdr2-60fps.log"), "a") as f:
        f.write("OLD-RUN-MARKER\n")
    d = FakeConsole(tmp, [NO_GAME, RDR2_EU])
    rc, _, log = run(exe, tmp, d)
    check("old content gone", "OLD-RUN-MARKER" not in log)

    print(f"\n{sum(results)}/{len(results)} checks passed")
    print("last log:\n" + log)
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
