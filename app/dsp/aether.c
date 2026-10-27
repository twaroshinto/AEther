/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 fm1-aether contributors
 * AI-generated code (Claude, Anthropic): no warranty, use at your own risk (see NOTICE). */
/* AETHER's engine: see aether.h. One frame, as a job of small steps:
 *
 *   WIN    the synthesis window for a new frame length
 *   LOAD   SAMPLE: N source samples around the scan position, Hann-windowed, into the FFT buffer
 *   AREV   bit-reversal permutation             } the forward FFT
 *   AFFT   radix-2 stages                       }
 *   MAG    the magnitudes, into src
 *   SBR1   a low-rate sample with an envelope: the band above its Nyquist rebuilt from its top octave  } SBR
 *   SBR2   each rebuilt band scaled to the envelope's energy at the scan                             }
 *   SPR1   SPREAD: prefix sums of src           } blur: each bin the mean of a band that widens with frequency
 *   SPR2   the blurred magnitudes               }
 *   CLEAR  the output magnitudes to zero
 *   VOICE  each voice's layers (the note, an octave down, an octave up) added into acc: SAMPLE moves src by the
 *          note's ratio, PAD lays a Gaussian band per harmonic (PADsynth's profile)
 *   PHASE  acc with two sets of random phases, packed as left + i * right into the FFT buffer (Hermitian halves)
 *   SREV   bit-reversal                         } the inverse FFT
 *   SFFT   radix-2 stages                       }
 *   DONE   waits for the play buffer to run out: then swap() overlaps it with the last tail
 *
 * Units: one unit is about one bin, sample or butterfly. aet_render spends a share of a frame's units per block. */
#include "aether.h"
#include "fastmath.h"

enum { S_WIN, S_LOAD, S_AREV, S_AFFT, S_MAG, S_SBR1, S_SBR2, S_SPR1, S_SPR2, S_CLEAR, S_VOICE, S_PHASE, S_SREV, S_SFFT, S_DONE };
#define CHUNK 256u                       /* items per step */
#define QN (AET_NMAX / 4u)               /* the quarter sine's last index */
#define DISP_LO_LOG2 5.321928f           /* log2(40 Hz) */
#define DISP_SPAN_LOG2 8.643856f         /* log2(16000 / 40) */

uint32_t aet_size_n(uint32_t size) { return AET_NMIN << (size < AET_NSIZE ? size : AET_NSIZE - 1u); }

/* ------------------------------------------------------------------ tables --- */
/* sin and cos of 2 pi t / NMAX, t in [0, NMAX), from the quarter wave qs[0 .. NMAX/4] */
static inline float q_sin(const float *qs, uint32_t t)
{
    uint32_t h = t & (AET_NMAX / 2u - 1u);
    float s = h <= QN ? qs[h] : qs[AET_NMAX / 2u - h];
    return t & (AET_NMAX / 2u) ? -s : s;
}
static inline float q_cos(const float *qs, uint32_t t) { return q_sin(qs, (t + QN) & (AET_NMAX - 1u)); }

static inline uint32_t rnd(aet_t *e)    /* xorshift32 */
{
    uint32_t x = e->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return e->rng = x;
}

static inline float win_at(const aet_t *e, uint32_t k)   /* the sine window of the current frame, k in [0, n) */
{
    return e->win[k < e->hop ? k : e->n - 1u - k];
}

/* -------------------------------------------------------------------- FFT --- */
/* in-place complex FFT on e->work (re, im interleaved), size e->n, in steps: the permutation (Gold-Rader's running
 * reversed counter in e->j), then the stages (e->h = half the butterfly span, e->i = butterfly) */
static int fft_rev_step(aet_t *e)       /* 1 = done */
{
    float *x = e->work;
    uint32_t n = e->n, i = e->i, end = i + CHUNK < n ? i + CHUNK : n, j = e->j;
    for (; i < end; i++) {
        uint32_t m;
        if (i < j) {
            float tr = x[2u * i], ti = x[2u * i + 1u];
            x[2u * i] = x[2u * j];
            x[2u * i + 1u] = x[2u * j + 1u];
            x[2u * j] = tr;
            x[2u * j + 1u] = ti;
        }
        m = n >> 1;
        while (m && (j & m)) {
            j ^= m;
            m >>= 1;
        }
        j |= m;
    }
    e->units += end - e->i;
    e->i = end;
    e->j = j;
    return end >= n;
}

static int fft_stage_step(aet_t *e, int inverse)   /* 1 = all stages done */
{
    float *x = e->work;
    const float *qs = e->qs;
    uint32_t half = e->n >> 1, h = e->h, b = e->i, end = b + CHUNK < half ? b + CHUNK : half;
    uint32_t tstep = AET_NMAX / (2u * h);
    for (; b < end; b++) {
        uint32_t jj = b & (h - 1u), i1 = ((b - jj) << 1) + jj, i2 = i1 + h, t = jj * tstep;
        float c = q_cos(qs, t), s = inverse ? q_sin(qs, t) : -q_sin(qs, t);
        float ar = x[2u * i2], ai = x[2u * i2 + 1u];
        float tr = ar * c - ai * s, ti = ar * s + ai * c;
        float ur = x[2u * i1], ui = x[2u * i1 + 1u];
        x[2u * i1] = ur + tr;
        x[2u * i1 + 1u] = ui + ti;
        x[2u * i2] = ur - tr;
        x[2u * i2 + 1u] = ui - ti;
    }
    e->units += end - e->i;
    e->i = end;
    if (end < half)
        return 0;
    e->i = 0;
    e->h <<= 1;
    return e->h >= e->n;
}

/* ----------------------------------------------------------------- frames --- */
static void enter(aet_t *e, uint32_t st)
{
    e->st = (uint8_t)st;
    e->i = 0;
    e->j = 0;
    e->h = 1;
    e->v = 0;
    e->layer = 0;
}

static void voice_cap(aet_t *e)          /* over the cap (the overload guard): the quietest let go, fast */
{
    uint32_t cap = e->fp.voice_cap ? e->fp.voice_cap : AET_VOICES, n = 0, v;
    for (v = 0; v < AET_VOICES; v++)
        n += e->voice[v].note >= 0 && e->voice[v].level > 0.002f;
    while (n > cap) {
        uint32_t q = AET_VOICES;
        float lo = 9.0f;
        for (v = 0; v < AET_VOICES; v++)
            if (e->voice[v].note >= 0 && e->voice[v].level > 0.002f && e->voice[v].level < lo) {
                lo = e->voice[v].level;
                q = v;
            }
        if (q == AET_VOICES)
            break;
        e->voice[q].gate = 0;
        e->voice[q].level *= 0.25f;
        if (e->voice[q].level <= 0.002f)
            e->voice[q].note = -1;
        n--;
    }
}

