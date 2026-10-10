#!/usr/bin/env python3
"""Host-side tests for rdr2-60fps.c with its resolution patch (fake 4K).

Builds the 60fps payload and runs it against fake_rdr2's fake game
(settings node, apply step, mode snap, size function, cave interpreter), with
the 60fps patch site added to its memory.

    python3 test/run_tests_combined.py
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time

import fake_rdr2 as rs
from run_tests import CTX as FPS_CTX, NEW as FPS_NEW, ROOT

FPS = 0x5853029
SCALE_CAVE = rs.CAVE
NO_GAME, RDR2 = rs.NO_GAME, rs.RDR2


class Console(rs.FakeConsole):
    """The resolution fake game plus the 60fps site."""

    def __init__(self, tmp, fg_seq, fps_ctx=FPS_CTX, **kw):
        super().__init__(tmp, fg_seq, **kw)
        self.regions[FPS - 0x80] = bytearray(0x100)
        self._put(FPS - 32, fps_ctx)


def build(tmp):
    exe = os.path.join(tmp, "rdr2-60fps-host")
    subprocess.run(
        ["clang", "-Wall", "-Wextra", "-Werror", "-O1",
         "-I", os.path.join(ROOT, "test", "stub"),
         "-DHOST_TEST", f'-DLOG_DIR="{tmp}/out"', f"-DFAKE_PORT={rs.PORT}",
         f'-DAPPMETA_DIR="{tmp}/appmeta"', "-DRES_SIZES_POLLS=20",
         os.path.join(ROOT, "src", "rdr2-60fps.c"),
         os.path.join(ROOT, "test", "stub", "notify_stub.c"),
         os.path.join(ROOT, "test", "stub", "game_stub.c"),
         "-o", exe], check=True)
    return exe


def run(exe, tmp, con, settings=None, timeout=60):
    out = os.path.join(tmp, "out")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    if settings is not None:
        with open(os.path.join(out, "scale.txt"), "w") as f:
            f.write(settings)
    p = subprocess.run([exe], capture_output=True, text=True, timeout=timeout)
    time.sleep(0.05)
    con.close()
    with open(os.path.join(out, "rdr2-60fps.log")) as f:
        log = f.read()
    # whole notifications, including multi-line ones
    toasts = [t.rstrip("\n") for t in ("\n" + p.stdout).split("\nTOAST ")[1:]]
    return p.returncode, toasts, log


results = []


def check(name, cond, detail=""):
    results.append(cond)
    print(f"  {'PASS' if cond else 'FAIL'}  {name}" + (f"  -- {detail}" if not cond else ""))


def scenario(title):
    print(f"\n[{title}]")


def has(toasts, s):
    return any(s in t for t in toasts)


def addrs(con):
    return [hex(a) for a, _ in con.writes]


def main():
    tmp = tempfile.mkdtemp(prefix="rdr2-combined-test-")
    exe = build(tmp)

    scenario("defaults: scale.txt created, 4K checkerboard + 60fps")
    con = Console(tmp, [NO_GAME, RDR2])
    rc, toasts, log = run(exe, tmp, con)
    check("exit 0", rc == 0, rc)
    conf = open(os.path.join(tmp, "out", "scale.txt")).read()
    check("scale.txt has fake4k=1 and 2160p=0",
          "\nfake4k=1\n2160p=0\n" in conf, conf)
    check("fake game ran clean", not con.errors, con.errors)
    check("writes: fake cave, snap, fake jump, then 60fps",
          addrs(con) == [hex(rs.FAKE_CAVE), hex(rs.SNAP), hex(rs.FAKE), hex(FPS)],
          addrs(con))
    check("60fps bytes", con.get(FPS, 3) == FPS_NEW, con.get(FPS, 3).hex())
    check("sizes 1920x2160 checkerboard -> 3840x2160",
          con.sizes() == (1920, 2160, 1920, 2160, 3840, 2160), con.sizes())
    check("final toast: 60fps + resolution",
          has(toasts, "60FPS IS ON, Enjoy.\n"
                      "Resolution: 1920x2160 checkerboard, 3840x2160 frame\n"), toasts)
    check("settings logged", "fake 4K on, 4K scale stock (0)" in log, log)
    print("  toasts:", *toasts, sep="\n    ")

    scenario("2160p=2: 1440p checkerboard")
    con = Console(tmp, [NO_GAME, RDR2])
    rc, toasts, _ = run(exe, tmp, con, settings="fake4k=1\n2160p=2\n")
    check("exit 0", rc == 0, rc)
    check("fake game ran clean", not con.errors, con.errors)
    check("writes: caves, snap, jumps, 60fps",
          addrs(con) == [hex(rs.FAKE_CAVE), hex(SCALE_CAVE), hex(rs.SNAP),
                         hex(rs.FAKE), hex(rs.SITE), hex(FPS)], addrs(con))
    check("node and index 2", con.render_scale == (2, 2), con.render_scale)
    check("sizes 1280x1440 checkerboard, final 3840x2160",
          con.sizes() == (1920, 2160, 1280, 1440, 3840, 2160), con.sizes())
    check("toast", has(toasts, "Resolution: 1280x1440 checkerboard, 3840x2160 frame"),
          toasts)

    scenario("fake4k=0, 2160p=0: 60fps only")
    con = Console(tmp, [NO_GAME, RDR2])
    rc, toasts, _ = run(exe, tmp, con, settings="fake4k=0\n2160p=0\n")
    check("exit 0", rc == 0, rc)
    check("only the 60fps write", addrs(con) == [hex(FPS)], addrs(con))
    check("no resolution line", not has(toasts, "Resolution:"), toasts)

    scenario("2160p=12 (above 1) is ignored")
    con = Console(tmp, [NO_GAME, RDR2])
    rc, toasts, log = run(exe, tmp, con, settings="fake4k=1\n2160p=12\n")
    check("exit 0", rc == 0, rc)
    check("warned", "ignoring 2160p=12" in log, log)
    check("stock 4K checkerboard",
          con.sizes() == (1920, 2160, 1920, 2160, 3840, 2160), con.sizes())

    scenario("real 4K TV: stock 4K checkerboard")
    con = Console(tmp, [NO_GAME, RDR2], mode=(3840, 2160))
    rc, toasts, _ = run(exe, tmp, con)
    check("exit 0", rc == 0, rc)
    check("fake game ran clean", not con.errors, con.errors)
    check("sizes stock 4K", con.sizes() == (1920, 2160, 1920, 2160, 3840, 2160),
          con.sizes())
    check("toast", has(toasts, "Resolution: 1920x2160 checkerboard, 3840x2160 frame"),
          toasts)

    scenario("RDR2 already open: 60fps only")
    con = Console(tmp, [RDR2])
    rc, toasts, _ = run(exe, tmp, con)
    check("exit 0", rc == 0, rc)
    check("only the 60fps write", addrs(con) == [hex(FPS)], addrs(con))
    check("resolution skipped toast",
          has(toasts, "resolution: RDR2 was already open"), toasts)

    scenario("settings applied before the payload: 60fps only")
    con = Console(tmp, [NO_GAME, RDR2], t_defaults="pre", t_apply="pre",
                  t_sizing=0.5)
    rc, toasts, _ = run(exe, tmp, con)
    check("exit 0", rc == 0, rc)
    check("only the 60fps write", addrs(con) == [hex(FPS)], addrs(con))
    check("too-late toast", has(toasts, "resolution: RDR2 already set it"), toasts)

    scenario("resolution code differs: 60fps only")
    bad = bytearray(rs.FAKE_CTX)
    bad[50] ^= 0xff
    con = Console(tmp, [NO_GAME, RDR2], fake_ctx=bytes(bad))
    rc, toasts, _ = run(exe, tmp, con)
    check("exit 0", rc == 0, rc)
    check("only the 60fps write", addrs(con) == [hex(FPS)], addrs(con))
    check("mismatch toast",
          has(toasts, "resolution: game code doesn't match v1.32 (fake 4K), 60fps only."),
          toasts)

    scenario("60fps site differs after the resolution patch: says so")
    bad = bytearray(FPS_CTX)
    bad[5] ^= 0xff
    con = Console(tmp, [NO_GAME, RDR2], fps_ctx=bytes(bad))
    rc, toasts, _ = run(exe, tmp, con)
    check("exit 5", rc == 5, rc)
    check("resolution-still-on toast",
          has(toasts, "the resolution patch is on; restart RDR2 to undo it."), toasts)

    scenario("loaded again in the same session")
    con = Console(tmp, [NO_GAME, RDR2])
    run(exe, tmp, con)
    saved = {a: con.get(a, n) for a, n in ((rs.TAIL, 0x4000), (rs.FAKE, 15),
                                           (rs.SNAP, 2), (FPS - 32, 64),
                                           (rs.SIZES, 24), (rs.VIDEO, 8))}
    con = Console(tmp, [NO_GAME, RDR2], t_apply=None, t_sizing=None)
    for a, b in saved.items():
        con._put(a, b)
    rc, toasts, _ = run(exe, tmp, con)
    check("exit 0", rc == 0, rc)
    check("no writes", not con.writes, con.writes)
    check("'already ON' toast", has(toasts, "60fps already ON"), toasts)

    shutil.rmtree(tmp, ignore_errors=True)
    print(f"\n{sum(results)}/{len(results)} checks passed")
    sys.exit(0 if all(results) else 1)


if __name__ == "__main__":
    main()
