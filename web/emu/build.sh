#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# SiDJ in the browser: the host simulator compiled to WebAssembly (Emscripten), plus its page.
#   web/emu/build.sh  ->  build/emu/{index.html, worklet.js, sidj.wasm}
#   python3 -m http.server -d build/emu 8790   # then open http://localhost:8790/
set -e
cd "$(dirname "$0")/../.."
PY="${PYTHON:-python3}"
[ -x .venv/bin/python3 ] && PY=.venv/bin/python3
command -v emcc >/dev/null || { echo "web/emu/build.sh: no emcc (brew install emscripten)"; exit 1; }
mkdir -p build/gen build/emu
[ -f build/gen/felucca_font.h ] || "$PY" tools/gen_font.py build/gen/felucca_font.h >/dev/null
U="sid/sid.c sid/sid_tables.c firmware/src/app/engine.c firmware/src/app/remix.c firmware/src/app/asid.c firmware/src/c64/cpu6502.c firmware/src/c64/c64.c firmware/src/c64/sidfile.c"
# shellcheck disable=SC2086
emcc -O2 -std=gnu99 -Wall -Wno-unused-function -Wno-unused-parameter -Wno-unused-variable \
    -DSIDJ_HOST -DSIDJ_WEB -DSIDJ_VERSION="\"$(tools/version.sh)\"" -Ifirmware/src -Isid -Ibuild/gen \
    --no-entry -sSTANDALONE_WASM -sSTACK_SIZE=1048576 -sINITIAL_MEMORY=33554432 -sFILESYSTEM=0 \
    -o build/emu/sidj.wasm web/emu/sidj_web.c $U
cp web/emu/index.html web/emu/worklet.js web/site/fm1songs.js build/emu/
rm -rf build/emu/songs && cp -R web/site/songs build/emu/songs   # tubesockor's tunes, in the store on first start
echo "emu: build/emu ($(wc -c < build/emu/sidj.wasm) B wasm)"