static void envelopes(aet_t *e)          /* once a frame: attack linear, release exponential */
{
    float dt = (float)e->hop / (float)AET_FS;
    float up = dt / fm_maxf(e->fp.attack, 0.005f);
    float down = fm_expf(-dt / fm_maxf(e->fp.release, 0.01f) * 4.6f);   /* -40 dB at the release time */
    uint32_t v;
    for (v = 0; v < AET_VOICES; v++) {
        aet_voice_t *vc = &e->voice[v];
        if (vc->note < 0)
            continue;
        vc->age++;
        if (vc->gate) {
            vc->level += up * vc->vel;
            if (vc->level > vc->vel)
                vc->level = vc->vel;
        } else {
            vc->level *= down;
            if (vc->level < 0.0005f) {
                vc->level = 0.0f;
                vc->note = -1;
            }
        }
    }
}

/* PAD: harmonic h (e->i + 1) as a Gaussian band of power a_h^2 (PADsynth); one harmonic a step */
#define PAD_HMAX 64u
static float pad_amp(const aet_params_t *p, uint32_t h)
{
    float slope = 2.2f - 1.7f * p->bright; /* 1/h^slope: dull .. bright */
    float a = fm_exp2f(-slope * fm_log2f((float)h));
    if (!(h & 1u))
        a *= 1.0f - p->odd;
    return a;
}
/* HARM */
float aet_harm_level(const aet_params_t *p, uint32_t h, float vel)
{
    float lh = fm_log2f((float)h), slope = p->h_slope + p->h_vel * (1.0f - vel) * 1.5f, a, d, db;
    a = fm_exp2f(-slope * lh);
    if (p->h_odd > 0.0f && !(h & 1u))
        a *= 1.0f - p->h_odd;
    if (p->h_odd < 0.0f && (h & 1u) && h > 1u)    /* (the fundamental stays: the note's pitch) */
        a *= 1.0f + p->h_odd;
    d = ((float)h - p->h_focus) / fm_maxf(p->h_width, 0.3f);
    db = p->h_boost * fm_expf(-d * d);
    return a * fm_exp2f(db * 0.16609640f);       /* 10^(dB / 20) */
}

int aet_harm_takes(uint32_t target, uint32_t h)
{
    if (target >= 1u && target <= AET_HT_H32)
        return h == target;
    switch (target) {
    case AET_HT_ODD: return (h & 1u) != 0;
    case AET_HT_EVEN: return !(h & 1u);
    case AET_HT_LOW: return h <= 4u;
    case AET_HT_MID: return h >= 5u && h <= 12u;
    case AET_HT_HIGH: return h >= 13u;
    case AET_HT_ALL: return 1;
    default: return 0;
    }
}

float aet_harm_env(const aet_params_t *p, uint32_t k, float t)   /* attack to 1, decay to sustain, then stays */
{
    float a = fm_maxf(p->h_att[k], 0.005f), d = fm_maxf(p->h_dec[k], 0.005f), s = fm_clampf(p->h_sus[k], 0.0f, 1.0f);
    if (t < a)
        return t / a;
    if (t < a + d)
        return 1.0f - (1.0f - s) * (t - a) / d;
    return s;
}

/* a harmonic's level in a layer: PAD's slope, or HARM's shape and envelopes (the voice's time since its note) */
static float src_amp(const aet_t *e, uint32_t v, uint32_t h)
{
    const aet_params_t *p = &e->fp;
    const aet_voice_t *vc = &e->voice[v];
    float a, t;
    uint32_t k;
    if (p->source != AET_SRC_HARM)
        return pad_amp(p, h);
    a = aet_harm_level(p, h, vc->vel);
    t = (float)vc->age * (float)e->hop / (float)AET_FS;
    for (k = 0; k < AET_HENV; k++)
        if (aet_harm_takes(p->h_target[k], h))
            a *= aet_harm_env(p, k, t);
    return a;
}

static uint32_t src_hmax(const aet_params_t *p)
{
    return p->source == AET_SRC_HARM ? (p->h_count < 1u ? 1u : p->h_count > AET_HARM_MAX ? AET_HARM_MAX : p->h_count)
                                     : PAD_HMAX;
}

/* the units voice_pad_step takes for one layer at this ratio (the same bands, counted) */
static uint32_t pad_units(const aet_t *e, float ratio)
{
    const aet_params_t *p = &e->fp;
    int harm = p->source == AET_SRC_HARM;
    float binhz = (float)AET_FS / (float)e->n, f0 = 261.6256f * ratio, bb = harm ? 0.0f : p->inharm * p->inharm * 0.0015f;
    float bw = fm_exp2f(fm_clampf(harm ? p->h_band : p->bandwidth, 1.0f, 300.0f) / 1200.0f) - 1.0f;
    uint32_t h, u = PAD_HMAX + 1u, hmax = src_hmax(p);
    for (h = 1; h <= hmax; h++) {
        float fh = f0 * (float)h * fm_sqrtf(1.0f + bb * (float)(h * h));
        if (fh >= 0.45f * (float)AET_FS)
            break;
        u += (uint32_t)(6.0f * fm_clampf(0.5f * bw * fh / binhz, 0.75f, 96.0f)) + 10u;
    }
    return u;
}

static int sbr_on(const aet_t *e)        /* a sample with an envelope whose band ends well below AET_SBR_HI */
{
    return e->fp.source == AET_SRC_SAMPLE && e->smp.env && e->smp.env_frames && e->smp.env_hop &&
           AET_SBR_LO(e->smp.rate) < 0.9f * AET_SBR_HI;
}

static int needs_analysis(const aet_t *e)
{
    return ((e->fp.source == AET_SRC_SAMPLE && e->smp.len) || e->fp.source == AET_SRC_LIVE) &&
           !(e->fp.freeze && e->src_ok);
}

/* the most this frame's job can take, in units, from what it will do: a sudden jump in work (FREEZE let go, more
 * notes, a longer frame) is budgeted for from the frame's first block, before the measured estimate catches up */
