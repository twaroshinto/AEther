#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""A WAV's level, second by second, and a failure if it is silence (CI: the firmware in fm1-emu)."""
import math, struct, sys, wave

w = wave.open(sys.argv[1])
ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
raw = w.readframes(n)
assert width == 2, width
x = struct.unpack('<%dh' % (len(raw) // 2), raw)
print(f'{sys.argv[1]}: {n / rate:.2f} s, {ch} ch, {rate} Hz')
peak_all = 0
for s0 in range(0, n, rate):
    seg = x[s0 * ch:(s0 + rate) * ch]
    if not seg:
        break
    peak = max(abs(v) for v in seg)
    rms = math.sqrt(sum(v * v for v in seg) / len(seg))
    peak_all = max(peak_all, peak)
    db = lambda v: 20 * math.log10(v / 32768) if v else float('-inf')
    print(f'  {s0 // rate:2d} s  peak {db(peak):6.1f} dBFS  rms {db(rms):6.1f} dBFS')
if peak_all < 33:
    sys.exit('silence')
