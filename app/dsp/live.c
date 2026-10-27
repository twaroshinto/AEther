/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 fm1-aether contributors */
/* AETHER's live sources: see live.h. */
#include "live.h"
#include "fastmath.h"

#define LIVE_FS 44100.0f
#define LIVE_F0 261.6256f                /* C4: the engine's note 60, its ratio 1 */
#define LIVE_RMS 0.25f                   /* the automatic gain's aim (about as loud as VA) */

static float rnd(live_t *g)              /* -1 .. 1 */
{
    g->rng ^= g->rng << 13;
    g->rng ^= g->rng >> 17;
    g->rng ^= g->rng << 5;
    return (float)(g->rng >> 8) / 8388608.0f - 1.0f;
}
static float rnd01(live_t *g) { return 0.5f + 0.5f * rnd(g); }

/* a Chamberlin state-variable filter's band-pass: f = 2 sin(pi fc / fs), q = 1 / Q (fine below ~fs/6) */
static inline float svf_bp(live_svf_t *s, float x, float f, float q)
{
    float hp;
    s->lp += f * s->bp;
    hp = x - s->lp - q * s->bp;
    s->bp += f * hp;
    return s->bp;
}
static float svf_f(float hz) { return 2.0f * fm_sinf(FM_PI * fm_minf(hz, 6000.0f) / LIVE_FS); }

static inline float blep(float t, float dt)
{
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0f;
    }
    if (t > 1.0f - dt) {
        t = (t - 1.0f) / dt;
        return t * t + t + t + 1.0f;
    }
    return 0.0f;
}

void live_init(live_t *g)
{
    uint8_t *b = (uint8_t *)g;
    uint32_t k;
    for (k = 0; k < sizeof *g; k++)
        b[k] = 0;
    g->rng = 0x7A11F00Du;
    g->gain = 1.0f;
    g->tone = g->move = g->character = g->density = 0.5f;
    for (k = 0; k < LIVE_NV; k++)
        g->ph[k] = (float)k * 0.37f;
    for (k = 0; k < LIVE_NR; k++)        /* (sounding from the first block: gusts and the bow half way) */
        g->gust[k] = g->gust_to[k] = 0.6f;
    g->press = g->press_to = 0.7f;
    g->vowel = g->vowel_to = 0.5f;
}

void live_kind(live_t *g, uint32_t kind)
{
    float t = g->tone, m = g->move, c = g->character, d = g->density;
    uint32_t r = g->rng;
    if (kind == g->kind && g->n)
        return;
    live_init(g);
    g->kind = (uint8_t)(kind < LIVE_N ? kind : 0u);
    g->tone = t, g->move = m, g->character = c, g->density = d;
    g->rng = r ? r : 1u;
    g->n = 1;
}

/* ---- VOX: voices through four formants. The vowels (Peterson & Barney's men; CHARACTER scales them up to
 * children's): a, o, u, e, i */
static const float VOWEL[5][LIVE_NF] = {
    {730, 1090, 2440, 3400}, {570, 840, 2410, 3300}, {300, 870, 2240, 3300}, {530, 1840, 2480, 3400},
    {270, 2290, 3010, 3500},
};
static const float FORM_GAIN[LIVE_NF] = {1.0f, 0.7f, 0.35f, 0.2f}, FORM_Q[LIVE_NF] = {7.0f, 9.0f, 13.0f, 15.0f};