static uint32_t bound(const aet_t *e)
{
    uint32_t n = e->n, half = n / 2u, lg = 0, layers = 0, v, b;
    while ((1u << lg) < n)
        lg++;
    b = n + half * lg + half + half + 2u * CHUNK;             /* clear, phase, the inverse FFT */
    if (e->n_win != n)
        b += 4u * half;
    if (needs_analysis(e))
        b += 3u * n + half * lg + half + (e->fp.spread > 0.001f ? n : 0u) + (sbr_on(e) ? 2u * n : 0u);
    for (v = 0; v < AET_VOICES; v++) {
        const aet_voice_t *vc = &e->voice[v];
        float r;
        if (vc->note < 0 || vc->level <= 0.0f)
            continue;
        if (e->fp.source != AET_SRC_PAD && e->fp.source != AET_SRC_HARM) {
            layers += 1u + (e->fp.sub >= 0.001f) + (e->fp.shimmer >= 0.001f);
            continue;
        }
        r = fm_exp2f((float)(vc->note - 60) / 12.0f);
        b += pad_units(e, r);
        if (e->fp.sub >= 0.001f)
            b += pad_units(e, r * 0.5f);
        if (e->fp.shimmer >= 0.001f)
            b += pad_units(e, r * 2.0f);
    }
    b += layers * (half + CHUNK);
    return b + b / 16u;                  /* (a margin: the painters' steps round up) */
}

/* the EQ: one band's gain at d octaves from its frequency (x = hz / its frequency), dB */
static float eq_band_db(uint32_t type, float d, float x, float db, float oct)
{
    float w = fm_maxf(oct, 0.05f), t;
    switch (type) {
    case AET_EQ_BELL:                    /* a bell: half its gain at +-oct/2 */
        t = d / (0.5f * w);
        return db * fm_exp2f(-t * t);
    case AET_EQ_LOSHELF:
        return db * (0.5f - 0.5f * fm_tanhf(2.0f * d / w));
    case AET_EQ_HISHELF:
        return db * (0.5f + 0.5f * fm_tanhf(2.0f * d / w));
    case AET_EQ_LOCUT:                   /* 12 dB/octave (two poles, Butterworth) */
    case AET_EQ_HICUT: {
        float x2 = x * x, den = (1.0f - x2) * (1.0f - x2) + 2.0f * x2;
        float g = type == AET_EQ_LOCUT ? x2 * x2 / den : 1.0f / den;   /* the gain, squared */
        return fm_maxf(3.0103f * fm_log2f(fm_maxf(g, 1e-12f)) * 1.0f, -72.0f);
    }
    default:
        return 0.0f;
    }
}

float aet_eq_db(const aet_params_t *p, float hz)
{
    float db = 0.0f;
    uint32_t b;
    for (b = 0; b < AET_EQ; b++)
        if (p->eq_type[b] != AET_EQ_OFF && p->eq_hz[b] > 0.0f) {
            float x = hz / p->eq_hz[b];
            db += eq_band_db(p->eq_type[b], fm_log2f(x), x, p->eq_db[b], p->eq_oct[b]);
        }
    return db;
}

/* this frame's EQ as a table of log2 gains (the bins interpolate it): only when a band does something */
static void eq_table(aet_t *e)
{
    uint32_t b, k;
    e->eq_on = 0;
    for (b = 0; b < AET_EQ; b++)
        if (e->fp.eq_type[b] == AET_EQ_LOCUT || e->fp.eq_type[b] == AET_EQ_HICUT ||
            (e->fp.eq_type[b] != AET_EQ_OFF && fm_fabsf(e->fp.eq_db[b]) > 0.05f))
            e->eq_on = 1;
    if (!e->eq_on)
        return;
    for (k = 0; k <= AET_EQ_TAB; k++)
        e->eq_tab[k] = aet_eq_db(&e->fp, 20.0f * fm_exp2f((float)k * (10.0f / (float)AET_EQ_TAB))) * 0.166096405f;
}

static void frame_begin(aet_t *e)
{
    uint32_t want;
    float pos;
    e->fp = e->p;                        /* one set of parameters for the whole frame */
    eq_table(e);
    want = aet_size_n(e->fp.size);
    if (want != e->n) {
        e->n = want;
        e->hop = want / 2u;
        e->src_ok = 0;
    }
    envelopes(e);
    voice_cap(e);
    /* the scan: POSITION moved, the scan jumps there; the loop runs from there to the end */
    pos = fm_clampf(e->fp.position, 0.0f, 1.0f);
    if (e->smp.len) {
        e->scan_start = pos * (float)(e->smp.len - 1u);
        if (pos != e->last_position)
            e->scan = e->scan_start;
    }
    e->last_position = pos;
    e->units_frame = 0;
    e->dual = e->par_run && !e->par_failed;
    e->units_bound = bound(e);
    if (e->n_win != e->n)
        enter(e, S_WIN);
    else if (needs_analysis(e))
        enter(e, S_LOAD);
    else
        enter(e, S_CLEAR);
}

static void scan_advance(aet_t *e)       /* once a frame, after its analysis */
{
    float len, adv;
    if (e->fp.source != AET_SRC_SAMPLE || !e->smp.len || e->fp.freeze || e->fp.stretch <= 0.0f)
        return;
    len = (float)e->smp.len;
    adv = (float)e->hop * (float)e->smp.rate / (float)AET_FS / fm_maxf(e->fp.stretch, 1.0f);
    e->scan += adv;
    if (e->scan >= len)
        e->scan = e->scan_start + (e->scan - len);
    if (e->scan >= len)
        e->scan = e->scan_start;
}

/* ------------------------------------------------------------- the sample --- */
static const int8_t AD_INDEX[8] = {-1, -1, -1, -1, 2, 4, 6, 8};
static const int16_t AD_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97,
    107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796,
    876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623,
    27086, 29794, 32767};

/* sample i of an ADPCM sample: decoded forward from where the reader is, or from its block's start. The engine reads
 * forward (with a step back of one for the interpolation), so this is a few operations a sample */
static int32_t adpcm_at(aet_t *e, uint32_t i)
{
    aet_adpcm_t *a = &e->ad;
    uint32_t b = i / AET_ADPCM_BLOCK, j = i % AET_ADPCM_BLOCK;
    const uint8_t *blk = e->smp.data + b * AET_ADPCM_BYTES;
    if (b == a->blk && j == a->pos)
        return a->pred;
    if (b == a->blk && j + 1u == a->pos)
        return a->prev;
    if (b != a->blk || j < a->pos) {
        a->blk = b;
        a->pos = 0;
        a->pred = a->prev = (int16_t)(blk[0] | blk[1] << 8);
        a->idx = blk[2] > 88u ? 88 : blk[2];
    }
    while (a->pos < j) {
        uint32_t c = blk[4u + a->pos / 2u] >> (a->pos & 1u ? 4 : 0) & 15u;
        int32_t st = AD_STEP[a->idx], d = st >> 3;
        if (c & 4u)
            d += st;
        if (c & 2u)
            d += st >> 1;
        if (c & 1u)
            d += st >> 2;
        a->prev = a->pred;
        a->pred += c & 8u ? -d : d;
        a->pred = a->pred < -32768 ? -32768 : a->pred > 32767 ? 32767 : a->pred;
        a->idx += AD_INDEX[c & 7u];
        a->idx = a->idx < 0 ? 0 : a->idx > 88 ? 88 : a->idx;
        a->pos++;
    }
    return a->pred;
}

