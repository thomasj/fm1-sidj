#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""Songs on the FM-1 running SiDJ: list, send, fetch, delete and play .sid files over USB-MIDI.

  tools/fm1_sid.py list
  tools/fm1_sid.py put SONG.sid [SONG2.sid ...]        (files or directories; sent in sorted order)
  tools/fm1_sid.py get N OUT.sid                        (N as `list` numbers them, from 1)
  tools/fm1_sid.py delete N
  tools/fm1_sid.py play N [SUBSONG]
  tools/fm1_sid.py compact                              (fetch everything live, wipe the pool, send it back)
  tools/fm1_sid.py wipe                                 (erase the whole pool)
  tools/fm1_sid.py output 0|1|2                         (the output: SID, ASID, both; not saved)
  tools/fm1_sid.py stats [SECONDS]                      (the audio load: mean and worst half buffer, once a second)
  tools/fm1_sid.py usb 0|1                              (USB audio: the mix, or the mix and the voices; not saved)
  tools/fm1_sid.py rx PARAM [VOICE] [VALUE]             (a remix event, as the panel sends it: tempo 0 100 is +100 %,
                                                         lfo_target 1 1 targets voice 2's LFO one step on; reset)
  --port PATTERN picks the MIDI port (default: the first whose name contains FM-1)
  --hvsc DIR     the HVSC root (default ~/Music/C64Music): Songlengths.md5 gives each file its song lengths

Protocol (firmware/src/app/songs.c): F0 7D 'S' cmd payload F7, the payload packed 7 data bytes
-> 8 MIDI bytes (a byte of the seven high bits first). Needs mido and python-rtmidi.
"""
import argparse
import hashlib
import os
import re
import struct
import sys
import time
from pathlib import Path

import mido

INFO, ENTRY, PUT_BEGIN, PUT_DATA, PUT_END, GET, DELETE, WIPE, PLAY, STATS, OUTPUT, RX, USB = range(1, 14)
MAX_LEN = 64


def pack7(data):
    out = bytearray()
    for i in range(0, len(data), 7):
        chunk = data[i:i + 7]
        out.append(sum((b >> 7) << j for j, b in enumerate(chunk)))
        out.extend(b & 0x7F for b in chunk)
    return bytes(out)


def unpack7(data):
    out = bytearray()
    i = 0
    while i < len(data):
        hi = data[i]
        i += 1
        for j in range(7):
            if i >= len(data):
                break
            out.append(data[i] | ((hi >> j) & 1) << 7)
            i += 1
    return bytes(out)


def crc16(data):
    c = 0
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) if c & 0x8000 else c << 1
        c &= 0xFFFF
    return c


class Device:
    def __init__(self, pattern):
        names = [n for n in mido.get_output_names() if pattern.lower() in n.lower()]
        if not names:
            raise SystemExit(f"no MIDI port matching {pattern!r}; outputs: {mido.get_output_names()}")
        self.out = mido.open_output(names[0])
        self.inp = mido.open_input([n for n in mido.get_input_names() if pattern.lower() in n.lower()][0])
        self.port = names[0]

    def ask(self, cmd, payload=b"", timeout=3.0, tries=3):
        for attempt in range(tries):
            self.out.send(mido.Message("sysex", data=bytes([0x7D, ord("S"), cmd]) + pack7(payload)))
            t0 = time.time()
            while time.time() - t0 < timeout:
                for m in self.inp.iter_pending():
                    d = bytes(m.data) if m.type == "sysex" else b""
                    if len(d) >= 3 and d[0] == 0x7D and d[1] == ord("S") and d[2] == (0x40 | cmd):
                        return unpack7(d[3:])
                time.sleep(0.002)
            if attempt + 1 < tries:
                sys.stderr.write(f"\n  no answer to command {cmd}, sending it again ({attempt + 2}/{tries})\n")
        raise SystemExit(f"no answer to command {cmd} from {self.port}. Is the FM-1 running SiDJ, and not busy "
                         f"writing flash or updating? Try again; --port picks another MIDI port.")


class Songlengths:
    """HVSC's DOCUMENTS/Songlengths.md5: md5 of the file -> song lengths in deciseconds"""
    def __init__(self, hvsc):
        self.table = {}
        p = Path(hvsc).expanduser() / "DOCUMENTS" / "Songlengths.md5"
        if not p.exists():
            return
        for line in p.read_text(errors="replace").splitlines():
            m = re.match(r"([0-9a-f]{32})=(.*)", line)
            if not m:
                continue
            lens = []
            for t in m.group(2).split():
                mm = re.match(r"(\d+):(\d+)(?:\.(\d+))?", t)
                if mm:
                    ds = (int(mm.group(1)) * 60 + int(mm.group(2))) * 10 + (int((mm.group(3) or "0")[:1]))
                    lens.append(min(ds, 65535))
            self.table[m.group(1)] = lens

    def lookup(self, data):
        md5 = hashlib.md5(data).hexdigest()
        return md5, self.table.get(md5, [])


def nsids(data):
    """the SIDs a file asks for, as the player's parser (sidfile.c) reads the header's second and
    third SID address (version 3 and 4): even, $D420 to $DFE0, not $D800 to $DDF0"""
    ok = lambda b: 0x42 <= b <= 0xFE and b & 1 == 0 and not 0x80 <= b <= 0xDF
    ver = data[4] << 8 | data[5]
    second = ver >= 3 and len(data) > 0x7A and ok(data[0x7A])
    third = ver >= 4 and len(data) > 0x7B and ok(data[0x7B])
    return 1 + second + (second and third)


def hvsc_str(b):
    return b.split(b"\0")[0].decode("latin-1")


def do_info(dev):
    r = dev.ask(INFO)
    size, used = struct.unpack("<II", r[:8])
    n = r[8] | r[9] << 8
    return size, used, n


def entries(dev):
    _, _, n = do_info(dev)
    out = []
    for k in range(n):
        r = dev.ask(ENTRY, bytes([k & 0xFF, k >> 8]))
        if r[0]:
            continue
        size = struct.unpack("<I", r[1:5])[0]
        nlen = r[5]
        lens = [r[6 + 2 * i] | r[7 + 2 * i] << 8 for i in range(nlen)]
        s = r[6 + 2 * nlen:]
        out.append((k, size, lens, hvsc_str(s[0:32]), hvsc_str(s[32:64]), hvsc_str(s[64:96])))
    return out


def fmt_len(ds):
    return f"{ds // 600}:{ds // 10 % 60:02d}"


def do_list(dev):
    size, used, n = do_info(dev)
    print(f"  {n} songs, {used * 100 // size}% of {size // 1024} KB used")
    for k, sz, lens, name, author, released in entries(dev):
        ln = " ".join(fmt_len(x) for x in lens) if lens else "?"
        print(f"  {k + 1:3d}. {name:32s} {author:24s} {sz:6d} B  {ln}")


def put_one(dev, path, songlengths, quiet=False):
    data = Path(path).read_bytes()
    if data[:4] not in (b"PSID", b"RSID"):
        print(f"  {path}: not a SID file, skipped")
        return False
    if nsids(data) > 1:
        print(f"  {path}: a {nsids(data)}SID tune, skipped (SiDJ plays one-SID tunes)")
        return False
    md5, lens = songlengths.lookup(data)
    lens = lens[:MAX_LEN]
    hdr = struct.pack("<IB", len(data), len(lens)) + bytes.fromhex(md5) + b"".join(struct.pack("<H", x) for x in lens)
    if dev.ask(PUT_BEGIN, hdr)[0]:
        raise SystemExit(f"{path}: the FM-1 refused the file ({len(data)} bytes; the limit is 61440)")
    for off in range(0, len(data), 256):
        chunk = data[off:off + 256]
        for attempt in range(3):
            r = dev.ask(PUT_DATA, off.to_bytes(3, "little") + chunk, timeout=4.0)
            if r[0] == 0 and int.from_bytes(r[1:4], "little") == off:
                break
            if r[0] == 1:
                sys.stderr.write("\n  the FM-1 has no transfer open: starting it again\n")
                if dev.ask(PUT_BEGIN, hdr)[0]:
                    raise SystemExit("the FM-1 refused the file")
        else:
            raise SystemExit(f"chunk at {off} not accepted")
        if not quiet:
            sys.stdout.write(f"\r  {Path(path).name}: {100 * (off + len(chunk)) // len(data):3d}%")
            sys.stdout.flush()
    status = dev.ask(PUT_END, crc16(data).to_bytes(2, "little"), timeout=30.0)[0]
    if not quiet:
        print()
    if status:
        raise SystemExit("the FM-1 did not keep the file: " + {1: "no transfer", 2: "bytes missing", 3: "CRC mismatch", 4: "too big",
                                                             5: "the pool is full (try `compact`)", 6: "flash write failed",
                                                             7: "a 2SID / 3SID tune (SiDJ plays one-SID tunes)"}.get(status, str(status)))
    print(f"  {Path(path).name}: {len(data)} bytes, {len(lens)} song length(s){'' if lens else ' (not in Songlengths.md5)'}")
    return True


def do_put(dev, paths, songlengths):
    files = []
    for p in paths:
        p = Path(p)
        if p.is_dir():
            files += sorted(x for x in p.rglob("*.sid"))
        else:
            files.append(p)
    for f in files:
        put_one(dev, f, songlengths)


def do_get(dev, k, path):
    r = dev.ask(ENTRY, bytes([k & 0xFF, k >> 8]))
    if r[0]:
        raise SystemExit(f"no song {k + 1}")
    size = struct.unpack("<I", r[1:5])[0]
    data = bytearray()
    while len(data) < size:
        want = min(256, size - len(data))
        r = dev.ask(GET, bytes([k & 0xFF, k >> 8]) + len(data).to_bytes(3, "little") + want.to_bytes(2, "little"))
        if len(r) < 3 or int.from_bytes(r[0:3], "little") != len(data):
            raise SystemExit("bad answer while reading")
        data += r[3:]
    Path(path).write_bytes(bytes(data))
    print(f"  song {k + 1} -> {path} ({size} bytes)")


def do_compact(dev, songlengths):
    live = entries(dev)
    tmp = []
    for k, sz, lens, name, author, released in live:
        r = bytearray()
        while len(r) < sz:
            want = min(256, sz - len(r))
            x = dev.ask(GET, bytes([k & 0xFF, k >> 8]) + len(r).to_bytes(3, "little") + want.to_bytes(2, "little"))
            r += x[3:]
        tmp.append((bytes(r), lens))
    print(f"  {len(tmp)} songs fetched; wiping the pool")
    if dev.ask(WIPE, b"WIPE", timeout=60.0)[0]:
        raise SystemExit("wipe failed")
    for data, lens in tmp:
        md5 = hashlib.md5(data).hexdigest()
        hdr = struct.pack("<IB", len(data), len(lens)) + bytes.fromhex(md5) + b"".join(struct.pack("<H", x) for x in lens)
        if dev.ask(PUT_BEGIN, hdr)[0]:
            raise SystemExit("refused while sending back")
        for off in range(0, len(data), 256):
            dev.ask(PUT_DATA, off.to_bytes(3, "little") + data[off:off + 256], timeout=4.0)
        if dev.ask(PUT_END, crc16(data).to_bytes(2, "little"), timeout=30.0)[0]:
            raise SystemExit("a song was not kept while sending back")
    print("  done")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="FM-1")
    ap.add_argument("--hvsc", default=os.environ.get("HVSC", "~/Music/C64Music"))
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list")
    p = sub.add_parser("put"); p.add_argument("files", nargs="+")
    p = sub.add_parser("get"); p.add_argument("n", type=int); p.add_argument("out")
    p = sub.add_parser("delete"); p.add_argument("n", type=int)
    p = sub.add_parser("play"); p.add_argument("n", type=int); p.add_argument("subsong", type=int, nargs="?", default=0)
    sub.add_parser("compact")
    sub.add_parser("wipe")
    p = sub.add_parser("output"); p.add_argument("mode", type=int, choices=[0, 1, 2])
    p = sub.add_parser("stats"); p.add_argument("seconds", type=int, nargs="?", default=5)
    p = sub.add_parser("usb"); p.add_argument("mode", type=int, choices=[0, 1])
    p = sub.add_parser("rx"); p.add_argument("param"); p.add_argument("voice", type=int, nargs="?", default=0)
    p.add_argument("value", type=int, nargs="?", default=0)
    a = ap.parse_args()
    dev = Device(a.port)
    if a.cmd == "list":
        do_list(dev)
    elif a.cmd == "put":
        do_put(dev, a.files, Songlengths(a.hvsc))
    elif a.cmd == "get":
        do_get(dev, a.n - 1, a.out)
    elif a.cmd == "delete":
        if dev.ask(DELETE, bytes([(a.n - 1) & 0xFF, (a.n - 1) >> 8]))[0]:
            raise SystemExit("delete failed")
        print(f"  song {a.n} deleted")
    elif a.cmd == "play":
        if dev.ask(PLAY, bytes([(a.n - 1) & 0xFF, (a.n - 1) >> 8, a.subsong & 0xFF]))[0]:
            raise SystemExit("the FM-1 could not play it")
    elif a.cmd == "compact":
        do_compact(dev, Songlengths(a.hvsc))
    elif a.cmd == "usb":
        if dev.ask(USB, bytes([a.mode]))[0]:
            raise SystemExit("the FM-1 refused the setting")
        print("  USB audio:", "the mix and the voices (5 channels)" if a.mode else "the mix (2 channels)", "- the FM-1 reconnects")
    elif a.cmd == "rx":
        names = rx_params()
        if a.param == "reset":
            param = 255
        elif a.param.upper() in names:
            param = names[a.param.upper()]
        else:
            raise SystemExit("rx: one of reset, " + ", ".join(n.lower() for n in names))
        if dev.ask(RX, bytes([param, a.voice]) + struct.pack("<i", a.value))[0]:
            raise SystemExit("the FM-1 refused the event")
    elif a.cmd == "output":
        if dev.ask(OUTPUT, bytes([a.mode]))[0]:
            raise SystemExit("the FM-1 refused the output")
    elif a.cmd == "stats":
        dev.ask(STATS)                                      # restart the window
        for _ in range(a.seconds):
            time.sleep(1.0)
            r = dev.ask(STATS)
            mean, peak = r[0] | r[1] << 8, r[2] | r[3] << 8
            xruns, frames = struct.unpack("<I", r[4:8])[0], struct.unpack("<I", r[12:16])[0]
            print(f"  load {mean / 10:5.1f} % mean, {peak / 10:5.1f} % peak   output {['SID', 'ASID', 'BOTH'][r[8]]}  "
                  f"{'playing' if r[9] else 'stopped'}  {r[11]} SID  frame {frames}  xruns {xruns}")
            if len(r) >= 37:                                # USB audio, firmware with it
                pk, un, ov, mi, st = struct.unpack("<5I", r[17:37])
                print(f"        USB audio {'streaming' if r[16] else 'idle':9s}  packets {pk}  underruns {un}  "
                      f"overruns {ov}  missed {mi}  stalls {st}")
            if len(r) >= 51:                                # the MASTER pot
                lo, hi, jumps, reads, gain = r[37] | r[38] << 8, r[39] | r[40] << 8, *struct.unpack("<2I", r[41:49]), r[49] | r[50] << 8
                print(f"        MASTER  reading {lo}..{hi} of 1023  jumps {jumps} of {reads} reads  gain {gain} of 4096")
            if len(r) >= 55:                                # the clock and the deck
                vin = r[54] - 256 if r[54] > 127 else r[54]
                print(f"        clock {'NTSC' if r[51] else 'PAL'}  tempo {r[53]} %  pitch {vin / 10:+.1f} %")
            if len(r) >= 57 and (r[55] or r[56]):           # the supplies set at boot
                print(f"        supplies VDC14 {1.25 + 0.05 * r[55]:.2f} V (level {r[55]})  SYSVDD {0.93 + 0.03 * r[56]:.2f} V (level {r[56]})")
    elif a.cmd == "wipe":
        if dev.ask(WIPE, b"WIPE", timeout=60.0)[0]:
            raise SystemExit("wipe failed")
        print("  the pool is empty")


def rx_params():
    """The remix parameters by name (tempo, vinyl, lfo_rate ...), read from firmware/src/app/engine.h's
    RX_P_* enum so the two cannot drift apart."""
    import re
    src = (Path(__file__).resolve().parent.parent / "firmware/src/app/engine.h").read_text()
    body = src[src.index("enum { RX_P_"):]
    body = body[:body.index("};")]
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    return {n[5:]: i for i, n in enumerate(re.findall(r"RX_P_[A-Z_0-9]+", body))}


if __name__ == "__main__":
    main()
