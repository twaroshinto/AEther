/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 fm1-aether contributors */
/* AETHER's engine on its own, on this machine: scenarios rendered to WAV files and checked.
 *
 *   tests/run.sh               builds this with the generated source and runs it
 *   dsp_test OUTDIR            the WAVs go to OUTDIR
 *
 * The checks: the output is finite and under full scale, it sounds when it should and is silent when it should,
 * a stretched sine keeps its pitch (and a key moves it), no frame is ever late at the FM-1's block size, and the
 * work a frame takes stays inside the budget measured in units. */
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "aether.h"
#include "va.h"
#include "aether_source.h"

#define BLOCK 256u                       /* the firmware's APP_BLOCK */
static float ram[AET_RAM_FLOATS];
static aet_t eng;
static int fails;
static const char *outdir = ".";
static va_t *live_va;                    /* the VA source, pushed into LIVE before each block (as app_render does) */

#define CHECK(cond, ...)                                                                                           \
    do {                                                                                                           \
        if (!(cond)) {                                                                                             \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                                          \
            printf(__VA_ARGS__);                                                                                   \
            printf("\n");                                                                                          \
            fails++;                                                                                               \
        }                                                                                                          \
    } while (0)

/* ---- a stereo recording */
typedef struct {
    float *l, *r;
    uint32_t n, cap;
} rec_t;

static void rec_run(rec_t *rc, float secs)
{
    uint32_t want = (uint32_t)(secs * (float)AET_FS), done = 0;
    if (rc->n + want > rc->cap) {
        rc->cap = (rc->n + want) * 2u;
        rc->l = realloc(rc->l, rc->cap * sizeof(float));
        rc->r = realloc(rc->r, rc->cap * sizeof(float));
    }
    while (done < want) {
        uint32_t n = want - done < BLOCK ? want - done : BLOCK;
        if (live_va) {
            va_render(live_va, rc->l + rc->n, n);
            aet_live_push(&eng, rc->l + rc->n, n);
            eng.p.lp_hz = live_va->fc_now;   /* the VA's filter: the engine's spectral low-pass */
        }
        aet_render(&eng, rc->l + rc->n, rc->r + rc->n, n);
        rc->n += n;
        done += n;
    }
}

static void rec_wav(const rec_t *rc, const char *name)
{
    char path[512];
    FILE *f;
    uint32_t i, bytes = rc->n * 4u;
    snprintf(path, sizeof path, "%s/%s", outdir, name);
    f = fopen(path, "wb");
    if (!f)
        return;
#define W32(v) do { uint32_t x_ = (v); fwrite(&x_, 4, 1, f); } while (0)
#define W16(v) do { uint16_t x_ = (uint16_t)(v); fwrite(&x_, 2, 1, f); } while (0)
    fwrite("RIFF", 1, 4, f); W32(36u + bytes); fwrite("WAVEfmt ", 1, 8, f);
    W32(16); W16(1); W16(2); W32(AET_FS); W32(AET_FS * 4u); W16(4); W16(16);
    fwrite("data", 1, 4, f); W32(bytes);
    for (i = 0; i < rc->n; i++) {
        int32_t a = (int32_t)lrintf(rc->l[i] * 32767.0f), b = (int32_t)lrintf(rc->r[i] * 32767.0f);
        W16(a < -32768 ? -32768 : a > 32767 ? 32767 : a);
        W16(b < -32768 ? -32768 : b > 32767 ? 32767 : b);
    }
    fclose(f);
    printf("  wav %s (%.1f s)\n", path, (double)rc->n / AET_FS);
}

/* RMS in dBFS of [t0, t1) seconds, both channels */
static double rms_db(const rec_t *rc, double t0, double t1)
{
    uint32_t a = (uint32_t)(t0 * AET_FS), b = (uint32_t)(t1 * AET_FS), i;
    double s = 0.0;
    if (b > rc->n)
        b = rc->n;
    if (b <= a)
        return -200.0;
    for (i = a; i < b; i++)
        s += (double)rc->l[i] * rc->l[i] + (double)rc->r[i] * rc->r[i];
    s /= 2.0 * (b - a);
    return s > 1e-20 ? 10.0 * log10(s) : -200.0;
}

