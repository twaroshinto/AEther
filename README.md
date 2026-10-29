# AEther

A spectral synthesizer for the **M-VAVE FM-1** that works like [PaulStretch](https://hypermammut.sourceforge.net/paulstretch/):
long windows, the spectrum's amplitudes kept, **random phases**, overlapping frames. Every note becomes a dense,
endless cloud. Custom firmware on [fm1-base](https://github.com/charlesvestal/fm1-base) /
[felucca-bsp](https://github.com/hugelton/felucca-bsp).

> **⚠ AI-generated code, no warranty of any kind, use at your own risk.** All of the code, tools, tests and
> documentation were written by an AI (Claude, Anthropic). Nobody has reviewed them line by line. Details in
> [NOTICE](NOTICE).

> **Status: v0.5, first runs on a real FM-1.** The engine and the whole app pass their tests in the fm1-base
> simulator (the same sources as the firmware), and CI builds the `.fwsc` with the JieLi toolchain and runs it in
> an emulator of the FM-1's processor. You install it at your own risk.

Polski: [README.pl.md](README.pl.md).

![panel](docs/img/manual/panel_numbered.png)

**Site: [twaroshinto.github.io/AEther](https://twaroshinto.github.io/AEther/)**: the web installer, AEther in the
browser, the editor and the manual.

**[User manual](docs/MANUAL.md)**: every knob, page and preset, with screenshots from the emulator
([po polsku](docs/MANUAL.pl.md)).

## What it sounds like and how it works

Five kinds of spectrum source, one engine:

- **Sample** (16 built in, plus USER): a sample scanned very slowly (STRETCH from 1x to ∞): PaulStretch as such.
  The built-in ones are synthesised at build time (`app/tools/gen_source.py`); your own goes in over USB-MIDI. The
  key moves the spectrum (C4 is the original pitch), so one sample plays chords.
- **VA**: a virtual analogue oscillator (saw → square → pulse, a slow LFO with drift) playing C4 all the time.
  Each frame the engine smears its newest window and moves it to each key's pitch. The filter (24 dB/octave,
  resonant) works on the spectrum after the voices are summed, so it has the same cutoff for every key: high
  notes do not get brighter, and the harmonics are blurred into bands as in PADsynth.
- **PAD**: [PADsynth](https://zynaddsubfx.sourceforge.io/doc/PADsynth/PADsynth.htm), by the same author: each
  harmonic of the note is a Gaussian band in the spectrum (its width BAND, in cents). No input material: the
  spectrum is made straight in the frequency domain.
- **HARM**: harmonics drawn straight into the spectrum: how many (1–64), their slope, the even / odd balance, a
  boost or a notch around a chosen harmonic, a tone that follows velocity, and **four envelopes** (ATTACK, DECAY,
  SUSTAIN), each on one harmonic or a group (ODD, EVEN, LOW, MID, HIGH, ALL).
- **LIVE VOX, AIR, BOW, BELL**: generators that play live (voices through formants, wind on resonances,
  Karplus-Strong strings under a "bow", a field of modal bells). As in 64k intros: a recipe instead of data, a few
  hundred bytes of code each, no sample memory, no loop, and they change slowly by themselves.

And a **modulation matrix**: 8 slots of "source → destination × amount (±100 %)". The sources are three LFOs
(7 shapes including random and drift, with FADE and SYNC to the note), two ADSR envelopes, velocity, the mod wheel
(CC1), key pressure and aftertouch, a random value per note and the note's pitch. There are 36 destinations,
among them another LFO's rate and the EQ's bands. One source can move several parameters, and one parameter can
follow several sources. **EVOLVE** (SEQ) moves the whole sound over time: every factory preset evolves slowly.

| Built-in samples | |
|---|---|
| CHOIR, BOWED, FLUTE, BRASS, WHISPR | voices and instruments through formants: a choir on vowels, a cello, a breathy flute, a brass section, a whisper |
| PIANO, STRING, KALIMB, GLASS, GAMELN | struck and plucked: a piano (inharmonicity, paired strings), Karplus-Strong, a kalimba, glass, a gamelan with ombak |
| ORGAN, DRONE, TAPE | an organ (registers), a tanpura with jawari buzz, an electric piano off tape (wow, flutter, hiss) |
| WIND, RAIN, BIRDS | wind, rain with drops, birds |

Each is 2.5–4 s long, synthesised at 44.1 kHz and kept at **11 kHz as IMA ADPCM with an SBR envelope** (~16 KB
for 3 s, 270 KB of flash in all). The engine rebuilds the band above 4.9 kHz as it plays (SBR, below). As 44.1 kHz
WAVs to listen to or edit: `python3 app/tools/gen_source.py --out /tmp/s.h --wav-dir starter/` (CI publishes them
too, as the `starter-samples` artifact).

### SBR: little in memory, the full band out

A sample can sit in memory at a low rate (11 or 22 kHz) while the engine rebuilds the missing top band like
**SBR (Spectral Band Replication)** in HE-AAC: it takes the sample's highest octave, moves it up by octaves
(harmonics stay harmonics) and scales it to a stored envelope: 8 bands every 50 ms, ~170 bytes a second. In a
codec SBR's weak points are phases and transients, but PaulStretch randomises the phases anyway, so here it has
practically no drawback. The test (`tests/dsp_test.c`): STRING kept at 11 kHz is on average 2.0 dB from the
full-band original over 5–16 kHz with SBR, and 14.4 dB without it.

## Playing

In short (all of it in [docs/MANUAL.md](docs/MANUAL.md)): **ALGORITHM** picks the kind of source (SAMPLE, LIVE,
PAD, HARM, VA), **SELECT** which sample or generator, **PRESETS** a preset, **KNOB 1–4** are the current page's four
parameters. The pages are under the buttons: **EDIT** (the source; with HARM also TONE and HARM ENV A–D), **ENV**
(AMP, MOD ENV 1–2), **LFO** (LFO 1–3, MATRIX), **SEQ** (EVOLVE: motion over time), **FX** (COLOR, EQ 1–3), **GLO**
(SETUP). A button brings back the page last seen in its group, and the next press steps on. **HOME** goes back to
the main page (on it, silences), and held shows the buttons' map. **LFO** or **ENV** held while turning a knob
routes modulation to it. **PLAY** is HOLD, **REC** is FREEZE, **SAVE** saves. MIDI: notes, CC64 sustain, CC1 and
aftertouch as matrix sources (WHEEL, PRESSURE), CC71 SPREAD, CC74 TILT.

## Processing power and the second core

The engine splits each frame's work into small steps spread over the audio blocks, each step weighted by what it
costs, so every block gets the same share and no block gets a whole frame. The analysis FFT is a real FFT (half
the work), the FFT's first stages need no multiplies, and the random phases take no branches. `tests/bench.c`
shows where a frame's time goes, stage by stage. The HARM and PAD spectra are drawn from a table instead of `expf`,
and harmonics below −120 dB are skipped.

**Overload guard** (always on): when an audio block takes over 90 % of its time, AEther drops SUB and SHIMMER,
then voices, down to two. After three quiet seconds it comes back step by step.

**Second core** (SETUP → 2ND CORE, **experimental**, off by default): core 1 paints half the voices' spectra while
core 0 waits in the audio interrupt. The core starts only at power-on. Each job carries a check word, and a late
or wrong answer turns the core off until power-off (the sound goes on, on one). Checked only in host tests (the
same result as one core); not yet on the device.

## In the browser

**The emulator.** The whole firmware (panel, screen, sound) runs in the browser too: the same code, compiled to
WebAssembly with Emscripten and played in an AudioWorklet (fm1-base's emulator). The computer keyboard: `Q`–`I`
and `A`–`K` are the white keys, digits the black ones, `Z`/`X` change the octave, `P` is HOLD, `O` FREEZE.
Shift-click holds a key (chords with the mouse).

```sh
./build.sh --web                                  # app/build/emu/ (needs emcc)
cd app/build/emu && python3 -m http.server 8000   # http://localhost:8000
```

**The editor.** `web/editor/` (on the site: **editor/**) connects to an FM-1 running AEther over USB-MIDI (Web
MIDI with SysEx: Chrome, Edge, Opera). It shows every page and parameter at once, the spectrum with the EQ curve,
the modulation matrix with its sources' values as they are, and where modulation moves each parameter right now.
A knob turned on the FM-1 shows on the page at once, and the other way round. A sound can be stored on the FM-1
as one of 16 user presets (it shows in the PRESETS list on the device too), or as the settings for the next
power-on. Without an FM-1 the page is a preview. The protocol: the "the editor" comment in `app/app.c`; the
parameter map (`params.json`) comes from the host build (`app/tests/params.txt`), so it always matches the
firmware.

The user presets take two flash sectors below the panel's settings, so the area for your own sample is 270 KB
(23 s rather than 24 s).

## Your own sample

```sh
pip install numpy mido python-rtmidi
python3 tools/aether_load.py my_sound.wav                 # mono, 22.05 kHz, ADPCM + SBR: up to 23 s
python3 tools/aether_load.py my_sound.wav --rate 11025    # 4.9 kHz in memory, the rest from SBR: up to 47 s
python3 tools/aether_load.py my_sound.wav --pcm16         # 16-bit, no ADPCM noise: up to 6.1 s
python3 tools/aether_load.py my_sound.wav --rate 44100    # the full band, no SBR: up to 12 s (3 s with --pcm16)
python3 tools/aether_load.py long.wav --start 12 --secs 5 # a part
python3 tools/aether_load.py my_sound.wav --syx out.syx   # a .syx file for SysEx Librarian / MIDI-OX
```

The sample goes into the flash (it survives power-off) and becomes the USER source at once. ADPCM's signal to
noise is about 27–36 dB: once the phases are smeared, the quantisation noise turns into a quiet "breath" under the
cloud, which rarely matters. For clean, quiet material use `--pcm16`. An interrupted load breaks nothing: the
header is erased first and written only after the CRC checks out.

## Building

```sh
git clone --recursive https://github.com/twaroshinto/AEther && cd AEther
sh tests/run.sh          # the engine's tests + the whole app in the simulator (cc, python3, Pillow, numpy)
./build.sh --host        # the simulator alone: app/build/host/aether_host SCRIPT DIR
./build.sh               # the firmware: app/build/aether.fwsc
```

The firmware needs the JieLi toolchain (Linux x86-64) and three files from the AC79 SDK:

```sh
sh base/tools/get_toolchain.sh ~/.jieli
sh base/bsp/tools/get_sdk.sh ~/.jieli/ac79-sdk
JIELI_TOOLCHAIN=~/.jieli/toolchain AC79_SDK=~/.jieli/ac79-sdk ./build.sh
```

The simplest: **GitHub Actions** (`.github/workflows/build.yml`) does the same and offers `aether.fwsc` as an
artifact of every push; `main` builds the release in `VERSION` and deploys the site, a tag `vX.Y` makes a release.

## Installing and going back

The easiest: the **[web installer](https://twaroshinto.github.io/AEther/install/)** (Chrome or Edge, the FM-1 on
USB). **When it is done, switch the FM-1 off and on again**: without a full restart after the install the sound
can drop out. From a terminal:

```sh
python3 base/tools/fm1_install.py app/build/aether.fwsc     # over USB-MIDI (mido + python-rtmidi)
```

Back to the official firmware: the installer's "Back to the stock firmware", or M-VAVE's updater. If something
goes wrong, holding **OCT− and OCT+ for 5 s** enters update mode, and `base/bsp/tools/fm1_rescue.sh` (macOS) puts
the original back on an FM-1 that does not start.

## Layout

| Path | What |
|---|---|
| `app/dsp/aether.c`, `aether.h` | the engine: portable C99, float, no allocation and no libc; the ADPCM decoder |
| `app/dsp/va.c`, `va.h` | the VA source: a polyBLEP oscillator and MOTION (the filter works on the spectrum, in `aether.c`) |
| `app/dsp/live.c`, `live.h` | the LIVE generators: VOX, AIR, BOW, BELL |
| `app/app.c`, `app/app.h` | the FM-1 app: panel, screen, MIDI, presets, the matrix, the overload guard, sample loading and the editor (SysEx) |
| `app/hw/core1.c`, `core1.S` | the second core (experimental): start, job mailbox, watch |
| `app/fm1.toml` | fm1-base's build settings |
| `app/tools/gen_source.py` | the built-in samples' generator (at build time, no recordings, no licences) |
| `app/tools/aet_adpcm.py` | the IMA ADPCM coder in the engine's format (the generator and `aether_load.py`) |
| `app/tools/aet_sbr.py` | resampling and the SBR envelope (numpy) |
| `app/tests/*.txt` | the simulator's scenarios: keys, pages, HOLD, FREEZE, MIDI, saving, sample loading, the editor |
| `tests/dsp_test.c` | the engine's tests: pitch, levels, silence after release, no late frames, the work budget |
| `tests/bench.c` | what a frame costs, stage by stage |
| `web/editor/` | the editor |
| `tools/aether_load.py` | loading WAVs over USB-MIDI |
| `tools/make_site.sh` | the site (GitHub Pages) |
| `base/` | fm1-base (a submodule) |

## Licence, no warranty, thanks

MPL-2.0, as fm1-base. **The code is AI-generated and offered with no warranty of any kind (the MPL's sections 6
and 7 apply in full). You install and use it at your own risk**: the authors are not liable for damage to a
device, data or hearing. All of it in [NOTICE](NOTICE). The idea of random phases over long windows and the
PADsynth profile are Nasca Octavian Paul's (PaulStretch, PADsynth). **AEther's code is written from scratch and
contains none of PaulStretch's** (which is GPL). The platform: fm1-base (Charles Vestal) and felucca-bsp (Leo
Kuroshita, Hügelton Instruments). An unofficial project, not affiliated with M-VAVE or JieLi.
