/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 fm1-smuga contributors */
/* SMUGA's engine on its own, on this machine: scenarios rendered to WAV files and checked.
 *
 *   tests/run.sh               builds this with the generated source and runs it
 *   dsp_test OUTDIR            the WAVs go to OUTDIR
 *
 * The checks: the output is finite and under full scale, it sounds when it should and is silent when it should,
 * a stretched sine keeps its pitch (and a key moves it), no frame is ever late at the FM-1's block size, and the
 * work a frame takes stays inside the budget measured in units. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "smuga.h"
#include "smuga_source.h"

#define BLOCK 256u                       /* the firmware's APP_BLOCK */
static float ram[SMG_RAM_FLOATS];
static smg_t eng;
static int fails;
static const char *outdir = ".";

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
    uint32_t want = (uint32_t)(secs * (float)SMG_FS), done = 0;
    if (rc->n + want > rc->cap) {
        rc->cap = (rc->n + want) * 2u;
        rc->l = realloc(rc->l, rc->cap * sizeof(float));
        rc->r = realloc(rc->r, rc->cap * sizeof(float));
    }
    while (done < want) {
        uint32_t n = want - done < BLOCK ? want - done : BLOCK;
        smg_render(&eng, rc->l + rc->n, rc->r + rc->n, n);
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
    W32(16); W16(1); W16(2); W32(SMG_FS); W32(SMG_FS * 4u); W16(4); W16(16);
    fwrite("data", 1, 4, f); W32(bytes);
    for (i = 0; i < rc->n; i++) {
        int32_t a = (int32_t)lrintf(rc->l[i] * 32767.0f), b = (int32_t)lrintf(rc->r[i] * 32767.0f);
        W16(a < -32768 ? -32768 : a > 32767 ? 32767 : a);
        W16(b < -32768 ? -32768 : b > 32767 ? 32767 : b);
    }
    fclose(f);
    printf("  wav %s (%.1f s)\n", path, (double)rc->n / SMG_FS);
}

/* RMS in dBFS of [t0, t1) seconds, both channels */
static double rms_db(const rec_t *rc, double t0, double t1)
{
    uint32_t a = (uint32_t)(t0 * SMG_FS), b = (uint32_t)(t1 * SMG_FS), i;
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
    uint32_t a = (uint32_t)(t0 * SMG_FS), b = (uint32_t)(t1 * SMG_FS), i;
    double best = 0.0, best_f = 0.0, f;
    for (f = lo; f <= hi; f += 2.0) {
        double w = 2.0 * M_PI * f / SMG_FS, c = 2.0 * cos(w), s1 = 0.0, s2 = 0.0, p;
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
    smg_init(&eng, ram);
    smg_set_sample(&eng, smg_source, SMG_SOURCE_LEN, SMG_SOURCE_RATE);
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
    smg_note_on(&eng, 60, 0.9f);
    rec_run(&rc, 8.0f);
    smg_note_off(&eng, 60);
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
    eng.p.source = SMG_SRC_PAD;
    eng.p.bandwidth = 60.0f;
    eng.p.shimmer = 0.3f;
    eng.p.sub = 0.2f;
    eng.p.attack = 1.5f;
    eng.p.release = 3.0f;
    smg_note_on(&eng, 45, 0.8f);
    smg_note_on(&eng, 57, 0.7f);
    smg_note_on(&eng, 60, 0.7f);
    smg_note_on(&eng, 64, 0.7f);
    smg_note_on(&eng, 71, 0.6f);
    rec_run(&rc, 7.0f);
    smg_all_off(&eng);
    rec_run(&rc, 6.0f);
    check_sane(&rc, "pad");
    CHECK(rms_db(&rc, 2.0, 6.5) > -30.0, "pad: too quiet (%.1f dB)", rms_db(&rc, 2.0, 6.5));
    CHECK(rms_db(&rc, 12.0, 13.0) < -50.0, "pad: not silent after the release (%.1f dB)", rms_db(&rc, 12.0, 13.0));
    CHECK(smg_active(&eng) == 0, "pad: %d voices left after the release", smg_active(&eng));
    CHECK(eng.late == 0, "pad: %u frames late", eng.late);
    printf("  held %.1f dBFS, units/frame %u\n", rms_db(&rc, 2.0, 6.5), eng.units_est);
    rec_wav(&rc, "pad_chord.wav");
    rec_free(&rc);
}

static int16_t sine_src[SMG_FS];
static void t_pitch(void)
{
    rec_t rc = {0};
    uint32_t i;
    double f1, f2;
    printf("pitch: a 440 Hz sine stretched keeps 440 Hz; a key an octave up gives 880 Hz\n");
    for (i = 0; i < SMG_FS; i++)
        sine_src[i] = (int16_t)lrint(16000.0 * sin(2.0 * M_PI * 440.0 * i / SMG_FS));
    smg_init(&eng, ram);
    smg_set_sample(&eng, sine_src, SMG_FS, SMG_FS);
    eng.p.attack = 0.05f;
    eng.p.release = 0.1f;
    eng.p.width = 0.0f;
    smg_note_on(&eng, 60, 1.0f);
    rec_run(&rc, 2.0f);
    smg_note_off(&eng, 60);
    rec_run(&rc, 1.0f);
    smg_note_on(&eng, 72, 1.0f);
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
        smg_note_on(&eng, 40 + i * 5, 1.0f);
    rec_run(&rc, 0.5f);
    for (i = 0; i < SMG_NSIZE * 2; i++) {
        eng.p.size = (uint8_t)(i % SMG_NSIZE);
        eng.p.spread = (float)(i & 1);
        eng.p.tilt = i & 2 ? 1.0f : -1.0f;
        eng.p.freeze = (uint8_t)((i >> 1) & 1);
        eng.p.stretch = i & 1 ? 0.0f : 10000.0f;
        eng.p.position = (float)i / (SMG_NSIZE * 2);
        rec_run(&rc, 0.37f);
    }
    eng.p.source = SMG_SRC_PAD;
    eng.p.bandwidth = 300.0f;
    eng.p.inharm = 1.0f;
    eng.p.bright = 1.0f;
    for (i = 0; i < SMG_NSIZE; i++) {
        eng.p.size = (uint8_t)i;
        rec_run(&rc, 0.4f);
    }
    smg_set_sample(&eng, 0, 0, 0);       /* no sample: silence, no crash */
    eng.p.source = SMG_SRC_SAMPLE;
    rec_run(&rc, 0.5f);
    smg_panic(&eng);
    rec_run(&rc, 0.3f);
    check_sane(&rc, "torture");
    CHECK(rms_db(&rc, rc.n / (double)SMG_FS - 0.2, rc.n / (double)SMG_FS) < -90.0, "torture: not silent after panic");
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
        smg_note_on(&eng, 48 + i * 3, 0.8f);
    rec_run(&rc, 5.0f);
    CHECK(eng.late == 0, "budget: %u frames late", eng.late);
    CHECK(eng.units_peak <= eng.units_bound, "budget: a frame took %u units, over its bound %u", eng.units_peak,
          eng.units_bound);
    printf("  units: peak frame %u, bound %u; per 256-sample block about %u (a unit: about one bin or butterfly)\n",
           eng.units_peak, eng.units_bound, (uint32_t)((uint64_t)eng.units_bound * 256u * 3u / (2u * eng.hop)));
    rec_free(&rc);
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
    printf(fails ? "dsp_test: %d FAILED\n" : "dsp_test: all passed\n", fails);
    return fails != 0;
}
