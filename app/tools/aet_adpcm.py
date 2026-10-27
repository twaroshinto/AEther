# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 fm1-aether contributors
# AI-generated code (Claude, Anthropic): no warranty, use at your own risk (see NOTICE).
"""AETHER's sample format AET_FMT_ADPCM (app/dsp/aether.h): IMA ADPCM, 4 bits a sample, in blocks of 256 samples of
132 bytes each: the first sample (int16 LE), the step index, a zero byte, then 255 codes, two a byte, low nibble first.
Every block starts afresh, so the engine can read from any point. The encoder mirrors the C decoder exactly."""
import struct

BLOCK, BYTES = 256, 132
INDEX = [-1, -1, -1, -1, 2, 4, 6, 8]
STEP = [7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
        107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
        876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
        27086, 29794, 32767]


def _step(pred, idx, code):
    st = STEP[idx]
    d = st >> 3
    if code & 4:
        d += st
    if code & 2:
        d += st >> 1
    if code & 1:
        d += st >> 2
    pred = max(-32768, min(32767, pred - d if code & 8 else pred + d))
    return pred, max(0, min(88, idx + INDEX[code & 7]))


def encode(pcm):
    """int16 samples -> bytes (AET_ADPCM_SIZE(len(pcm)) of them)"""
    out = bytearray()
    idx = 0
    for b0 in range(0, len(pcm), BLOCK):
        blk = list(pcm[b0:b0 + BLOCK]) + [0] * max(0, b0 + BLOCK - len(pcm))
        pred = blk[0]
        out += struct.pack("<hBB", pred, idx, 0)
        codes = []
        for x in blk[1:]:
            d, best = x - pred, 0          # the IMA rule; the state follows the decoder's own arithmetic
            if d < 0:
                best, d = 8, -d
            st = STEP[idx]
            if d >= st:
                best, d = best | 4, d - st
            if d >= st >> 1:
                best, d = best | 2, d - (st >> 1)
            if d >= st >> 2:
                best |= 1
            pred, idx = _step(pred, idx, best)
            codes.append(best)
        codes.append(0)                  # 255 codes: the last byte's high nibble is unused
        out += bytes(codes[i] | codes[i + 1] << 4 for i in range(0, 256, 2))
    return bytes(out)


def decode(data, n):
    """bytes -> int16 samples (for checks)"""
    out = []
    for b in range(0, len(data), BYTES):
        pred, idx, _ = struct.unpack_from("<hBB", data, b)
        out.append(pred)
        for j in range(255):
            code = data[b + 4 + j // 2] >> (4 if j & 1 else 0) & 15
            pred, idx = _step(pred, min(idx, 88), code)
            out.append(pred)
    return out[:n]
