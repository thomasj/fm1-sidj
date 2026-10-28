#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-only
# After Felucca's web/make_site.py, Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments.
"""Make the web site (GitHub Pages) in one folder:

  index.html, fm1pkg.js, fm1ota.js, fm1songs.js   the installer and the song manager (web/site/)
  img/                                             the device's screens (from the simulator)
  songs/                                           tubesockor's tunes, offered on both pages
  firmware/sidj-VER.fwsc, manifest.json      the package the installer writes
  firmware/LICENSE, LICENSING.md, Apache-2.0.txt   its licences (the package holds three JieLi
                                                   SDK files under Apache-2.0)
  emu/                                             the browser emulator (web/emu, needs emcc)
  .nojekyll

  tools/make_site.py [PACKAGE] [--out DIR]          (default build/sidj.fwsc, build/site)

The version comes from the package's identity: FM-1_64XXYYZZ is release X.Y.Z (./build.sh
--release X.Y.Z), FM-1_64000000 a development build. Only packages made by tools/fm1pkg_make.py (Felucca's
loader, no vendor files) are published.
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import fm1_install as I  # noqa: E402  (product_of, LOADER_MARK)


def version_of(product):
    if product == "FM-1_64000000":
        return "dev"
    m = re.fullmatch(r"FM-1_64(\d\d)(\d\d)(\d\d)", product)
    if m:
        x, y, z = (int(v) for v in m.groups())
        return f"{x}.{y}.{z}"
    raise SystemExit(f"identity {product!r} is not a SiDJ package (FM-1_64000000 or FM-1_64XXYYZZ)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("package", nargs="?", default=str(ROOT / "build/sidj.fwsc"))
    ap.add_argument("--out", default=str(ROOT / "build/site"))
    ap.add_argument("--no-emu", action="store_true", help="leave the browser emulator out")
    a = ap.parse_args()
    pkg, out = Path(a.package), Path(a.out)
    raw = pkg.read_bytes()
    product = I.product_of(raw)
    version = version_of(product)
    if I.LOADER_MARK not in raw:
        raise SystemExit(f"{pkg}: no Felucca loader in it; the site ships only fm1pkg_make.py packages")
    if out.exists():
        shutil.rmtree(out)
    fw = out / "firmware"
    fw.mkdir(parents=True)
    for f in ("index.html", "fm1pkg.js", "fm1ota.js", "fm1songs.js"):
        shutil.copy2(ROOT / "web/site" / f, out / f)
    shutil.copytree(ROOT / "web/site/img", out / "img")
    shutil.copytree(ROOT / "web/site/songs", out / "songs")
    name = f"sidj-{version}.fwsc"
    shutil.copy2(pkg, fw / name)
    (fw / "manifest.json").write_text(json.dumps({"version": version, "product": product, "file": name}) + "\n")
    shutil.copy2(ROOT / "LICENSE", fw / "LICENSE")
    shutil.copy2(ROOT / "LICENSING.md", fw / "LICENSING.md")
    shutil.copy2(ROOT / "third_party/licenses/Apache-2.0.txt", fw / "Apache-2.0.txt")
    (out / ".nojekyll").write_text("")
    if not a.no_emu:
        if shutil.which("emcc"):
            subprocess.run([str(ROOT / "web/emu/build.sh")], check=True, stdout=subprocess.DEVNULL)
        if (ROOT / "build/emu/sidj.wasm").exists():
            shutil.copytree(ROOT / "build/emu", out / "emu")
        else:
            print("make_site: no emulator (needs emcc: brew install emscripten); --no-emu to leave it out on purpose")
    print(f"site     {out}  (SiDJ {version}, identity {product})")
    print(f"preview  python3 -m http.server -d {out} 8790   then http://localhost:8790/")


if __name__ == "__main__":
    main()
