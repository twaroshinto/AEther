# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 fm1-smuga contributors
"""SMUGA's sample preparation, shared by gen_source.py and tools/smuga_load.py (needs numpy):

  resample(x, rate, to)          band-limited (windowed-sinc low-pass, then interpolation)
  envelope(x, rate, keep, hop)   the SBR envelope (app/dsp/smuga.h): for a sample kept at `keep` Hz, one frame every
                                 `hop` kept samples, SMG_SBR_BANDS bytes of band energy from SMG_SBR_LO(keep) to
                                 SMG_SBR_HI, measured on the full-band original x (at `rate`) in the engine's own scale
  env_hop(keep)                  the hop used: about 50 ms
"""
import numpy as np

BANDS, HI, FS = 8, 16000.0, 44100


def lo_of(rate):
    return 0.44 * rate                    # SMG_SBR_LO


def env_hop(keep):
    return max(64, keep // 20)


def lowpass(x, cutoff, rate, taps=255):
    n = np.arange(taps) - (taps - 1) / 2
    h = np.sinc(2 * cutoff / rate * n) * np.blackman(taps)
    h /= h.sum()
    return np.convolve(x, h, mode="same")


def resample(x, rate, to):
    x = np.asarray(x, dtype=np.float64)
    if rate == to:
        return x
    if to < rate:
        x = lowpass(x, 0.45 * to, rate)
    n = int(len(x) * to / rate)
    t = np.arange(n) * (rate / to)
    if to > rate:                          # up: through 4x with the low-pass, then linear interpolation
        up = np.zeros(len(x) * 4)
        up[::4] = x * 4
        x = lowpass(up, 0.45 * rate, rate * 4)
        t = t * 4
    return np.interp(t, np.arange(len(x)), x)


def envelope(x, rate, keep, hop):
    """bytes: frames x BANDS; frame f measures the original around kept sample f * hop"""
    if lo_of(keep) >= 0.9 * HI:
        return b""
    full = resample(x, rate, FS)
    ne = 2048
    w = np.hanning(ne)
    fr = np.fft.rfftfreq(ne, 1 / FS)
    edges = lo_of(keep) * (HI / lo_of(keep)) ** (np.arange(BANDS + 1) / BANDS)
    sel = [(fr >= edges[b]) & (fr < edges[b + 1]) for b in range(BANDS)]
    frames = int(np.ceil(len(full) * keep / FS / hop)) or 1
    pad = np.concatenate([np.zeros(ne), full, np.zeros(ne)])
    out = bytearray()
    for f in range(frames):
        c = int(round(f * hop * FS / keep)) + ne
        seg = pad[c - ne // 2:c + ne // 2] * w
        m2 = (np.abs(np.fft.rfft(seg)) * 4 / ne) ** 2
        for b in range(BANDS):
            e = m2[sel[b]].sum()
            out.append(0 if e < 1e-12 else int(np.clip(round(2 * (10 * np.log10(e) + 120)), 1, 255)))
    return bytes(out)