static void vox(live_t *g, float *out, uint32_t n)
{
    uint32_t nv = 1u + (uint32_t)(g->density * 5.0f + 0.5f), i, k;
    float f[LIVE_NF], q[LIVE_NF], inc[LIVE_NV], scale = 0.85f + 0.5f * g->character;
    float spread = 4.0f + 14.0f * g->density, lpa = 0.08f + 0.4f * g->tone, breath = 0.05f + 0.35f * g->tone;
    float v, w;
    uint32_t v0;
    /* the vowel wanders: a new one every 3 .. 14 s (MOVE), glided to */
    if (g->move > 0.01f && rnd01(g) < (float)n / LIVE_FS / (14.0f - 11.0f * g->move))
        g->vowel_to = 4.0f * rnd01(g);
    g->vowel += (g->vowel_to - g->vowel) * fm_minf(1.0f, (float)n / LIVE_FS * (0.15f + 0.6f * g->move));
    v = fm_clampf(g->vowel, 0.0f, 3.999f);
    v0 = (uint32_t)v;
    w = v - (float)v0;
    for (k = 0; k < LIVE_NF; k++) {
        f[k] = svf_f(scale * (VOWEL[v0][k] * (1.0f - w) + VOWEL[v0 + 1u][k] * w));
        q[k] = 1.0f / FORM_Q[k];
    }
    for (k = 0; k < nv; k++) {           /* each voice detuned a little, with a slow vibrato */
        float det = nv > 1u ? ((float)k / (float)(nv - 1u) * 2.0f - 1.0f) * spread : 0.0f;
        g->vib[k] += (float)n / LIVE_FS * (0.17f + 0.05f * (float)k);
        if (g->vib[k] >= 1.0f)
            g->vib[k] -= 1.0f;
        det += 6.0f * fm_sinf(FM_TWO_PI * g->vib[k]);
        inc[k] = LIVE_F0 * fm_exp2f(det / 1200.0f) / LIVE_FS;
    }
    for (i = 0; i < n; i++) {
        float src = 0.0f, y = 0.0f;
        for (k = 0; k < nv; k++) {
            float ph = g->ph[k], dt = inc[k];
            src += 2.0f * ph - 1.0f - blep(ph, dt);
            ph += dt;
            g->ph[k] = ph >= 1.0f ? ph - 1.0f : ph;
        }
        src = src / (float)nv + breath * rnd(g);
        g->breath_lp += lpa * (src - g->breath_lp);   /* the glottis' slope (TONE: brighter) */
        for (k = 0; k < LIVE_NF; k++)
            y += FORM_GAIN[k] * svf_bp(&g->form[k], g->breath_lp, f[k], q[k]);
        out[i] = y;
    }
}

/* ---- AIR: noise through resonances on C4's harmonics, from a register (TONE) up, each gusting on its own */
static void air(live_t *g, float *out, uint32_t n)
{
    uint32_t nr = 1u + (uint32_t)(g->density * 7.0f + 0.5f), i, k;
    float qq = 1.0f / (3.0f + 77.0f * g->character * g->character), f[LIVE_NR], gn[LIVE_NR];
    float floor_amt = 0.25f * (1.0f - g->character), blk = (float)n / LIVE_FS;
    if (rnd01(g) < blk / 9.0f)           /* the register drifts too */
        g->centre_to = rnd(g);
    g->centre += (g->centre_to - g->centre) * fm_minf(1.0f, blk * 0.1f * (0.2f + g->move));
    for (k = 0; k < nr; k++) {
        float h = fm_floorf(1.0f + 6.0f * g->tone + 1.5f * g->centre * g->move) + (float)k;
        if (g->move > 0.01f && rnd01(g) < blk / (8.0f - 6.5f * g->move))
            g->gust_to[k] = rnd01(g);
        if (g->move <= 0.01f)
            g->gust_to[k] = 0.6f;
        g->gust[k] += (g->gust_to[k] - g->gust[k]) * fm_minf(1.0f, blk * (0.3f + 1.5f * g->move));
        f[k] = svf_f(LIVE_F0 * fm_maxf(h, 1.0f));
        gn[k] = g->gust[k] * g->gust[k];
    }
    for (i = 0; i < n; i++) {
        float x = rnd(g), y = 0.0f;
        for (k = 0; k < nr; k++)
            y += gn[k] * svf_bp(&g->res[k], x, f[k], qq);
        g->exc_lp += 0.05f * (x - g->exc_lp);
        out[i] = y * qq * 4.0f + floor_amt * g->exc_lp;
    }
}

/* ---- BOW: plucked-string loops (Karplus-Strong) kept sounding by noise, a "bow" whose pressure wanders */
static const float STRING_HZ[LIVE_NS] = {LIVE_F0, LIVE_F0 * 0.5f, LIVE_F0 * 1.004f, LIVE_F0 * 2.0f};

static void bow(live_t *g, float *out, uint32_t n)
{
    uint32_t ns = 1u + (uint32_t)(g->density * 3.0f + 0.5f), i, k;
    float a = 0.25f + 0.65f * g->tone, len[LIVE_NS], blk = (float)n / LIVE_FS;
    float exa = 0.05f + 0.6f * g->character;
    if (g->move > 0.01f && rnd01(g) < blk / (7.0f - 5.5f * g->move))
        g->press_to = 0.3f + 0.7f * rnd01(g);
    if (g->move <= 0.01f)
        g->press_to = 0.7f;
    g->press += (g->press_to - g->press) * fm_minf(1.0f, blk * (0.2f + g->move));
    for (k = 0; k < ns; k++)             /* the loop, less the one-pole's own delay at the pitch */
        len[k] = fm_clampf(LIVE_FS / STRING_HZ[k] - (1.0f - a) / a, 2.0f, (float)LIVE_SLEN - 2.0f);
    for (i = 0; i < n; i++) {
        float y = 0.0f, ex;
        g->exc_lp += exa * (rnd(g) - g->exc_lp);
        ex = g->exc_lp * g->press * 0.03f;
        for (k = 0; k < ns; k++) {
            float *s = g->str[k], rp = (float)g->sw[k] - len[k], fr, d;
            uint32_t r0;
            if (rp < 0.0f)
                rp += (float)LIVE_SLEN;
            r0 = (uint32_t)rp;
            fr = rp - (float)r0;
            d = s[r0] + fr * (s[r0 + 1u < LIVE_SLEN ? r0 + 1u : 0u] - s[r0]);
            g->slp[k] += a * (d - g->slp[k]);
            s[g->sw[k]] = g->slp[k] * 0.996f + ex;
            g->sw[k] = g->sw[k] + 1u < LIVE_SLEN ? g->sw[k] + 1u : 0u;
            y += d;
        }
        out[i] = y;
    }
}

