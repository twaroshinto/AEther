#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 fm1-smuga contributors
"""Load a sample into SMUGA on an FM-1 over USB-MIDI (SysEx), for its USER source.

  smuga_load.py SOUND.wav                  load it (mono, resampled to 22050 Hz, trimmed to fit: 6.3 s)
  smuga_load.py SOUND.wav --rate 44100     full bandwidth, half the length (3.1 s)
  smuga_load.py SOUND.wav --start 1.5 --secs 4   a part of it
  smuga_load.py SOUND.wav --image OUT.bin  no FM-1: the flash image instead (header sector + data), for the host
                                           simulator's `data OUT.bin 0x97000` (tests)
  smuga_load.py SOUND.wav --syx OUT.syx    no FM-1 either: the SysEx messages into a file, for any SysEx sender
                                           (SysEx Librarian, MIDI-OX, amidi: with a pause of 20 ms or more between
                                           messages) or the host simulator's `sysex OUT.syx`
  smuga_load.py --list                     the MIDI ports

Needs mido with python-rtmidi (pip install mido python-rtmidi) to talk to the FM-1. Reads WAV files (8, 16, 24 or
32-bit PCM, any channels) with the standard library; other formats: convert first (e.g. ffmpeg -i in.mp3 out.wav).

The protocol (app/app.c, "sample load"): F0 7D 53 4D cmd ... F7, each command answered F0 7D 53 4D 7F cmd status F7.
BEGIN (01) len rate name, DATA (02) offset + 7-in-8 packed bytes, END (03) CRC-32."""
import argparse
import struct
import sys
import time
import wave
import zlib

HDR_SECTOR = 0x1000
MAX_BYTES = 0xDC000 - 0x98000          # app.h SMP_MAX_BYTES
CHUNK = 256                            # bytes a DATA message
STATUS = {0: "ok", 1: "not now", 2: "too long", 3: "out of order", 4: "the flash refused", 5: "CRC mismatch"}


def read_wav(path):
    with wave.open(path, "rb") as w:
        ch, width, rate, n = w.getnchannels(), w.getsampwidth(), w.getframerate(), w.getnframes()
        raw = w.readframes(n)
    frames = []
    step = ch * width
    for i in range(0, len(raw) - step + 1, step):
        acc = 0.0
        for c in range(ch):
            b = raw[i + c * width:i + (c + 1) * width]
            if width == 1:
                v = (b[0] - 128) / 128.0
            elif width == 2:
                v = struct.unpack("<h", b)[0] / 32768.0
            elif width == 3:
                v = int.from_bytes(b, "little", signed=True) / 8388608.0
            else:
                v = struct.unpack("<i", b)[0] / 2147483648.0
            acc += v
        frames.append(acc / ch)
    return frames, rate


def resample(x, rate, to):
    if rate == to:
        return x
    # a gentle low-pass (a 2nd-order running mean) when going down, then linear interpolation
    if to < rate:
        k = max(1, int(rate / to))
        y, s = [], 0.0
        for i, v in enumerate(x):
            s += v
            if i >= k:
                s -= x[i - k]
            y.append(s / min(i + 1, k))
        x = y
    n = int(len(x) * to / rate)
    out = []
    for i in range(n):
        t = i * rate / to
        i0 = int(t)
        f = t - i0
        a = x[min(i0, len(x) - 1)]
        b = x[min(i0 + 1, len(x) - 1)]
        out.append(a + (b - a) * f)
    return out


