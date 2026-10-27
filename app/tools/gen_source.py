#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 fm1-aether contributors
# AI-generated code (Claude, Anthropic): no warranty, use at your own risk (see NOTICE).
"""AETHER's built-in samples: sixteen short sounds synthesized here, at build time, so the firmware has material to
stretch before any sample is loaded, and no recording's licence to carry. Each is made at 44.1 kHz, then kept at
11025 Hz as IMA ADPCM (app/tools/aet_adpcm.py) with an SBR envelope of its band above 4.9 kHz (app/tools/aet_sbr.py):
the engine rebuilds that band at play time. About 16 KB for three seconds.

  gen_source.py --out GEN/aether_source.h [--wav-dir DIR]     (needs numpy)

The header holds aet_builtin[AET_NBUILTIN]: {name, data, len, rate, env, env_frames, env_hop}. --wav-dir also
writes each at 44.1 kHz as a WAV, to listen to or to load and edit."""
import argparse
import struct
import sys
import wave
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import aet_adpcm  # noqa: E402
import aet_sbr  # noqa: E402

FS = 44100
KEEP = 11025
VERSION = "v5"


def hz(note):
    return 440.0 * 2 ** ((note - 69) / 12)


def tt(secs):
    return np.arange(int(FS * secs)) / FS


def env_ad(t, t0, attack, decay):
    """0 before t0, a linear attack, then an exponential decay (time constant decay)"""
    d = t - t0
    e = np.where(d < attack, d / max(attack, 1e-4), np.exp(-(d - attack) / decay))
    return np.where(d < 0, 0.0, e)


def svf_bp(x, fc, q):
    """a band-pass, its centre per sample (fc an array or a number)"""
    fc = np.broadcast_to(np.asarray(fc, dtype=np.float64), x.shape)
    g = np.tan(np.pi * np.clip(fc, 20, FS * 0.45) / FS)
    k = 1.0 / q
    a1 = 1 / (1 + g * (g + k))
    ic1 = ic2 = 0.0
    y = np.empty_like(x)
    for i in range(len(x)):
        v3 = x[i] - ic2
        v1 = a1[i] * ic1 + g[i] * a1[i] * v3
        v2 = ic2 + g[i] * a1[i] * ic1 + g[i] * g[i] * a1[i] * v3
        ic1, ic2 = 2 * v1 - ic1, 2 * v2 - ic2
        y[i] = v1
    return y


def additive(t, f0, amps, fmod=None, partials=None):
    """a sum of partials (harmonic, or the ratios in partials), the pitch moved by fmod (a ratio per sample)"""
    ph = 2 * np.pi * np.cumsum(f0 * (fmod if fmod is not None else 1.0)) / FS
    out = np.zeros_like(t)
    for i, a in enumerate(amps):
        r = partials[i] if partials is not None else i + 1
        if f0 * r < FS * 0.45 and np.any(a):
            out += a * np.sin(ph * r + 0.7 * i)
    return out


def formants(f, fs_and_gains):
    return 0.04 + sum(g / (1 + ((f - fc) / bw) ** 2) for fc, bw, g in fs_and_gains)


VOWEL = {"a": [(800, 80, 1.0), (1150, 90, 0.5), (2900, 120, 0.25)],
         "o": [(450, 70, 1.0), (800, 80, 0.45), (2830, 100, 0.12)],
         "e": [(400, 60, 1.0), (1600, 90, 0.4), (2700, 120, 0.3)],
         "i": [(300, 50, 1.0), (2300, 120, 0.35), (3000, 150, 0.25)],
         "u": [(325, 50, 1.0), (700, 60, 0.3), (2530, 100, 0.08)]}


def xfade_segments(t, n_seg, fade):
    """weights for n_seg equal segments, cross-faded"""
    secs = t[-1] + 1 / FS
    out = []
    for i in range(n_seg):
        a, b = i * secs / n_seg, (i + 1) * secs / n_seg
        w = np.clip(np.minimum((t - a + fade) / (2 * fade), (b + fade - t) / (2 * fade)), 0, 1)
        if i == 0:
            w = np.where(t < a + fade, 1.0, w)
        if i == n_seg - 1:
            w = np.where(t > b - fade, 1.0, w)
        out.append(w)
    return out


# ---- the sources -------------------------------------------------------------------------------------------------
def choir(rng):
    t = tt(4.0)
    chords = [((45, 57, 61, 64), "a"), ((42, 57, 61, 66), "o"), ((38, 57, 62, 66), "e"), ((40, 56, 59, 64), "u")]
    out = np.zeros_like(t)
    for w, (notes, vowel) in zip(xfade_segments(t, 4, 0.12), chords):
        for vi, note in enumerate(notes):
            f0 = hz(note)
            hs = np.arange(1, 80)
            amps = [formants(f0 * h, VOWEL[vowel]) / h ** 0.7 for h in hs]
            vib = 1 + 0.006 * np.sin(2 * np.pi * (4.6 + 0.37 * vi) * t + vi)
            out += w * additive(t, f0, amps, vib) * (0.6 if vi == 0 else 0.45)
    return out + rng.normal(0, 0.01, len(t))


