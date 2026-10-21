/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 fm1-smuga contributors */
/* SMUGA's VA source: see va.h. */
#include "va.h"
#include "fastmath.h"

#define VA_FS 44100.0f
#define VA_F0 261.6256f                  /* C4: the engine's note 60, its ratio 1 */

float va_cutoff_hz(float cutoff) { return 60.0f * fm_exp2f(fm_clampf(cutoff, 0.0f, 1.0f) * 8.2f); }

static inline float blep(float t, float dt)   /* polyBLEP: the step's residual around a wrap */
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

static inline float saw(float ph, float dt) { return 2.0f * ph - 1.0f - blep(ph, dt); }

void va_init(va_t *v)
{
    uint8_t *b = (uint8_t *)v;
    uint32_t k;
    for (k = 0; k < sizeof *v; k++)
        b[k] = 0;
    v->shape = 0.0f;
    v->cutoff = 0.6f;
    v->reso = 0.3f;
    v->rng = 0x2545F491u;
}

void va_render(va_t *v, float *out, uint32_t n)
{
    const float dt = VA_F0 / VA_FS;
    float shape = fm_clampf(v->shape, 0.0f, 1.0f), mo = fm_clampf(v->motion, 0.0f, 1.0f);
    float morph = shape < 0.5f ? 2.0f * shape : 1.0f;              /* saw -> square */
    float pw = shape < 0.5f ? 0.5f : 0.5f - (shape - 0.5f) * 0.8f; /* square -> 10 % pulse */
    float oct, fc, g, k, a1, a2, a3;
    uint32_t i;
    /* MOTION, once a block: a sine LFO (0.03 .. 0.3 Hz) and a drift to a new random point every few seconds */
    v->lfo += (0.03f + 0.27f * mo) * (float)n / VA_FS;
    if (v->lfo >= 1.0f)
        v->lfo -= 1.0f;
    if ((v->n += n) >= 120000u) {
        v->n = 0;
        v->rng ^= v->rng << 13;
        v->rng ^= v->rng >> 17;
        v->rng ^= v->rng << 5;
        v->drift_to = (float)(v->rng >> 8) / 16777216.0f * 2.0f - 1.0f;
    }
    v->drift += (v->drift_to - v->drift) * 0.002f;
    oct = mo * (2.5f * fm_sinf(FM_TWO_PI * v->lfo) + 1.0f * v->drift);
    fc = fm_clampf(va_cutoff_hz(v->cutoff) * fm_exp2f(oct), 30.0f, 0.45f * VA_FS);
    g = fm_tanf(FM_PI * fc / VA_FS);
    k = 2.0f - 1.94f * fm_clampf(v->reso, 0.0f, 1.0f);
    a1 = 1.0f / (1.0f + g * (g + k));
    a2 = g * a1;
    a3 = g * a2;
    for (i = 0; i < n; i++) {
        float p2 = v->ph + pw, s, sq, x, v1, v2, v3;
        if (p2 >= 1.0f)
            p2 -= 1.0f;
        s = saw(v->ph, dt);
        sq = s - saw(p2, dt);            /* a pulse: two saws apart by its width */
        x = (1.0f - morph) * s + morph * sq * 0.8f;
        v->ph += dt;
        if (v->ph >= 1.0f)
            v->ph -= 1.0f;
        v3 = x - v->ic2;                 /* the TPT state-variable filter (Zavalishin) */
        v1 = a1 * v->ic1 + a2 * v3;
        v2 = v->ic2 + a2 * v->ic1 + a3 * v3;
        v->ic1 = 2.0f * v1 - v->ic1;
        v->ic2 = 2.0f * v2 - v->ic2;
        out[i] = 0.5f * fm_tanhf(0.9f * v2);
    }
    v->ic1 = fm_flush(v->ic1);
    v->ic2 = fm_flush(v->ic2);
}