int32_t aet_sample_read(aet_t *e, uint32_t i)
{
    if (!e->smp.data || i >= e->smp.len)
        return 0;
    if (e->smp.fmt == AET_FMT_ADPCM)
        return adpcm_at(e, i);
    return (int16_t)(e->smp.data[2u * i] | e->smp.data[2u * i + 1u] << 8);
}

/* the source around the scan, resampled to AET_FS, Hann-windowed (the window squared: sin^2); LIVE: the newest n
 * samples of the ring */
static void load_step(aet_t *e)
{
    uint32_t n = e->n, k = e->i, end = k + CHUNK < n ? k + CHUNK : n, len = e->smp.len;
    float rs = (float)e->smp.rate / (float)AET_FS, flen = (float)len;
    float t = e->scan - (float)(n / 2u) * rs + (float)k * rs;
    if (k == 0)
        e->load_scan = e->scan;
    if (e->fp.source == AET_SRC_LIVE) {
        if (k == 0)
            e->live_at = e->ring_w - n;
        for (; k < end; k++) {
            float w = win_at(e, k);
            e->work[2u * k] = e->ring[(e->live_at + k) & (AET_NMAX - 1u)] * w * w;
            e->work[2u * k + 1u] = 0.0f;
        }
        k = end;
    }
    for (; k < end; k++, t += rs) {
        float tt = t, w, s0, s1, fr;
        uint32_t i0, i1;
        while (tt < 0.0f)
            tt += flen;
        while (tt >= flen)
            tt -= flen;
        i0 = (uint32_t)tt;
        if (i0 >= len)
            i0 = len - 1u;
        i1 = i0 + 1u < len ? i0 + 1u : 0u;
        fr = tt - (float)i0;
        s0 = (float)aet_sample_read(e, i0);
        s1 = (float)aet_sample_read(e, i1);
        w = win_at(e, k);
        e->work[2u * k] = (s0 + fr * (s1 - s0)) * (1.0f / 32768.0f) * w * w;
        e->work[2u * k + 1u] = 0.0f;
    }
    e->units += 2u * (end - e->i);
    e->i = end;
    if (end >= n)
        enter(e, S_AREV);
}

void aet_live_push(aet_t *e, const float *x, uint32_t n)
{
    uint32_t k, w = e->ring_w;
    for (k = 0; k < n; k++)
        e->ring[(w + k) & (AET_NMAX - 1u)] = x[k];
    e->ring_w = w + n;
}

static void mag_step(aet_t *e)
{
    uint32_t half = e->n / 2u, k = e->i, end = k + CHUNK < half + 1u ? k + CHUNK : half + 1u;
    float norm = 4.0f / (float)e->n;     /* 2 / sum of the Hann window: a sine's peak bin reads its amplitude */
    for (; k < end; k++) {
        float re = e->work[2u * k], im = e->work[2u * k + 1u];
        e->src[k] = fm_sqrtf(re * re + im * im) * norm;
    }
    e->units += end - e->i;
    e->i = end;
    if (end > half) {
        e->src_ok = 1;
        scan_advance(e);
        enter(e, sbr_on(e) ? S_SBR1 : e->fp.spread > 0.001f ? S_SPR1 : S_CLEAR);
    }
}

/* SBR1: the band edges, then each bin above the sample's band from its top octave (k halved until it falls in
 * [kc / 2, kc)), the bands' energies summed */
static void sbr_fill_step(aet_t *e)
{
    uint32_t half = e->n / 2u, k, end, b;
    float binhz = (float)AET_FS / (float)e->n, kc;
    if (e->i == 0) {
        float lo = AET_SBR_LO(e->smp.rate), hi = AET_SBR_HI;
        for (b = 0; b <= AET_SBR_BANDS; b++) {
            uint32_t edge = (uint32_t)(lo * fm_exp2f(fm_log2f(hi / lo) * (float)b / (float)AET_SBR_BANDS) / binhz);
            e->sbr_edge[b] = edge < half ? edge : half;
        }
        for (b = 0; b < AET_SBR_BANDS; b++)
            e->sbr_p[b] = 0.0f;
        e->i = e->sbr_edge[0];
        e->j = 0;
    }
    kc = (float)e->sbr_edge[0];
    k = e->i;
    end = k + CHUNK < half + 1u ? k + CHUNK : half + 1u;
    for (; k < end; k++) {
        float idx = (float)k, v = 0.0f, fr;
        uint32_t i0;
        if (k < e->sbr_edge[AET_SBR_BANDS] && kc >= 4.0f) {
            while (idx >= kc)
                idx *= 0.5f;
            i0 = (uint32_t)idx;
            fr = idx - (float)i0;
            v = (float)(i0 + 1u) < kc ? e->src[i0] + fr * (e->src[i0 + 1u] - e->src[i0]) : e->src[i0];
            while (e->j + 1u < AET_SBR_BANDS && k >= e->sbr_edge[e->j + 1u])
                e->j++;
            e->sbr_p[e->j] += v * v;
        }
        e->src[k] = v;
    }
    e->units += 2u * (end - e->i);
    e->i = end;
    if (end > half)
        enter(e, S_SBR2);
}

/* SBR2: the gains, from the envelope at the scan (interpolated between its frames, in dB), then applied */
static void sbr_gain_step(aet_t *e)
{
    uint32_t k, end, b, half = e->n / 2u;
    if (e->i == 0) {
        const aet_sample_t *sm = &e->smp;
        float pos = e->load_scan / (float)sm->env_hop, fr;
        uint32_t f0;
        if (pos < 0.0f)
            pos = 0.0f;
        f0 = (uint32_t)pos;
        if (f0 >= sm->env_frames - 1u) {
            f0 = sm->env_frames - 1u;
            fr = 0.0f;
        } else {
            fr = pos - (float)f0;
        }
        for (b = 0; b < AET_SBR_BANDS; b++) {
            float qa = (float)sm->env[f0 * AET_SBR_BANDS + b];
            float qb = (float)sm->env[(f0 + (fr > 0.0f ? 1u : 0u)) * AET_SBR_BANDS + b];
            float q = qa + fr * (qb - qa), want, g = 0.0f;
            if (q > 0.5f && e->sbr_p[b] > 1e-24f) {
                want = fm_exp2f((q * 0.5f - 120.0f) * 0.33219281f);   /* 10^(dB / 10) */
                g = fm_sqrtf(want / e->sbr_p[b]);
                if (g > 100.0f)
                    g = 100.0f;
            }
            e->sbr_p[b] = g;
        }
        e->i = e->sbr_edge[0];
        e->j = 0;
    }
    k = e->i;
    end = k + CHUNK < e->sbr_edge[AET_SBR_BANDS] ? k + CHUNK : e->sbr_edge[AET_SBR_BANDS];
    for (; k < end; k++) {
        while (e->j + 1u < AET_SBR_BANDS && k >= e->sbr_edge[e->j + 1u])
            e->j++;
        e->src[k] *= e->sbr_p[e->j];
    }
    e->units += end > e->i ? end - e->i : 0u;
    e->i = end;
    if (end >= e->sbr_edge[AET_SBR_BANDS] || end >= half)
        enter(e, e->fp.spread > 0.001f ? S_SPR1 : S_CLEAR);
}