def glass(rng):
    t = tt(3.0)
    out = np.zeros_like(t)
    for t0, note in [(0.0, 76), (0.4, 83), (0.8, 80), (1.2, 85), (1.6, 71), (2.0, 78)]:
        for r, a, d in [(1.0, 1.0, 1.6), (2.32, 0.6, 1.2), (2.76, 0.5, 1.0), (4.07, 0.35, 0.7), (5.40, 0.3, 0.55),
                        (6.80, 0.18, 0.4), (8.93, 0.12, 0.3)]:
            if hz(note) * r < FS * 0.45:
                out += a * env_ad(t, t0, 0.002, d) * np.sin(2 * np.pi * hz(note) * r * (t - t0))
    return out


def karplus(t, t0, note, rng, loss=0.996, bright=0.5):
    n, i0 = len(t), int(t0 * FS)
    period = FS / hz(note)
    L = int(period)
    frac = period - L
    buf = list(np.convolve(rng.uniform(-1, 1, L + 2), [bright, 1 - bright], mode="same"))
    out = np.zeros(n)
    pos = 0
    for i in range(i0, n):
        a, b = buf[pos % len(buf)], buf[(pos + 1) % len(buf)]
        out[i] = a * (1 - frac) + b * frac
        buf[pos % len(buf)] = loss * 0.5 * (a + b)
        pos += 1
    return out


def string(rng):
    t = tt(3.0)
    return sum(karplus(t, t0, n, rng) for t0, n in [(0.0, 50), (0.2, 57), (0.4, 62), (0.6, 66), (0.8, 69),
                                                    (1.0, 74), (1.3, 78), (1.6, 74), (1.9, 69), (2.2, 66)])


def wind(rng):
    t = tt(3.0)
    x = rng.uniform(-0.5, 0.5, len(t))
    a = svf_bp(x, 350 + 900 * (0.5 - 0.5 * np.cos(2 * np.pi * t / 2.0)), 4.0)
    b = svf_bp(x, 1800 + 900 * np.sin(2 * np.pi * t / 3.0), 30.0) * (0.3 + 0.3 * np.sin(2 * np.pi * t / 1.5))
    hiss = x * 0.08
    return (a + b + hiss) * (0.35 + 0.65 * np.sin(np.pi * t / 3.0) ** 1.5)


def piano(rng):
    t = tt(2.5)
    out = np.zeros_like(t)
    for t0, note in [(0.0, 48), (0.12, 55), (0.24, 64), (0.36, 71), (1.2, 52), (1.3, 59), (1.4, 67)]:
        f0, B = hz(note), 0.0004
        for h in range(1, 40):
            f = f0 * h * np.sqrt(1 + B * h * h)
            if f > FS * 0.45:
                break
            a = (1 / h ** 1.1) * (1 + 0.6 * np.cos(np.pi * h / 7))   # the hammer's position: a comb
            d = 2.5 / (1 + 0.15 * h)
            for det in (0.9995, 1.0005):                             # two strings a note: slow beats
                out += 0.5 * a * env_ad(t, t0, 0.003, d) * np.sin(2 * np.pi * f * det * (t - t0))
    return out


def bowed(rng):
    t = tt(2.5)
    out = np.zeros_like(t)
    body = [(220, 60, 0.8), (450, 90, 1.0), (1100, 200, 0.6), (2500, 500, 0.3)]
    for w, note in zip(xfade_segments(t, 3, 0.15), (50, 57, 53)):
        f0 = hz(note)
        vib = 1 + 0.004 * np.sin(2 * np.pi * 5.5 * t)
        amps = [formants(f0 * h, body) / h for h in range(1, 60)]
        out += w * additive(t, f0, amps, vib)
    bow = svf_bp(rng.normal(0, 0.15, len(t)), 2500, 2.0)
    return out * (0.3 + 0.7 * np.minimum(t / 0.3, 1)) + bow


def flute(rng):
    t = tt(2.5)
    out = np.zeros_like(t)
    for w, note in zip(xfade_segments(t, 5, 0.06), (69, 72, 76, 74, 72)):
        f0 = hz(note)
        vib = 1 + 0.005 * np.sin(2 * np.pi * 5.0 * t) * np.minimum(t / 0.5, 1)
        out += w * additive(t, f0, [1.0, 0.25, 0.12, 0.05], vib)
    breath = svf_bp(rng.normal(0, 0.2, len(t)), 1500, 0.8) * 0.5
    return out + breath