static void check_sane(const rec_t *rc, const char *what)
{
    uint32_t i, bad = 0;
    float peak = 0.0f;
    for (i = 0; i < rc->n; i++) {
        if (!isfinite(rc->l[i]) || !isfinite(rc->r[i]))
            bad++;
        if (fabsf(rc->l[i]) > peak)
            peak = fabsf(rc->l[i]);
        if (fabsf(rc->r[i]) > peak)
            peak = fabsf(rc->r[i]);
    }
    CHECK(bad == 0, "%s: %u samples not finite", what, bad);
    CHECK(peak <= 1.0f, "%s: peak %.3f over full scale", what, peak);
}

/* the strongest frequency in [t0, t1) of the left channel, by a Goertzel sweep in 2 Hz steps over [lo, hi] */
static double peak_hz(const rec_t *rc, double t0, double t1, double lo, double hi)
{
    uint32_t a = (uint32_t)(t0 * AET_FS), b = (uint32_t)(t1 * AET_FS), i;
    double best = 0.0, best_f = 0.0, f;
    for (f = lo; f <= hi; f += 2.0) {
        double w = 2.0 * M_PI * f / AET_FS, c = 2.0 * cos(w), s1 = 0.0, s2 = 0.0, p;
        for (i = a; i < b; i++) {
            double s0 = rc->l[i] + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        p = s1 * s1 + s2 * s2 - c * s1 * s2;
        if (p > best) {
            best = p;
            best_f = f;
        }
    }
    return best_f;
}

static void reset(void)
{
    aet_init(&eng, ram);
    live_va = 0;
    aet_set_sample(&eng, aet_builtin[0].data, aet_builtin[0].len, aet_builtin[0].rate, AET_FMT_ADPCM);
    aet_set_env(&eng, aet_builtin[0].env, aet_builtin[0].env_frames, aet_builtin[0].env_hop);
}

static void rec_free(rec_t *rc) { free(rc->l); free(rc->r); }

/* ---- scenarios */
static void t_sample_stretch(void)
{
    rec_t rc = {0};
    printf("sample: the built-in choir stretched 40x, a held C4 then released\n");
    reset();
    eng.p.attack = 0.5f;
    eng.p.release = 2.0f;
    aet_note_on(&eng, 60, 0.9f);
    rec_run(&rc, 8.0f);
    aet_note_off(&eng, 60);
    rec_run(&rc, 4.0f);
    check_sane(&rc, "stretch");
    CHECK(rms_db(&rc, 1.5, 7.5) > -30.0, "stretch: too quiet while held (%.1f dB)", rms_db(&rc, 1.5, 7.5));
    CHECK(rms_db(&rc, 11.0, 12.0) < -50.0, "stretch: not silent after the release (%.1f dB)", rms_db(&rc, 11.0, 12.0));
    CHECK(eng.late == 0, "stretch: %u frames late", eng.late);
    printf("  held %.1f dBFS, after release %.1f dBFS, frames %u, units/frame %u\n", rms_db(&rc, 1.5, 7.5),
           rms_db(&rc, 11.0, 12.0), eng.frames, eng.units_est);
    rec_wav(&rc, "sample_stretch.wav");
    rec_free(&rc);
}

static void t_pad_chord(void)
{
    rec_t rc = {0};
    printf("pad: an A minor 9 chord, wide bands, shimmer\n");
    reset();
    eng.p.source = AET_SRC_PAD;
    eng.p.bandwidth = 60.0f;
    eng.p.shimmer = 0.3f;
    eng.p.sub = 0.2f;
    eng.p.attack = 1.5f;
    eng.p.release = 3.0f;
    aet_note_on(&eng, 45, 0.8f);
    aet_note_on(&eng, 57, 0.7f);
    aet_note_on(&eng, 60, 0.7f);
    aet_note_on(&eng, 64, 0.7f);
    aet_note_on(&eng, 71, 0.6f);
    rec_run(&rc, 7.0f);
    aet_all_off(&eng);
    rec_run(&rc, 6.0f);
    check_sane(&rc, "pad");
    CHECK(rms_db(&rc, 2.0, 6.5) > -30.0, "pad: too quiet (%.1f dB)", rms_db(&rc, 2.0, 6.5));
    CHECK(rms_db(&rc, 12.0, 13.0) < -50.0, "pad: not silent after the release (%.1f dB)", rms_db(&rc, 12.0, 13.0));
    CHECK(aet_active(&eng) == 0, "pad: %d voices left after the release", aet_active(&eng));
    CHECK(eng.late == 0, "pad: %u frames late", eng.late);
    printf("  held %.1f dBFS, units/frame %u\n", rms_db(&rc, 2.0, 6.5), eng.units_est);
    rec_wav(&rc, "pad_chord.wav");
    rec_free(&rc);
}

static int16_t sine_src[AET_FS];
static void t_pitch(void)
{
    rec_t rc = {0};
    uint32_t i;
    double f1, f2;
    printf("pitch: a 440 Hz sine stretched keeps 440 Hz; a key an octave up gives 880 Hz\n");
    for (i = 0; i < AET_FS; i++)
        sine_src[i] = (int16_t)lrint(16000.0 * sin(2.0 * M_PI * 440.0 * i / AET_FS));
    aet_init(&eng, ram);
    aet_set_sample(&eng, sine_src, AET_FS, AET_FS, AET_FMT_PCM16);
    eng.p.attack = 0.05f;
    eng.p.release = 0.1f;
    eng.p.width = 0.0f;
    aet_note_on(&eng, 60, 1.0f);
    rec_run(&rc, 2.0f);
    aet_note_off(&eng, 60);
    rec_run(&rc, 1.0f);
    aet_note_on(&eng, 72, 1.0f);
    rec_run(&rc, 2.0f);
    check_sane(&rc, "pitch");
    f1 = peak_hz(&rc, 0.8, 1.8, 200.0, 1200.0);
    f2 = peak_hz(&rc, 3.8, 4.8, 200.0, 1200.0);
    printf("  C4 %.0f Hz, C5 %.0f Hz, level %.1f dBFS (a -6.2 dBFS sine in)\n", f1, f2, rms_db(&rc, 0.8, 1.8));
    CHECK(fabs(f1 - 440.0) <= 6.0, "pitch: %.0f Hz, not 440", f1);
    CHECK(fabs(f2 - 880.0) <= 8.0, "pitch: %.0f Hz an octave up, not 880", f2);
    CHECK(fabs(rms_db(&rc, 0.8, 1.8) - -6.2) < 4.0, "pitch: level %.1f dBFS, far from the input's", rms_db(&rc, 0.8, 1.8));
    rec_wav(&rc, "pitch.wav");
    rec_free(&rc);
}

/* everything at its extremes, changed while it plays: sizes, freeze, spread, tilt, all voices, stealing */
static void t_torture(void)
{
    rec_t rc = {0};
    int32_t i;
    printf("torture: sizes, freeze, spread, tilt, 12 notes on 8 voices, the source switched\n");
    reset();
    eng.p.attack = 0.01f;
    eng.p.release = 0.05f;
    eng.p.sub = 1.0f;
    eng.p.shimmer = 1.0f;
    eng.p.level = 2.0f;
    for (i = 0; i < 12; i++)
        aet_note_on(&eng, 40 + i * 5, 1.0f);
    rec_run(&rc, 0.5f);
    for (i = 0; i < AET_NSIZE * 2; i++) {
        eng.p.size = (uint8_t)(i % AET_NSIZE);
        eng.p.spread = (float)(i & 1);
        eng.p.tilt = i & 2 ? 1.0f : -1.0f;
        eng.p.freeze = (uint8_t)((i >> 1) & 1);
        eng.p.stretch = i & 1 ? 0.0f : 10000.0f;
        eng.p.position = (float)i / (AET_NSIZE * 2);
        rec_run(&rc, 0.37f);
    }
    eng.p.source = AET_SRC_PAD;
    eng.p.bandwidth = 300.0f;
    eng.p.inharm = 1.0f;
    eng.p.bright = 1.0f;
    for (i = 0; i < AET_NSIZE; i++) {
        eng.p.size = (uint8_t)i;
        rec_run(&rc, 0.4f);
    }
    aet_set_sample(&eng, 0, 0, 0, 0);    /* no sample: silence, no crash */
    eng.p.source = AET_SRC_SAMPLE;
    rec_run(&rc, 0.5f);
    aet_panic(&eng);
    rec_run(&rc, 0.3f);
    check_sane(&rc, "torture");
    CHECK(rms_db(&rc, rc.n / (double)AET_FS - 0.2, rc.n / (double)AET_FS) < -90.0, "torture: not silent after panic");
    CHECK(eng.late == 0, "torture: %u frames late", eng.late);
    printf("  frames %u, late %u\n", eng.frames, eng.late);
    rec_wav(&rc, "torture.wav");
    rec_free(&rc);
}

/* the work per block at the FM-1's block size: the most any block spent, against the frame's whole */
static void t_budget(void)
{
    rec_t rc = {0};
    int32_t i;
    printf("budget: 8 voices with both octaves, SAMPLE with SPREAD, at the longest frame\n");
    reset();
    eng.p.spread = 0.6f;
    eng.p.sub = 0.5f;
    eng.p.shimmer = 0.5f;
    for (i = 0; i < 8; i++)
        aet_note_on(&eng, 48 + i * 3, 0.8f);
    rec_run(&rc, 5.0f);
    CHECK(eng.late == 0, "budget: %u frames late", eng.late);
    CHECK(eng.units_peak <= eng.units_bound, "budget: a frame took %u units, over its bound %u", eng.units_peak,
          eng.units_bound);
    printf("  units: peak frame %u, bound %u; per 256-sample block about %u (a unit: about one bin or butterfly)\n",
           eng.units_peak, eng.units_bound, (uint32_t)((uint64_t)eng.units_bound * 256u * 3u / (2u * eng.hop)));
    rec_free(&rc);
}

/* VA through LIVE: the saw's fundamental at the key's pitch, C4 and C5 */
static void t_va(void)
{
    rec_t rc = {0};
    va_t v;
    double f1, f2;
    printf("va: the VA voice smeared live: C4 at 262 Hz, C5 at 523 Hz; MOTION and RESO at their extremes\n");
    reset();
    va_init(&v);
    v.cutoff = 0.6f;
    live_va = &v;
    eng.p.source = AET_SRC_LIVE;
    eng.p.attack = 0.05f;
    eng.p.release = 0.1f;
    eng.p.width = 0.0f;
    aet_note_on(&eng, 60, 1.0f);
    rec_run(&rc, 2.0f);
    aet_note_off(&eng, 60);
    rec_run(&rc, 1.0f);
    aet_note_on(&eng, 72, 1.0f);
    rec_run(&rc, 2.0f);
    f1 = peak_hz(&rc, 0.8, 1.8, 150.0, 1200.0);
    f2 = peak_hz(&rc, 3.8, 4.8, 150.0, 1200.0);
    printf("  C4 %.0f Hz, C5 %.0f Hz, level %.1f dBFS\n", f1, f2, rms_db(&rc, 0.8, 1.8));
    CHECK(fabs(f1 - 261.6) <= 6.0, "va: %.0f Hz, not 262", f1);
    CHECK(fabs(f2 - 523.3) <= 8.0, "va: %.0f Hz an octave up, not 523", f2);
    CHECK(rms_db(&rc, 0.8, 1.8) > -30.0, "va: too quiet (%.1f dB)", rms_db(&rc, 0.8, 1.8));
    v.motion = 1.0f;                     /* everything moving, resonance at its peak, every shape */
    eng.p.lp_reso = 1.0f;
    eng.p.size = AET_SIZE_1K;
    aet_note_on(&eng, 36, 1.0f);
    aet_note_on(&eng, 84, 1.0f);
    for (int i = 0; i <= 8; i++) {
        v.shape = (float)i / 8.0f;
        v.cutoff = (float)(i % 3) / 2.0f;
        rec_run(&rc, 0.6f);
    }
    eng.p.freeze = 1;                    /* FREEZE holds the VA's spectrum */
    rec_run(&rc, 1.0f);
    check_sane(&rc, "va");
    CHECK(eng.late == 0, "va: %u frames late", eng.late);
    rec_wav(&rc, "va.wav");
    rec_free(&rc);
}

/* every built-in sample, stretched: it sounds, nothing breaks */
static void t_builtins(void)
{
    uint32_t b;
    printf("builtins: each built-in sample stretched on a C major chord\n");
    for (b = 0; b < AET_NBUILTIN; b++) {
        rec_t rc = {0};
        char name[64];
        reset();
        aet_set_sample(&eng, aet_builtin[b].data, aet_builtin[b].len, aet_builtin[b].rate, AET_FMT_ADPCM);
        aet_set_env(&eng, aet_builtin[b].env, aet_builtin[b].env_frames, aet_builtin[b].env_hop);
        eng.p.stretch = 20.0f;
        eng.p.attack = 0.3f;
        aet_note_on(&eng, 48, 0.8f);
        aet_note_on(&eng, 60, 0.8f);
        aet_note_on(&eng, 64, 0.8f);
        rec_run(&rc, 6.0f);
        check_sane(&rc, aet_builtin[b].name);
        printf("  %-7s %.1f dBFS\n", aet_builtin[b].name, rms_db(&rc, 1.0, 6.0));
        CHECK(rms_db(&rc, 1.0, 6.0) > -32.0, "%s: too quiet (%.1f dB)", aet_builtin[b].name, rms_db(&rc, 1.0, 6.0));
        CHECK(eng.late == 0, "%s: %u frames late", aet_builtin[b].name, eng.late);
        snprintf(name, sizeof name, "builtin_%s.wav", aet_builtin[b].name);
        rec_wav(&rc, name);
        rec_free(&rc);
    }
}

/* the Python ADPCM encoder (tools/aether_load.py, the same as the built-ins') against the engine's decoder: the same
 * sweep as PCM16 and as ADPCM (flash images from tests/run.sh), read forward and with jumps back */
static uint8_t *slurp(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    uint8_t *b;
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)*n);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n)
        *n = 0;
    fclose(f);
    return b;
}
static void t_adpcm(const char *pcm_img, const char *adpcm_img)
{
    long np = 0, na = 0;
    uint8_t *p = slurp(pcm_img, &np), *a = slurp(adpcm_img, &na);
    uint32_t len, i, mism = 0;
    double sig = 0.0, err = 0.0;
    int32_t *fwd;
    printf("adpcm: the encoder's sweep decoded by the engine (%s)\n", adpcm_img);
    if (!p || !a || np < 0x1000 + 2 || na < 0x1000 + 4) {
        CHECK(0, "adpcm: the images are missing (tests/run.sh makes them)");
        return;
    }
    len = (uint32_t)(np - 0x1000) / 2u;
    CHECK((uint32_t)(na - 0x1000) == AET_ADPCM_SIZE(len), "adpcm: %ld bytes for %u samples", na - 0x1000, len);
    aet_init(&eng, ram);
    aet_set_sample(&eng, a + 0x1000, len, 22050, AET_FMT_ADPCM);
    fwd = malloc(len * sizeof *fwd);
    for (i = 0; i < len; i++) {
        int32_t x = (int16_t)(p[0x1000 + 2 * i] | p[0x1000 + 2 * i + 1] << 8);
        fwd[i] = aet_sample_read(&eng, i);
        sig += (double)x * x;
        err += (double)(x - fwd[i]) * (x - fwd[i]);
    }
    for (i = 0; i < 4000u; i++) {        /* random access (backwards, across blocks) gives the same values */
        uint32_t k = (uint32_t)(((uint64_t)i * 2654435761u) % len);
        if (aet_sample_read(&eng, k) != fwd[k])
            mism++;
    }
    printf("  %u samples, SNR %.1f dB, random reads that differ: %u\n", len, 10.0 * log10(sig / (err + 1e-9)), mism);
    CHECK(10.0 * log10(sig / (err + 1e-9)) > 25.0, "adpcm: SNR %.1f dB", 10.0 * log10(sig / (err + 1e-9)));
    CHECK(mism == 0, "adpcm: %u random reads differ from the forward ones", mism);
    free(fwd);
    free(p);
    free(a);
}

