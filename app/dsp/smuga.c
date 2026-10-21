/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 fm1-smuga contributors */
/* SMUGA's engine: see smuga.h. One frame, as a job of small steps:
 *
 *   WIN    the synthesis window for a new frame length
 *   LOAD   SAMPLE: N source samples around the scan position, Hann-windowed, into the FFT buffer
 *   AREV   bit-reversal permutation             } the forward FFT
 *   AFFT   radix-2 stages                       }
 *   MAG    the magnitudes, into src
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
 * Units: one unit is about one bin, sample or butterfly. smg_render spends a share of a frame's units per block. */
#include "smuga.h"
#include "fastmath.h"

enum { S_WIN, S_LOAD, S_AREV, S_AFFT, S_MAG, S_SPR1, S_SPR2, S_CLEAR, S_VOICE, S_PHASE, S_SREV, S_SFFT, S_DONE };
#define CHUNK 256u                       /* items per step */
#define QN (SMG_NMAX / 4u)               /* the quarter sine's last index */
#define DISP_LO_LOG2 5.321928f           /* log2(40 Hz) */
#define DISP_SPAN_LOG2 8.643856f         /* log2(16000 / 40) */

uint32_t smg_size_n(uint32_t size) { return SMG_NMIN << (size < SMG_NSIZE ? size : SMG_NSIZE - 1u); }

/* ------------------------------------------------------------------ tables --- */
/* sin and cos of 2 pi t / NMAX, t in [0, NMAX), from the quarter wave qs[0 .. NMAX/4] */
static inline float q_sin(const float *qs, uint32_t t)
{
    uint32_t h = t & (SMG_NMAX / 2u - 1u);
    float s = h <= QN ? qs[h] : qs[SMG_NMAX / 2u - h];
    return t & (SMG_NMAX / 2u) ? -s : s;
}
static inline float q_cos(const float *qs, uint32_t t) { return q_sin(qs, (t + QN) & (SMG_NMAX - 1u)); }

static inline uint32_t rnd(smg_t *e)    /* xorshift32 */
{
    uint32_t x = e->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return e->rng = x;
}

static inline float win_at(const smg_t *e, uint32_t k)   /* the sine window of the current frame, k in [0, n) */
{
    return e->win[k < e->hop ? k : e->n - 1u - k];
}

/* -------------------------------------------------------------------- FFT --- */
/* in-place complex FFT on e->work (re, im interleaved), size e->n, in steps: the permutation (Gold-Rader's running
 * reversed counter in e->j), then the stages (e->h = half the butterfly span, e->i = butterfly) */
static int fft_rev_step(smg_t *e)       /* 1 = done */
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