def organ(rng):
    t = tt(2.5)
    out = np.zeros_like(t)
    lesl = 1 + 0.003 * np.sin(2 * np.pi * 6.2 * t)
    for note in (48, 55, 60, 64, 67):
        f0 = hz(note)
        # drawbars 16' 8' 5 1/3' 4' 2 2/3' 2': the ratios 0.5 1 1.5 2 3 4
        out += additive(t, f0 * 0.5, [0.6, 1.0, 0.5, 0.7, 0, 0.4, 0, 0.3], lesl)
    return out * (0.85 + 0.15 * np.sin(2 * np.pi * 6.2 * t + 1))


def kalimba(rng):
    t = tt(2.5)
    out = np.zeros_like(t)
    for t0, note in [(0.0, 67), (0.25, 71), (0.5, 74), (0.75, 79), (1.0, 76), (1.35, 74), (1.7, 71), (2.0, 67)]:
        f0 = hz(note)
        for r, a, d in [(1.0, 1.0, 0.9), (5.4, 0.25, 0.08), (13.5, 0.08, 0.02)]:
            if f0 * r < FS * 0.45:
                out += a * env_ad(t, t0, 0.001, d) * np.sin(2 * np.pi * f0 * r * (t - t0))
    return out


def brass(rng):
    t = tt(2.5)
    swell = np.clip(np.sin(np.pi * t / 2.5) * 1.3, 0, 1)
    out = np.zeros_like(t)
    for note in (53, 57, 60, 65):
        f0 = hz(note)
        hs = np.arange(1, 80)
        # brightness follows the swell: the harmonics' cutoff rises with it
        out += additive(t, f0, [np.exp(-h / (5 + 30 * swell)) / h ** 0.3 for h in hs])
    return out * swell


def drone(rng):
    t = tt(3.0)
    out = np.zeros_like(t)
    for t0, note in [(0.0, 43), (0.75, 48), (1.5, 48), (2.25, 36)]:    # Pa Sa Sa Sa (G2 C3 C3 C2)
        f0 = hz(note)
        hs = np.arange(1, 160)
        for h in hs:
            if f0 * h > FS * 0.45:
                break
            # jawari: a buzz peak that sweeps down the harmonics as the string decays
            peak = 30 * np.exp(-(t - t0) / 1.2) + 4
            a = np.exp(-((h - peak) / 6.0) ** 2) * 0.6 + 0.5 / h
            out += a * env_ad(t, t0, 0.005, 2.5) * np.sin(2 * np.pi * f0 * h * (t - t0) + h)
    return out


def rain(rng):
    t = tt(3.0)
    out = svf_bp(rng.normal(0, 0.05, len(t)), 3000, 0.5)
    for _ in range(90):                                              # droplets: bubbles whose pitch rises
        t0 = rng.uniform(0, 2.9)
        f = rng.uniform(1200, 4500)
        d = t - t0
        chirp = f * (1 + 0.8 * np.clip(d, 0, None) / 0.03)
        ph = 2 * np.pi * np.cumsum(np.where(d >= 0, chirp, 0)) / FS
        out += rng.uniform(0.1, 0.4) * env_ad(t, t0, 0.0005, 0.012) * np.sin(ph)
    return out


def birds(rng):
    """chirps spread over the whole length (no gap of silence for the stretch to make endless) over a soft bed of
    leaves and air"""
    t = tt(3.0)
    bed = rng.normal(0, 1.0, len(t))
    out = 0.06 * svf_bp(bed, 900, 0.7) + 0.025 * svf_bp(bed, 2600, 1.5)
    for i in range(30):
        t0 = (i + rng.uniform(0, 1)) * 2.85 / 30
        f, span, dur = rng.uniform(2500, 5000), rng.uniform(-0.5, 0.6), rng.uniform(0.04, 0.15)
        d = np.clip((t - t0) / dur, 0, 1)
        freq = f * (1 + span * d + 0.15 * np.sin(2 * np.pi * d * rng.integers(1, 4)))
        ph = 2 * np.pi * np.cumsum(freq) / FS
        win = np.where((t >= t0) & (t <= t0 + dur), np.sin(np.pi * d) ** 2, 0)
        out += rng.uniform(0.3, 1.0) * win * (np.sin(ph) + 0.2 * np.sin(2 * ph))
    return out


def whisper(rng):
    t = tt(3.0)
    x = rng.normal(0, 0.3, len(t))
    out = np.zeros_like(t)
    for w, v in zip(xfade_segments(t, 5, 0.12), "aeiou"):
        out += w * sum(g * svf_bp(x, fc, fc / bw) for fc, bw, g in VOWEL[v])
    return out * (0.4 + 0.6 * np.sin(np.pi * t / 3.0))


