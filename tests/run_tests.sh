#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# SiDJ host tests: the SID engine against reSID 0.16, the 6510, the SID file parser, the
# C64 environment, the remix, the ASID sink, the storage, and the whole app in the simulator
# (tests/scenarios/*.in, output in build/scenarios/). HVSC at $HVSC (default ~/Music/C64Music)
# gives the scenarios real tunes; without it they play the tune c64_test writes.
set -e
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
HVSC="${HVSC:-$HOME/Music/C64Music}"
mkdir -p build/host build/scenarios
echo "== sid_ref_test"
make -s test
echo "== stems_test (the voices as stems: each its voice alone, the three add up to the output)"
$CC -O2 -Wall -Wextra -std=c11 -Isid -o build/host/stems_test tests/stems_test.c sid/sid.c sid/sid_tables.c -lm
build/host/stems_test
echo "== cpu_test (the 6510 core against the 6502 functional test)"
$CC -O2 -Wall -Wextra -std=c11 -o build/host/cpu_test tests/cpu_test.c firmware/src/c64/cpu6502.c
build/host/cpu_test
echo "== sidfile_test"
$CC -O2 -Wall -Wextra -std=c11 -o build/host/sidfile_test tests/sidfile_test.c firmware/src/c64/sidfile.c
build/host/sidfile_test
echo "== c64_test (interrupts, timers, banking, the KERNAL replacement)"
$CC -O2 -Wall -Wextra -std=c11 -o build/host/c64_test tests/c64_test.c firmware/src/c64/c64.c firmware/src/c64/cpu6502.c firmware/src/c64/sidfile.c
build/host/c64_test
echo "== remix_test"
$CC -O2 -Wall -Wextra -std=c11 -o build/host/remix_test tests/remix_test.c firmware/src/app/remix.c
build/host/remix_test
echo "== asid_test (the ASID sink against the protocol's message layout)"
$CC -O2 -Wall -Wextra -std=c11 -o build/host/asid_test tests/asid_test.c firmware/src/app/asid.c
build/host/asid_test
echo "== enc_test (the encoder decoder: one click, one step, slow or fast)"
$CC -O2 -Wall -Wextra -std=c11 -o build/host/enc_test tests/enc_test.c
build/host/enc_test
echo "== storage_test"
$CC -O1 -w -Ifirmware/src -o build/host/storage_test tests/storage_test.c
build/host/storage_test
echo "== host scenarios"
host/build_host.sh
SID1="$HVSC/MUSICIANS/H/Hubbard_Rob/Commando.sid"
SID2="$HVSC/MUSICIANS/G/Galway_Martin/Wizball.sid"
SID3="$HVSC/MUSICIANS/J/JVD/Double_2SID.sid"
SID4="$HVSC/MUSICIANS/G/Galway_Martin/Arkanoid.sid"   # an RSID
if [ ! -f "$SID1" ]; then SID1=build/host/synth.sid; SID2=build/host/synth.sid; SID3=build/host/synth.sid; SID4=build/host/synth.sid; echo "  (no HVSC: playing the test tune)"; fi
for s in tests/scenarios/*.in; do
    out="build/scenarios/$(basename "$s" .in).sidj"
    sed -e "s#@SID1@#$SID1#g" -e "s#@SID2@#$SID2#g" -e "s#@SID3@#$SID3#g" -e "s#@SID4@#$SID4#g" "$s" > "$out"
    echo "-- $s"
    build/host/sidj_host "$out" build/scenarios
done
# the same scenarios under AddressSanitizer and UndefinedBehaviorSanitizer: an out-of-bounds write
# in the UI or the engine lands in the C64's RAM on the FM-1 (the scopes' correlation once did)
echo "== host scenarios, sanitized"
if $CC -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -std=c99 -w -DSIDJ_HOST -Ifirmware/src -Isid -Ibuild/gen \
       -o build/host/sidj_host_san host/sidj_host.c sid/sid.c sid/sid_tables.c firmware/src/app/engine.c \
       firmware/src/app/remix.c firmware/src/app/asid.c firmware/src/c64/cpu6502.c firmware/src/c64/c64.c \
       firmware/src/c64/sidfile.c -lm 2>/dev/null; then
    san=0
    for out in build/scenarios/*.sidj; do
        if build/host/sidj_host_san "$out" build/scenarios 2>&1 | grep -E "ERROR: AddressSanitizer|runtime error"; then
            echo "  sanitizer: $out"; san=1
        fi
    done
    [ $san = 0 ] && echo "sanitized scenarios clean" || { echo "SANITIZER FAIL"; exit 1; }
else
    echo "  (no sanitizers with $CC: skipped)"
fi
if [ -d "$HVSC/MUSICIANS" ]; then
    echo "== hvsc_play (a few tunes through the host player, no faults)"
    make -s sidplay
    for t in "H/Hubbard_Rob/Monty_on_the_Run.sid" "T/Tel_Jeroen/Cybernoid_II.sid" "E/Emotional_Mozes/Gods_Gift.sid" "J/JVD/Double_2SID.sid" "B/Bogas_Ed/Ducks_Ahoy.sid"; do
        [ -f "$HVSC/MUSICIANS/$t" ] && build/host/sidplay "$HVSC/MUSICIANS/$t" -t 5 -q | tail -2 | head -1
    done
fi
# the browser emulator: built and run headless (needs Emscripten and node)
if command -v emcc >/dev/null && command -v node >/dev/null; then
    echo "== emu_test (the browser emulator, headless in node)"
    web/emu/build.sh >/dev/null 2>&1 || { echo "web/emu/build.sh failed"; exit 1; }
    node tests/emu_test.mjs "$SID1"
    echo "== site_test (the site's song module against the device's song store, in the emulator)"
    node tests/site_test.mjs "$SID1" "$SID2" "$HVSC"
else
    echo "  skip emu_test (needs emcc and node)"
fi
# the update path, against the firmware package (Felucca's tests; needs ./build.sh first)
PY="${PYTHON:-python3}"
[ -x .venv/bin/python3 ] && PY=.venv/bin/python3
if [ -f build/sidj.fwsc ]; then
    echo "== ota_test (the update entry, against build/sidj.fwsc)"
    $CC -O1 -w -o build/host/ota_test tests/ota_test.c && build/host/ota_test build/sidj.fwsc
    if [ -f "${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}/cpu/wl82/tools/uboot.boot" ]; then
        echo "== ldr_test (the update loader: an old app replaced by the package)"
        export AC79_SDK="${AC79_SDK:-$HOME/fw-AC79_AIoT_SDK}"
        head -c 100000 build/sidj.bin > build/host/old_app.bin
        "$PY" tools/fm1pkg_make.py build/host/old_app.bin build/loader/ota.bin build/host/old.fwsc >/dev/null
        $CC -O1 -w -o build/host/ldr_test tests/ldr_test.c && build/host/ldr_test build/host/old.fwsc build/sidj.fwsc
    else
        echo "  skip ldr_test (needs the AC79 SDK, as the build)"
    fi
    echo "== install_test (the command-line installer against a simulated FM-1)"
    "$PY" tests/install_test.py
else
    echo "  skip update-path tests (no build/sidj.fwsc: run ./build.sh)"
fi
echo "all tests passed"