/* SPREAD: prefix sums of src into the (now free) FFT buffer, then each bin the mean of [k - w, k + w] */
static void spread_step(aet_t *e)
{
    float *ps = e->work;
    uint32_t half = e->n / 2u, k = e->i, end;
    if (e->st == S_SPR1) {
        end = k + CHUNK < half + 1u ? k + CHUNK : half + 1u;
        if (k == 0)
            ps[0] = 0.0f;
        for (; k < end; k++)
            ps[k + 1u] = ps[k] + e->src[k];
        e->units += end - e->i;
        e->i = end;
        if (end > half)
            enter(e, S_SPR2);
        return;
    }
    end = k + CHUNK < half + 1u ? k + CHUNK : half + 1u;
    {
        float sp = e->fp.spread * e->fp.spread, rel = sp * 0.35f, abs_ = 1.0f + sp * 24.0f * (float)e->n / 8192.0f;
        for (; k < end; k++) {
            uint32_t w = (uint32_t)((float)k * rel + abs_), lo = k > w ? k - w : 0u, hi = k + w < half ? k + w : half;
            e->src[k] = (ps[hi + 1u] - ps[lo]) / (float)(hi - lo + 1u);
        }
    }
    e->units += end - e->i;
    e->i = end;
    if (end > half)
        enter(e, S_CLEAR);
}

/* Painting the voices into the spectrum: a cursor walks voices (vfirst, vfirst + vstep, ...), their layers (0 the note,
 * 1 an octave down, 2 an octave up) and the bins or harmonics of each, adding into its own spectrum (acc, or acc2 for
 * the second core's cursor). Two cursors can run at once on two cores: they share nothing they write. */
static int layer_next(aet_t *e, aet_cur_t *c)
{
    for (; c->v < AET_VOICES; c->v += c->vstep, c->layer = 0) {
        aet_voice_t *vc = &e->voice[c->v];
        if (vc->note < 0 || vc->level <= 0.0f)
            continue;
        for (; c->layer < 3u; c->layer++) {
            float g = c->layer == 0 ? 1.0f : e->fp.no_layers ? 0.0f : c->layer == 1 ? e->fp.sub : e->fp.shimmer;
            if (g < 0.001f)
                continue;
            c->gain = g * vc->level;
            c->ratio = fm_exp2f((float)(vc->note - 60) / 12.0f) * (c->layer == 1 ? 0.5f : c->layer == 2 ? 2.0f : 1.0f);
            c->norm = 0.0f;
            c->i = 0;
            return 1;
        }
    }
    c->done = 1;
    return 0;
}
static void layer_done(aet_t *e, aet_cur_t *c)
{
    c->layer++;
    layer_next(e, c);
}

static void cur_start(aet_t *e, aet_cur_t *c, uint32_t first, uint32_t step, float *acc)
{
    c->v = first;
    c->vstep = step;
    c->layer = 0;
    c->acc = acc;
    c->done = 0;
    c->units = 0;
    layer_next(e, c);
}

/* SAMPLE: the source spectrum moved by the note's ratio (bin k reads src at k / ratio) */
static void voice_sample_step(aet_t *e, aet_cur_t *c)
{
    uint32_t half = e->n / 2u, k = c->i ? c->i : 1u, end = k + CHUNK < half ? k + CHUNK : half;
    float inv = 1.0f / c->ratio, g = c->gain, *acc = c->acc;
    for (; k < end; k++) {
        float idx = (float)k * inv, fr;
        uint32_t i0 = (uint32_t)idx;
        if (i0 >= half) {
            end = half;
            break;
        }
        fr = idx - (float)i0;
        acc[k] += g * (e->src[i0] + fr * (e->src[i0 + 1u] - e->src[i0]));
    }
    c->units += end - (c->i ? c->i : 1u);
    c->i = end;
    if (end >= half)
        layer_done(e, c);
}

/* exp(-x^2 / 2) for x = 0 .. 3.2 in steps of 1/40 (the bands' shape: a table instead of an exp a bin) */
#define GT_STEP 40.0f
static inline float gauss(const float *gt, float x)   /* x >= 0 */
{
    float f = x * GT_STEP;
    uint32_t i = (uint32_t)f;
    if (i >= AET_GT - 1u)
        return 0.0f;
    f -= (float)i;
    return gt[i] + f * (gt[i + 1u] - gt[i]);
}

static void voice_pad_step(aet_t *e, aet_cur_t *c)   /* PAD and HARM: one harmonic a step */
{
    const aet_params_t *p = &e->fp;
    int harm = p->source == AET_SRC_HARM;
    float binhz = (float)AET_FS / (float)e->n, half = (float)(e->n / 2u);
    float f0 = 261.6256f * c->ratio, bb = harm ? 0.0f : p->inharm * p->inharm * 0.0015f;
    float bw = fm_exp2f(fm_clampf(harm ? p->h_band : p->bandwidth, 1.0f, 300.0f) / 1200.0f) - 1.0f;
    uint32_t h = c->i + 1u, hmax = src_hmax(p);
    float fh, a, cc, sig, amp, isig, *acc = c->acc;
    int32_t lo, hi, k;
    if (h == 1u) {                       /* the voice's norm: its harmonics' power summed, to an RMS of 0.25 */
        float pw = 0.0f;
        uint32_t q;
        for (q = 1; q <= hmax && f0 * (float)q < 0.45f * (float)AET_FS; q++) {
            float aq = harm ? aet_harm_level(p, q, e->voice[c->v].vel) : pad_amp(p, q);
            pw += aq * aq;
        }
        c->norm = pw > 0.0f ? 0.22f * fm_rsqrtf(pw) : 0.0f;
        c->units += q;
    }
    fh = f0 * (float)h * fm_sqrtf(1.0f + bb * (float)(h * h));
    if (h > hmax || fh >= 0.45f * (float)AET_FS) {
        layer_done(e, c);
        return;
    }
    c->i++;
    a = src_amp(e, c->v, h) * c->norm * c->gain;
    if (a < 1e-6f) {                     /* -120 dB: not heard, not painted */
        c->units += 4u;
        return;
    }
    cc = fh / binhz;
    sig = fm_clampf(0.5f * bw * fh / binhz, 0.75f, 96.0f);
    amp = a * fm_rsqrtf(sig * 1.7724539f);   /* a / sqrt(sigma sqrt(pi)): the band's power is a^2 */
    isig = 1.0f / sig;
    lo = (int32_t)(cc - 3.0f * sig);
    hi = (int32_t)(cc + 3.0f * sig) + 1;
    if (lo < 1)
        lo = 1;
    if (hi > (int32_t)half - 1)
        hi = (int32_t)half - 1;
    for (k = lo; k <= hi; k++)
        acc[k] += amp * gauss(e->gt, fm_fabsf((float)k - cc) * isig);
    c->units += (uint32_t)(hi >= lo ? hi - lo + 1 : 0) + 8u;
}