def gamelan(rng):
    t = tt(3.0)
    out = np.zeros_like(t)
    for t0, note in [(0.0, 64), (0.3, 65), (0.6, 67), (1.0, 71), (1.3, 72), (1.8, 67), (2.2, 64)]:   # pelog-ish
        f0 = hz(note)
        for r, a, d in [(1.0, 1.0, 1.8), (1.52, 0.4, 1.0), (3.46, 0.3, 0.5), (3.92, 0.25, 0.4)]:
            for beat in (0.0, 5.0):                                  # ombak: the pair tuned 5 Hz apart
                if (f0 * r + beat) < FS * 0.45:
                    out += 0.5 * a * env_ad(t, t0, 0.002, d) * np.sin(2 * np.pi * (f0 * r + beat) * (t - t0))
    return out


def tape(rng):
    t = tt(3.0)
    wow = 1 + 0.004 * np.sin(2 * np.pi * 0.7 * t) + 0.0015 * np.sin(2 * np.pi * 6.5 * t)
    out = np.zeros_like(t)
    for t0, notes in [(0.0, (57, 60, 64, 67)), (1.5, (53, 57, 60, 64))]:
        for note in notes:
            f0 = hz(note)
            tine = additive(t, f0, [1.0, 0.3, 0.1], wow) + 0.15 * additive(t, f0 * 7.1, [1.0], wow)
            out += tine * env_ad(t, t0, 0.003, 1.2)
    hiss = svf_bp(rng.normal(0, 0.02, len(t)), 6000, 0.7)
    lp = np.convolve(out, np.ones(6) / 6, mode="same")               # a dull tape head
    return lp + hiss


SOURCES = [("CHOIR", choir), ("GLASS", glass), ("STRING", string), ("WIND", wind), ("PIANO", piano),
           ("BOWED", bowed), ("FLUTE", flute), ("ORGAN", organ), ("KALIMB", kalimba), ("BRASS", brass),
           ("DRONE", drone), ("RAIN", rain), ("BIRDS", birds), ("WHISPR", whisper), ("GAMELN", gamelan),
           ("TAPE", tape)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--wav-dir")
    a = ap.parse_args()
    out = Path(a.out)
    stamp = f"/* gen_source.py {VERSION} {KEEP} {' '.join(s[0] for s in SOURCES)} */"
    if out.exists() and out.read_text().startswith(stamp) and not a.wav_dir:
        return                             # made already, with these settings
    lines = [stamp, "/* AETHER's built-in samples (app/tools/gen_source.py): generated, do not edit */",
             "#pragma once", "#include <stdint.h>", '#include "aether.h"', ""]
    table, total = [], 0
    hop = aet_sbr.env_hop(KEEP)
    for i, (name, fn) in enumerate(SOURCES):
        x = fn(np.random.default_rng(1000 + i))
        x = x / (np.max(np.abs(x)) or 1.0) * 0.89
        kept = aet_sbr.resample(x, FS, KEEP)
        kept = kept / (np.max(np.abs(kept)) or 1.0) * 0.89
        pcm = [int(v) for v in np.round(kept * 32767)]
        data = aet_adpcm.encode(pcm)
        env = aet_sbr.envelope(x * (0.89 / (np.max(np.abs(aet_sbr.resample(x, FS, KEEP))) or 1.0)), FS, KEEP, hop)
        total += len(data) + len(env)
        ident = f"aet_src_{name.lower()}"
        for nm, blob in ((ident, data), (ident + "_env", env)):
            lines.append(f"static const uint8_t {nm}[{len(blob)}] = {{")
            for k in range(0, len(blob), 24):
                lines.append("    " + ", ".join(str(v) for v in blob[k:k + 24]) + ",")
            lines.append("};")
        table.append(f'    {{"{name}", {ident}, {len(pcm)}u, {KEEP}u, {ident}_env, {len(env) // aet_sbr.BANDS}u, '
                     f'{hop}u}},')
        if a.wav_dir:
            Path(a.wav_dir).mkdir(parents=True, exist_ok=True)
            with wave.open(str(Path(a.wav_dir) / f"{name.lower()}.wav"), "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(FS)
                w.writeframes(struct.pack(f"<{len(x)}h", *(int(v) for v in np.round(x * 32767))))
    lines += ["typedef struct {", "    const char *name;", "    const uint8_t *data;   /* AET_FMT_ADPCM */",
              "    uint32_t len, rate;", "    const uint8_t *env;    /* SBR (aet_set_env) */",
              "    uint32_t env_frames, env_hop;", "} aet_builtin_t;", f"#define AET_NBUILTIN {len(SOURCES)}u",
              "static const aet_builtin_t aet_builtin[AET_NBUILTIN] = {", *table, "};"]
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text("\n".join(lines) + "\n")
    print(f"sources: {len(SOURCES)}, {total} B (ADPCM at {KEEP} Hz + SBR envelopes) -> {out}")


if __name__ == "__main__":
    main()
