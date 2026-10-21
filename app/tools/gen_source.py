#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 fm1-smuga contributors
"""SMUGA's built-in source: a short synthetic "choir" (four chords sung on changing vowels), made here so the
firmware has something to stretch before any sample is loaded, and no recording's licence to carry.

  gen_source.py --out GEN/smuga_source.h [--rate 22050] [--secs 2.0] [--wav FILE]

Additive: every voice is a sum of harmonics weighted by a vowel's formants, with a slow vibrato; the chords cross-
fade. Pure Python (no numpy), about a second to run. The header holds `SMG_SOURCE_RATE`, `SMG_SOURCE_LEN` and
`smg_source[]` (int16, mono)."""
import argparse
import math
import struct
import wave
from pathlib import Path

VOWELS = {                                 # (formant Hz, bandwidth Hz, gain)
    "a": [(800, 80, 1.0), (1150, 90, 0.5), (2900, 120, 0.25)],
    "o": [(450, 70, 1.0), (800, 80, 0.45), (2830, 100, 0.12)],
    "e": [(400, 60, 1.0), (1600, 90, 0.4), (2700, 120, 0.3)],
    "u": [(325, 50, 1.0), (700, 60, 0.3), (2530, 100, 0.08)],
}
# (MIDI notes, vowel) per chord: A, F#m, D, E
CHORDS = [((45, 57, 61, 64), "a"), ((42, 57, 61, 66), "o"), ((38, 57, 62, 66), "e"), ((40, 56, 59, 64), "u")]


def formant(f, vowel):
    g = 0.04
    for fc, bw, a in VOWELS[vowel]:
        g += a / (1.0 + ((f - fc) / bw) ** 2)
    return g


def render(rate, secs):
    n = int(rate * secs)
    seg = n / len(CHORDS)
    fade = int(0.08 * rate)
    out = [0.0] * n
    for ci, (notes, vowel) in enumerate(CHORDS):
        s0 = max(0, int(ci * seg) - fade)
        s1 = min(n, int((ci + 1) * seg) + fade)
        for vi, note in enumerate(notes):
            f0 = 440.0 * 2 ** ((note - 69) / 12)
            vib_rate, vib_depth = 4.6 + 0.37 * vi, 0.006
            hs = [h for h in range(1, 80) if f0 * h < min(7000.0, rate * 0.45)]
            amps = [formant(f0 * h, vowel) / h ** 0.7 for h in hs]
            ph = [0.37 * h * (vi + 1) for h in hs]          # fixed, varied start phases
            vph = vi * 1.3
            for i in range(s0, s1):
                # cross-fade at the chord's edges
                env = 1.0
                if i < int(ci * seg) + fade and ci > 0:
                    env = (i - s0) / (2 * fade)
                if i > int((ci + 1) * seg) - fade and ci < len(CHORDS) - 1:
                    env = min(env, (s1 - i) / (2 * fade))
                vph += 2 * math.pi * vib_rate / rate
                fr = f0 * (1.0 + vib_depth * math.sin(vph)) * 2 * math.pi / rate
                s = 0.0
                for k, h in enumerate(hs):
                    ph[k] += fr * h
                    s += amps[k] * math.sin(ph[k])
                out[i] += s * env * (0.6 if vi == 0 else 0.45)
    # a breath of noise, for SPREAD to have something to blur
    seed = 12345
    for i in range(n):
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        out[i] += ((seed >> 8) / 8388608.0 - 0.5) * 0.02
    peak = max(abs(v) for v in out) or 1.0
    return [int(round(v / peak * 0.89 * 32767)) for v in out]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--rate", type=int, default=22050)
    ap.add_argument("--secs", type=float, default=2.0)
    ap.add_argument("--wav")
    a = ap.parse_args()
    out = Path(a.out)
    stamp = f"/* gen_source.py rate={a.rate} secs={a.secs} v1 */"
    if out.exists() and out.read_text().startswith(stamp):
        return                             # made already, with these settings
    pcm = render(a.rate, a.secs)
    lines = [stamp, "/* SMUGA's built-in source (app/tools/gen_source.py): generated, do not edit */",
             "#pragma once", "#include <stdint.h>",
             f"#define SMG_SOURCE_RATE {a.rate}u", f"#define SMG_SOURCE_LEN {len(pcm)}u",
             "static const int16_t smg_source[SMG_SOURCE_LEN] = {"]
    for i in range(0, len(pcm), 16):
        lines.append("    " + ", ".join(str(v) for v in pcm[i:i + 16]) + ",")
    lines.append("};")
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n")
    if a.wav:
        with wave.open(a.wav, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(a.rate)
            w.writeframes(struct.pack(f"<{len(pcm)}h", *pcm))
    print(f"source: {len(pcm)} samples at {a.rate} Hz -> {out}")


if __name__ == "__main__":
    main()
