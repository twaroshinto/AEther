/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 fm1-smuga contributors */
/* SMUGA: a spectral "smear" synthesizer in the spirit of PaulStretch (Nasca Octavian Paul's extreme time stretch).
 *
 * Every frame is built in the frequency domain: a magnitude spectrum (from a sample scanned very slowly, or from a
 * PADsynth-like harmonic profile of each held note), then random phases for every bin, an inverse FFT and a sine
 * window, overlapped by half a frame. Random phases are what turns any sound into a smooth, endless cloud.
 *
 * Polyphony is almost free: all voices are summed into ONE magnitude spectrum, so there is one inverse FFT per frame
 * whatever the number of notes. Stereo is free too: the left and right channels are two independent random-phase
 * frames packed into one complex FFT (the real part is the left, the imaginary part the right).
 *
 * The engine never allocates: the caller gives it SMG_RAM_FLOATS floats (on the FM-1, the .pool RAM). Its work is
 * split into small steps spread over the audio blocks of one hop (smg_render does it), so no block carries a whole
 * FFT. Plain C99 and float only: no double, no libc (fastmath.h), so the same file runs on the FM-1 (pi32v2 FPU),
 * in fm1-base's host simulator and in the tests. */
#pragma once
#include <stdint.h>

#define SMG_FS 44100u
#define SMG_NMAX 8192u                   /* the longest frame: 186 ms at 44.1 kHz */
#define SMG_NMIN 1024u
#define SMG_VOICES 8u
#define SMG_DISP 60u                     /* bands of the display spectrum (log-spaced, 40 Hz .. 16 kHz) */

/* the RAM the engine needs, in floats: the complex FFT buffer (2N), the source and output magnitudes (N/2 + 1 each),
 * the play and tail buffers of both channels (N/2 each), the synthesis window (N/2), a quarter sine (N/4 + 1), the
 * LIVE source's ring (N) */
#define SMG_RAM_FLOATS (2u * SMG_NMAX + 2u * (SMG_NMAX / 2u + 1u) + 4u * (SMG_NMAX / 2u) + SMG_NMAX / 2u + \
                        SMG_NMAX / 4u + 1u + SMG_NMAX)

/* PAD: harmonic bands built in the spectrum; SAMPLE: a sample scanned slowly; LIVE: whatever the caller pushes
 * (smg_live_push: a synthesizer running beside the engine), its newest window analysed every frame */
/* HARM: an additive generator drawn straight into the spectrum: each note's harmonics (how many, their slope,
 * odd against even, a boost or cut around one of them, brightness by velocity), each a narrow band as PAD's, and
 * four envelopes that each move the harmonics they are given (one, a group, all), from the note's start */
enum { SMG_SRC_PAD, SMG_SRC_SAMPLE, SMG_SRC_LIVE, SMG_SRC_HARM, SMG_NSRC };
#define SMG_HARM_MAX 64u
#define SMG_HENV 4u
/* an envelope's target: 0 none, 1 .. 32 that harmonic, then the groups */
enum { SMG_HT_OFF = 0, SMG_HT_H32 = 32, SMG_HT_ODD, SMG_HT_EVEN, SMG_HT_LOW, SMG_HT_MID, SMG_HT_HIGH, SMG_HT_ALL,
       SMG_NHT };                        /* LOW 1-4, MID 5-12, HIGH 13 and up */
/* sample formats: 16-bit PCM, or IMA ADPCM (4 bits a sample) in blocks of SMG_ADPCM_BLOCK samples, each block
 * SMG_ADPCM_BYTES bytes: the first sample (int16, little-endian), the step index, a zero, then the other 255
 * samples' codes, two a byte, the low nibble first. Blocks start afresh, so any point is at most 255 codes away. */
enum { SMG_FMT_PCM16, SMG_FMT_ADPCM, SMG_NFMT };
#define SMG_ADPCM_BLOCK 256u
#define SMG_ADPCM_BYTES 132u
/* SBR (spectral band replication, after HE-AAC): a sample kept at a low rate has the band above its Nyquist rebuilt
 * from its own top octave, transposed up by octaves (so harmonics stay harmonics) and shaped to a stored envelope:
 * SMG_SBR_BANDS energies a frame, log-spaced from SMG_SBR_LO(rate) to SMG_SBR_HI, one frame every env_hop source
 * samples, each a byte: 2 (10 log10 E + 120), 0 = silence. E is the band's sum of squared magnitudes in the engine's
 * own scale (Hann window, |X| 4 / N), which does not depend on N. Random phases make SBR's usual flaws (phase,
 * transients) moot. */
#define SMG_SBR_BANDS 8u
#define SMG_SBR_HI 16000.0f
#define SMG_SBR_LO(rate) (0.44f * (float)(rate))   /* 88 % of the sample's Nyquist: above its anti-alias filter */
#define SMG_ADPCM_SIZE(len) (((len) + SMG_ADPCM_BLOCK - 1u) / SMG_ADPCM_BLOCK * SMG_ADPCM_BYTES)
enum { SMG_SIZE_1K, SMG_SIZE_2K, SMG_SIZE_4K, SMG_SIZE_8K, SMG_NSIZE };

