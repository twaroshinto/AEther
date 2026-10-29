/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 fm1-aether contributors
 * AI-generated code (Claude, Anthropic): no warranty, use at your own risk (see NOTICE). */
/* AETHER: a spectral "smear" synthesizer in the spirit of PaulStretch (Nasca Octavian Paul's extreme time stretch).
 *
 * Every frame is built in the frequency domain: a magnitude spectrum (from a sample scanned very slowly, or from a
 * PADsynth-like harmonic profile of each held note), then random phases for every bin, an inverse FFT and a sine
 * window, overlapped by half a frame. Random phases are what turns any sound into a smooth, endless cloud.
 *
 * Polyphony is almost free: all voices are summed into ONE magnitude spectrum, so there is one inverse FFT per frame
 * whatever the number of notes. Stereo is free too: the left and right channels are two independent random-phase
 * frames packed into one complex FFT (the real part is the left, the imaginary part the right).
 *
 * The engine never allocates: the caller gives it AET_RAM_FLOATS floats (on the FM-1, the .pool RAM). Its work is
 * split into small steps spread over the audio blocks of one hop (aet_render does it), so no block carries a whole
 * FFT. Plain C99 and float only: no double, no libc (fastmath.h), so the same file runs on the FM-1 (pi32v2 FPU),
 * in fm1-base's host simulator and in the tests. */
#pragma once
#include <stdint.h>

#define AET_FS 44100u
#define AET_NMAX 8192u                   /* the longest frame: 186 ms at 44.1 kHz */
#define AET_NMIN 1024u
#define AET_VOICES 8u
#define AET_DISP 60u                     /* bands of the display spectrum (log-spaced, 40 Hz .. 16 kHz) */

/* the RAM the engine needs, in floats: the complex FFT buffer (2N), the source and output magnitudes (N/2 + 1 each),
 * the play and tail buffers of both channels (N/2 each), the synthesis window (N/2), a quarter sine (N/4 + 1), the
 * LIVE source's ring (N), the second core's spectrum (N/2 + 1) */
#define AET_RAM_FLOATS (2u * AET_NMAX + 2u * (AET_NMAX / 2u + 1u) + 4u * (AET_NMAX / 2u) + AET_NMAX / 2u + \
                        AET_NMAX / 4u + 1u + AET_NMAX + AET_NMAX / 2u + 1u)

/* PAD: harmonic bands built in the spectrum; SAMPLE: a sample scanned slowly; LIVE: whatever the caller pushes
 * (aet_live_push: a synthesizer running beside the engine), its newest window analysed every frame */
/* HARM: an additive generator drawn straight into the spectrum: each note's harmonics (how many, their slope,
 * odd against even, a boost or cut around one of them, brightness by velocity), each a narrow band as PAD's, and
 * four envelopes that each move the harmonics they are given (one, a group, all), from the note's start */
enum { AET_SRC_PAD, AET_SRC_SAMPLE, AET_SRC_LIVE, AET_SRC_HARM, AET_NSRC };
#define AET_HARM_MAX 64u
#define AET_HENV 4u
/* an envelope's target: 0 none, 1 .. 32 that harmonic, then the groups */
enum { AET_HT_OFF = 0, AET_HT_H32 = 32, AET_HT_ODD, AET_HT_EVEN, AET_HT_LOW, AET_HT_MID, AET_HT_HIGH, AET_HT_ALL,
       AET_NHT };                        /* LOW 1-4, MID 5-12, HIGH 13 and up */
/* sample formats: 16-bit PCM, or IMA ADPCM (4 bits a sample) in blocks of AET_ADPCM_BLOCK samples, each block
 * AET_ADPCM_BYTES bytes: the first sample (int16, little-endian), the step index, a zero, then the other 255
 * samples' codes, two a byte, the low nibble first. Blocks start afresh, so any point is at most 255 codes away. */
enum { AET_FMT_PCM16, AET_FMT_ADPCM, AET_NFMT };
#define AET_ADPCM_BLOCK 256u
#define AET_ADPCM_BYTES 132u
/* SBR (spectral band replication, after HE-AAC): a sample kept at a low rate has the band above its Nyquist rebuilt
 * from its own top octave, transposed up by octaves (so harmonics stay harmonics) and shaped to a stored envelope:
 * AET_SBR_BANDS energies a frame, log-spaced from AET_SBR_LO(rate) to AET_SBR_HI, one frame every env_hop source
 * samples, each a byte: 2 (10 log10 E + 120), 0 = silence. E is the band's sum of squared magnitudes in the engine's
 * own scale (Hann window, |X| 4 / N), which does not depend on N. Random phases make SBR's usual flaws (phase,
 * transients) moot. */
#define AET_SBR_BANDS 8u
#define AET_SBR_HI 16000.0f
#define AET_SBR_LO(rate) (0.44f * (float)(rate))   /* 88 % of the sample's Nyquist: above its anti-alias filter */
#define AET_ADPCM_SIZE(len) (((len) + AET_ADPCM_BLOCK - 1u) / AET_ADPCM_BLOCK * AET_ADPCM_BYTES)
enum { AET_SIZE_1K, AET_SIZE_2K, AET_SIZE_4K, AET_SIZE_8K, AET_NSIZE };