/* ---- SBR: a full-band sample against the same kept at 11025 Hz, with its envelope and without */
typedef struct {
    uint32_t magic, rate, len, crc, fmt, bytes, env_frames, env_hop;
} img_hdr_t;

static void fft(double *re, double *im, uint32_t n)   /* in place, radix 2 */
{
    uint32_t i, j = 0, len, k;
    for (i = 1; i < n; i++) {
        uint32_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j |= bit;
        if (i < j) {
            double t = re[i]; re[i] = re[j]; re[j] = t;
            t = im[i]; im[i] = im[j]; im[j] = t;
        }
    }
    for (len = 2; len <= n; len <<= 1)
        for (i = 0; i < n; i += len)
            for (k = 0; k < len / 2; k++) {
                double a = -2.0 * M_PI * k / len, c = cos(a), s_ = sin(a);
                double ur = re[i + k], ui = im[i + k];
                double vr = re[i + k + len / 2] * c - im[i + k + len / 2] * s_;
                double vi = re[i + k + len / 2] * s_ + im[i + k + len / 2] * c;
                re[i + k] = ur + vr; im[i + k] = ui + vi;
                re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
            }
}

#define NB3 10                           /* bands of a sixth of an octave from 5 to 16 kHz (what SBR rebuilds) */
static void sixth_octaves(const rec_t *rc, double t0, double t1, double *db)
{
    static double re[8192], im[8192];
    double e[NB3] = {0};
    uint32_t a = (uint32_t)(t0 * AET_FS), b = (uint32_t)(t1 * AET_FS), i, k, nb;
    for (i = a; i + 8192u <= b; i += 4096u) {
        for (k = 0; k < 8192u; k++) {
            double w = 0.5 - 0.5 * cos(2.0 * M_PI * k / 8192.0);
            re[k] = (rc->l[i + k] + rc->r[i + k]) * w;
            im[k] = 0.0;
        }
        fft(re, im, 8192u);
        for (k = 1; k < 4096u; k++) {
            double f = k * (double)AET_FS / 8192.0;
            for (nb = 0; nb < NB3; nb++)
                if (f >= 5000.0 * pow(3.2, (double)nb / NB3) && f < 5000.0 * pow(3.2, (nb + 1.0) / NB3))
                    e[nb] += re[k] * re[k] + im[k] * im[k];
        }
    }
    for (nb = 0; nb < NB3; nb++)
        db[nb] = 10.0 * log10(e[nb] + 1e-20);
}

