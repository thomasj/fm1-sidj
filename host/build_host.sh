#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Build the SiDJ host simulator (the whole app on this machine) into build/host/sidj_host.
set -e
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
PY="${PYTHON:-python3}"
[ -x .venv/bin/python3 ] && PY=.venv/bin/python3
mkdir -p build/gen build/host
[ -f build/gen/felucca_font.h ] || "$PY" tools/gen_font.py build/gen/felucca_font.h >/dev/null
U="sid/sid.c sid/sid_tables.c firmware/src/app/engine.c firmware/src/app/remix.c firmware/src/app/asid.c firmware/src/c64/cpu6502.c firmware/src/c64/c64.c firmware/src/c64/sidfile.c"
# shellcheck disable=SC2086
$CC -O2 -std=c99 -Wall -Wextra -Wno-unused-function -Wno-unused-parameter \
    -DSIDJ_HOST -DSIDJ_VERSION="\"$(tools/version.sh)\"" -Ifirmware/src -Isid -Ibuild/gen -o build/host/sidj_host host/sidj_host.c $U -lm
echo "host: build/host/sidj_host"
