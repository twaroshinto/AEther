#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# Copyright (C) 2026 fm1-aether contributors
# AI-generated code (Claude, Anthropic): no warranty, use at your own risk (see NOTICE).
"""Load a sample into AETHER on an FM-1 over USB-MIDI (SysEx), for its USER source.

  aether_load.py SOUND.wav                  load it: mono, 22050 Hz, IMA ADPCM (4 bits a sample), up to 24 s;
                                           the band above 9.7 kHz rebuilt on the FM-1 (SBR) from a small envelope
  aether_load.py SOUND.wav --rate 11025     twice as long (48 s): kept to 4.9 kHz, the rest rebuilt by SBR
  aether_load.py SOUND.wav --pcm16          16-bit instead: no ADPCM hiss, a quarter of the length
  aether_load.py SOUND.wav --rate 44100     the full band kept, no SBR (12 s; 3.1 s with --pcm16)
  aether_load.py SOUND.wav --no-sbr         no envelope: the band above the kept rate's Nyquist stays empty
  aether_load.py SOUND.wav --start 1.5 --secs 4   a part of it
  aether_load.py SOUND.wav --image OUT.bin  no FM-1: the flash image instead (header sector + data), for the host
                                           simulator's `data OUT.bin 0x97000` (tests)
  aether_load.py SOUND.wav --syx OUT.syx    no FM-1 either: the SysEx messages into a file, for any SysEx sender
                                           (SysEx Librarian, MIDI-OX, amidi: with a pause of 20 ms or more between
                                           messages) or the host simulator's `sysex OUT.syx`
  aether_load.py --list                     the MIDI ports

Needs numpy (resampling, the SBR envelope), and mido with python-rtmidi to talk to the FM-1
(pip install numpy mido python-rtmidi). Reads WAV files (8, 16, 24 or
32-bit PCM, any channels) with the standard library; other formats: convert first (e.g. ffmpeg -i in.mp3 out.wav).

The protocol (app/app.c, "sample load"): F0 7D 41 45 cmd ... F7, each command answered F0 7D 41 45 7F cmd status F7.
BEGIN (01) len rate format bytes env_frames env_hop name, DATA (02) offset + 7-in-8 packed bytes (the samples, then
the SBR envelope), END (03) CRC-32."""
import argparse
import struct
import sys
import time
import wave
import zlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "app" / "tools"))
import aet_adpcm  # noqa: E402
import aet_sbr  # noqa: E402

HDR_SECTOR = 0x1000
MAX_BYTES = 0xDA000 - 0x98000          # app.h SMP_MAX_BYTES
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


def prepare(a):
    """(samples, bytes: the audio then the envelope, env_frames, env_hop)"""
    import numpy as np
    x, rate = read_wav(a.wav)
    x = np.asarray(x[int(a.start * rate):])
    if a.secs:
        x = x[:int(a.secs * rate)]
    peak = float(np.max(np.abs(x))) if len(x) else 0.0
    x = x * (0.95 / peak if a.normalize and peak > 0 else 1.0)
    room = MAX_BYTES - 8192                       # (8 KB kept for the envelope: 48 s at 11025 Hz take 7.7 KB)
    keep = aet_sbr.resample(x, rate, a.rate)
    keep = keep[:room // 2 if a.pcm16 else room // aet_adpcm.BYTES * aet_adpcm.BLOCK]
    pcm = [int(v) for v in np.clip(np.round(keep * 32767), -32768, 32767)]
    data = struct.pack(f"<{len(pcm)}h", *pcm) if a.pcm16 else aet_adpcm.encode(pcm)
    hop = aet_sbr.env_hop(a.rate)
    env = b"" if a.no_sbr else aet_sbr.envelope(x[:int(len(pcm) * rate / a.rate) + 1], rate, a.rate, hop)
    print(f"{a.wav}: {len(pcm)} samples at {a.rate} Hz ({len(pcm) / a.rate:.2f} s), peak "
          f"{20 * np.log10(peak or 1e-9):.1f} dBFS, {'PCM16' if a.pcm16 else 'ADPCM'} {len(data)} B"
          f"{f', SBR envelope {len(env)} B' if env else ''}")
    return len(pcm), data + env, len(env) // aet_sbr.BANDS, hop if env else 0


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


def fmt_of(a):
    return 0 if a.pcm16 else 1         # AET_FMT_PCM16, AET_FMT_ADPCM


def image(a, smp):
    n, data, frames, hop = smp
    hdr = struct.pack("<IIIIIIII16s", 0x31534541, a.rate, n, zlib.crc32(data), fmt_of(a), len(data), frames, hop,
                      name_of(a))
    with open(a.image, "wb") as f:
        f.write(hdr + b"\xff" * (HDR_SECTOR - len(hdr)) + data)
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
        self.out.send(self.mido.Message("sysex", data=bytes([0x7D, 0x41, 0x45, cmd]) + body))
        t0 = time.time()
        while time.time() - t0 < timeout:
            for m in self.inp.iter_pending():
                d = bytes(m.data) if m.type == "sysex" else b""
                if d[:5] == bytes([0x7D, 0x41, 0x45, 0x7F, cmd]):
                    return d[5]
            time.sleep(0.002)
        raise SystemExit(f"no answer from the FM-1 (command {cmd}): is AETHER running?")


def messages(a, smp):
    """the whole load as SysEx bodies (between F0 and F7)"""
    n, data, frames, hop = smp
    yield 1, w7(n, 4) + w7(a.rate, 3) + bytes([fmt_of(a)]) + w7(len(data), 4) + w7(frames, 3) + w7(hop, 2) + name_of(a)
    for off in range(0, len(data), CHUNK):
        yield 2, w7(off, 4) + pack78(data[off:off + CHUNK])
    yield 3, w7(zlib.crc32(data), 5)


def syx(a, smp):
    with open(a.syx, "wb") as f:
        for cmd, body in messages(a, smp):
            f.write(bytes([0xF0, 0x7D, 0x41, 0x45, cmd]) + body + b"\xf7")
    print(f"syx: {a.syx}")


def load(a, smp):
    pcm = smp[1]
    link = Link(a.port)
    t0 = time.time()
    for cmd, body in messages(a, smp):
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
    ap.add_argument("--pcm16", action="store_true", help="16-bit PCM instead of ADPCM")
    ap.add_argument("--no-sbr", action="store_true", help="no SBR envelope")
    ap.add_argument("--port", default="AETHER", help="part of the FM-1's MIDI port name")
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
    smp = prepare(a)
    if a.image or a.syx:
        if a.image:
            image(a, smp)
        if a.syx:
            syx(a, smp)
    else:
        load(a, smp)


if __name__ == "__main__":
    main()