#define AET_EQ 3u                        /* the EQ's bands */
enum { AET_EQ_OFF, AET_EQ_BELL, AET_EQ_LOSHELF, AET_EQ_HISHELF, AET_EQ_LOCUT, AET_EQ_HICUT, AET_NEQ_TYPE };
#define AET_EQ_TAB 160u                  /* the EQ's curve: points over 10 octaves from 20 Hz */

typedef struct {
    const uint8_t *data;                 /* mono, AET_FMT_*; 0 = none */
    uint32_t len;                        /* samples */
    uint32_t rate;                       /* Hz (any: read with interpolation) */
    uint32_t fmt;
    const uint8_t *env;                  /* SBR envelope (aet_set_env): env_frames x AET_SBR_BANDS bytes; 0 = none */
    uint32_t env_frames, env_hop;
} aet_sample_t;

typedef struct {                         /* the ADPCM reader: the block decoded so far */
    uint32_t blk;                        /* 0xFFFFFFFF: none */
    uint32_t pos;                        /* the last code decoded, in the block */
    int32_t pred, prev, idx;             /* the sample at pos, the one before it, the step index */
} aet_adpcm_t;

/* the sound's parameters: the main loop writes them, the engine takes a copy at the start of each frame */
typedef struct {
    uint8_t source;                      /* AET_SRC_* */
    uint8_t size;                        /* AET_SIZE_*: the frame, the smear's length */
    uint8_t freeze;                      /* sample: the scan stops, the spectrum held */
    uint8_t rsv;
    /* SAMPLE */
    float stretch;                       /* 1 .. 10000: how many times slower than the original; 0 = infinite */
    float position;                      /* 0 .. 1: where the scan starts (and loops back to) */
    float spread;                        /* 0 .. 1: spectral blur, tones into noise bands */
    /* PAD */
    float bright;                        /* 0 .. 1: the harmonics' slope */
    float bandwidth;                     /* cents, 1 .. 300: each harmonic's width */
    float inharm;                        /* 0 .. 1: stiff-string stretch of the harmonics */
    float odd;                           /* 0 .. 1: the even harmonics taken out (hollow) */
    /* both */
    float tilt;                          /* -1 .. 1: -6 .. +6 dB per octave around 500 Hz */
    float sub;                           /* 0 .. 1: an octave below, mixed in */
    float shimmer;                       /* 0 .. 1: an octave above, mixed in */
    float attack, release;               /* seconds */
    float width;                         /* 0 .. 1: mono .. two independent channels */
    float level;                         /* the output's gain */
    float lp_hz;                         /* a resonant 24 dB/octave low-pass on the spectrum, the same for every
                                          * key (the VA's filter); 0 = off */
    float lp_reso;                       /* 0 .. 1 */
    uint8_t voice_cap;                   /* the overload guard: voices at most (0: all AET_VOICES) */
    uint8_t no_layers;                   /* the overload guard: SUB and SHIMMER left out */
    /* HARM */
    uint8_t h_count;                     /* harmonics, 1 .. AET_HARM_MAX */
    uint8_t h_target[AET_HENV];          /* each envelope's harmonics: AET_HT_* */
    uint8_t rsv2[3];
    float h_slope;                       /* 0 .. 3: the harmonics fall as 1 / h^slope */
    float h_odd;                         /* -1 .. 1: even only .. both .. odd only */
    float h_band;                        /* cents: each harmonic's width */
    float h_focus;                       /* the harmonic the boost is centred on, 1 .. 32 */
    float h_boost;                       /* dB, -24 .. +24, at the focus */
    float h_width;                       /* the boost's width, in harmonics */
    float h_vel;                         /* 0 .. 1: softer notes darker */
    float h_att[AET_HENV], h_dec[AET_HENV], h_sus[AET_HENV];   /* seconds, seconds, 0 .. 1 */
    /* the EQ: three bands, the last thing on the spectrum (with TILT); a band of 0 dB (or OFF) costs nothing */
    uint8_t eq_type[AET_EQ];             /* AET_EQ_* */
    uint8_t rsv3[1];
    float eq_hz[AET_EQ];                 /* the band's frequency (a cut's corner) */
    float eq_db[AET_EQ];                 /* its gain, dB (shelves and bells) */
    float eq_oct[AET_EQ];                /* its width in octaves (a bell's at half its gain; a shelf's slope) */
} aet_params_t;

typedef struct {
    int8_t note;                         /* MIDI note, -1 = free */
    uint8_t gate;                        /* held */
    uint8_t rsv[2];
    float vel;                           /* 0 .. 1 */
    float level;                         /* the envelope, per frame */
    uint32_t age;                        /* frames since its note-on (stealing) */
} aet_voice_t;