static int load_img(const char *path, uint8_t **blob, img_hdr_t *h)
{
    long n = 0;
    *blob = slurp(path, &n);
    if (!*blob || n < 0x1000)
        return 0;
    memcpy(h, *blob, sizeof *h);
    aet_set_sample(&eng, *blob + 0x1000, h->len, h->rate, h->fmt);
    if (h->env_frames)
        aet_set_env(&eng, *blob + 0x1000 + (h->fmt == AET_FMT_ADPCM ? AET_ADPCM_SIZE(h->len) : 2u * h->len),
                    h->env_frames, h->env_hop);
    return 1;
}

static double sbr_render(const char *path, double *db, const char *wav)
{
    rec_t rc = {0};
    uint8_t *blob;
    img_hdr_t h;
    double all;
    aet_init(&eng, ram);
    if (!load_img(path, &blob, &h)) {
        CHECK(0, "sbr: no image %s (tests/run.sh makes it)", path);
        return 0.0;
    }
    eng.p.stretch = 6.0f;
    eng.p.attack = 0.05f;
    aet_note_on(&eng, 60, 0.8f);
    rec_run(&rc, 5.0f);
    sixth_octaves(&rc, 0.5, 5.0, db);
    all = rms_db(&rc, 0.5, 5.0);
    check_sane(&rc, path);
    CHECK(eng.late == 0, "sbr: %u frames late (%s)", eng.late, path);
    rec_wav(&rc, wav);
    rec_free(&rc);
    free(blob);
    return all;
}

