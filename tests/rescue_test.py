#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
"""tools/fm1_rescue.py without an FM-1: the stock version is picked by the FM-1's flash head, and
only [0x4000, that version's end) is written. A fake UBOOT keeps the flash in a bytearray behind
the tool's own Uboot class, so its write_sector checks run. With $FM1_STOCK (M-VAVE's packages,
space-separated; not redistributable, so not in the tree), the real V15 and V16 are loaded too and
each one's flash image must pick itself.
"""
import os
import random
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
import fm1_rescue as R  # noqa: E402

fails = 0


def check(name, ok):
    global fails
    print(f"  {'ok  ' if ok else 'FAIL'} {name}")
    fails += 0 if ok else 1


class FakeUboot(R.Uboot):
    """The chip's update mode with its flash loader running, as the tool sees it."""
    def __init__(self, flash):
        super().__init__(None)
        self.flash = bytearray(flash)
        self.erased = []
        self.ran = False

    def inquiry(self):
        return "WL82", "UBOOT1.00"

    def start_loader(self, blob):
        pass

    def chip(self):
        return 0x980F, 3, 0x856014

    def read(self, addr, n):
        return bytes(self.flash[addr:addr + n])

    def cmd(self, cmd, args=b""):
        if cmd == R.CMD_ERASE_SECTOR:
            a = int.from_bytes(args[:4], "big")
            self.erased.append(a)
            self.flash[a:a + R.SECTOR] = b"\xff" * R.SECTOR
        elif cmd == R.CMD_RUN_APP:
            self.ran = True
        return b""

    def write_block(self, cmd, addr, data):
        self.flash[addr:addr + len(data)] = data


def image(head_byte, size, seed):
    rnd = random.Random(seed)
    return bytes([head_byte]) * R.WRITE_MIN + bytes(rnd.getrandbits(8) for _ in range(size - R.WRITE_MIN))


def run(packages, flash, write=True):
    dev = FakeUboot(flash)
    out = []
    try:
        dev_ok = R.rescue(dev, packages, b"", write, BACKUPS, say=lambda *a, **k: out.append(" ".join(map(str, a))))
        return dev, dev_ok, None
    except R.Fail as e:
        return dev, None, str(e)


_tmp = tempfile.TemporaryDirectory()           # (the backups rescue() saves)
BACKUPS = _tmp.name
print("== rescue_test (the stock version picked by the FM-1's flash head)")
v15 = image(0x15, 0x93000, 1)
v16 = image(0x16, 0xA9000, 2)
packages = [("V16", v16), ("V15", v15)]
rnd = random.Random(3)
rest = bytes(rnd.getrandbits(8) for _ in range(R.FLASH_SIZE))   # another firmware, songs, settings

# an FM-1 that last had V15: its head, SiDJ above it
flash = v15[:R.WRITE_MIN] + rest[R.WRITE_MIN:]
dev, ok, err = run(packages, flash)
check("a V15 head picks V15 and writes it", ok is True and err is None and dev.ran)
check("V15: the flash holds V15's image, the head and everything above 0x93000 as they were",
      bytes(dev.flash) == v15 + flash[0x93000:])
check("V15: only sectors in [0x4000, 0x93000) erased",
      dev.erased and min(dev.erased) >= R.WRITE_MIN and max(dev.erased) < 0x93000)

flash = v16[:R.WRITE_MIN] + rest[R.WRITE_MIN:]
dev, ok, err = run(packages, flash)
check("a V16 head picks V16 and writes it", ok is True and bytes(dev.flash) == v16 + flash[0xA9000:])

flash = bytes([0x42]) * R.WRITE_MIN + rest[R.WRITE_MIN:]
dev, ok, err = run(packages, flash)
check("a head that is neither: refused, nothing written", err is not None and "not writing" in err
      and not dev.erased and bytes(dev.flash) == flash)

flash = v15[:R.WRITE_MIN] + rest[R.WRITE_MIN:]
dev, ok, err = run([("V16", v16)], flash)
check("only V16 given to a V15 head: refused, nothing written", err is not None and not dev.erased)

dev, ok, err = run(packages, flash, write=False)
check("without --write: checked, nothing written", ok is False and not dev.erased and not dev.ran)

try:
    FakeUboot(flash).write_sector(0x3000, bytes(R.SECTOR), 0x93000)
    check("a write below 0x4000 refused", False)
except R.Fail:
    check("a write below 0x4000 refused", True)
try:
    FakeUboot(flash).write_sector(0x93000, bytes(R.SECTOR), 0x93000)
    check("a write past the version's end refused", False)
except R.Fail:
    check("a write past the version's end refused", True)

stock = os.environ.get("FM1_STOCK", "").split()
if stock:
    pk = R.load_packages(stock)
    names = sorted(n for n, _ in pk)
    check(f"M-VAVE's packages load: {', '.join(names)}", len(pk) == len(set(names)) >= 1)
    if len(pk) == 2:
        check("V15's and V16's heads differ", pk[0][1][:R.WRITE_MIN] != pk[1][1][:R.WRITE_MIN])
    for name, im in pk:
        got, _ = R.pick(pk, im + rest[len(im):])
        check(f"{name}'s own flash picks {name}", got == name)
else:
    print("  (FM1_STOCK not set: M-VAVE's packages skipped)")

print("rescue tests passed" if not fails else f"rescue tests: {fails} failed")
sys.exit(1 if fails else 0)