/* a cursor's work for up to budget units (the second core runs this for its cursor) */
static void paint(aet_t *e, aet_cur_t *c, uint32_t budget)
{
    uint32_t start = c->units;
    while (!c->done && c->units - start < budget) {
        if (e->fp.source == AET_SRC_PAD || e->fp.source == AET_SRC_HARM)
            voice_pad_step(e, c);
        else if (e->src_ok)
            voice_sample_step(e, c);
        else
            c->done = 1;                 /* no source yet: silence */
    }
}

static void clear_step(aet_t *e)
{
    uint32_t half = e->n / 2u, k = e->i, end = k + CHUNK < half + 1u ? k + CHUNK : half + 1u;
    for (; k < end; k++)
        e->acc[k] = 0.0f;
    if (e->dual)
        for (k = e->i; k < end; k++)
            e->acc2[k] = 0.0f;
    e->units += end - e->i;
    e->i = end;
    if (end > half) {
        uint32_t b;
        for (b = 0; b < AET_DISP; b++)
            e->disp_acc[b] = 0.0f;
        enter(e, S_VOICE);
        cur_start(e, &e->c0, 0, e->dual ? 2u : 1u, e->acc);
        if (e->dual)
            cur_start(e, &e->c1, 1, 2u, e->acc2);
        else
            e->c1.done = 1;
    }
}

/* acc with random phases: A (left) and B (right), packed X[k] = A + iB, X[n-k] = conj(A) + i conj(B) */
static void phase_step(aet_t *e)
{
    float *x = e->work;
    const float *qs = e->qs;
    uint32_t n = e->n, half = n / 2u, k = e->i, end = k + CHUNK < half + 1u ? k + CHUNK : half + 1u;
    float binhz = (float)AET_FS / (float)n, lb = fm_log2f(binhz), tilt = fm_clampf(e->fp.tilt, -1.0f, 1.0f);
    float t500 = lb - 8.965784f;         /* log2(bin Hz / 500) */
    float lp_x = e->fp.lp_hz > 0.0f ? binhz / e->fp.lp_hz : 0.0f;   /* f / fc a bin */
    float lp_iq2 = 1.0f / ((0.55f + 7.5f * e->fp.lp_reso * e->fp.lp_reso) * (0.55f + 7.5f * e->fp.lp_reso * e->fp.lp_reso));
    for (; k < end; k++) {
        float m, lk, ar, ai, br, bi;
        uint32_t t1, t2, b;
        int32_t bi_;
        if (k == 0 || k == half) {
            x[2u * k] = x[2u * k + 1u] = 0.0f;
            continue;
        }
        m = e->acc[k] + (e->dual ? e->acc2[k] : 0.0f);
        if (m <= 0.0f) {
            x[2u * k] = x[2u * k + 1u] = x[2u * (n - k)] = x[2u * (n - k) + 1u] = 0.0f;
            continue;
        }
        lk = fm_log2f((float)k);
        if (tilt != 0.0f || e->eq_on) {  /* TILT and the EQ: one gain, in log2 */
            float g = tilt * (lk + t500);
            if (e->eq_on) {
                float t = (lk + lb - 4.321928f) * ((float)AET_EQ_TAB / 10.0f);   /* (log2 20 Hz) */
                int32_t i = (int32_t)t;
                if (i < 0)
                    g += e->eq_tab[0];
                else if (i >= (int32_t)AET_EQ_TAB)
                    g += e->eq_tab[AET_EQ_TAB];
                else
                    g += e->eq_tab[i] + (t - (float)i) * (e->eq_tab[i + 1] - e->eq_tab[i]);
            }
            m *= fm_exp2f(g);
        }
        if (lp_x > 0.0f) {               /* two 2-pole sections: one resonant (Q 0.55 .. 8), one Butterworth */
            float x = lp_x * (float)k, x2 = x * x, d = (1.0f - x2) * (1.0f - x2);
            m *= fm_rsqrtf(d + x2 * lp_iq2) * fm_rsqrtf(d + 2.0f * x2);
        }
        bi_ = (int32_t)((lk + lb - DISP_LO_LOG2) * ((float)AET_DISP / DISP_SPAN_LOG2));
        if (bi_ >= 0 && bi_ < (int32_t)AET_DISP) {
            b = (uint32_t)bi_;
            if (m > e->disp_acc[b])
                e->disp_acc[b] = m;
        }
        m *= 0.5f;                       /* the two Hermitian halves add up to m cos(...) */
        t1 = rnd(e) & (AET_NMAX - 1u);
        t2 = rnd(e) & (AET_NMAX - 1u);
        ar = m * q_cos(qs, t1);
        ai = m * q_sin(qs, t1);
        br = m * q_cos(qs, t2);
        bi = m * q_sin(qs, t2);
        x[2u * k] = ar - bi;
        x[2u * k + 1u] = ai + br;
        x[2u * (n - k)] = ar + bi;
        x[2u * (n - k) + 1u] = br - ai;
    }
    e->units += end - e->i;
    e->i = end;
    if (end > half)
        enter(e, S_SREV);
}

static void win_step(aet_t *e)           /* sin(pi (k + 0.5) / n), the first half (the window is symmetric) */
{
    uint32_t k = e->i, end = k + CHUNK < e->hop ? k + CHUNK : e->hop;
    for (; k < end; k++)
        e->win[k] = fm_sinf(FM_PI * ((float)k + 0.5f) / (float)e->n);
    e->units += 4u * (end - e->i);
    e->i = end;
    if (end >= e->hop) {
        e->n_win = e->n;
        if (needs_analysis(e))
            enter(e, S_LOAD);
        else
            enter(e, S_CLEAR);
    }
}

