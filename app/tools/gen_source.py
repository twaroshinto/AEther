#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 fm1-smuga contributors
"""SMUGA's built-in sources: short sounds made here, at build time, so the firmware has material to stretch before
any sample is loaded, and no recording's licence to carry. Stored as IMA ADPCM (app/tools/smg_adpcm.py), 4 bits a
sample: the four together take about 140 KB of the app's flash.

  gen_source.py --out GEN/smuga_source.h [--wav-dir DIR]

  CHOIR   four chords sung on changing vowels (additive voices through formants, vibrato)       4.0 s
  GLASS   struck glass and bells: inharmonic partials, long decays, a slow arpeggio             3.0 s
  STRING  plucked strings (Karplus-Strong), an arpeggio over two octaves                        3.0 s
  WIND    noise through two sweeping resonant band-passes: wind, breath, a whistle              3.0 s

The header holds `smg_builtin[SMG_NBUILTIN]` ({name, data, len, rate}, all SMG_FMT_ADPCM at 22050 Hz).
--wav-dir also writes each as a WAV (to listen to, or to load and edit). Pure Python, a few seconds."""
import argparse
import math
import struct
import sys
import wave
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import smg_adpcm  # noqa: E402

RATE = 22050
VERSION = "v2"


def lcg(seed):
    while True:
        seed = (seed * 1103515245 + 12345) & 0x7FFFFFFF
        yield (seed >> 8) / 8388608.0 - 0.5


def normalize(x, peak_to=0.89):
    peak = max(abs(v) for v in x) or 1.0
    return [int(round(v / peak * peak_to * 32767)) for v in x]


# ---- CHOIR
VOWELS = {                                 # (formant Hz, bandwidth Hz, gain)
    "a": [(800, 80, 1.0), (1150, 90, 0.5), (2900, 120, 0.25)],
    "o": [(450, 70, 1.0), (800, 80, 0.45), (2830, 100, 0.12)],
    "e": [(400, 60, 1.0), (1600, 90, 0.4), (2700, 120, 0.3)],
    "u": [(325, 50, 1.0), (700, 60, 0.3), (2530, 100, 0.08)],
}
CHORDS = [((45, 57, 61, 64), "a"), ((42, 57, 61, 66), "o"), ((38, 57, 62, 66), "e"), ((40, 56, 59, 64), "u")]


def formant(f, vowel):
    return 0.04 + sum(a / (1.0 + ((f - fc) / bw) ** 2) for fc, bw, a in VOWELS[vowel])


def choir(secs):
    n = int(RATE * secs)
    seg = n / len(CHORDS)
    fade = int(0.12 * RATE)
    out = [0.0] * n
    for ci, (notes, vowel) in enumerate(CHORDS):
        s0, s1 = max(0, int(ci * seg) - fade), min(n, int((ci + 1) * seg) + fade)
        for vi, note in enumerate(notes):
            f0 = 440.0 * 2 ** ((note - 69) / 12)
            hs = [h for h in range(1, 60) if f0 * h < 6000.0]
            amps = [formant(f0 * h, vowel) / h ** 0.7 for h in hs]
            ph = [0.37 * h * (vi + 1) for h in hs]
            vph = vi * 1.3
            for i in range(s0, s1):
                env = 1.0
                if ci > 0 and i < int(ci * seg) + fade:
                    env = (i - s0) / (2 * fade)
                if ci < len(CHORDS) - 1 and i > int((ci + 1) * seg) - fade:
                    env = min(env, (s1 - i) / (2 * fade))
                vph += 2 * math.pi * (4.6 + 0.37 * vi) / RATE
                fr = f0 * (1.0 + 0.006 * math.sin(vph)) * 2 * math.pi / RATE
                s = 0.0
                for k, h in enumerate(hs):
                    ph[k] += fr * h
                    s += amps[k] * math.sin(ph[k])
                out[i] += s * env * (0.6 if vi == 0 else 0.45)
    for i, r in zip(range(n), lcg(12345)):
        out[i] += r * 0.02
    return out


# ---- GLASS: struck partials of a glass bowl / bell (ratios from a free bar and a bell), each with its own decay
PARTIALS = [(1.0, 1.0, 1.6), (2.32, 0.6, 1.2), (2.76, 0.5, 1.0), (4.07, 0.35, 0.7), (5.40, 0.3, 0.55),
            (6.80, 0.18, 0.4), (8.93, 0.12, 0.3)]
GLASS_NOTES = [(0.0, 76), (0.45, 83), (0.9, 80), (1.35, 85), (1.8, 71), (2.25, 78)]