static int fft_stage_step(smg_t *e, int inverse)   /* 1 = all stages done */
{
    float *x = e->work;
    const float *qs = e->qs;
    uint32_t half = e->n >> 1, h = e->h, b = e->i, end = b + CHUNK < half ? b + CHUNK : half;
    uint32_t tstep = SMG_NMAX / (2u * h);
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
static void enter(smg_t *e, uint32_t st)
{
    e->st = (uint8_t)st;
    e->i = 0;
    e->j = 0;
    e->h = 1;
    e->v = 0;
    e->layer = 0;
}

static void envelopes(smg_t *e)          /* once a frame: attack linear, release exponential */
{
    float dt = (float)e->hop / (float)SMG_FS;
    float up = dt / fm_maxf(e->fp.attack, 0.005f);
    float down = fm_expf(-dt / fm_maxf(e->fp.release, 0.01f) * 4.6f);   /* -40 dB at the release time */
    uint32_t v;
    for (v = 0; v < SMG_VOICES; v++) {
        smg_voice_t *vc = &e->voice[v];
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
static float pad_amp(const smg_params_t *p, uint32_t h)
{
    float slope = 2.2f - 1.7f * p->bright; /* 1/h^slope: dull .. bright */
    float a = fm_exp2f(-slope * fm_log2f((float)h));
    if (!(h & 1u))
        a *= 1.0f - p->odd;
    return a;
}
/* the units voice_pad_step takes for one layer at this ratio (the same bands, counted) */
static uint32_t pad_units(const smg_t *e, float ratio)
{
    const smg_params_t *p = &e->fp;
    float binhz = (float)SMG_FS / (float)e->n, f0 = 261.6256f * ratio, bb = p->inharm * p->inharm * 0.0015f;
    float bw = fm_exp2f(fm_clampf(p->bandwidth, 1.0f, 300.0f) / 1200.0f) - 1.0f;
    uint32_t h, u = PAD_HMAX + 1u;
    for (h = 1; h <= PAD_HMAX; h++) {
        float fh = f0 * (float)h * fm_sqrtf(1.0f + bb * (float)(h * h));
        if (fh >= 0.45f * (float)SMG_FS)
            break;
        u += (uint32_t)(6.0f * fm_clampf(0.5f * bw * fh / binhz, 0.75f, 96.0f)) + 10u;
    }
    return u;
}

static int needs_analysis(const smg_t *e)
{
    return e->fp.source == SMG_SRC_SAMPLE && e->smp.len && !(e->fp.freeze && e->src_ok);
}

/* the most this frame's job can take, in units, from what it will do: a sudden jump in work (FREEZE let go, more
 * notes, a longer frame) is budgeted for from the frame's first block, before the measured estimate catches up */
static uint32_t bound(const smg_t *e)
{
    uint32_t n = e->n, half = n / 2u, lg = 0, layers = 0, v, b;
    while ((1u << lg) < n)
        lg++;
    b = n + half * lg + half + half + 2u * CHUNK;             /* clear, phase, the inverse FFT */
    if (e->n_win != n)
        b += 4u * half;
    if (needs_analysis(e))
        b += 2u * n + half * lg + half + (e->fp.spread > 0.001f ? n : 0u);
    for (v = 0; v < SMG_VOICES; v++) {
        const smg_voice_t *vc = &e->voice[v];
        float r;
        if (vc->note < 0 || vc->level <= 0.0f)
            continue;
        if (e->fp.source == SMG_SRC_SAMPLE) {
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
    return b + layers * (half + CHUNK);
}

static void frame_begin(smg_t *e)
{
    uint32_t want;
    float pos;
    e->fp = e->p;                        /* one set of parameters for the whole frame */
    want = smg_size_n(e->fp.size);
    if (want != e->n) {
        e->n = want;
        e->hop = want / 2u;
        e->src_ok = 0;
    }
    envelopes(e);
    /* the scan: POSITION moved, the scan jumps there; the loop runs from there to the end */
    pos = fm_clampf(e->fp.position, 0.0f, 1.0f);
    if (e->smp.len) {
        e->scan_start = pos * (float)(e->smp.len - 1u);
        if (pos != e->last_position)
            e->scan = e->scan_start;
    }
    e->last_position = pos;
    e->units_frame = 0;
    e->units_bound = bound(e);
    if (e->n_win != e->n)
        enter(e, S_WIN);
    else if (needs_analysis(e))
        enter(e, S_LOAD);
    else
        enter(e, S_CLEAR);
}

static void scan_advance(smg_t *e)       /* once a frame, after its analysis */
{
    float len, adv;
    if (!e->smp.len || e->fp.freeze || e->fp.stretch <= 0.0f)
        return;
    len = (float)e->smp.len;
    adv = (float)e->hop * (float)e->smp.rate / (float)SMG_FS / fm_maxf(e->fp.stretch, 1.0f);
    e->scan += adv;
    if (e->scan >= len)
        e->scan = e->scan_start + (e->scan - len);
    if (e->scan >= len)
        e->scan = e->scan_start;
}

/* the source around the scan, resampled to SMG_FS, Hann-windowed (the window squared: sin^2) */
static void load_step(smg_t *e)
{
    const int16_t *pcm = e->smp.pcm;
    uint32_t n = e->n, k = e->i, end = k + CHUNK < n ? k + CHUNK : n, len = e->smp.len;
    float rs = (float)e->smp.rate / (float)SMG_FS, flen = (float)len;
    float t = e->scan - (float)(n / 2u) * rs + (float)k * rs;
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
        s0 = (float)pcm[i0];
        s1 = (float)pcm[i1];
        w = win_at(e, k);
        e->work[2u * k] = (s0 + fr * (s1 - s0)) * (1.0f / 32768.0f) * w * w;
        e->work[2u * k + 1u] = 0.0f;
    }
    e->units += end - e->i;
    e->i = end;
    if (end >= n)
        enter(e, S_AREV);
}

static void mag_step(smg_t *e)
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
        enter(e, e->fp.spread > 0.001f ? S_SPR1 : S_CLEAR);
    }
}

/* SPREAD: prefix sums of src into the (now free) FFT buffer, then each bin the mean of [k - w, k + w] */
static void spread_step(smg_t *e)
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

/* the next voice layer to add: e->v the voice, e->layer 0 the note, 1 an octave down, 2 an octave up; 0 = none */
static int layer_next(smg_t *e)
{
    for (; e->v < SMG_VOICES; e->v++, e->layer = 0) {
        smg_voice_t *vc = &e->voice[e->v];
        if (vc->note < 0 || vc->level <= 0.0f)
            continue;
        for (; e->layer < 3u; e->layer++) {
            float g = e->layer == 0 ? 1.0f : e->layer == 1 ? e->fp.sub : e->fp.shimmer;
            if (g < 0.001f)
                continue;
            e->lay_gain = g * vc->level;
            e->lay_ratio = fm_exp2f((float)(vc->note - 60) / 12.0f) * (e->layer == 1 ? 0.5f : e->layer == 2 ? 2.0f : 1.0f);
            e->lay_norm = 0.0f;
            e->i = 0;
            return 1;
        }
    }
    return 0;
}
static void layer_done(smg_t *e)
{
    e->layer++;
    if (!layer_next(e))
        enter(e, S_PHASE);
}

/* SAMPLE: the source spectrum moved by the note's ratio (bin k reads src at k / ratio) */
static void voice_sample_step(smg_t *e)
{
    uint32_t half = e->n / 2u, k = e->i ? e->i : 1u, end = k + CHUNK < half ? k + CHUNK : half;
    float inv = 1.0f / e->lay_ratio, g = e->lay_gain;
    for (; k < end; k++) {
        float idx = (float)k * inv, fr;
        uint32_t i0 = (uint32_t)idx;
        if (i0 >= half) {
            end = half;
            break;
        }
        fr = idx - (float)i0;
        e->acc[k] += g * (e->src[i0] + fr * (e->src[i0 + 1u] - e->src[i0]));
    }
    e->units += end - (e->i ? e->i : 1u);
    e->i = end;
    if (end >= half)
        layer_done(e);
}

static void voice_pad_step(smg_t *e)
{
    const smg_params_t *p = &e->fp;
    float binhz = (float)SMG_FS / (float)e->n, half = (float)(e->n / 2u);
    float f0 = 261.6256f * e->lay_ratio, bb = p->inharm * p->inharm * 0.0015f;
    float bw = fm_exp2f(fm_clampf(p->bandwidth, 1.0f, 300.0f) / 1200.0f) - 1.0f;
    uint32_t h = e->i + 1u;
    float fh, a, c, sig, amp, inv2s2;
    int32_t lo, hi, k;
    if (h == 1u) {                       /* the voice's norm: its harmonics' power summed, to an RMS of 0.25 */
        float pw = 0.0f;
        uint32_t q;
        for (q = 1; q <= PAD_HMAX && f0 * (float)q < 0.45f * (float)SMG_FS; q++) {
            float aq = pad_amp(p, q);
            pw += aq * aq;
        }
        e->lay_norm = pw > 0.0f ? 0.22f * fm_rsqrtf(pw) : 0.0f;
        e->units += q;
    }
    fh = f0 * (float)h * fm_sqrtf(1.0f + bb * (float)(h * h));
    if (h > PAD_HMAX || fh >= 0.45f * (float)SMG_FS) {
        layer_done(e);
        return;
    }
    a = pad_amp(p, h) * e->lay_norm * e->lay_gain;
    c = fh / binhz;
    sig = fm_clampf(0.5f * bw * fh / binhz, 0.75f, 96.0f);
    amp = a * fm_rsqrtf(sig * 1.7724539f);   /* a / sqrt(sigma sqrt(pi)): the band's power is a^2 */
    inv2s2 = 0.5f / (sig * sig);
    lo = (int32_t)(c - 3.0f * sig);
    hi = (int32_t)(c + 3.0f * sig) + 1;
    if (lo < 1)
        lo = 1;
    if (hi > (int32_t)half - 1)
        hi = (int32_t)half - 1;
    for (k = lo; k <= hi; k++) {
        float d = (float)k - c;
        e->acc[k] += amp * fm_expf(-d * d * inv2s2);
    }
    e->units += (uint32_t)(hi >= lo ? hi - lo + 1 : 0) + 8u;
    e->i++;
}

static void clear_step(smg_t *e)
{
    uint32_t half = e->n / 2u, k = e->i, end = k + CHUNK < half + 1u ? k + CHUNK : half + 1u;
    for (; k < end; k++)
        e->acc[k] = 0.0f;
    e->units += end - e->i;
    e->i = end;
    if (end > half) {
        uint32_t b;
        for (b = 0; b < SMG_DISP; b++)
            e->disp_acc[b] = 0.0f;
        enter(e, S_VOICE);
        if (!layer_next(e))
            enter(e, S_PHASE);
    }
}

/* acc with random phases: A (left) and B (right), packed X[k] = A + iB, X[n-k] = conj(A) + i conj(B) */
static void phase_step(smg_t *e)
{
    float *x = e->work;
    const float *qs = e->qs;
    uint32_t n = e->n, half = n / 2u, k = e->i, end = k + CHUNK < half + 1u ? k + CHUNK : half + 1u;
    float binhz = (float)SMG_FS / (float)n, lb = fm_log2f(binhz), tilt = fm_clampf(e->fp.tilt, -1.0f, 1.0f);
    float t500 = lb - 8.965784f;         /* log2(bin Hz / 500) */
    for (; k < end; k++) {
        float m, lk, ar, ai, br, bi;
        uint32_t t1, t2, b;
        int32_t bi_;
        if (k == 0 || k == half) {
            x[2u * k] = x[2u * k + 1u] = 0.0f;
            continue;
        }
        m = e->acc[k];
        if (m <= 0.0f) {
            x[2u * k] = x[2u * k + 1u] = x[2u * (n - k)] = x[2u * (n - k) + 1u] = 0.0f;
            continue;
        }
        lk = fm_log2f((float)k);
        if (tilt != 0.0f)
            m *= fm_exp2f(tilt * (lk + t500));
        bi_ = (int32_t)((lk + lb - DISP_LO_LOG2) * ((float)SMG_DISP / DISP_SPAN_LOG2));
        if (bi_ >= 0 && bi_ < (int32_t)SMG_DISP) {
            b = (uint32_t)bi_;
            if (m > e->disp_acc[b])
                e->disp_acc[b] = m;
        }
        m *= 0.5f;                       /* the two Hermitian halves add up to m cos(...) */
        t1 = rnd(e) & (SMG_NMAX - 1u);
        t2 = rnd(e) & (SMG_NMAX - 1u);
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

static void win_step(smg_t *e)           /* sin(pi (k + 0.5) / n), the first half (the window is symmetric) */
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
static void step(smg_t *e)
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
    case S_SPR1:
    case S_SPR2: spread_step(e); break;
    case S_CLEAR: clear_step(e); break;
    case S_VOICE:
        if (e->fp.source == SMG_SRC_SAMPLE) {
            if (e->smp.len && e->src_ok)
                voice_sample_step(e);
            else
                enter(e, S_PHASE);       /* no source yet: silence */
        } else {
            voice_pad_step(e);
        }
        break;
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

static void work(smg_t *e, uint32_t budget)   /* budget: units; 0xFFFFFFFF = to the end of the frame */
{
    uint32_t spent = 0;
    while (e->st != S_DONE && spent < budget) {
        e->units = 0;
        step(e);
        spent += e->units;
        e->units_frame += e->units;
    }
}

/* the play buffer ran out: the finished frame (windowed, mixed to stereo) over the last tail */
static void swap(smg_t *e)
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
    for (k = 0; k < SMG_DISP; k++)       /* the display: up fast, down slowly */
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

void smg_render(smg_t *e, float *l, float *r, uint32_t n)
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
void smg_note_on(smg_t *e, int32_t note, float vel)
{
    uint32_t v, best = 0;
    float best_level = 2.0f;
    if (note < 0 || note > 127)
        return;
    for (v = 0; v < SMG_VOICES; v++)     /* the same note again: that voice */
        if (e->voice[v].note == note) {
            best = v;
            goto found;
        }
    for (v = 0; v < SMG_VOICES; v++)     /* a free voice */
        if (e->voice[v].note < 0) {
            best = v;
            e->voice[v].level = 0.0f;
            goto found;
        }
    for (v = 0; v < SMG_VOICES; v++) {   /* else the quietest released one, or the quietest */
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

void smg_note_off(smg_t *e, int32_t note)
{
    uint32_t v;
    for (v = 0; v < SMG_VOICES; v++)
        if (e->voice[v].note == note)
            e->voice[v].gate = 0;
}

void smg_all_off(smg_t *e)
{
    uint32_t v;
    for (v = 0; v < SMG_VOICES; v++)
        e->voice[v].gate = 0;
}

void smg_panic(smg_t *e)
{
    uint32_t v, k;
    for (v = 0; v < SMG_VOICES; v++) {
        e->voice[v].note = -1;
        e->voice[v].gate = 0;
        e->voice[v].level = 0.0f;
    }
    for (k = 0; k < SMG_NMAX / 2u; k++)
        e->play[0][k] = e->play[1][k] = e->tail[0][k] = e->tail[1][k] = 0.0f;
}

int smg_active(const smg_t *e)
{
    int c = 0;
    uint32_t v;
    for (v = 0; v < SMG_VOICES; v++)
        c += e->voice[v].note >= 0;
    return c;
}

void smg_set_sample(smg_t *e, const int16_t *pcm, uint32_t len, uint32_t rate)
{
    e->smp.pcm = len ? pcm : 0;
    e->smp.len = pcm ? len : 0u;
    e->smp.rate = rate ? rate : SMG_FS;
    e->src_ok = 0;
    e->scan = e->smp.len ? fm_clampf(e->p.position, 0.0f, 1.0f) * (float)(e->smp.len - 1u) : 0.0f;
    e->last_position = -1.0f;
    e->units_bound = bound(e);           /* the frame under way may have to analyse now */
}

void smg_defaults(smg_params_t *p)
{
    p->source = SMG_SRC_SAMPLE;
    p->size = SMG_SIZE_8K;
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
}

void smg_init(smg_t *e, float *ram)
{
    uint32_t k;
    float *m = ram;
    smg_params_t keep;
    smg_defaults(&keep);
    for (k = 0; k < SMG_RAM_FLOATS; k++)
        ram[k] = 0.0f;
    {   /* the struct to zero, the parameters to their defaults */
        uint8_t *b = (uint8_t *)e;
        for (k = 0; k < sizeof *e; k++)
            b[k] = 0;
    }
    e->p = keep;
    e->work = m; m += 2u * SMG_NMAX;
    e->src = m; m += SMG_NMAX / 2u + 1u;
    e->acc = m; m += SMG_NMAX / 2u + 1u;
    e->play[0] = m; m += SMG_NMAX / 2u;
    e->play[1] = m; m += SMG_NMAX / 2u;
    e->tail[0] = m; m += SMG_NMAX / 2u;
    e->tail[1] = m; m += SMG_NMAX / 2u;
    e->win = m; m += SMG_NMAX / 2u;
    e->qs = m;
    for (k = 0; k <= QN; k++)
        e->qs[k] = fm_sinf(FM_TWO_PI * (float)k / (float)SMG_NMAX);
    e->qs[QN] = 1.0f;
    for (k = 0; k < SMG_VOICES; k++)
        e->voice[k].note = -1;
    e->rng = 0x9E3779B9u;
    e->units_est = 0;                    /* measured from the first frame; until then, the bound */
    e->last_position = -1.0f;
    e->n = smg_size_n(e->p.size);
    e->hop = e->n / 2u;
    e->play_len = e->hop;                /* the first hop is silence while the first frame is made */
    e->fp = e->p;
    frame_begin(e);
}
