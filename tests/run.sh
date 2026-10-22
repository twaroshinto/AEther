#!/bin/sh
# SPDX-License-Identifier: MPL-2.0
# AETHER's tests on this machine (cc, python3):
#   1. the engine alone (tests/dsp_test.c): scenarios to WAVs in build/test/, checked;
#   2. the whole firmware in fm1-base's host simulator (app/tests/*.txt), when fm1-base is checked out.
# Strict like the device: no double in the engine (-Wdouble-promotion as an error), no fused multiply-add.
set -e
cd "$(dirname "$0")/.."
CC="${CC:-cc}"
OUT=build/test
mkdir -p "$OUT/gen"
python3 app/tools/gen_source.py --out "$OUT/gen/aether_source.h" --wav-dir "$OUT/starter"
for u in aether va; do
    $CC -O2 -ffp-contract=off -std=c99 -Wall -Wextra -Werror -Wdouble-promotion -Wno-unused-function \
        -c app/dsp/$u.c -Ibase/include -o "$OUT/$u.o"
done
$CC -O2 -ffp-contract=off -std=gnu99 -Wall -Wextra -Iapp/dsp -I"$OUT/gen" \
    tests/dsp_test.c "$OUT/aether.o" "$OUT/va.o" -lm -lpthread -o "$OUT/dsp_test"
# a stereo 24-bit sweep: the loader's own conversion, as PCM16 and ADPCM flash images (and SysEx) for the tests
python3 - "$OUT/sweep.wav" <<'PY'
import math, sys, wave
rate, n = 44100, 66150
with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(2); w.setsampwidth(3); w.setframerate(rate)
    ph, out = 0.0, bytearray()
    for i in range(n):
        ph += 2 * math.pi * 150.0 * 2 ** (5.0 * i / n) / rate
        out += int(0.5 * math.sin(ph) * 8388607).to_bytes(3, "little", signed=True) * 2
    w.writeframes(bytes(out))
PY
L="python3 tools/aether_load.py"
$L "$OUT/sweep.wav" --name sweep --image "$OUT/user.bin" --syx "$OUT/user.syx"   # the scenarios': ADPCM + SBR
$L "$OUT/sweep.wav" --pcm16 --no-sbr --image "$OUT/a16.bin"
$L "$OUT/sweep.wav" --no-sbr --image "$OUT/a4.bin"
# SBR: STRING (full band) kept whole, and at 11025 Hz with and without its envelope
$L "$OUT/starter/string.wav" --rate 44100 --pcm16 --no-sbr --image "$OUT/sbr_ref.bin"
$L "$OUT/starter/string.wav" --rate 11025 --image "$OUT/sbr_on.bin"
$L "$OUT/starter/string.wav" --rate 11025 --no-sbr --image "$OUT/sbr_off.bin"
"$OUT/dsp_test" "$OUT" "$OUT/a16.bin" "$OUT/a4.bin" "$OUT/sbr_ref.bin" "$OUT/sbr_on.bin" "$OUT/sbr_off.bin"
if [ -f base/tools/build.py ] && [ -z "$AETHER_DSP_ONLY" ]; then
    ./build.sh --host
    cp "$OUT/user.bin" "$OUT/user.syx" app/build/host/
    head -c 2000 app/build/host/user.syx > app/build/host/cut.syx
    for t in app/tests/*.txt; do
        echo "scenario $t"
        app/build/host/aether_host "$t" app/build/host
    done
fi
echo "tests: ok"