/* one step of the job; the units it took are added to e->units */
static void step(aet_t *e)
{
    switch (e->st) {
    case S_WIN: win_step(e); break;
    case S_LOAD: load_step(e); break;
    case S_AREV:
        if (fft_rev_step(e))
            enter(e, S_AFFT);
        break;
    case S_AFFT:
        if (fft_stage_step(e, 0))
            enter(e, S_MAG);
        break;
    case S_MAG: mag_step(e); break;
    case S_SBR1: sbr_fill_step(e); break;
    case S_SBR2: sbr_gain_step(e); break;
    case S_SPR1:
    case S_SPR2: spread_step(e); break;
    case S_CLEAR: clear_step(e); break;
    case S_VOICE: {                      /* core 0's cursor; the second's too when there is no second core */
        aet_cur_t *c = !e->c0.done ? &e->c0 : &e->c1;
        uint32_t u = c->units;
        if (!c->done)
            paint(e, c, CHUNK);
        e->units += c->units - u + 1u;
        if (e->c0.done && e->c1.done)
            enter(e, S_PHASE);
        break;
    }
    case S_PHASE: phase_step(e); break;
    case S_SREV:
        if (fft_rev_step(e))
            enter(e, S_SFFT);
        break;
    case S_SFFT:
        if (fft_stage_step(e, 1))
            enter(e, S_DONE);
        break;
    default: break;
    }
}

static void par_job(void *arg)            /* on the second core: its cursor, for the budget it was given */
{
    aet_t *e = (aet_t *)arg;
    paint(e, &e->c1, e->par_budget);
}

static void work(aet_t *e, uint32_t budget)   /* budget: units; 0xFFFFFFFF = to the end of the frame */
{
    uint32_t spent = 0;
    while (e->st != S_DONE && spent < budget) {
        e->units = 0;
        if (e->st == S_VOICE && e->dual && !e->c1.done && !e->c0.done && e->par_run && !e->par_failed) {
            /* both cores paint at once: the second its voices into acc2, this one its own into acc */
            uint32_t u = e->c0.units;
            e->par_budget = budget - spent;
            if (e->par_run(par_job, e)) {
                paint(e, &e->c0, e->par_budget);
                if (e->par_wait()) {     /* it did not answer: one core from now on (this frame finishes here) */
                    e->par_failed = 1;
                }
                e->par_jobs++;
            } else {
                paint(e, &e->c0, e->par_budget);
            }
            e->units = e->c0.units - u + 1u;
            if (e->c0.done && e->c1.done)
                enter(e, S_PHASE);
        } else {
            step(e);
        }
        spent += e->units;
        e->units_frame += e->units;
    }
}

/* the play buffer ran out: the finished frame (windowed, mixed to stereo) over the last tail */
static void swap(aet_t *e)
{
    uint32_t hop = e->hop, k;
    float th = fm_clampf(e->fp.width, 0.0f, 1.0f) * (FM_PI * 0.25f), c = fm_cosf(th), s = fm_sinf(th);
    float g = e->fp.level;
    if (e->st != S_DONE) {               /* not ready in time: finish it now (a spike, counted) */
        e->late++;
        work(e, 0xFFFFFFFFu);
    }
    for (k = 0; k < hop; k++) {
        float w0 = e->win[k], w1 = e->win[hop - 1u - k];
        float a0 = e->work[2u * k] * w0 * g, b0 = e->work[2u * k + 1u] * w0 * g;
        float a1 = e->work[2u * (k + hop)] * w1 * g, b1 = e->work[2u * (k + hop) + 1u] * w1 * g;
        e->play[0][k] = e->tail[0][k] + c * a0 + s * b0;
        e->play[1][k] = e->tail[1][k] + c * a0 - s * b0;
        e->tail[0][k] = c * a1 + s * b1;
        e->tail[1][k] = c * a1 - s * b1;
    }
    for (k = 0; k < AET_DISP; k++)       /* the display: up fast, down slowly */
        e->disp[k] = e->disp_acc[k] > e->disp[k] ? e->disp_acc[k] : e->disp[k] * 0.8f + e->disp_acc[k] * 0.2f;
    e->play_len = hop;
    e->play_pos = 0;
    e->frames++;
    /* the estimate of a frame's work: follows a bigger frame at once, a smaller one slowly */
    if (e->units_frame > e->units_est)
        e->units_est = e->units_frame;
    else
        e->units_est -= (e->units_est - e->units_frame) / 8u;
    if (e->units_frame > e->units_peak)
        e->units_peak = e->units_frame;
    frame_begin(e);
    for (k = hop; k < e->hop; k++)       /* a longer frame next: the tail ends where this frame's did */
        e->tail[0][k] = e->tail[1][k] = 0.0f;
}

static inline float ceiling(float x)    /* a soft ceiling: linear up to 0.5, then towards 1 */
{
    float a = fm_fabsf(x), y;
    if (a < 0.5f)
        return x;
    y = 0.5f + 0.5f * fm_tanhf(2.0f * (a - 0.5f));
    return x < 0.0f ? -y : y;
}

void aet_render(aet_t *e, float *l, float *r, uint32_t n)
{
    uint32_t i = 0;
    /* this block's share of the frame's work, with a margin, so the frame is done before the play buffer is */
    uint32_t est = e->units_est > e->units_bound ? e->units_est : e->units_bound;
    /* (the frame being made must be done when the play buffer now playing runs out: its length is the time) */
    uint32_t budget = (uint32_t)((float)est * (float)n * 1.5f / (float)e->play_len) + CHUNK;
    work(e, budget);
    while (i < n) {
        uint32_t take = e->play_len - e->play_pos, k;
        if (take > n - i)
            take = n - i;
        for (k = 0; k < take; k++) {
            float a = e->play[0][e->play_pos + k], b = e->play[1][e->play_pos + k];
            l[i + k] = ceiling(a);
            r[i + k] = ceiling(b);
        }
        i += take;
        e->play_pos += take;
        if (e->play_pos >= e->play_len)
            swap(e);
    }
}

/* ------------------------------------------------------------------ voices --- */
void aet_note_on(aet_t *e, int32_t note, float vel)
{
    uint32_t v, best = 0;
    float best_level = 2.0f;
    if (note < 0 || note > 127)
        return;
    for (v = 0; v < AET_VOICES; v++)     /* the same note again: that voice */
        if (e->voice[v].note == note) {
            best = v;
            goto found;
        }
    for (v = 0; v < AET_VOICES; v++)     /* a free voice */
        if (e->voice[v].note < 0) {
            best = v;
            e->voice[v].level = 0.0f;
            goto found;
        }
    for (v = 0; v < AET_VOICES; v++) {   /* else the quietest released one, or the quietest */
        float lv = e->voice[v].level + (e->voice[v].gate ? 1.0f : 0.0f);
        if (lv < best_level) {
            best_level = lv;
            best = v;
        }
    }
    e->voice[best].level = 0.0f;
found:
    e->voice[best].note = (int8_t)note;
    e->voice[best].gate = 1;
    e->voice[best].vel = fm_clampf(vel, 0.0f, 1.0f);
    e->voice[best].age = 0;
}

