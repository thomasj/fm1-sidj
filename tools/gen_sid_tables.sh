#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# Regenerates sid/sid_tables.c from the vendored reSID 0.16 sources (needs a host C++ compiler).
set -e
cd "$(dirname "$0")/.."
RESID=third_party/resid-0.16
OUT=build/gen
mkdir -p "$OUT"
CXX="${CXX:-c++}"
# The reference filter is compiled without its fc-plotter friends; -w hides 2004-era warnings.
$CXX -O2 -w -DVERSION='"0.16"' -I"$RESID" tools/gen_sid_tables.cc "$RESID/filter.cc" -o "$OUT/gen_sid_tables"
"$OUT/gen_sid_tables" "$RESID" sid/sid_tables.c
echo "wrote sid/sid_tables.c ($(wc -c < sid/sid_tables.c) bytes)"
