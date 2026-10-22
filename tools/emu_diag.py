#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""CI, the firmware in fm1-emu: an LCD snapshot (BMP) as text, and a --dump's words as floats.

  emu_diag.py lcd FILE.bmp            the screen, two by two pixels a character
  emu_diag.py dump RUN.log            the dump's words: hex, float, and counts of NaN / zero
  emu_diag.py sym SYMS NAME...        a symbol's address and size from `objdump -t`"""
import math, re, struct, sys


def lcd(path):
    d = open(path, 'rb').read()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    bpp = struct.unpack_from('<H', d, 28)[0]
    stride = (w * bpp // 8 + 3) & ~3
    rows = []
    for y in range(abs(h)):
        yy = abs(h) - 1 - y if h > 0 else y
        r = d[off + yy * stride: off + yy * stride + stride]
        px = []
        for x in range(w):
            if bpp == 24 or bpp == 32:
                b, g, rr = r[x * bpp // 8: x * bpp // 8 + 3]
                v = (b + g + rr) / 3
            elif bpp == 16:
                p = struct.unpack_from('<H', r, 2 * x)[0]
                v = ((p >> 11) & 31) * 8
            else:
                v = 255 if (r[x // 8] >> (7 - x % 8)) & 1 else 0
            px.append(v)
        rows.append(px)
    print(f'{path}: {w}x{abs(h)} {bpp} bpp')
    for y in range(0, len(rows) - 1, 2):
        m = [max(rows[y][x], rows[y + 1][x], rows[y][x + 1], rows[y + 1][x + 1]) for x in range(0, len(rows[y]) - 1, 2)]
        print(''.join('#' if v > 96 else '.' if v > 32 else ' ' for v in m).rstrip())


def dump(path):
    words = []
    on = False
    for line in open(path, errors='replace'):
        if line.startswith('dump '):
            on = True
            continue
        if on:
            ws = re.findall(r'\b(?:0x)?([0-9a-f]{8})\b', line.split(':', 1)[1] if ':' in line else line)
            if not ws:
                break
            words += [int(w, 16) for w in ws]
    fl = [struct.unpack('<f', struct.pack('<I', w))[0] for w in words]
    nan = sum(1 for f in fl if math.isnan(f) or math.isinf(f))
    zero = sum(1 for w in words if w == 0)
    print(f'{len(words)} words: {nan} NaN/inf, {zero} zero')
    for i in range(0, len(words), 8):
        print(f'{i:5d} ' + ' '.join(f'{w:08x}' for w in words[i:i + 8]) + '   ' + ' '.join(f'{f:.3g}' for f in fl[i:i + 8]))


def sym(path, names):
    found = {}
    for line in open(path, errors='replace'):
        f = line.split()
        if len(f) >= 4 and re.fullmatch(r'[0-9a-f]{8}', f[0]) and re.fullmatch(r'[0-9a-f]{8}', f[-2]):
            found.setdefault(f[-1], (f[0], int(f[-2], 16)))
    for n in names:
        print(n, ('0x%s %d' % found[n]) if n in found else 'missing')


if __name__ == '__main__':
    {'lcd': lambda a: [lcd(p) for p in a], 'dump': lambda a: dump(a[0]), 'sym': lambda a: sym(a[0], a[1:])}[sys.argv[1]](sys.argv[2:])
