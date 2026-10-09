#!/usr/bin/env python3
"""Host-side tests for rdr2-60fps.c.

Builds the payload for this machine (clang) with stubbed notifications, runs it
against a fake ps5debug-NG server, and checks what it wrote, what it showed
and how it exited. No PS5 needed; this checks the logic, not the hardware.

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
SUCCESS, ERROR = 0x80000000, 0xF0000001

RVA = 0x5453029
CTX = bytes.fromhex(
    "8904e949ffc54d39f50f82e8fdffff8b"
    "05b2cfd5038b3dccd9d6038d70ff85c0"
    "0f44f0e82fad700185c00f88ac100000"
    "448b0568cfd503448b0d65cfd503488d")
ORIG, NEW = bytes.fromhex("0f44f0"), bytes.fromhex("31f690")
NO_GAME = (0, "", "")


class FakeDebugger:
    """Just enough of the ps5debug-NG protocol for the payload."""

    def __init__(self, fg_seq, base=0x400000, ctx=CTX, fail_reads=0,
                 reject_writes=False):
        self.fg_seq = list(fg_seq)
        self.base = base
        self.target = base + RVA
        self.mem_start = self.target - 0x80
        self.mem = bytearray(0x100)
        self.mem[0x80 - 32:0x80 + 32] = ctx
        self.fail_reads = fail_reads
        self.reject_writes = reject_writes
        self.writes = []
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", PORT))
        self.srv.listen(4)
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
            while True:
                magic, cmd, ln = struct.unpack("<III", self._recvn(c, 12))
                assert magic == 0xFFAABBCC
                body = self._recvn(c, ln) if ln else b""
                if cmd == 0xBD000001:
                    c.sendall(struct.pack("<I", 3) + b"1.3")
                elif cmd == 0xBD000500:
                    c.sendall(struct.pack("<H", 1360))
                elif cmd == 0xBDDD0006:
                    pid, tid, ver = self.fg_seq.pop(0) if len(self.fg_seq) > 1 else self.fg_seq[0]
                    st(SUCCESS)
                    c.sendall(struct.pack("<I16s64s40s16s", pid, tid.encode(),
                                          b"", b"", ver.encode()))
                elif cmd == 0xBDAA0002:
                    pid, addr, n = struct.unpack("<IQI", body)
                    data = self._read(addr, n)
                    if data is None:
                        st(ERROR)
                    else:
                        st(SUCCESS)
                        c.sendall(data)
                elif cmd == 0xBDAA0003:
                    pid, addr, n = struct.unpack("<IQI", body)
                    st(SUCCESS)
                    data = self._recvn(c, n)
                    if self.reject_writes:
                        st(ERROR)
                        continue
                    off = addr - self.mem_start
                    self.mem[off:off + n] = data
                    self.writes.append((addr, data))
                    st(SUCCESS)
                elif cmd == 0xBDAA0004:
                    st(SUCCESS)
                    entries = [("executable", self.base, self.base + 0x6000000, 5),
                               ("libc.prx", 0x800000000, 0x800100000, 5)]
                    c.sendall(struct.pack("<I", len(entries)))
                    for name, s, e, prot in entries:
                        c.sendall(struct.pack("<32sQQQH", name.encode(), s, e, 0, prot))
                else:
                    st(ERROR)
        except (ConnectionError, OSError, struct.error):
            c.close()


def build(tmp):
    exe = os.path.join(tmp, "rdr2-60fps-host")
    subprocess.run(
        ["clang", "-Wall", "-Wextra", "-Werror", "-O1",
         "-I", os.path.join(ROOT, "test", "stub"),
         f'-DLOG_DIR="{tmp}"', f"-DDBG_PORT={PORT}",
         os.path.join(ROOT, "src", "rdr2-60fps.c"),
         os.path.join(ROOT, "test", "stub", "notify_stub.c"),
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

    scenario("CUSA08519 v01.32 starts after the payload")
    d = FakeDebugger([NO_GAME, NO_GAME, RDR2_EU])
    rc, toasts, log = run(exe, tmp, d)
    check("exit 0", rc == 0, rc)
    check("one write of 31 f6 90 at 0x5853029", d.writes == [(0x5853029, NEW)], d.writes)
    check("'60fps ON' toast", any("60fps ON" in t for t in toasts), toasts)
    check("log has firmware 13.60", "firmware 13.60" in log, log)
    print("  toasts:", *toasts, sep="\n    ")

    scenario("wrong game version (01.29)")
    d = FakeDebugger([NO_GAME, (4242, "CUSA08519", "01.29")])
    rc, toasts, _ = run(exe, tmp, d)
    check("exit 4", rc == 4, rc)
    check("nothing written", not d.writes, d.writes)
    check("version toast", any("01.29 is not supported" in t for t in toasts), toasts)

    scenario("different code around the patch site")
    bad = bytearray(CTX); bad[5] ^= 0xFF
    d = FakeDebugger([NO_GAME, RDR2_EU], ctx=bytes(bad))
    rc, toasts, _ = run(exe, tmp, d)
    check("exit 5", rc == 5, rc)
    check("nothing written", not d.writes, d.writes)
    check("mismatch toast", any("doesn't match" in t for t in toasts), toasts)

    scenario("already patched (original tool reported 'not found' here)")
    pre = bytearray(CTX); pre[32:35] = NEW
    d = FakeDebugger([NO_GAME, RDR2_EU], ctx=bytes(pre))
    rc, toasts, _ = run(exe, tmp, d)
    check("exit 0", rc == 0, rc)
    check("nothing written", not d.writes, d.writes)
    check("'already ON' toast", any("already ON" in t for t in toasts), toasts)

    scenario("unexpected target bytes")
    odd = bytearray(CTX); odd[32:35] = b"\xaa\xbb\xcc"
    d = FakeDebugger([NO_GAME, RDR2_EU], ctx=bytes(odd))
    rc, toasts, _ = run(exe, tmp, d)
    check("exit 6", rc == 6, rc)
    check("nothing written", not d.writes, d.writes)

    scenario("eboot at a non-default base (memory-map fallback)")
    d = FakeDebugger([NO_GAME, RDR2_EU], base=0x1000000)
    rc, toasts, log = run(exe, tmp, d)
    check("exit 0", rc == 0, rc)
    check("write at base+RVA", d.writes == [(0x1000000 + RVA, NEW)], d.writes)

    scenario("game memory not readable for the first reads (still loading)")
    d = FakeDebugger([NO_GAME, RDR2_EU], fail_reads=3)
    rc, toasts, log = run(exe, tmp, d)
    check("exit 0 after retries", rc == 0, rc)
    check("one write", d.writes == [(0x5853029, NEW)], d.writes)
    check("retries logged", "try 1/10" in log, log)

    scenario("RDR2 already open when the payload starts")
    d = FakeDebugger([RDR2_EU])
    rc, toasts, _ = run(exe, tmp, d)
    check("exit 0", rc == 0, rc)
    check("written", d.writes == [(0x5853029, NEW)], d.writes)
    check("'already open' warning toast", any("already open" in t for t in toasts), toasts)

    scenario("another game first, then RDR2")
    d = FakeDebugger([NO_GAME, (777, "CUSA00001", "01.00"), (777, "CUSA00001", "01.00"), RDR2_EU])
    rc, toasts, _ = run(exe, tmp, d)
    check("exit 0", rc == 0, rc)
    check("one 'not RDR2' toast", sum("not RDR2" in t for t in toasts) == 1, toasts)
    check("only RDR2 pid patched", d.writes == [(0x5853029, NEW)], d.writes)

    scenario("debugger rejects the write")
    d = FakeDebugger([NO_GAME, RDR2_EU], reject_writes=True)
    rc, toasts, _ = run(exe, tmp, d)
    check("exit 7", rc == 7, rc)
    check("memory unchanged", d.mem[0x80:0x83] == ORIG, d.mem[0x80:0x83].hex())
    check("'write failed' toast", any("write failed after 3 tries" in t for t in toasts), toasts)

    scenario("payload loaded twice")
    d = FakeDebugger([NO_GAME])
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
    d = FakeDebugger([NO_GAME, RDR2_EU])
    rc, _, log = run(exe, tmp, d)
    check("old content gone", "OLD-RUN-MARKER" not in log)

    print(f"\n{sum(results)}/{len(results)} checks passed")
    print("last log:\n" + log)
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