void aet_note_off(aet_t *e, int32_t note)
{
    uint32_t v;
    for (v = 0; v < AET_VOICES; v++)
        if (e->voice[v].note == note)
            e->voice[v].gate = 0;
}

void aet_all_off(aet_t *e)
{
    uint32_t v;
    for (v = 0; v < AET_VOICES; v++)
        e->voice[v].gate = 0;
}

void aet_panic(aet_t *e)
{
    uint32_t v, k;
    for (v = 0; v < AET_VOICES; v++) {
        e->voice[v].note = -1;
        e->voice[v].gate = 0;
        e->voice[v].level = 0.0f;
    }
    for (k = 0; k < AET_NMAX / 2u; k++)
        e->play[0][k] = e->play[1][k] = e->tail[0][k] = e->tail[1][k] = 0.0f;
}

int aet_active(const aet_t *e)
{
    int c = 0;
    uint32_t v;
    for (v = 0; v < AET_VOICES; v++)
        c += e->voice[v].note >= 0;
    return c;
}

void aet_set_sample(aet_t *e, const void *data, uint32_t len, uint32_t rate, uint32_t fmt)
{
    e->smp.data = len ? (const uint8_t *)data : 0;
    e->smp.len = data ? len : 0u;
    e->smp.rate = rate ? rate : AET_FS;
    e->smp.fmt = fmt < AET_NFMT ? fmt : AET_FMT_PCM16;
    e->smp.env = 0;                      /* (aet_set_env gives the new one its own) */
    e->smp.env_frames = 0;
    e->ad.blk = 0xFFFFFFFFu;
    e->src_ok = 0;
    e->scan = e->smp.len ? fm_clampf(e->p.position, 0.0f, 1.0f) * (float)(e->smp.len - 1u) : 0.0f;
    e->last_position = -1.0f;
    e->units_bound = bound(e);           /* the frame under way may have to analyse now */
}

void aet_set_env(aet_t *e, const uint8_t *env, uint32_t frames, uint32_t hop)
{
    e->smp.env = frames && hop ? env : 0;
    e->smp.env_frames = env ? frames : 0u;
    e->smp.env_hop = hop;
    e->src_ok = 0;
    e->units_bound = bound(e);
}

void aet_defaults(aet_params_t *p)
{
    p->source = AET_SRC_SAMPLE;
    p->size = AET_SIZE_8K;
    p->freeze = 0;
    p->rsv = 0;
    p->stretch = 40.0f;
    p->position = 0.0f;
    p->spread = 0.0f;
    p->bright = 0.5f;
    p->bandwidth = 40.0f;
    p->inharm = 0.0f;
    p->odd = 0.0f;
    p->tilt = 0.0f;
    p->sub = 0.0f;
    p->shimmer = 0.0f;
    p->attack = 1.0f;
    p->release = 3.0f;
    p->width = 0.8f;
    p->level = 1.0f;
    p->lp_hz = 0.0f;
    p->lp_reso = 0.0f;
    p->voice_cap = 0;
    p->no_layers = 0;
    p->h_count = 24;
    p->h_slope = 1.0f;
    p->h_odd = 0.0f;
    p->h_band = 12.0f;
    p->h_focus = 4.0f;
    p->h_boost = 0.0f;
    p->h_width = 1.5f;
    p->h_vel = 0.5f;
    for (int k = 0; k < (int)AET_HENV; k++) {
        p->h_target[k] = AET_HT_OFF;
        p->h_att[k] = 0.5f;
        p->h_dec[k] = 2.0f;
        p->h_sus[k] = 0.5f;
    }
    for (int k = 0; k < (int)AET_EQ; k++) {   /* the EQ: flat */
        p->eq_type[k] = AET_EQ_OFF;
        p->eq_hz[k] = 1000.0f;
        p->eq_db[k] = 0.0f;
        p->eq_oct[k] = 1.0f;
    }
}

void aet_init(aet_t *e, float *ram)
{
    uint32_t k;
    float *m = ram;
    aet_params_t keep;
    aet_defaults(&keep);
    for (k = 0; k < AET_RAM_FLOATS; k++)
        ram[k] = 0.0f;
    {   /* the struct to zero, the parameters to their defaults */
        uint8_t *b = (uint8_t *)e;
        for (k = 0; k < sizeof *e; k++)
            b[k] = 0;
    }
    e->p = keep;
    e->work = m; m += 2u * AET_NMAX;
    e->src = m; m += AET_NMAX / 2u + 1u;
    e->acc = m; m += AET_NMAX / 2u + 1u;
    e->acc2 = m; m += AET_NMAX / 2u + 1u;
    e->play[0] = m; m += AET_NMAX / 2u;
    e->play[1] = m; m += AET_NMAX / 2u;
    e->tail[0] = m; m += AET_NMAX / 2u;
    e->tail[1] = m; m += AET_NMAX / 2u;
    e->win = m; m += AET_NMAX / 2u;
    e->qs = m; m += AET_NMAX / 4u + 1u;
    e->ring = m;
    e->ad.blk = 0xFFFFFFFFu;
    for (k = 0; k < AET_GT; k++) {
        float x = (float)k / GT_STEP;
        e->gt[k] = fm_expf(-0.5f * x * x);
    }
    e->gt[AET_GT - 1u] = 0.0f;
    for (k = 0; k <= QN; k++)
        e->qs[k] = fm_sinf(FM_TWO_PI * (float)k / (float)AET_NMAX);
    e->qs[QN] = 1.0f;
    for (k = 0; k < AET_VOICES; k++)
        e->voice[k].note = -1;
    e->rng = 0x9E3779B9u;
    e->units_est = 0;                    /* measured from the first frame; until then, the bound */
    e->last_position = -1.0f;
    e->n = aet_size_n(e->p.size);
    e->hop = e->n / 2u;
    e->play_len = e->hop;                /* the first hop is silence while the first frame is made */
    e->fp = e->p;
    frame_begin(e);
}

void aet_set_par(aet_t *e, int (*run)(void (*fn)(void *), void *arg), int (*wait)(void))
{
    e->par_run = run && wait ? run : 0;
    e->par_wait = run && wait ? wait : 0;
    e->par_failed = 0;
}
