/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 fm1-aether contributors */
/* AETHER's live sources: small generators that play all the time at C4 and feed the engine's LIVE source
 * (aet_live_push), the way VA does. They are recipes, not recordings, in the spirit of the demoscene's 64k intros:
 * a few hundred bytes of code each, no flash for samples, never repeating, and they change by themselves, slowly.
 *
 *   VOX    voices: a few detuned glottal pulses through four formants that wander from vowel to vowel
 *   AIR    wind: noise through resonances on C4's harmonics, each gusting on its own
 *   BOW    bowed strings: plucked-string loops kept sounding by a noisy "bow" whose pressure wanders
 *   BELL   a field of bells: modal resonators (harmonic to bell-like ratios) struck at random
 *
 * Their four knobs mean, for each:
 *   tone       VOX breath and brightness, AIR the resonances' register, BOW the strings' damping, BELL how long the
 *              upper modes ring
 *   move       how fast each changes by itself (vowels, gusts, bow pressure, strikes' strength), 0 = still
 *   character  VOX low voices .. high voices, AIR breathy .. whistling, BOW smooth .. scratchy, BELL harmonic .. bell
 *   density    VOX 1 .. 6 voices, AIR 1 .. 8 resonances, BOW 1 .. 4 strings, BELL strikes a second
 * An automatic gain keeps them about as loud as VA. Float only, no libc (fastmath.h); at most ~150 operations a
 * sample (BELL with every mode). */
#pragma once
#include <stdint.h>

enum { LIVE_VOX, LIVE_AIR, LIVE_BOW, LIVE_BELL, LIVE_N };

#define LIVE_NV 6u                       /* VOX: voices */
#define LIVE_NF 4u                       /* VOX: formants */
#define LIVE_NR 8u                       /* AIR: resonances */
#define LIVE_NS 4u                       /* BOW: strings */
#define LIVE_SLEN 352u                   /* BOW: a string's loop, C3 at most (337 samples) */
#define LIVE_NM 12u                      /* BELL: modes */

typedef struct {
    float lp, bp;                        /* a state-variable filter's two states */
} live_svf_t;

typedef struct {
    uint8_t kind;                        /* LIVE_* */
    float tone, move, character, density;   /* the knobs, 0 .. 1 */
    uint32_t rng, n;
    float gain, rms;                     /* the automatic gain */
    /* VOX */
    float ph[LIVE_NV], vib[LIVE_NV], vowel, vowel_to, breath_lp;
    live_svf_t form[LIVE_NF];
    /* AIR */
    live_svf_t res[LIVE_NR];
    float gust[LIVE_NR], gust_to[LIVE_NR], centre, centre_to;
    /* BOW */
    float str[LIVE_NS][LIVE_SLEN];
    uint32_t sw[LIVE_NS];
    float slp[LIVE_NS], press, press_to, exc_lp;
    /* BELL */
    float y1[LIVE_NM], y2[LIVE_NM], amp[LIVE_NM], next_strike;
} live_t;

void live_init(live_t *g);
void live_kind(live_t *g, uint32_t kind); /* (a new kind starts from silence) */
void live_render(live_t *g, float *out, uint32_t n);   /* n samples at 44.1 kHz */
