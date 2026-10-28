#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Survey HVSC and play a random sample of its files through the host player, 3 s each, to find
tunes the C64 environment cannot play (a jam, or no SID writes at all).

  tools/hvsc_scan.py [N]          N files (default 200); HVSC at $HVSC or ~/Music/C64Music
Needs build/host/sidplay (make sidplay). Run with python3 -I.
"""
import sys, os, struct, random, subprocess, collections
root = os.path.expanduser(os.environ.get("HVSC", "~/Music/C64Music"))
files = []
for d, _, fs in os.walk(root):
    for f in fs:
        if f.endswith(".sid"): files.append(os.path.join(d, f))
files.sort()
kinds = collections.Counter(); ex = collections.defaultdict(list)
for p in files:
    with open(p, "rb") as fh: h = fh.read(0x7c)
    if len(h) < 0x76: continue
    magic = h[:4].decode("ascii", "replace"); ver = struct.unpack(">H", h[4:6])[0]
    play = struct.unpack(">H", h[0xc:0xe])[0]; flags = struct.unpack(">H", h[0x76:0x78])[0] if ver >= 2 else 0
    sid2 = h[0x7a] if ver >= 3 else 0; sid3 = h[0x7b] if ver >= 4 else 0
    k = magic + (" basic" if (magic == "RSID" and flags & 2) else "") + (" play0" if (magic == "PSID" and play == 0) else "") + \
        (" ntsc" if (flags >> 2) & 3 == 2 else "") + (" 2sid" if sid2 else "") + (" 3sid" if sid3 else "")
    kinds[k] += 1; ex[k].append(p)
print(len(files), "files"); 
for k, n in kinds.most_common(): print(f"{n:6d} {k}   e.g. {ex[k][len(ex[k])//3].replace(root,'')}")
# robustness: play 3 s of a random sample
random.seed(1)
sample = random.sample(files, int(sys.argv[1]) if len(sys.argv) > 1 else 200)
bad = []
for p in sample:
    r = subprocess.run(["build/host/sidplay", p, "-t", "3", "-q"], capture_output=True, text=True, errors="replace")
    out = r.stdout
    fault = "FAULT" in out or r.returncode != 0
    try:
        writes = int(out.split(" SID writes")[0].rsplit(" ", 1)[-1])
    except Exception:
        writes = -1
    if fault or writes < 10: bad.append((p.replace(root, ""), "fault" if fault else f"writes {writes}", out.strip().splitlines()[-3:] if out else r.stderr[-200:]))
print(f"\n{len(sample)} random files played for 3 s: {len(bad)} problems")
for b in bad: print(" ", b[0], b[1]); [print("     ", l) for l in b[2]]
