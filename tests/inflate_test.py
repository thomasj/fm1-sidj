#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""firmware/src/app/inflate.c against zlib: what zlib compresses, at every level and strategy (stored,
fixed and dynamic blocks), the FM-1's unpacker must give back byte for byte; HVSC tunes when HVSC
is on this machine, generated data otherwise. Damaged input gives -1 or wrong bytes, never a crash
or a write past the buffer (built with the address and undefined-behaviour sanitizers)."""
import os
import random
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "app/inflate.c"
int main(int argc, char **argv)
{
    static uint8_t in[1 << 20];
    uint32_t max = (uint32_t)atoi(argv[1]);
    uint8_t *out = malloc(max ? max : 1);
    size_t n = fread(in, 1, sizeof in, stdin);
    int r = inflate_raw(in, (uint32_t)n, out, max);
    if (r < 0) return 3;
    fwrite(out, 1, (size_t)r, stdout);
    return 0;
}
"""


def deflate(data, level=9, strategy=zlib.Z_DEFAULT_STRATEGY, mem=8):
    c = zlib.compressobj(level, zlib.DEFLATED, -15, mem, strategy)
    return c.compress(data) + c.flush()


def main():
    tmp = Path(tempfile.mkdtemp())
    src, exe = tmp / "h.c", tmp / "h"
    src.write_text(HARNESS)
    cc = os.environ.get("CC", "cc")
    subprocess.run([cc, "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-Wall",
                    "-Wno-unused-function", f"-I{ROOT}/firmware/src", "-o", str(exe), str(src)], check=True)
    run = lambda z, mx: subprocess.run([str(exe), str(mx)], input=z, capture_output=True)

    rnd = random.Random(1)
    samples = [b"", b"a", b"abc" * 5000, bytes(rnd.getrandbits(8) for _ in range(61440)), bytes(61440)]
    hvsc = Path(os.path.expanduser("~/Music/C64Music"))
    if hvsc.is_dir():
        files = sorted(hvsc.glob("MUSICIANS/*/*/*.sid"))
        rnd.shuffle(files)
        samples += [f.read_bytes() for f in files[:60]]
    print(f"== inflate_test ({len(samples)} samples)")
    bad = 0
    variants = [dict(level=0), dict(level=1), dict(level=6), dict(level=9), dict(strategy=zlib.Z_FIXED),
                dict(strategy=zlib.Z_HUFFMAN_ONLY), dict(strategy=zlib.Z_RLE), dict(mem=1)]
    for s in samples:
        for v in variants:
            r = run(deflate(s, **v), 70000)
            if r.returncode or r.stdout != s:
                bad += 1
                print(f"  FAIL {len(s)} bytes, {v}: rc {r.returncode}")
    print(f"  {'ok  ' if not bad else 'FAIL'} every variant gives the bytes back")

    z = deflate(samples[2])
    fail = 0
    if run(z, len(samples[2]) - 1).returncode != 3:
        fail += 1
    if run(z[:len(z) // 2], 70000).returncode != 3:
        fail += 1
    print(f"  {'ok  ' if not fail else 'FAIL'} too small a buffer and cut input give -1")
    bad += fail

    crashes = 0
    pool = [deflate(s) for s in samples if 100 < len(s) < 20000][:12] or [z]
    for i in range(1500):
        d = bytearray(rnd.choice(pool))
        for _ in range(rnd.randint(1, 8)):
            d[rnd.randrange(len(d))] = rnd.getrandbits(8)
        r = run(bytes(d), 70000)
        if r.returncode not in (0, 3):
            crashes += 1
            print("  FAIL damaged input:", r.returncode, r.stderr[:300])
            break
    print(f"  {'ok  ' if not crashes else 'FAIL'} 1500 damaged inputs: no crash, no stray write")
    bad += crashes
    print("inflate tests passed" if not bad else f"inflate tests: {bad} failed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