def glass(secs):
    n = int(RATE * secs)
    out = [0.0] * n
    for t0, note in GLASS_NOTES:
        f0 = 440.0 * 2 ** ((note - 69) / 12)
        i0 = int(t0 * RATE)
        for ratio, amp, decay in PARTIALS:
            f = f0 * ratio
            if f > RATE * 0.45:
                continue
            w = 2 * math.pi * f / RATE
            # a damped sinusoid by recursion: y[k] = 2 r cos(w) y[k-1] - r^2 y[k-2]
            r = math.exp(-1.0 / (decay * RATE))
            c = 2 * r * math.cos(w)
            y1, y2 = amp * math.sin(w), 0.0
            for i in range(i0, n):
                out[i] += y1
                y1, y2 = c * y1 - r * r * y2, y1
    return out


# ---- STRING: Karplus-Strong plucks
STRING_NOTES = [(0.0, 50), (0.2, 57), (0.4, 62), (0.6, 66), (0.8, 69), (1.0, 74), (1.3, 78), (1.6, 74),
                (1.9, 69), (2.2, 66)]


def string(secs):
    n = int(RATE * secs)
    out = [0.0] * n
    noise = lcg(777)
    for t0, note in STRING_NOTES:
        f0 = 440.0 * 2 ** ((note - 69) / 12)
        period = RATE / f0
        L = int(period)
        frac = period - L
        buf = [next(noise) for _ in range(L + 2)]
        # a softer pluck: the burst low-passed once
        buf = [(buf[i] + buf[i - 1]) * 0.5 for i in range(len(buf))]
        pos, i0 = 0, int(t0 * RATE)
        loss = 0.996
        for i in range(i0, n):
            a = buf[pos % len(buf)]
            b = buf[(pos + 1) % len(buf)]
            v = a * (1 - frac) + b * frac
            out[i] += v
            buf[pos % len(buf)] = loss * 0.5 * (a + b)
            pos += 1
    return out


# ---- WIND: noise through two resonant band-passes that sweep
def wind(secs):
    n = int(RATE * secs)
    out = [0.0] * n
    noise = lcg(4242)
    st = [[0.0, 0.0], [0.0, 0.0]]
    for i in range(n):
        t = i / RATE
        x = next(noise)
        fcs = [350 + 900 * (0.5 - 0.5 * math.cos(2 * math.pi * t / secs * 1.5)),
               1800 + 700 * math.sin(2 * math.pi * t / secs)]
        qs = [6.0, 30.0]
        gains = [1.0, 0.25 * (0.5 + 0.5 * math.sin(2 * math.pi * t / secs * 2))]
        y = 0.0
        for k in range(2):
            g = math.tan(math.pi * fcs[k] / RATE)
            kk = 1.0 / qs[k]
            a1 = 1 / (1 + g * (g + kk))
            ic1, ic2 = st[k]
            v3 = x - ic2
            v1 = a1 * ic1 + g * a1 * v3
            v2 = ic2 + g * a1 * ic1 + g * g * a1 * v3
            st[k] = [2 * v1 - ic1, 2 * v2 - ic2]
            y += gains[k] * v1 * kk * 4
        swell = 0.35 + 0.65 * math.sin(math.pi * t / secs) ** 1.5
        out[i] = y * swell
    return out


SOURCES = [("CHOIR", choir, 4.0), ("GLASS", glass, 3.0), ("STRING", string, 3.0), ("WIND", wind, 3.0)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--wav-dir")
    a = ap.parse_args()
    out = Path(a.out)
    stamp = f"/* gen_source.py {VERSION} {RATE} {' '.join(f'{s[0]}:{s[2]}' for s in SOURCES)} */"
    if out.exists() and out.read_text().startswith(stamp) and not a.wav_dir:
        return                             # made already, with these settings
    lines = [stamp, "/* SMUGA's built-in sources (app/tools/gen_source.py): generated, do not edit */",
             "#pragma once", "#include <stdint.h>", '#include "smuga.h"', ""]
    table, total = [], 0
    for name, fn, secs in SOURCES:
        pcm = normalize(fn(secs))
        data = smg_adpcm.encode(pcm)
        total += len(data)
        ident = f"smg_src_{name.lower()}"
        lines.append(f"static const uint8_t {ident}[{len(data)}] = {{")
        for i in range(0, len(data), 24):
            lines.append("    " + ", ".join(str(v) for v in data[i:i + 24]) + ",")
        lines.append("};")
        table.append(f'    {{"{name}", {ident}, {len(pcm)}u, {RATE}u}},')
        if a.wav_dir:
            Path(a.wav_dir).mkdir(parents=True, exist_ok=True)
            with wave.open(str(Path(a.wav_dir) / f"{name.lower()}.wav"), "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(RATE)
                w.writeframes(struct.pack(f"<{len(pcm)}h", *pcm))
    lines += ["typedef struct {", "    const char *name;", "    const uint8_t *data;   /* SMG_FMT_ADPCM */",
              "    uint32_t len, rate;", "} smg_builtin_t;", f"#define SMG_NBUILTIN {len(SOURCES)}u",
              "static const smg_builtin_t smg_builtin[SMG_NBUILTIN] = {", *table, "};"]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n")
    print(f"sources: {len(SOURCES)}, {total} B of ADPCM -> {out}")


if __name__ == "__main__":
    main()