/* ---- BELL: modal resonators; the modes from C3's harmonics (CHARACTER 0) to a church bell's partials (1) */
static const float MODE_HARM[LIVE_NM] = {0.5f, 1.0f, 1.5f, 2.0f, 2.5f, 3.0f, 3.5f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f};
static const float MODE_BELL[LIVE_NM] = {0.5f, 1.0f, 1.19f, 1.5f, 2.0f, 2.51f, 2.66f, 3.01f, 4.17f, 5.43f, 6.8f, 8.21f};

static void bell(live_t *g, float *out, uint32_t n)
{
    float c1[LIVE_NM], c2[LIVE_NM], rate = 0.2f + 5.0f * g->density * g->density, blk = (float)n / LIVE_FS;
    uint32_t i, k;
    for (k = 0; k < LIVE_NM; k++) {
        float ratio = MODE_HARM[k] + (MODE_BELL[k] - MODE_HARM[k]) * g->character;
        float t60 = 7.0f * fm_powf(ratio * 2.0f, -1.3f * (1.0f - g->tone));
        float r = fm_expf(-6.9078f / (t60 * LIVE_FS)), w = FM_TWO_PI * LIVE_F0 * ratio / LIVE_FS;
        c1[k] = 2.0f * r * fm_cosf(w);
        c2[k] = r * r;
    }
    g->next_strike -= blk;
    if (g->next_strike <= 0.0f) {        /* a strike: every mode, at a strength that varies with MOVE */
        float s = 1.0f - g->move * 0.8f * rnd01(g);
        for (k = 0; k < LIVE_NM; k++)
            g->y1[k] += s * (0.4f + 0.6f * rnd01(g)) / (1.0f + 0.35f * (float)k) * 0.05f;
        g->next_strike = -fm_logf(fm_maxf(rnd01(g), 0.01f)) / rate;   /* at random: on average RATE a second */
    }
    for (i = 0; i < n; i++) {
        float y = 0.0f;
        for (k = 0; k < LIVE_NM; k++) {
            float v = c1[k] * g->y1[k] - c2[k] * g->y2[k];
            g->y2[k] = g->y1[k];
            g->y1[k] = v;
            y += v;
        }
        out[i] = y;
    }
    for (k = 0; k < LIVE_NM; k++) {      /* (no denormals while a mode dies away) */
        g->y1[k] = fm_flush(g->y1[k]);
        g->y2[k] = fm_flush(g->y2[k]);
    }
}

void live_render(live_t *g, float *out, uint32_t n)
{
    float e = 0.0f, rms, aim;
    uint32_t i;
    g->tone = fm_clampf(g->tone, 0.0f, 1.0f);
    g->move = fm_clampf(g->move, 0.0f, 1.0f);
    g->character = fm_clampf(g->character, 0.0f, 1.0f);
    g->density = fm_clampf(g->density, 0.0f, 1.0f);
    switch (g->kind) {
    case LIVE_AIR: air(g, out, n); break;
    case LIVE_BOW: bow(g, out, n); break;
    case LIVE_BELL: bell(g, out, n); break;
    default: vox(g, out, n); break;
    }
    /* the automatic gain: the block's level, smoothed, brought towards LIVE_RMS (x 1/20 .. x 400) */
    for (i = 0; i < n; i++)
        e += out[i] * out[i];
    rms = fm_sqrtf(e / (float)(n ? n : 1u));
    g->rms += (rms - g->rms) * 0.05f;
    aim = fm_clampf(LIVE_RMS / (g->rms * g->gain + 1e-6f) * g->gain, 0.05f, 400.0f);
    g->gain += (aim - g->gain) * 0.03f;
    for (i = 0; i < n; i++)
        out[i] = fm_clampf(out[i] * g->gain, -1.0f, 1.0f);
}