def prepare(a):
    x, rate = read_wav(a.wav)
    x = x[int(a.start * rate):]
    if a.secs:
        x = x[:int(a.secs * rate)]
    x = resample(x, rate, a.rate)
    x = x[:MAX_BYTES // 2]
    peak = max((abs(v) for v in x), default=0.0) or 1.0
    g = 0.95 / peak if a.normalize else 1.0
    pcm = struct.pack(f"<{len(x)}h", *(max(-32768, min(32767, int(round(v * g * 32767)))) for v in x))
    print(f"{a.wav}: {len(x)} samples at {a.rate} Hz ({len(x) / a.rate:.2f} s), peak {20 * __import__('math').log10(peak):.1f} dBFS")
    return pcm


def name_of(a):
    import os
    return (a.name or os.path.splitext(os.path.basename(a.wav))[0]).upper().encode("ascii", "replace")[:15]


def w7(v, n):
    return bytes((v >> (7 * k)) & 0x7F for k in range(n))


def pack78(b):
    out = bytearray()
    for i in range(0, len(b), 7):
        g = b[i:i + 7]
        out.append(sum(((x >> 7) & 1) << k for k, x in enumerate(g)))
        out.extend(x & 0x7F for x in g)
    return bytes(out)


def image(a, pcm):
    hdr = struct.pack("<IIII16s", 0x31534D53, a.rate, len(pcm) // 2, zlib.crc32(pcm), name_of(a))
    with open(a.image, "wb") as f:
        f.write(hdr + b"\xff" * (HDR_SECTOR - len(hdr)) + pcm)
    print(f"image: {a.image} (load at 0x97000)")


class Link:
    def __init__(self, port):
        import mido
        self.mido = mido
        ins = [n for n in mido.get_input_names() if port.lower() in n.lower()]
        outs = [n for n in mido.get_output_names() if port.lower() in n.lower()]
        if not ins or not outs:
            raise SystemExit(f"no MIDI port with '{port}' in its name (--list shows them)")
        self.inp = mido.open_input(ins[0])
        self.out = mido.open_output(outs[0])
        time.sleep(0.3)

    def cmd(self, cmd, body, timeout=5.0):
        self.out.send(self.mido.Message("sysex", data=bytes([0x7D, 0x53, 0x4D, cmd]) + body))
        t0 = time.time()
        while time.time() - t0 < timeout:
            for m in self.inp.iter_pending():
                d = bytes(m.data) if m.type == "sysex" else b""
                if d[:5] == bytes([0x7D, 0x53, 0x4D, 0x7F, cmd]):
                    return d[5]
            time.sleep(0.002)
        raise SystemExit(f"no answer from the FM-1 (command {cmd}): is SMUGA running?")


def messages(a, pcm):
    """the whole load as SysEx bodies (between F0 and F7)"""
    yield 1, w7(len(pcm) // 2, 4) + w7(a.rate, 3) + name_of(a)
    for off in range(0, len(pcm), CHUNK):
        yield 2, w7(off, 4) + pack78(pcm[off:off + CHUNK])
    yield 3, w7(zlib.crc32(pcm), 5)


def syx(a, pcm):
    with open(a.syx, "wb") as f:
        for cmd, body in messages(a, pcm):
            f.write(bytes([0xF0, 0x7D, 0x53, 0x4D, cmd]) + body + b"\xf7")
    print(f"syx: {a.syx}")


def load(a, pcm):
    link = Link(a.port)
    t0 = time.time()
    for cmd, body in messages(a, pcm):
        st = link.cmd(cmd, body, timeout=15.0 if cmd == 3 else 5.0)
        if st:
            raise SystemExit(f"command {cmd} refused: {STATUS.get(st, st)}")
        if cmd == 2:
            off = sum(body[k] << (7 * k) for k in range(4))
            if off % (CHUNK * 64) == 0:
                sys.stdout.write(f"\r{100 * off // len(pcm):3d} %")
                sys.stdout.flush()
    print(f"\r100 %  {len(pcm)} bytes in {time.time() - t0:.1f} s: loaded")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("wav", nargs="?")
    ap.add_argument("--rate", type=int, default=22050, help="the rate it is kept at (22050 or 44100)")
    ap.add_argument("--start", type=float, default=0.0, help="seconds into the file")
    ap.add_argument("--secs", type=float, help="how much of it")
    ap.add_argument("--name", help="its name on the screen (default: the file's)")
    ap.add_argument("--no-normalize", dest="normalize", action="store_false")
    ap.add_argument("--port", default="SMUGA", help="part of the FM-1's MIDI port name")
    ap.add_argument("--image", help="write the flash image instead of loading")
    ap.add_argument("--syx", help="write the SysEx messages to a file instead of loading")
    ap.add_argument("--list", action="store_true")
    a = ap.parse_args()
    if a.list:
        import mido
        print("in: ", mido.get_input_names())
        print("out:", mido.get_output_names())
        return
    if not a.wav:
        ap.error("a WAV file")
    pcm = prepare(a)
    if a.image or a.syx:
        if a.image:
            image(a, pcm)
        if a.syx:
            syx(a, pcm)
    else:
        load(a, pcm)


if __name__ == "__main__":
    main()