static void t_sbr(const char *ref, const char *sbr, const char *nosbr)
{
    double r[NB3], a[NB3], b[NB3], da = 0.0, db = 0.0;
    uint32_t i;
    printf("sbr: STRING kept at 11025 Hz, with its SBR envelope and without, against the full band (5-16 kHz)\n");
    sbr_render(ref, r, "sbr_ref.wav");
    sbr_render(sbr, a, "sbr_on.wav");
    sbr_render(nosbr, b, "sbr_off.wav");
    printf("  sixth octaves, dB against the full band:\n   with SBR ");
    for (i = 0; i < NB3; i++) {
        printf(" %+5.1f", a[i] - r[i]);
        da += (a[i] - r[i]) * (a[i] - r[i]);
        db += (b[i] - r[i]) * (b[i] - r[i]);
    }
    printf("\n   without  ");
    for (i = 0; i < NB3; i++)
        printf(" %+5.1f", b[i] - r[i]);
    da = sqrt(da / NB3);
    db = sqrt(db / NB3);
    printf("\n  log-spectral distance: with SBR %.1f dB, without %.1f dB\n", da, db);
    CHECK(da < 6.0, "sbr: %.1f dB from the full band", da);
    CHECK(db > 3.0 * da, "sbr: no better than without (%.1f against %.1f dB)", da, db);
}