/* a cursor painting voices into a spectrum (aether.c, "Painting the voices") */
typedef struct {
    uint32_t v, vstep, layer, i, units;
    float ratio, gain, norm;
    float *acc;
    uint8_t done, rsv[3];
} aet_cur_t;

#define AET_GT 130u                      /* the Gaussian band's table: exp(-x^2 / 2), x = 0 .. 3.225 */

typedef struct {
    aet_params_t p;                      /* written by the caller */
    float eq_tab[AET_EQ_TAB + 1u];       /* this frame's EQ (and TILT), log2 of its gain, 20 Hz .. 20.5 kHz */
    uint8_t eq_on, rsv_eq[3];
    aet_params_t fp;                     /* this frame's copy */
    aet_sample_t smp;                    /* the source sample (aet_set_sample) */
    aet_adpcm_t ad;
    float *ring;                         /* LIVE: the newest AET_NMAX samples pushed */
    uint32_t ring_w, live_at;            /* the next write; where this frame's window starts */
    float load_scan;                     /* the scan where this frame's window was read (the SBR envelope's time) */
    uint32_t sbr_edge[AET_SBR_BANDS + 1u];   /* SBR: the bands' first bins (the last: the end) */
    float sbr_p[AET_SBR_BANDS];          /* the rebuilt bands' energies, then their gains */
    aet_voice_t voice[AET_VOICES];
    float *work, *src, *acc, *acc2, *play[2], *tail[2], *win, *qs;
    aet_cur_t c0, c1;                    /* the voices' painters: this core's, the second core's (or this one's) */
    float gt[AET_GT];
    /* a second core (aet_set_par): run(fn, arg) starts fn(arg) there, 1 = taken; wait() 0 = done, else it failed
     * (one core from then on). The engine gives it half the voices of each frame. */
    int (*par_run)(void (*fn)(void *), void *arg);
    int (*par_wait)(void);
    uint32_t par_budget, par_jobs;
    uint8_t dual, par_failed, rsv2[2];
    uint32_t n, hop, n_win;              /* the frame being built, its hop, the window's frame */
    uint32_t fn;                         /* the FFT running: n / 2 (the analysis, a real FFT) or n (the synthesis) */
    uint32_t play_pos, play_len;
    uint8_t st, layer, src_ok, rsv;      /* the job: state, the voice's layer, the source spectrum valid */
    uint32_t i, j, h, v;                 /* the job's indices */
    float lay_ratio, lay_gain, lay_norm; /* the layer being added */
    float scan;                          /* the scan position, in source samples */
    float scan_start;                    /* where it loops back to */
    float last_position;
    uint32_t rng;
    uint32_t units, units_frame, units_est;   /* work done: this step, this frame, the estimate per frame */
    uint32_t units_bound, units_peak;    /* the most this frame can take (from its parameters); the most any took */
    uint32_t frames, late;               /* frames made; frames that were not ready in time (finished at once) */
    float disp_acc[AET_DISP];
    float disp[AET_DISP];                /* the last frame's spectrum by band, linear (for a screen) */
} aet_t;

void aet_init(aet_t *e, float *ram);    /* ram: AET_RAM_FLOATS floats; e->p gets defaults */
void aet_defaults(aet_params_t *p);
void aet_set_sample(aet_t *e, const void *data, uint32_t len, uint32_t rate, uint32_t fmt);   /* audio context */
void aet_set_env(aet_t *e, const uint8_t *env, uint32_t frames, uint32_t hop);   /* after aet_set_sample */
/* a second core: from the next frame half of the voices are painted there (0, 0: one core again) */
void aet_set_par(aet_t *e, int (*run)(void (*fn)(void *), void *arg), int (*wait)(void));
int32_t aet_sample_read(aet_t *e, uint32_t i);   /* the sample's value at i (decoded), for tests and screens */
void aet_live_push(aet_t *e, const float *x, uint32_t n);   /* audio context: LIVE's newest samples, before render */
void aet_note_on(aet_t *e, int32_t note, float vel);    /* audio context */
void aet_note_off(aet_t *e, int32_t note);
void aet_all_off(aet_t *e);              /* release every voice */
void aet_panic(aet_t *e);                /* silence now */
/* n frames of output (any n); the frame work is done here too, a share of it each call */
void aet_render(aet_t *e, float *l, float *r, uint32_t n);
uint32_t aet_size_n(uint32_t size);      /* AET_SIZE_* -> frame length */
int aet_active(const aet_t *e);          /* voices sounding */
/* HARM's pieces, for a screen as well: harmonic h's level before the envelopes (velocity vel 0 .. 1), whether a
 * target takes harmonic h, an envelope's value t seconds into a note */
float aet_harm_level(const aet_params_t *p, uint32_t h, float vel);
int aet_harm_takes(uint32_t target, uint32_t h);
float aet_harm_env(const aet_params_t *p, uint32_t k, float t);
float aet_eq_db(const aet_params_t *p, float hz);   /* the EQ's gain at hz, dB (TILT not in it): for a screen */