typedef struct {
    const uint8_t *data;                 /* mono, SMG_FMT_*; 0 = none */
    uint32_t len;                        /* samples */
    uint32_t rate;                       /* Hz (any: read with interpolation) */
    uint32_t fmt;
    const uint8_t *env;                  /* SBR envelope (smg_set_env): env_frames x SMG_SBR_BANDS bytes; 0 = none */
    uint32_t env_frames, env_hop;
} smg_sample_t;

typedef struct {                         /* the ADPCM reader: the block decoded so far */
    uint32_t blk;                        /* 0xFFFFFFFF: none */
    uint32_t pos;                        /* the last code decoded, in the block */
    int32_t pred, prev, idx;             /* the sample at pos, the one before it, the step index */
} smg_adpcm_t;

/* the sound's parameters: the main loop writes them, the engine takes a copy at the start of each frame */
typedef struct {
    uint8_t source;                      /* SMG_SRC_* */
    uint8_t size;                        /* SMG_SIZE_*: the frame, the smear's length */
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
    /* HARM */
    uint8_t h_count;                     /* harmonics, 1 .. SMG_HARM_MAX */
    uint8_t h_target[SMG_HENV];          /* each envelope's harmonics: SMG_HT_* */
    uint8_t rsv2[3];
    float h_slope;                       /* 0 .. 3: the harmonics fall as 1 / h^slope */
    float h_odd;                         /* -1 .. 1: even only .. both .. odd only */
    float h_band;                        /* cents: each harmonic's width */
    float h_focus;                       /* the harmonic the boost is centred on, 1 .. 32 */
    float h_boost;                       /* dB, -24 .. +24, at the focus */
    float h_width;                       /* the boost's width, in harmonics */
    float h_vel;                         /* 0 .. 1: softer notes darker */
    float h_att[SMG_HENV], h_dec[SMG_HENV], h_sus[SMG_HENV];   /* seconds, seconds, 0 .. 1 */
} smg_params_t;

typedef struct {
    int8_t note;                         /* MIDI note, -1 = free */
    uint8_t gate;                        /* held */
    uint8_t rsv[2];
    float vel;                           /* 0 .. 1 */
    float level;                         /* the envelope, per frame */
    uint32_t age;                        /* frames since its note-on (stealing) */
} smg_voice_t;

typedef struct {
    smg_params_t p;                      /* written by the caller */
    smg_params_t fp;                     /* this frame's copy */
    smg_sample_t smp;                    /* the source sample (smg_set_sample) */
    smg_adpcm_t ad;
    float *ring;                         /* LIVE: the newest SMG_NMAX samples pushed */
    uint32_t ring_w, live_at;            /* the next write; where this frame's window starts */
    float load_scan;                     /* the scan where this frame's window was read (the SBR envelope's time) */
    uint32_t sbr_edge[SMG_SBR_BANDS + 1u];   /* SBR: the bands' first bins (the last: the end) */
    float sbr_p[SMG_SBR_BANDS];          /* the rebuilt bands' energies, then their gains */
    smg_voice_t voice[SMG_VOICES];
    float *work, *src, *acc, *play[2], *tail[2], *win, *qs;
    uint32_t n, hop, n_win;              /* the frame being built, its hop, the window's frame */
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
    float disp_acc[SMG_DISP];
    float disp[SMG_DISP];                /* the last frame's spectrum by band, linear (for a screen) */
} smg_t;

void smg_init(smg_t *e, float *ram);    /* ram: SMG_RAM_FLOATS floats; e->p gets defaults */
void smg_defaults(smg_params_t *p);
void smg_set_sample(smg_t *e, const void *data, uint32_t len, uint32_t rate, uint32_t fmt);   /* audio context */
void smg_set_env(smg_t *e, const uint8_t *env, uint32_t frames, uint32_t hop);   /* after smg_set_sample */
int32_t smg_sample_read(smg_t *e, uint32_t i);   /* the sample's value at i (decoded), for tests and screens */
void smg_live_push(smg_t *e, const float *x, uint32_t n);   /* audio context: LIVE's newest samples, before render */
void smg_note_on(smg_t *e, int32_t note, float vel);    /* audio context */
void smg_note_off(smg_t *e, int32_t note);
void smg_all_off(smg_t *e);              /* release every voice */
void smg_panic(smg_t *e);                /* silence now */
/* n frames of output (any n); the frame work is done here too, a share of it each call */
void smg_render(smg_t *e, float *l, float *r, uint32_t n);
uint32_t smg_size_n(uint32_t size);      /* SMG_SIZE_* -> frame length */
int smg_active(const smg_t *e);          /* voices sounding */
/* HARM's pieces, for a screen as well: harmonic h's level before the envelopes (velocity vel 0 .. 1), whether a
 * target takes harmonic h, an envelope's value t seconds into a note */
float smg_harm_level(const smg_params_t *p, uint32_t h, float vel);
int smg_harm_takes(uint32_t target, uint32_t h);
float smg_harm_env(const smg_params_t *p, uint32_t k, float t);