/* HARM: what each control promises, measured at the harmonics' own frequencies */
static double goertzel_db(const rec_t *rc, double t0, double t1, double f)
{
    uint32_t a = (uint32_t)(t0 * AET_FS), b = (uint32_t)(t1 * AET_FS), i;
    double w = 2.0 * M_PI * f / AET_FS, c = 2.0 * cos(w), s1 = 0.0, s2 = 0.0, best = 0.0, df;
    for (df = -24.0; df <= 24.0; df += 2.0) {    /* the band's energy, +-24 Hz (random phases make one point waver) */
        w = 2.0 * M_PI * (f + df) / AET_FS;
        c = 2.0 * cos(w);
        s1 = s2 = 0.0;
        for (i = a; i < b; i++) {
            double s0 = rc->l[i] + c * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        best += s1 * s1 + s2 * s2 - c * s1 * s2;
    }
    return 10.0 * log10(best / ((double)(b - a) * (b - a)) + 1e-30);
}

static void harm_reset(void)
{
    aet_init(&eng, ram);
    eng.p.source = AET_SRC_HARM;
    eng.p.attack = 0.02f;
    eng.p.release = 0.2f;
    eng.p.width = 0.0f;
    eng.p.h_band = 4.0f;
    eng.p.h_slope = 0.5f;
    eng.p.h_vel = 0.0f;
}

static void t_harm(void)
{
    rec_t rc = {0};
    const double f0 = 130.8128;          /* C3 */
    double h2, h3, h4, h5, h6, early3, late3, early1, late1;
    printf("harm: ODD takes the even harmonics out; BOOST lifts the focus; an envelope on H3 moves H3 only\n");
    harm_reset();
    eng.p.h_odd = 1.0f;
    aet_note_on(&eng, 48, 1.0f);
    rec_run(&rc, 1.5f);
    h2 = goertzel_db(&rc, 0.5, 1.5, 2 * f0);
    h3 = goertzel_db(&rc, 0.5, 1.5, 3 * f0);
    printf("  ODD 100%%: H2 %.1f dB, H3 %.1f dB\n", h2, h3);
    CHECK(h3 - h2 > 30.0, "harm: ODD leaves H2 at %.1f dB under H3", h3 - h2);
    check_sane(&rc, "harm odd");
    rec_free(&rc);

    memset(&rc, 0, sizeof rc);
    harm_reset();
    eng.p.h_focus = 5.0f;
    eng.p.h_boost = 18.0f;
    eng.p.h_width = 0.6f;
    aet_note_on(&eng, 48, 1.0f);
    rec_run(&rc, 1.5f);
    h4 = goertzel_db(&rc, 0.5, 1.5, 4 * f0);
    h5 = goertzel_db(&rc, 0.5, 1.5, 5 * f0);
    h6 = goertzel_db(&rc, 0.5, 1.5, 6 * f0);
    printf("  BOOST +18 dB at H5: H4 %.1f, H5 %.1f, H6 %.1f dB\n", h4, h5, h6);
    CHECK(h5 - h4 > 10.0 && h5 - h6 > 10.0, "harm: H5 not lifted (%.1f / %.1f / %.1f)", h4, h5, h6);
    rec_free(&rc);

    memset(&rc, 0, sizeof rc);
    harm_reset();
    eng.p.size = AET_SIZE_2K;            /* (shorter frames: the envelope's time is clearer) */
    eng.p.h_target[0] = 3;               /* envelope A on the third harmonic: up fast, gone in a second */
    eng.p.h_att[0] = 0.05f;
    eng.p.h_dec[0] = 0.8f;
    eng.p.h_sus[0] = 0.0f;
    aet_note_on(&eng, 48, 1.0f);
    rec_run(&rc, 3.0f);
    early3 = goertzel_db(&rc, 0.1, 0.4, 3 * f0);
    late3 = goertzel_db(&rc, 2.0, 3.0, 3 * f0);
    early1 = goertzel_db(&rc, 0.1, 0.9, f0);    /* (long windows: a narrow band of random phases wavers) */
    late1 = goertzel_db(&rc, 1.5, 3.0, f0);
    printf("  ENV A on H3 (decay 0.8 s, sustain 0): H3 %.1f -> %.1f dB, H1 %.1f -> %.1f dB\n", early3, late3, early1, late1);
    CHECK(early3 - late3 > 30.0, "harm: H3 only fell %.1f dB", early3 - late3);
    /* (H1 wavers about +-2 dB window to window: its narrow band of random phases; measured with a windowed FFT) */
    CHECK(fabs(early1 - late1) < 5.0, "harm: H1 moved %.1f dB", late1 - early1);
    CHECK(eng.late == 0, "harm: %u frames late", eng.late);
    rec_wav(&rc, "harm_env.wav");
    rec_free(&rc);

    memset(&rc, 0, sizeof rc);           /* everything at once, 8 voices, every target */
    harm_reset();
    eng.p.h_count = 64;
    eng.p.h_band = 80.0f;
    eng.p.sub = eng.p.shimmer = 0.5f;
    for (int k = 0; k < 4; k++)
        eng.p.h_target[k] = (uint8_t)(AET_HT_ODD + k);
    for (int i = 0; i < 8; i++)
        aet_note_on(&eng, 36 + i * 5, 0.3f + 0.1f * i);
    rec_run(&rc, 3.0f);
    check_sane(&rc, "harm all");
    CHECK(eng.late == 0, "harm all: %u frames late", eng.late);
    CHECK(eng.units_peak <= eng.units_bound, "harm all: %u units over the bound %u", eng.units_peak, eng.units_bound);
    rec_free(&rc);
}

/* the second core, as a thread: the same frames as one core, bit for bit but for the order of two sums */
static pthread_t par_th;
static void (*par_fn)(void *);
static void *par_arg;
static int par_fail_after = -1, par_count;
static void *par_thread(void *x) { (void)x; par_fn(par_arg); return 0; }
static int par_run(void (*fn)(void *), void *arg)
{
    par_fn = fn;
    par_arg = arg;
    return pthread_create(&par_th, 0, par_thread, 0) == 0;
}
static int par_wait(void)
{
    pthread_join(par_th, 0);
    return par_fail_after >= 0 && ++par_count > par_fail_after ? -1 : 0;   /* a core that stops answering */
}

static void dual_case(rec_t *rc, int dual, int src)
{
    aet_init(&eng, ram);
    aet_set_sample(&eng, aet_builtin[2].data, aet_builtin[2].len, aet_builtin[2].rate, AET_FMT_ADPCM);
    aet_set_env(&eng, aet_builtin[2].env, aet_builtin[2].env_frames, aet_builtin[2].env_hop);
    if (dual)
        aet_set_par(&eng, par_run, par_wait);
    eng.p.source = (uint8_t)src;
    eng.p.h_count = 48;
    eng.p.h_band = 40.0f;
    eng.p.sub = eng.p.shimmer = 0.5f;
    for (int i = 0; i < 8; i++)
        aet_note_on(&eng, 40 + i * 4, 0.5f + 0.06f * i);
    rec_run(rc, 3.0f);
}

static void t_dual(void)
{
    static const int SRC[3] = {AET_SRC_HARM, AET_SRC_PAD, AET_SRC_SAMPLE};
    static const char *const NAME[3] = {"HARM", "PAD", "SAMPLE"};
    printf("dual: half the voices painted on a second core (a thread here): the same sound as one core\n");
    for (int k = 0; k < 3; k++) {
        rec_t a = {0}, b = {0};
        double d = 0.0, s = 0.0;
        dual_case(&a, 0, SRC[k]);
        dual_case(&b, 1, SRC[k]);
        for (uint32_t i = 0; i < a.n; i++) {
            d += (double)(a.l[i] - b.l[i]) * (a.l[i] - b.l[i]);
            s += (double)a.l[i] * a.l[i];
        }
        printf("  %-6s jobs %u, difference %.1f dB under the signal\n", NAME[k], eng.par_jobs, 10.0 * log10(s / (d + 1e-30)));
        CHECK(eng.par_jobs > 10, "dual %s: the second core got %u jobs", NAME[k], eng.par_jobs);
        CHECK(10.0 * log10(s / (d + 1e-30)) > 100.0, "dual %s: differs from one core", NAME[k]);
        CHECK(eng.late == 0 && !eng.par_failed, "dual %s: late %u, failed %u", NAME[k], eng.late, eng.par_failed);
        rec_free(&a);
        rec_free(&b);
    }
    {   /* the second core stops answering after a few jobs: one core from then on, the sound goes on */
        rec_t c = {0};
        par_fail_after = 5;
        par_count = 0;
        dual_case(&c, 1, AET_SRC_HARM);
        par_fail_after = -1;
        check_sane(&c, "dual fail");
        CHECK(eng.par_failed == 1 && !eng.dual, "dual: a failing core not given up");
        CHECK(rms_db(&c, 2.0, 3.0) > -30.0, "dual: silent after the second core failed");
        printf("  a core that stops answering: given up after %u jobs, the sound goes on (%.1f dBFS)\n", eng.par_jobs,
               rms_db(&c, 2.0, 3.0));
        rec_free(&c);
    }
}

int main(int argc, char **argv)
{
    if (argc > 1)
        outdir = argv[1];
    t_sample_stretch();
    t_pad_chord();
    t_pitch();
    t_torture();
    t_budget();
    t_va();
    t_builtins();
    t_harm();
    t_dual();
    if (argc > 3)
        t_adpcm(argv[2], argv[3]);
    if (argc > 6)
        t_sbr(argv[4], argv[5], argv[6]);
    printf(fails ? "dsp_test: %d FAILED\n" : "dsp_test: all passed\n", fails);
    return fails != 0;
}
