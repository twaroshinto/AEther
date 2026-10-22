/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 fm1-smuga contributors */
/* SMUGA on the FM-1: the panel, the screen, MIDI and the sample loader around the engine (dsp/smuga.c).
 * Part of fm1-base's unity build (base/device/unity.c, base/host/plat_host.c), after the platform: gfx.c, announce.c
 * and lib/touch.c are in scope. Structure from fm1-base's template.
 *
 * The panel:
 *   keys            notes (velocity from the keys); OCT- / OCT+ the octave
 *   K1 .. K4        the page's four parameters
 *   SEL, SELECT     the next page (SOURCE, TONE, ENV A-D, SPACE, LFO 1, LFO 2, MOD ENV, COLOR; TONE and ENV A-D only
 *                   with HARM). EDIT: SOURCE / TONE; ENV: SPACE / ENV A-D; LFO: LFO 1 / LFO 2 / MOD ENV; FX: COLOR
 *   ALGORITHM       the source: PAD (harmonic clouds), VA (a virtual-analog voice, smeared live), HARM (an
 *                   additive generator with four harmonic envelopes), 16 built-in samples, USER (loaded over USB)
 *   PRESET          a factory preset
 *   PLAY            HOLD: the notes stay after the keys are let go (a new chord replaces them)
 *   REC             FREEZE: the sample's scan stops, the spectrum held
 *   HOME            all notes off (and HOLD off)
 *   SAVE            the settings kept across a power cycle
 * MIDI in (USB, TRS): notes on any channel, CC64 sustain, CC1 POSITION, CC71 SPREAD, CC74 TILT. Notes from the keys
 * go out over USB. */
#include "fastmath.h"
#include "smuga.h"
#include "va.h"
#include "smuga_source.h"                /* the built-in samples (tools/gen_source.py, at build time) */

/* ------------------------------------------------------------- parameters --- */
enum { P_STRETCH, P_POSITION, P_SPREAD, P_SIZE, P_BRIGHT, P_BAND, P_INHARM, P_ODD,
       P_ATTACK, P_RELEASE, P_WIDTH, P_LEVEL, P_TILT, P_SUB, P_SHIMMER, P_SHAPE, P_CUTOFF, P_RESO, P_MOTION,
       /* HARM */
       P_H_COUNT, P_H_SLOPE, P_H_ODD, P_H_BAND, P_H_FOCUS, P_H_BOOST, P_H_WIDTH, P_H_VEL,
       P_HENV0,                          /* ENV A-D: TARGET, ATTACK, DECAY, SUSTAIN each */
       /* modulation: LFO 1, LFO 2 (RATE, SHAPE, DEST, DEPTH), MOD ENV (ATTACK, DECAY, DEST, DEPTH) */
       P_LFO0 = P_HENV0 + 4 * SMG_HENV, P_MENV = P_LFO0 + 8, NP = P_MENV + 4 };
#define P_HENV(k, i) (P_HENV0 + 4 * (k) + (i))
#define P_LFO(k, i) (P_LFO0 + 4 * (k) + (i))
enum { LFO_SINE, LFO_TRI, LFO_SAW_UP, LFO_SAW_DOWN, LFO_SQUARE, LFO_SH, LFO_DRIFT, NLFO_SHAPE };
static const char *const LFO_SHAPE_NAME[NLFO_SHAPE] = {"SINE", "TRIANGLE", "SAW UP", "SAW DOWN", "SQUARE",
                                                      "RANDOM", "DRIFT"};
/* what a modulator may move: these parameters (their knobs' values, so every mapping holds) */
static const uint8_t MOD_DEST[] = {0xFF, P_STRETCH, P_POSITION, P_SPREAD, P_TILT, P_SUB, P_SHIMMER, P_WIDTH, P_LEVEL,
                                   P_BRIGHT, P_BAND, P_INHARM, P_ODD, P_SHAPE, P_CUTOFF, P_RESO, P_H_SLOPE, P_H_ODD,
                                   P_H_FOCUS, P_H_BOOST, P_H_BAND};
#define NDEST (sizeof MOD_DEST)
typedef struct {
    const char *name;
    uint8_t max;                         /* the values are 0 .. max (127 mostly) */
} pdef_t;
#define HENV_DEF {"TARGET", SMG_NHT - 1u}, {"ATTACK", 127}, {"DECAY", 127}, {"SUSTAIN", 127}
#define LFO_DEF {"RATE", 127}, {"SHAPE", NLFO_SHAPE - 1u}, {"DEST", NDEST - 1u}, {"DEPTH", 127}
static const pdef_t PDEF[NP] = {
    {"STRETCH", 127}, {"POSITION", 127}, {"SPREAD", 127}, {"SIZE", SMG_NSIZE - 1u},
    {"BRIGHT", 127}, {"BAND", 127}, {"INHARM", 127}, {"ODD", 127},
    {"ATTACK", 127}, {"RELEASE", 127}, {"WIDTH", 127}, {"LEVEL", 127},
    {"TILT", 127}, {"SUB", 127}, {"SHIMMER", 127},
    {"SHAPE", 127}, {"CUTOFF", 127}, {"RESO", 127}, {"MOTION", 127},
    {"HARMONICS", SMG_HARM_MAX - 1u}, {"SLOPE", 127}, {"EVEN/ODD", 127}, {"H BAND", 127},
    {"FOCUS", 31}, {"BOOST", 127}, {"FOCUS W", 127}, {"VEL>TONE", 127},
    HENV_DEF, HENV_DEF, HENV_DEF, HENV_DEF,
    LFO_DEF, LFO_DEF,
    {"ATTACK", 127}, {"DECAY", 127}, {"DEST", NDEST - 1u}, {"DEPTH", 127},
};
/* the factory values; a preset changes some of them */
static const uint8_t PDEFAULT[NP] = {
    52, 0, 0, 3, 64, 50, 0, 0, 80, 90, 110, 96, 64, 0, 0, 0, 80, 40, 30,
    23, 40, 64, 30, 3, 64, 30, 64,
    0, 50, 80, 64, 0, 50, 80, 64, 0, 50, 80, 64, 0, 50, 80, 64,
    40, LFO_SINE, 0, 96, 60, LFO_DRIFT, 0, 96,
    40, 80, 0, 96,
};

/* the sources: PAD, VA, HARM, the built-in samples (smg_builtin[], SLOT_SMP0 on), USER */
enum { SLOT_PAD, SLOT_VA, SLOT_HARM, SLOT_SMP0, SLOT_CHOIR = SLOT_SMP0, SLOT_GLASS, SLOT_STRING, SLOT_WIND, SLOT_PIANO,
       SLOT_BOWED, SLOT_FLUTE, SLOT_ORGAN, SLOT_KALIMBA, SLOT_BRASS, SLOT_DRONE, SLOT_RAIN, SLOT_BIRDS,
       SLOT_WHISPER, SLOT_GAMELAN, SLOT_TAPE };   /* in gen_source.py's order */
#define SLOT_USER (SLOT_SMP0 + SMG_NBUILTIN)
#define NSLOT (SLOT_USER + 1u)
static int slot_sample(uint32_t s) { return s >= SLOT_SMP0; }   /* a sample, built-in or loaded */
static const char *slot_name(uint32_t s)
{
    return s == SLOT_PAD ? "PAD" : s == SLOT_VA ? "VA" : s == SLOT_HARM ? "HARM" :
           s < SLOT_USER ? smg_builtin[s - SLOT_SMP0].name : "USER";
}
enum { PG_SOURCE, PG_TONE, PG_ENVA, PG_ENVB, PG_ENVC, PG_ENVD, PG_SPACE, PG_LFO1, PG_LFO2, PG_MENV, PG_COLOR, NPAGE };
static const char *const PAGE_NAME[NPAGE] = {"SOURCE", "TONE", "ENV A", "ENV B", "ENV C", "ENV D", "SPACE", "LFO 1",
                                             "LFO 2", "MOD ENV", "COLOR"};
static const uint8_t PAGE_SAMPLE[4] = {P_STRETCH, P_POSITION, P_SPREAD, P_SIZE};
static const uint8_t PAGE_PAD[4] = {P_BRIGHT, P_BAND, P_INHARM, P_ODD};
static const uint8_t PAGE_VA[4] = {P_SHAPE, P_CUTOFF, P_RESO, P_MOTION};
static const uint8_t PAGE_HARM[4] = {P_H_COUNT, P_H_SLOPE, P_H_ODD, P_H_BAND};
static const uint8_t PAGE_TONE[4] = {P_H_FOCUS, P_H_BOOST, P_H_WIDTH, P_H_VEL};
static const uint8_t PAGE_SPACE[4] = {P_ATTACK, P_RELEASE, P_WIDTH, P_LEVEL};
static const uint8_t PAGE_COLOR[4] = {P_TILT, P_SUB, P_SHIMMER, P_SIZE};

typedef struct {
    const char *name;
    uint8_t slot;
    uint8_t ov[24][2];                   /* {parameter + 1, value}, then zeros */
} preset_t;
/*                          STR POS SPR SZ  BRI BND INH ODD ATK REL WID LEV TLT SUB SHM SHP CUT RES MOT */
/* a preset: its source, and the parameters it changes from PDEFAULT (O(parameter, value); index + 1, so a
 * zero ends the list) */
#define O(p, v) {(uint8_t)((p) + 1), (uint8_t)(v)}
static const preset_t PRESETS[] = {
    {"GLACIER", SLOT_CHOIR, {O(P_RELEASE, 88), O(P_WIDTH, 100)}},
    {"FOG", SLOT_CHOIR, {O(P_STRETCH, 80), O(P_POSITION, 30), O(P_SPREAD, 70), O(P_ATTACK, 90), O(P_RELEASE, 96), O(P_TILT, 44), O(P_SUB, 40)}},
    {"HALO", SLOT_PAD, {O(P_BRIGHT, 80), O(P_BAND, 40), O(P_ATTACK, 85), O(P_RELEASE, 92), O(P_LEVEL, 90), O(P_SHIMMER, 50)}},
    {"ORGAN DUST", SLOT_PAD, {O(P_SIZE, 2), O(P_BRIGHT, 50), O(P_BAND, 18), O(P_ODD, 100), O(P_ATTACK, 50), O(P_RELEASE, 70), O(P_WIDTH, 80), O(P_SUB, 30)}},
    {"BELL CLOUD", SLOT_PAD, {O(P_BRIGHT, 90), O(P_BAND, 30), O(P_INHARM, 90), O(P_ATTACK, 60), O(P_RELEASE, 96), O(P_LEVEL, 90), O(P_TILT, 70), O(P_SHIMMER, 20)}},
    {"ANALOG MIST", SLOT_VA, {O(P_SUB, 30), O(P_CUTOFF, 72), O(P_MOTION, 45)}},
    {"RESO SWEEP", SLOT_VA, {O(P_SIZE, 2), O(P_ATTACK, 60), O(P_RELEASE, 85), O(P_WIDTH, 100), O(P_LEVEL, 90), O(P_SHAPE, 64), O(P_CUTOFF, 60), O(P_RESO, 90), O(P_MOTION, 90)}},
    {"HARM BLOOM", SLOT_HARM, {O(P_H_COUNT, 31), O(P_H_SLOPE, 30), O(P_HENV(0, 0), SMG_HT_HIGH), O(P_HENV(0, 1), 105),
                               O(P_HENV(0, 3), 127), O(P_HENV(1, 0), 1), O(P_HENV(1, 1), 20), O(P_HENV(1, 2), 95),
                               O(P_HENV(1, 3), 70), O(P_LFO(0, 2), 4), O(P_LFO(0, 3), 80), O(P_ATTACK, 70)}},
    {"HARM BELL", SLOT_HARM, {O(P_H_COUNT, 23), O(P_H_SLOPE, 25), O(P_H_FOCUS, 4), O(P_H_BOOST, 100), O(P_H_WIDTH, 15),
                              O(P_HENV(0, 0), SMG_HT_HIGH), O(P_HENV(0, 1), 5), O(P_HENV(0, 2), 70), O(P_HENV(0, 3), 0),
                              O(P_HENV(1, 0), 3), O(P_HENV(1, 1), 5), O(P_HENV(1, 2), 90), O(P_HENV(1, 3), 20),
                              O(P_ATTACK, 10), O(P_RELEASE, 100)}},
    {"HARM VOWELS", SLOT_HARM, {O(P_H_COUNT, 40), O(P_H_SLOPE, 35), O(P_H_FOCUS, 6), O(P_H_BOOST, 110),
                                O(P_H_WIDTH, 25), O(P_LFO(0, 0), 25), O(P_LFO(0, 1), LFO_TRI), O(P_LFO(0, 2), 18),
                                O(P_LFO(0, 3), 110), O(P_LFO(1, 2), 19), O(P_LFO(1, 3), 84)}},
    {"HARM HOLLOW", SLOT_HARM, {O(P_H_COUNT, 31), O(P_H_SLOPE, 45), O(P_H_ODD, 127), O(P_H_BAND, 50),
                                O(P_HENV(0, 0), SMG_HT_HIGH), O(P_HENV(0, 1), 110), O(P_HENV(0, 3), 100), O(P_SUB, 40)}},
    {"DRIFT FOG", SLOT_CHOIR, {O(P_STRETCH, 80), O(P_SPREAD, 40), O(P_LFO(0, 0), 10), O(P_LFO(0, 1), LFO_DRIFT),
                               O(P_LFO(0, 2), 2), O(P_LFO(0, 3), 100), O(P_LFO(1, 0), 30), O(P_LFO(1, 2), 4),
                               O(P_LFO(1, 3), 90)}},
    {"GLASS RAIN", SLOT_GLASS, {O(P_STRETCH, 60), O(P_SPREAD, 10), O(P_ATTACK, 70), O(P_RELEASE, 96), O(P_WIDTH, 120), O(P_LEVEL, 90), O(P_TILT, 70), O(P_SHIMMER, 40)}},
    {"HARP DREAM", SLOT_STRING, {O(P_STRETCH, 45), O(P_ATTACK, 60), O(P_WIDTH, 120), O(P_SUB, 20)}},
    {"WIND TUNNEL", SLOT_WIND, {O(P_STRETCH, 70), O(P_SPREAD, 50), O(P_ATTACK, 90), O(P_RELEASE, 96), O(P_WIDTH, 127), O(P_LEVEL, 100), O(P_TILT, 50)}},
    {"FELT DREAM", SLOT_PIANO, {O(P_STRETCH, 58), O(P_ATTACK, 70), O(P_RELEASE, 96), O(P_TILT, 56), O(P_SUB, 20)}},
    {"CELLO SEA", SLOT_BOWED, {O(P_STRETCH, 64), O(P_SPREAD, 20), O(P_ATTACK, 90), O(P_RELEASE, 100), O(P_WIDTH, 120), O(P_TILT, 60), O(P_SUB, 30)}},
    {"TANPURA", SLOT_DRONE, {O(P_STRETCH, 50), O(P_ATTACK, 60), O(P_RELEASE, 100), O(P_TILT, 46), O(P_SUB, 20)}},
    {"RAIN ROOM", SLOT_RAIN, {O(P_STRETCH, 75), O(P_POSITION, 10), O(P_SPREAD, 40), O(P_RELEASE, 96), O(P_WIDTH, 127), O(P_TILT, 50)}},
    {"DAWN", SLOT_BIRDS, {O(P_STRETCH, 70), O(P_SPREAD, 30), O(P_ATTACK, 90), O(P_RELEASE, 100), O(P_WIDTH, 127), O(P_LEVEL, 90), O(P_TILT, 60), O(P_SHIMMER, 30)}},
    {"TAPE MEMORY", SLOT_TAPE, {O(P_STRETCH, 60), O(P_SPREAD, 10), O(P_RELEASE, 96), O(P_WIDTH, 100), O(P_TILT, 52), O(P_SUB, 30)}},
    {"GRAIN", SLOT_CHOIR, {O(P_STRETCH, 30), O(P_POSITION, 20), O(P_SPREAD, 20), O(P_SIZE, 0), O(P_ATTACK, 20), O(P_RELEASE, 60), O(P_WIDTH, 70)}},
    {"FROZEN", SLOT_CHOIR, {O(P_STRETCH, 127), O(P_POSITION, 64), O(P_SPREAD, 90), O(P_ATTACK, 100), O(P_RELEASE, 110), O(P_WIDTH, 127), O(P_TILT, 50), O(P_SUB, 20), O(P_SHIMMER, 30)}},
    {"DEEP", SLOT_PAD, {O(P_BRIGHT, 25), O(P_BAND, 80), O(P_ODD, 30), O(P_ATTACK, 95), O(P_RELEASE, 105), O(P_WIDTH, 100), O(P_LEVEL, 100), O(P_TILT, 40), O(P_SUB, 80)}},
};
#define NPRESET (sizeof PRESETS / sizeof PRESETS[0])

#define SET_MAGIC 0x33474D53u            /* "SMG3": the settings record; another magic loads a preset */
typedef struct {
    uint32_t magic;
    uint8_t v[NP];
    uint8_t slot, preset;
    int8_t octave;
    uint8_t rsv[3];
} settings_t;
static settings_t set;
static uint8_t set_dirty;

static void ui_dirty_params(void);

/* ------------------------------------------------------------------ engine --- */
static smg_t eng;
static float eng_ram[SMG_RAM_FLOATS] PLAT_POOL;
static va_t va;                          /* the VA source: rendered into the engine's LIVE ring */
static volatile uint8_t va_on;
static uint8_t freeze, hold;

/* the knobs' mappings, from a knob's value (0 .. 127, a fraction when modulated) */
static float unit(float v) { return v / 127.0f; }
static float stretch_of(float v) { return v >= 126.5f ? 0.0f : fm_exp2f(v * (13.0f / 126.0f)); }   /* 1x .. INF */
static float attack_of(float v) { return 0.01f * fm_exp2f(v * (11.0f / 127.0f)); }     /* 10 ms .. 20 s */
static float release_of(float v) { return 0.05f * fm_exp2f(v * (9.25f / 127.0f)); }    /* 50 ms .. 30 s */
static float band_of(float v) { float x = unit(v); return 1.0f + 299.0f * x * x; }    /* cents */
static float hband_of(float v) { float x = unit(v); return 1.0f + 149.0f * x * x; }   /* cents */
static float bipolar(float v) { return fm_clampf((v - 64.0f) / 63.0f, -1.0f, 1.0f); }  /* 64 = 0 */
static float lfo_hz(float v) { return 0.01f * fm_exp2f(v * (10.0f / 127.0f)); }       /* 0.01 .. 10 Hz */

/* ---------------------------------------------------------------- modulation --- */
/* two LFOs and a modulation envelope, each moving one parameter (DEST) by DEPTH (-100 .. +100 % of its range),
 * worked out once an audio block. The values they give are what the engine plays; the knobs keep theirs. */
static float mod_v[NP];                  /* every parameter's value as played: the knob's plus its modulation */
static struct {
    float ph, out, held, drift, drift_to;
    uint32_t rng;
} lfo[2] = {{0, 0, 0, 0, 0, 0x1234567u}, {0.25f, 0, 0, 0, 0, 0x89ABCDEu}};
static float menv;                       /* the modulation envelope, 0 .. 1 */
static uint8_t menv_up;                  /* rising */
static volatile uint32_t menv_trig;      /* note-ons (main loop and MIDI): each starts the envelope again */
static uint32_t menv_seen;

static float rnd_unit(uint32_t *r)       /* -1 .. 1 */
{
    *r ^= *r << 13;
    *r ^= *r >> 17;
    *r ^= *r << 5;
    return (float)(*r >> 8) / 8388608.0f - 1.0f;
}

static void mod_step(float dt)           /* audio interrupt: dt seconds since the last */
{
    uint32_t k;
    for (k = 0; k < 2u; k++) {
        const uint8_t *v = &set.v[P_LFO(k, 0)];
        float ph = lfo[k].ph + lfo_hz(v[0]) * dt;
        int wrapped = ph >= 1.0f;
        if (wrapped)
            ph -= (float)(uint32_t)ph;
        lfo[k].ph = ph;
        if (wrapped) {
            lfo[k].held = rnd_unit(&lfo[k].rng);
            lfo[k].drift_to = rnd_unit(&lfo[k].rng);
        }
        switch (v[1]) {
        case LFO_SINE: lfo[k].out = fm_sinf(FM_TWO_PI * ph); break;
        case LFO_TRI: lfo[k].out = ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph; break;
        case LFO_SAW_UP: lfo[k].out = 2.0f * ph - 1.0f; break;
        case LFO_SAW_DOWN: lfo[k].out = 1.0f - 2.0f * ph; break;
        case LFO_SQUARE: lfo[k].out = ph < 0.5f ? 1.0f : -1.0f; break;
        case LFO_SH: lfo[k].out = lfo[k].held; break;
        default:                         /* DRIFT: towards a new random point each cycle, smoothly */
            lfo[k].drift += (lfo[k].drift_to - lfo[k].drift) * fm_minf(1.0f, dt * lfo_hz(v[0]) * 4.0f);
            lfo[k].out = lfo[k].drift;
            break;
        }
    }
    if (menv_trig != menv_seen) {        /* a note: the envelope starts again from where it is */
        menv_seen = menv_trig;
        menv_up = 1;
    }
    if (menv_up) {
        menv += dt / attack_of(set.v[P_MENV]);
        if (menv >= 1.0f) {
            menv = 1.0f;
            menv_up = 0;
        }
    } else {
        menv -= dt / release_of(set.v[P_MENV + 1]);
        if (menv < 0.0f)
            menv = 0.0f;
    }
    for (k = 0; k < NP; k++)
        mod_v[k] = (float)set.v[k];
    for (k = 0; k < 3u; k++) {           /* LFO 1, LFO 2, MOD ENV */
        const uint8_t *v = k < 2u ? &set.v[P_LFO(k, 0)] : &set.v[P_MENV];
        uint32_t d = v[2] < NDEST ? MOD_DEST[v[2]] : 0xFFu;
        float amt = bipolar(v[3]) * (k < 2u ? lfo[k].out : menv);
        if (d < NP)
            mod_v[d] = fm_clampf(mod_v[d] + amt * (float)PDEF[d].max, 0.0f, (float)PDEF[d].max);
    }
}

/* the played values into the engine's parameters: the audio interrupt only (the engine's own context), once a block */
static void map_params(void)
{
    smg_params_t *p = &eng.p;
    const float *v = mod_v;
    uint32_t k;
    p->source = set.slot == SLOT_PAD ? SMG_SRC_PAD : set.slot == SLOT_VA ? SMG_SRC_LIVE :
                set.slot == SLOT_HARM ? SMG_SRC_HARM : SMG_SRC_SAMPLE;
    va_on = set.slot == SLOT_VA;
    va.shape = unit(v[P_SHAPE]);
    va.cutoff = unit(v[P_CUTOFF]);
    va.motion = unit(v[P_MOTION]);
    p->lp_hz = va_on ? va_cutoff_hz(va.cutoff) : 0.0f;   /* (the render follows MOTION: va.fc_now) */
    p->lp_reso = unit(v[P_RESO]);
    p->size = set.v[P_SIZE];
    p->freeze = freeze;
    p->stretch = stretch_of(v[P_STRETCH]);
    p->position = unit(v[P_POSITION]);
    p->spread = unit(v[P_SPREAD]);
    p->bright = unit(v[P_BRIGHT]);
    p->bandwidth = band_of(v[P_BAND]);
    p->inharm = unit(v[P_INHARM]);
    p->odd = unit(v[P_ODD]);
    p->attack = attack_of(v[P_ATTACK]);
    p->release = release_of(v[P_RELEASE]);
    p->width = unit(v[P_WIDTH]);
    p->level = 2.0f * unit(v[P_LEVEL]) * unit(v[P_LEVEL]);   /* 96: 1.14, 127: 2 */
    p->tilt = 2.0f * unit(v[P_TILT]) - 1.0f;
    p->sub = unit(v[P_SUB]);
    p->shimmer = unit(v[P_SHIMMER]);
    if (va_on)                           /* VA: its harmonics softened into narrow bands, as PADsynth's are */
        p->spread = 0.22f;
    p->h_count = (uint8_t)(set.v[P_H_COUNT] + 1u);
    p->h_slope = 3.0f * unit(v[P_H_SLOPE]);
    p->h_odd = bipolar(v[P_H_ODD]);
    p->h_band = hband_of(v[P_H_BAND]);
    p->h_focus = v[P_H_FOCUS] + 1.0f;
    p->h_boost = 24.0f * bipolar(v[P_H_BOOST]);
    p->h_width = 0.4f + 5.6f * unit(v[P_H_WIDTH]);
    p->h_vel = unit(v[P_H_VEL]);
    for (k = 0; k < SMG_HENV; k++) {
        p->h_target[k] = set.v[P_HENV(k, 0)];
        p->h_att[k] = attack_of(v[P_HENV(k, 1)]);
        p->h_dec[k] = release_of(v[P_HENV(k, 2)]);
        p->h_sus[k] = unit(v[P_HENV(k, 3)]);
    }
}

/* the main loop's changes: nothing to do here any more (the audio interrupt maps the knobs every block); kept as
 * the one place a change is announced, for the screen */
static void apply(void)
{
    ui_dirty_params();
}

static uint32_t audio_bytes(uint32_t len, uint32_t fmt)
{
    return fmt == SMG_FMT_ADPCM ? SMG_ADPCM_SIZE(len) : fmt == SMG_FMT_PCM16 ? 2u * len : 0u;
}
static int sample_size_ok(uint32_t len, uint32_t rate, uint32_t fmt, uint32_t bytes, uint32_t frames, uint32_t hop)
{
    return len && len <= SMP_MAX_BYTES * 2u && rate >= 4000u && rate <= 96000u && bytes <= SMP_MAX_BYTES &&
           audio_bytes(len, fmt) && bytes == audio_bytes(len, fmt) + frames * SMG_SBR_BANDS && (!frames || hop);
}

/* the loaded sample in the flash, checked: its header, 0 = none */
static const smp_hdr_t *user_sample(void)
{
    const smp_hdr_t *h = (const smp_hdr_t *)plat_data(SMP_HDR_OFF);
    if (!h || h->magic != SMP_MAGIC || !sample_size_ok(h->len, h->rate, h->fmt, h->bytes, h->env_frames, h->env_hop))
        return 0;
    return h;
}

/* --------------------------------------------------- main loop -> audio --- */
/* key notes and source changes go to the audio interrupt through a ring (the engine is the interrupt's) */
enum { Q_ON, Q_OFF, Q_ALLOFF, Q_PANIC, Q_SOURCE };
static volatile uint32_t evq[64];
static volatile uint32_t ev_w, ev_r;
static void ev_push(uint32_t type, uint32_t a, uint32_t b)
{
    uint32_t w = ev_w;
    if (w - ev_r >= 64u)
        return;                          /* full: dropped (64 events within one 5.8 ms block) */
    evq[w & 63u] = type << 24 | (a & 0xFFFFu) << 8 | (b & 0xFFu);
    __asm__ volatile("" ::: "memory");   /* the event before the index */
    ev_w = w + 1u;
}
static uint8_t uploading;                /* a sample is being written: USER plays the built-in meanwhile */
static void source_set(uint32_t slot)    /* audio interrupt: a sample source's sample (PAD and VA keep the last) */
{
    const smp_hdr_t *h = slot == SLOT_USER && !uploading ? user_sample() : 0;
    const smg_builtin_t *b = &smg_builtin[slot >= SLOT_SMP0 && slot < SLOT_USER ? slot - SLOT_SMP0 : 0u];
    if (h) {
        smg_set_sample(&eng, plat_data(SMP_DATA_OFF), h->len, h->rate, h->fmt);
        smg_set_env(&eng, plat_data(SMP_DATA_OFF) + audio_bytes(h->len, h->fmt), h->env_frames, h->env_hop);
    } else if (slot_sample(slot) || !eng.smp.len) {
        smg_set_sample(&eng, b->data, b->len, b->rate, SMG_FMT_ADPCM);
        smg_set_env(&eng, b->env, b->env_frames, b->env_hop);
    }
}

/* ---------------------------------------------------------------- MIDI in --- */
static volatile int16_t cc_in[3] = {-1, -1, -1};   /* CC1, CC71, CC74 from the interrupt, for the main loop */
static uint32_t sus_on, sus_notes[4];    /* CC64: note-offs held back, by note */
static volatile uint32_t midi_notes_in;
static void midi_in(void)                /* audio interrupt */
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24 & 0x7Fu;
        if ((st & 0xF0u) == 0x90u && d2) {
            smg_note_on(&eng, (int32_t)d1, (float)d2 / 127.0f);
            menv_trig++;
            sus_notes[d1 >> 5] &= ~(1u << (d1 & 31u));
            midi_notes_in++;
        } else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) {
            if (sus_on)
                sus_notes[d1 >> 5] |= 1u << (d1 & 31u);
            else
                smg_note_off(&eng, (int32_t)d1);
        } else if ((st & 0xF0u) == 0xB0u) {
            if (d1 == 64u) {
                sus_on = d2 >= 64u;
                if (!sus_on) {
                    uint32_t k;
                    for (k = 0; k < 128u; k++)
                        if (sus_notes[k >> 5] >> (k & 31u) & 1u)
                            smg_note_off(&eng, (int32_t)k);
                    sus_notes[0] = sus_notes[1] = sus_notes[2] = sus_notes[3] = 0;
                }
            } else if (d1 == 1u) {
                cc_in[0] = (int16_t)d2;
            } else if (d1 == 71u) {
                cc_in[1] = (int16_t)d2;
            } else if (d1 == 74u) {
                cc_in[2] = (int16_t)d2;
            } else if (d1 == 120u || d1 == 123u) {   /* all sound off, all notes off */
                smg_all_off(&eng);
            }
        }
    }
}

/* ------------------------------------------------------------------ audio --- */
static float gain_now;
static float buf_l[APP_BLOCK], buf_r[APP_BLOCK];
void app_render(int32_t *lr, uint32_t n, uint32_t master_q12)
{
    float gain = (float)master_q12 / 4096.0f * 8300000.0f;   /* full scale: 24-bit */
    uint32_t r = ev_r, i;
    while (r != ev_w) {
        uint32_t e = evq[r & 63u], a = e >> 8 & 0xFFFFu, b = e & 0xFFu;
        switch (e >> 24) {
        case Q_ON: smg_note_on(&eng, (int32_t)a, (float)b / 127.0f); break;
        case Q_OFF: smg_note_off(&eng, (int32_t)a); break;
        case Q_ALLOFF: smg_all_off(&eng); break;
        case Q_PANIC: smg_panic(&eng); break;
        case Q_SOURCE: source_set(a); break;
        default: break;
        }
        r++;
    }
    ev_r = r;
    midi_in();
    while (n) {
        uint32_t m = n < APP_BLOCK ? n : APP_BLOCK;
        mod_step((float)m / 44100.0f);
        map_params();
        if (va_on) {                     /* VA: its newest block into the LIVE ring first */
            va_render(&va, buf_l, m);
            smg_live_push(&eng, buf_l, m);
            eng.p.lp_hz = va.fc_now;
        }
        smg_render(&eng, buf_l, buf_r, m);
        for (i = 0; i < m; i++) {
            gain_now += (gain - gain_now) * 0.004f;          /* turning MASTER does not click */
            lr[2u * i] = (int32_t)(buf_l[i] * gain_now);
            lr[2u * i + 1u] = (int32_t)(buf_r[i] * gain_now);
        }
        lr += 2u * m;
        n -= m;
    }
}

void app_panic(void)
{
    smg_panic(&eng);
}

/* --------------------------------------------------------------- settings --- */
static void preset_load(uint32_t i)
{
    uint32_t k;
    const preset_t *p = &PRESETS[i % NPRESET];
    for (k = 0; k < NP; k++)
        set.v[k] = PDEFAULT[k];
    for (k = 0; k < 24u && p->ov[k][0]; k++)
        set.v[p->ov[k][0] - 1u] = p->ov[k][1];
    set.preset = (uint8_t)(i % NPRESET);
    set.slot = p->slot;
}

static int settings_ok(void)
{
    uint32_t k;
    if (set.magic != SET_MAGIC || set.slot >= NSLOT || set.octave < -2 || set.octave > 2 || set.preset >= NPRESET)
        return 0;
    for (k = 0; k < NP; k++)
        if (set.v[k] > PDEF[k].max)
            return 0;
    return 1;
}

void app_init(void)
{
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || !settings_ok()) {
        set.magic = SET_MAGIC;
        set.octave = 0;
        preset_load(0);
    }
    if (set.slot == SLOT_USER && !user_sample())
        set.slot = SLOT_CHOIR;
    smg_init(&eng, eng_ram);
    va_init(&va);
    mod_step(0.0f);
    map_params();
    source_set(set.slot);
}

/* ------------------------------------------------------------- sample load --- */
/* SysEx F0 7D 53 4D cmd ... F7 (tools/smuga_load.py); every command answered F0 7D 53 4D 7F cmd status F7.
 *   01 BEGIN  len (samples, 4 x 7 bits), rate (3 x 7 bits), format (SMG_FMT_*: 0 PCM16, 1 ADPCM), bytes (4 x 7
 *             bits: the samples' 2 len or SMG_ADPCM_SIZE(len), plus the envelope's), the SBR envelope's frames (3 x 7
 *             bits) and hop (2 x 7 bits; 0 0: none), name (up to 15 ASCII)
 *   02 DATA   byte offset (4 x 7 bits), then the bytes packed 7 in 8 (the first of each 8 holds the next seven's top
 *             bits); in order, at most 512 bytes a message
 *   03 END    CRC-32 of the bytes (5 x 7 bits): checked against the flash, then the header written
 * Status: 0 ok, 1 not now, 2 too long, 3 out of order, 4 the flash refused, 5 CRC mismatch */
static struct {
    uint32_t len, rate, fmt, bytes, next;   /* samples; Hz; SMG_FMT_*; the data's size; the next byte expected */
    uint32_t env_frames, env_hop;
    char name[16];
    uint8_t buf[512];
} up;

static uint32_t rd7(const uint8_t *p, uint32_t n)
{
    uint32_t v = 0, k;
    for (k = 0; k < n; k++)
        v |= (uint32_t)(p[k] & 0x7Fu) << (7u * k);
    return v;
}

static uint32_t crc32_add(uint32_t c, const uint8_t *p, uint32_t n)   /* zlib's, bit by bit (once per load) */
{
    uint32_t k;
    c = ~c;
    while (n--) {
        c ^= *p++;
        for (k = 0; k < 8u; k++)
            c = c & 1u ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}

static uint32_t up_errors;              /* answers other than ok (the host simulator's check) */
static void up_answer(uint32_t cmd, uint32_t status)
{
    up_errors += status != 0;
    uint8_t f[8] = {0xF0, 0x7D, 0x53, 0x4D, 0x7F, (uint8_t)cmd, (uint8_t)status, 0xF7};
    plat_sysex_send(f, sizeof f);
}

static uint32_t up_begin(const uint8_t *p, uint32_t n)
{
    uint32_t k;
    if (n < 21u)
        return 1;
    up.len = rd7(p + 4, 4);
    up.rate = rd7(p + 8, 3);
    up.fmt = p[11];
    up.bytes = rd7(p + 12, 4);
    up.env_frames = rd7(p + 16, 3);
    up.env_hop = rd7(p + 19, 2);
    if (!sample_size_ok(up.len, up.rate, up.fmt, up.bytes, up.env_frames, up.env_hop))
        return 2;
    for (k = 0; k < 15u && 21u + k < n; k++)
        up.name[k] = (char)p[21u + k];
    up.name[k] = 0;
    uploading = 1;
    if (set.slot == SLOT_USER)
        ev_push(Q_SOURCE, SLOT_USER, 0);   /* (plays the built-in while uploading) */
    up.next = 0;
    if (plat_data_erase(SMP_HDR_OFF))    /* a load cut short leaves no header: nothing half-made plays */
        return 4;
    app_say("LOADING", up.name);
    return 0;
}

static uint32_t up_data(const uint8_t *p, uint32_t n)
{
    uint32_t off, m = 0, k;
    if (!uploading || n < 8u)
        return 1;
    off = rd7(p + 4, 4);
    if (off != up.next)
        return 3;
    for (k = 8; k < n && m < sizeof up.buf;) {      /* 7 in 8 */
        uint32_t top = p[k++], b;
        for (b = 0; b < 7u && k < n && m < sizeof up.buf; b++)
            up.buf[m++] = (uint8_t)(p[k++] | ((top >> b) & 1u) << 7);
    }
    if (off + m > up.bytes)
        return 2;
    for (k = 0; k < m;) {                /* erase each sector as the data reaches it, then write */
        uint32_t at = SMP_DATA_OFF + off + k, room = 0x1000u - (at & 0xFFFu), w = m - k < room ? m - k : room;
        if ((at & 0xFFFu) == 0 && plat_data_erase(at))
            return 4;
        if (plat_data_write(at, up.buf + k, w))
            return 4;
        k += w;
    }
    up.next = off + m;
    return 0;
}

static uint32_t up_end(const uint8_t *p, uint32_t n)
{
    smp_hdr_t h;
    uint32_t want, k;
    const uint8_t *d = plat_data(SMP_DATA_OFF);
    if (!uploading || n < 9u || up.next != up.bytes)
        return 1;
    want = rd7(p + 4, 5);
    uploading = 0;
    if (!d || crc32_add(0, d, up.bytes) != want) {
        app_say("LOAD", "FAILED");
        return 5;
    }
    h.magic = SMP_MAGIC;
    h.rate = up.rate;
    h.len = up.len;
    h.crc = want;
    h.fmt = up.fmt;
    h.bytes = up.bytes;
    h.env_frames = up.env_frames;
    h.env_hop = up.env_hop;
    for (k = 0; k < 16u; k++)
        h.name[k] = up.name[k];
    if (plat_data_write(SMP_HDR_OFF, &h, sizeof h))
        return 4;
    set.slot = SLOT_USER;                /* the new sample, playing */
    set_dirty = 1;
    apply();
    ev_push(Q_SOURCE, SLOT_USER, 0);
    app_say("LOADED", up.name);
    return 0;
}

int app_sysex(const uint8_t *p, uint32_t n)
{
    uint32_t st;
    if (n < 4u || p[0] != 0x7Du || p[1] != 0x53u || p[2] != 0x4Du)
        return 0;
    switch (p[3]) {
    case 1: st = up_begin(p, n); break;
    case 2: st = up_data(p, n); break;
    case 3: st = up_end(p, n); break;
    default: st = 1; break;
    }
    if (st && p[3] != 2u)
        uploading = 0;
    up_answer(p[3], st);
    return 1;
}

/* --------------------------------------------------------------------- UI --- */
static struct {
    char msg[2][24];
    uint32_t msg_until;
    uint32_t keys, btn;
    uint32_t latched[4];                 /* HOLD: the notes the keys left on */
    uint8_t page;
    uint8_t redraw, redraw_spec;
    uint32_t touched_at;                 /* a knob turned: its value shown big for a moment */
    uint8_t touched;
} ui;
static void ui_dirty_params(void) { ui.redraw = 1; }

static void text_cat(char *d, const char *s)
{
    while (*d)
        d++;
    while (*s)
        *d++ = *s++;
    *d = 0;
}

static void int_text(int32_t v, char *b)
{
    char t[12];
    int n = 0;
    uint32_t u = (uint32_t)(v < 0 ? -v : v);
    do
        t[n++] = (char)('0' + u % 10u);
    while ((u /= 10u) && n < 11);
    if (v < 0)
        *b++ = '-';
    while (n)
        *b++ = t[--n];
    *b = 0;
}

static void tenths_text(float x, char *b)   /* 1 decimal under 10, none above: 0.4, 3.2, 12 */
{
    int32_t t = (int32_t)(x * 10.0f + 0.5f);
    char d[2];
    if (t >= 100) {
        int_text((t + 5) / 10, b);
        return;
    }
    int_text(t / 10, b);
    d[0] = (char)('0' + t % 10);
    d[1] = 0;
    text_cat(b, ".");
    text_cat(b, d);
}

static void harm_target_text(uint32_t t, char *b)
{
    static const char *const G[] = {"ODD", "EVEN", "LOW 1-4", "MID 5-12", "HIGH 13+", "ALL"};
    b[0] = 0;
    if (t == SMG_HT_OFF)
        text_cat(b, "OFF");
    else if (t <= SMG_HT_H32) {
        text_cat(b, "H");
        int_text((int32_t)t, b + 1);
    } else
        text_cat(b, G[(t - SMG_HT_ODD) % 6u]);
}

/* a parameter's value as the screen shows it */
static void value_text(uint32_t p, char *b)
{
    uint32_t v = set.v[p];
    b[0] = 0;
    switch (p) {
    case P_STRETCH:
        if (v >= 127u) {
            text_cat(b, "INF");
        } else {
            float s = stretch_of((float)v);
            if (s < 10.0f)
                tenths_text(s, b);
            else
                int_text((int32_t)(s + 0.5f), b);
            text_cat(b, "X");
        }
        break;
    case P_SIZE: {
        static const char *const MS[SMG_NSIZE] = {"23 MS", "46 MS", "93 MS", "186 MS"};
        text_cat(b, MS[v]);
        break;
    }
    case P_ATTACK:
    case P_RELEASE:
        tenths_text(p == P_ATTACK ? attack_of((float)v) : release_of((float)v), b);
        text_cat(b, " S");
        break;
    case P_SHAPE:
        if (v == 0u)
            text_cat(b, "SAW");
        else if (v < 64u)
            text_cat(b, "SAW-SQUARE");
        else if (v == 64u)
            text_cat(b, "SQUARE");
        else {
            int_text((int32_t)(50.0f - (unit((float)v) - 0.5f) * 80.0f + 0.5f), b);   /* va.c: 50 % .. 10 % */
            text_cat(b, "% PULSE");
        }
        break;
    case P_CUTOFF: {
        float hz = va_cutoff_hz(unit((float)v));
        if (hz < 1000.0f) {
            int_text((int32_t)(hz + 0.5f), b);
            text_cat(b, " HZ");
        } else {
            tenths_text(hz / 1000.0f, b);
            text_cat(b, " KHZ");
        }
        break;
    }
    case P_BAND:
    case P_H_BAND:
        int_text((int32_t)((p == P_BAND ? band_of((float)v) : hband_of((float)v)) + 0.5f), b);
        text_cat(b, " CT");
        break;
    case P_H_COUNT:
        int_text((int32_t)v + 1, b);
        break;
    case P_H_SLOPE:                      /* 1 / h^slope: 6 dB an octave for each 1 */
        text_cat(b, "-");
        tenths_text(3.0f * unit((float)v) * 6.02f, b + 1);
        text_cat(b, " DB/OCT");
        break;
    case P_H_ODD: {
        int32_t pc = (int32_t)(bipolar((float)v) * 100.0f + (v >= 64u ? 0.5f : -0.5f));
        char t[8];
        if (pc == 0) {
            text_cat(b, "BOTH");
            break;
        }
        int_text(pc < 0 ? -pc : pc, t);
        text_cat(b, pc < 0 ? "EVEN " : "ODD ");
        text_cat(b, t);
        text_cat(b, "%");
        break;
    }
    case P_H_FOCUS:
        text_cat(b, "H");
        int_text((int32_t)v + 1, b + 1);
        break;
    case P_H_BOOST: {
        int32_t db = (int32_t)(24.0f * bipolar((float)v) + (v >= 64u ? 0.5f : -0.5f));
        if (db > 0)
            text_cat(b, "+");
        int_text(db, b + (db > 0));
        text_cat(b, " DB");
        break;
    }
    case P_H_WIDTH:
        tenths_text(0.4f + 5.6f * unit((float)v), b);
        text_cat(b, " H");
        break;
    case P_TILT: {
        int32_t db = (int32_t)((2.0f * unit((float)v) - 1.0f) * 60.0f);   /* tenths of dB per octave */
        char t[12];
        if (db > 0)
            text_cat(b, "+");
        if (db < 0) {
            text_cat(b, "-");
            db = -db;
        }
        int_text(db / 10, t);
        text_cat(b, t);
        text_cat(b, ".");
        t[0] = (char)('0' + db % 10);
        t[1] = 0;
        text_cat(b, t);
        text_cat(b, " DB");
        break;
    }
    default:
        if (p >= P_HENV0 && p < P_LFO0) {           /* ENV A-D */
            uint32_t i = (p - P_HENV0) % 4u;
            if (i == 0u)
                harm_target_text(v, b);
            else if (i == 3u) {
                int_text((int32_t)(unit((float)v) * 100.0f + 0.5f), b);
                text_cat(b, "%");
            } else {
                tenths_text(i == 1u ? attack_of((float)v) : release_of((float)v), b);
                text_cat(b, " S");
            }
        } else if (p >= P_LFO0) {                   /* LFO 1, LFO 2, MOD ENV */
            uint32_t i = (p - P_LFO0) % 4u;
            int menv_page = p >= P_MENV;
            if (i == 2u) {
                text_cat(b, v == 0u || v >= NDEST ? "OFF" : PDEF[MOD_DEST[v]].name);
            } else if (i == 3u) {
                int32_t pc = (int32_t)(bipolar((float)v) * 100.0f + (v >= 64u ? 0.5f : -0.5f));
                if (pc > 0)
                    text_cat(b, "+");
                int_text(pc, b + (pc > 0));
                text_cat(b, "%");
            } else if (menv_page) {
                tenths_text(i == 0u ? attack_of((float)v) : release_of((float)v), b);
                text_cat(b, " S");
            } else if (i == 1u) {
                text_cat(b, LFO_SHAPE_NAME[v < NLFO_SHAPE ? v : 0u]);
            } else {
                float hz = lfo_hz((float)v);
                if (hz < 1.0f) {
                    tenths_text(1.0f / hz, b);
                    text_cat(b, " S");
                } else {
                    tenths_text(hz, b);
                    text_cat(b, " HZ");
                }
            }
        } else {
            int_text((int32_t)(unit((float)v) * 100.0f + 0.5f), b);
            text_cat(b, "%");
        }
        break;
    }
}

static int page_ok(uint32_t pg)          /* TONE and ENV A-D are HARM's */
{
    return set.slot == SLOT_HARM || pg < PG_TONE || pg > PG_ENVD;
}

static const uint8_t *page_params(void)
{
    static uint8_t pp[4];
    uint32_t k;
    switch (ui.page) {
    case PG_TONE: return PAGE_TONE;
    case PG_SPACE: return PAGE_SPACE;
    case PG_COLOR: return PAGE_COLOR;
    case PG_ENVA: case PG_ENVB: case PG_ENVC: case PG_ENVD:
        for (k = 0; k < 4u; k++)
            pp[k] = (uint8_t)P_HENV(ui.page - PG_ENVA, k);
        return pp;
    case PG_LFO1: case PG_LFO2: case PG_MENV:
        for (k = 0; k < 4u; k++)
            pp[k] = (uint8_t)(ui.page == PG_MENV ? P_MENV + k : P_LFO(ui.page - PG_LFO1, k));
        return pp;
    default:
        return set.slot == SLOT_PAD ? PAGE_PAD : set.slot == SLOT_VA ? PAGE_VA : set.slot == SLOT_HARM ? PAGE_HARM :
               PAGE_SAMPLE;
    }
}

void app_say(const char *a, const char *b)
{
    uint32_t i;
    char t[48];
    for (i = 0; i < 23u && a && a[i]; i++)
        ui.msg[0][i] = a[i];
    ui.msg[0][i] = 0;
    for (i = 0; i < 23u && b && b[i]; i++)
        ui.msg[1][i] = b[i];
    ui.msg[1][i] = 0;
    ui.msg_until = plat_ms() + 1300u;
    ui.redraw = 1;
    if (ann_on()) {                      /* and to a screen reader (announce.c) */
        t[0] = 0;
        text_cat(t, ui.msg[0]);
        if (ui.msg[1][0]) {
            text_cat(t, " ");
            text_cat(t, ui.msg[1]);
        }
        ann_say(t, 0, ANN_NOW);
    }
}

static void speak(void)                  /* a screen reader that has just started listening hears the screen */
{
    char t[120], v[16];
    uint32_t k;
    const uint8_t *pp = page_params();
    if (ann_on() && ann_where()) {
        t[0] = 0;
        text_cat(t, APP_NAME ". ");
        text_cat(t, slot_name(set.slot));
        text_cat(t, ". ");
        text_cat(t, PRESETS[set.preset].name);
        text_cat(t, ". ");
        text_cat(t, PAGE_NAME[ui.page]);
        for (k = 0; k < 4u; k++) {
            text_cat(t, ". ");
            text_cat(t, PDEF[pp[k]].name);
            text_cat(t, " ");
            value_text(pp[k], v);
            text_cat(t, v);
        }
        ann_say(t, 0, ANN_NOW);
    }
    ann_tick(plat_ms());
}

static void param_say(uint32_t p)
{
    char v[16];
    value_text(p, v);
    app_say(PDEF[p].name, v);
}

static void param_turn(uint32_t p, int32_t d)
{
    int32_t v = clamp((int32_t)set.v[p] + d, 0, PDEF[p].max);
    if (v == set.v[p])
        return;
    set.v[p] = (uint8_t)v;
    set_dirty = 1;
    apply();
    param_say(p);
}

static void slot_set(uint32_t slot)
{
    if (slot == SLOT_USER && !user_sample()) {
        app_say("USER", "NO SAMPLE LOADED");
        return;
    }
    set.slot = (uint8_t)slot;
    set_dirty = 1;
    if (!page_ok(ui.page))
        ui.page = PG_SOURCE;
    apply();
    ev_push(Q_SOURCE, slot, 0);
    if (slot == SLOT_USER)
        app_say("SOURCE", ((const smp_hdr_t *)plat_data(SMP_HDR_OFF))->name);
    else
        app_say("SOURCE", slot_name(slot));
}

static int key_note(uint32_t k) { return 53 + (int)k + 12 * set.octave; }   /* key 0 is F3 */

static void hold_release(void)           /* the latched notes the keys do not hold, released */
{
    uint32_t k, held[4] = {0, 0, 0, 0};
    for (k = 0; k < NKEYS; k++)
        if (ui.keys >> k & 1u) {
            uint32_t note = (uint32_t)key_note(k);
            held[note >> 5] |= 1u << (note & 31u);
        }
    for (k = 0; k < 128u; k++)
        if ((ui.latched[k >> 5] & ~held[k >> 5]) >> (k & 31u) & 1u) {
            ev_push(Q_OFF, k, 0);
            plat_midi_send(0x08u | 0x80u << 8 | k << 16, TO_USB);
        }
    ui.latched[0] = ui.latched[1] = ui.latched[2] = ui.latched[3] = 0;
}

static void octave_step(int32_t d)
{
    int32_t o = clamp(set.octave + d, -2, 2);
    char t[4];
    if (o == set.octave)
        return;
    if (!hold) {                         /* a key held across the change would not find its note-off */
        uint32_t k;
        for (k = 0; k < NKEYS; k++)
            if (ui.keys >> k & 1u)
                ev_push(Q_OFF, (uint32_t)key_note(k), 0);
    }
    set.octave = (int8_t)o;
    set_dirty = 1;
    t[0] = o < 0 ? '-' : '+';
    t[1] = (char)('0' + (o < 0 ? -o : o));
    t[2] = 0;
    app_say("OCTAVE", t);
}

static void page_set(uint32_t pg)
{
    ui.page = (uint8_t)(pg % NPAGE);
    ui.redraw = 1;
    app_say("PAGE", PAGE_NAME[ui.page]);
}

static void page_step(int32_t d)         /* the next page this source has, d pages on */
{
    uint32_t pg = ui.page, n = d < 0 ? (uint32_t)-d : (uint32_t)d;
    while (n--)
        do
            pg = (pg + (d > 0 ? 1u : NPAGE - 1u)) % NPAGE;
        while (!page_ok(pg));
    page_set(pg);
}

/* a page button: the first of its pages, then the next of them at each press */
static void page_button(uint32_t first, uint32_t last)
{
    uint32_t pg = ui.page >= first && ui.page < last ? ui.page + 1u : first;
    while (pg <= last && !page_ok(pg))
        pg++;
    page_set(pg > last || !page_ok(pg) ? first : pg);
}

static void input(void)
{
    uint32_t keys, btn = plat_buttons(), k;
    int32_t e;
    touch_poll();
    keys = touch_keys();
    for (k = 0; k < NKEYS; k++) {
        uint32_t bit = 1u << k, note = (uint32_t)key_note(k);
        if ((keys & bit) && !(ui.keys & bit)) {
            uint32_t v = touch_vel(k);
            if (hold) {
                if (!(ui.keys & keys & ~bit))    /* the first key of a new chord: the old one goes */
                    hold_release();
                ui.latched[note >> 5] |= 1u << (note & 31u);
            }
            ev_push(Q_ON, note, v ? v : 100u);
            menv_trig++;
            plat_midi_send(0x09u | 0x90u << 8 | note << 16 | (v ? v : 100u) << 24, TO_USB);
        } else if (!(keys & bit) && (ui.keys & bit) && !hold) {
            ev_push(Q_OFF, note, 0);
            plat_midi_send(0x08u | 0x80u << 8 | note << 16, TO_USB);
        }
    }
    for (k = 0; k < NB; k++) {
        uint32_t bit = 1u << k;
        if (!(btn & bit) || (ui.btn & bit))
            continue;
        switch (k) {
        case B_OCTDN: octave_step(-1); break;
        case B_OCTUP: octave_step(1); break;
        case B_SEL: page_step(1); break;
        case B_EDIT: page_button(PG_SOURCE, PG_TONE); break;
        case B_ENV:                      /* SPACE, then (HARM) ENV A-D */
            if (set.slot == SLOT_HARM && (ui.page == PG_SPACE || (ui.page >= PG_ENVA && ui.page < PG_ENVD)))
                page_set(ui.page == PG_SPACE ? PG_ENVA : ui.page + 1u);
            else
                page_set(PG_SPACE);
            break;
        case B_LFO: page_button(PG_LFO1, PG_MENV); break;
        case B_FX: page_set(PG_COLOR); break;
        case B_PLAY:
            hold = (uint8_t)!hold;
            if (!hold)
                hold_release();
            app_say("HOLD", hold ? "ON" : "OFF");
            break;
        case B_REC:
            freeze = (uint8_t)!freeze;
            apply();
            app_say("FREEZE", freeze ? "ON" : "OFF");
            break;
        case B_HOME:
            hold = 0;
            ui.latched[0] = ui.latched[1] = ui.latched[2] = ui.latched[3] = 0;
            ev_push(Q_ALLOFF, 0, 0);
            app_say("ALL NOTES", "OFF");
            break;
        case B_SAVE:
            if (plat_store_save(OBJ_SETTINGS, &set, sizeof set) == 0) {
                set_dirty = 0;
                app_say("SAVED", "");
            } else {
                app_say("SAVE", "FAILED");
            }
            break;
        default: break;
        }
    }
    if ((e = plat_enc(EN_SELECT)) != 0)
        page_step(e);
    if ((e = plat_enc(EN_ALGO)) != 0) {
        int32_t s = set.slot, i;
        for (i = 0; i < (e < 0 ? -e : e); i++) {   /* USER only when a sample is loaded */
            s = (s + (e > 0 ? 1 : NSLOT - 1)) % NSLOT;
            if (s == SLOT_USER && !user_sample())
                s = (s + (e > 0 ? 1 : NSLOT - 1)) % NSLOT;
        }
        slot_set((uint32_t)s);
    }
    if ((e = plat_enc(EN_PRESET)) != 0) {
        preset_load((uint32_t)((int32_t)set.preset + (int32_t)NPRESET * 4 + e));
        set_dirty = 1;
        if (!page_ok(ui.page))
            ui.page = PG_SOURCE;
        apply();
        ev_push(Q_SOURCE, set.slot, 0);
        app_say("PRESET", PRESETS[set.preset].name);
    }
    {
        const uint8_t *pp = page_params();
        static const uint8_t KN[4] = {EN_K1, EN_K2, EN_K3, EN_K4};
        for (k = 0; k < 4u; k++)
            if ((e = plat_enc(KN[k])) != 0) {
                /* the sizes step one a detent; the rest faster when turned fast */
                param_turn(pp[k], PDEF[pp[k]].max < 127u ? e : e * (e > 2 || e < -2 ? 3 : 1));
                ui.touched = (uint8_t)k;
                ui.touched_at = plat_ms();
            }
    }
    {   /* CC1 POSITION, CC71 SPREAD, CC74 TILT from MIDI in */
        static const uint8_t CCP[3] = {P_POSITION, P_SPREAD, P_TILT};
        for (k = 0; k < 3u; k++) {
            int32_t c = cc_in[k];
            if (c >= 0) {
                cc_in[k] = -1;
                set.v[CCP[k]] = (uint8_t)c;
                set_dirty = 1;
                apply();
                ui.redraw = 1;
            }
        }
    }
    if (keys != ui.keys || btn != ui.btn)
        ui.redraw = 1;
    ui.keys = keys;
    ui.btn = btn;
    plat_glow(1);
    plat_leds(btn | (set_dirty ? 1u << B_SAVE : 0u) | (hold ? 1u << B_PLAY : 0u) | (freeze ? 1u << B_REC : 0u) |
              1u << (ui.page <= PG_TONE ? B_EDIT : ui.page <= PG_SPACE ? B_ENV : ui.page <= PG_MENV ? B_LFO : B_FX), keys);
}

void app_input_only(void) { input(); }

/* ----------------------------------------------------------------- screen --- */
#define SPEC_Y 32
#define SPEC_H 108
#define PAR_Y 142
static void draw_header(void)
{
    uint32_t now = plat_ms();
    cv_begin(240, 30, C_BLACK);
    if (now < ui.msg_until) {
        int32_t x = cv_text(8, 6, &FONT_B, ui.msg[0], C_WHITE);
        cv_text(x + 8, 6, &FONT_B, ui.msg[1], C_HI);
    } else {
        int32_t x = cv_text(8, 6, &FONT_B, APP_NAME, C_WHITE);
        x = cv_text(x + 10, 10, &FONT_XS, slot_name(set.slot), C_HI);
        cv_text(x + 8, 10, &FONT_XS, PRESETS[set.preset].name, C_GRAY);
        if (hold)
            cv_text(196, 2, &FONT_XS, "HOLD", C_HI);
        if (freeze && set.slot != SLOT_PAD)
            cv_text(196, 16, &FONT_XS, "FREEZE", C_WHITE);
    }
    cv_blit(0, 0);
}

/* HARM's pages: its first 32 harmonics as bars (their levels before the envelopes, -48 .. 0 dB), those an envelope
 * moves marked (this page's in white), and on ENV A-D that envelope's shape */
static void draw_harmonics(void)
{
    const smg_params_t *p = &eng.p;
    uint32_t h, k, env = ui.page >= PG_ENVA && ui.page <= PG_ENVD ? (uint32_t)(ui.page - PG_ENVA) : 99u;
    float top = 0.0f, lv[33];
    cv_begin(240, SPEC_H, C_BLACK);
    for (h = 1; h <= 32u; h++) {
        lv[h] = h <= p->h_count ? smg_harm_level(p, h, 1.0f) : 0.0f;
        top = fm_maxf(top, lv[h]);
    }
    for (h = 1; h <= 32u; h++) {
        int32_t x = (int32_t)(h - 1u) * 15 / 2 + 1, ht;
        uint16_t c = C_HI;
        float db = lv[h] > 0.0f ? 6.0206f * fm_log2f(lv[h] / top) : -99.0f;
        for (k = 0; k < SMG_HENV; k++)
            if (smg_harm_takes(p->h_target[k], h))
                c = k == env ? C_WHITE : (env == 99u ? C_WHITE : C_AMB);
        ht = clamp((int32_t)((db + 48.0f) * (float)(SPEC_H - 14) / 48.0f), 0, SPEC_H - 14);
        if (ht > 0)
            cv_rect(x, SPEC_H - 10 - ht, 6, ht, c);
        if (h == 1u || h == 8u || h == 16u || h == 32u) {
            char t[4];
            int_text((int32_t)h, t);
            cv_text(h == 32u ? 226 : x, SPEC_H - 9, &FONT_XS, t, C_DIM);
        }
    }
    if (env < SMG_HENV && p->h_target[env]) {   /* the envelope: 0 .. its attack + decay + 1 s, top right */
        float span = p->h_att[env] + p->h_dec[env] + 1.0f;
        int32_t x0 = 120, w = 116, y0 = 2, hh = 34, i, py = y0 + hh;
        cv_rect(x0, y0 + hh, w, 1, C_LINE);
        for (i = 0; i <= w; i++) {
            int32_t y = y0 + hh - (int32_t)(smg_harm_env(p, env, span * (float)i / (float)w) * (float)hh);
            if (i)
                cv_line(x0 + i - 1, py, x0 + i, y, C_WHITE);
            py = y;
        }
    }
    cv_blit(0, SPEC_Y);
}

/* LFO 1, LFO 2: one cycle of the wave, where it is now, and what it moves; MOD ENV: its shape and where it is */
static void draw_mod(void)
{
    uint32_t k = ui.page == PG_LFO2 ? 1u : 0u;
    int32_t i, mid = SPEC_H / 2, amp = SPEC_H / 2 - 14, py = mid;
    const uint8_t *v = ui.page == PG_MENV ? &set.v[P_MENV] : &set.v[P_LFO(k, 0)];
    char t[24];
    cv_begin(240, SPEC_H, C_BLACK);
    cv_rect(0, mid, 240, 1, C_LINE);
    for (i = 0; i <= 236; i++) {
        float ph = (float)i / 236.0f, y;
        if (ui.page == PG_MENV) {        /* attack, then decay, on one scale */
            float a = attack_of(v[0]), d = release_of(v[1]), t_ = ph * (a + d);
            y = t_ < a ? t_ / a : 1.0f - (t_ - a) / d;
            y = 2.0f * y - 1.0f;
        } else {
            switch (v[1]) {
            case LFO_SINE: y = fm_sinf(FM_TWO_PI * ph); break;
            case LFO_TRI: y = ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph; break;
            case LFO_SAW_UP: y = 2.0f * ph - 1.0f; break;
            case LFO_SAW_DOWN: y = 1.0f - 2.0f * ph; break;
            case LFO_SQUARE: y = ph < 0.5f ? 1.0f : -1.0f; break;
            default: y = fm_sinf(FM_TWO_PI * ph * 3.0f) * 0.6f + fm_sinf(FM_TWO_PI * ph * 7.0f + 1.0f) * 0.3f; break;
            }
        }
        {
            int32_t yy = mid - (int32_t)(y * (float)amp);
            if (i)
                cv_line(i + 1, py, i + 2, yy, C_HI);
            py = yy;
        }
    }
    if (ui.page == PG_MENV) {
        int32_t yy = mid - (int32_t)((2.0f * menv - 1.0f) * (float)amp);
        cv_rect(2, yy - 2, 236, 1, C_WHITE);
    } else {
        int32_t x = (int32_t)(lfo[k].ph * 236.0f) + 1, yy = mid - (int32_t)(lfo[k].out * (float)amp);
        cv_rect(x - 2, yy - 2, 5, 5, C_WHITE);
    }
    t[0] = 0;
    text_cat(t, "> ");
    text_cat(t, v[2] == 0u || v[2] >= NDEST ? "OFF" : PDEF[MOD_DEST[v[2]]].name);
    cv_text(4, 2, &FONT_XS, t, v[2] ? C_WHITE : C_DIM);
    cv_blit(0, SPEC_Y);
}

/* the spectrum of the last frame, log frequency (40 Hz .. 16 kHz), -72 .. -6 dB; the sample's scan along the top */
static void draw_spectrum(void)
{
    uint32_t b;
    if (set.slot == SLOT_HARM && ui.page <= PG_ENVD) {
        draw_harmonics();
        return;
    }
    if (ui.page >= PG_LFO1 && ui.page <= PG_MENV) {
        draw_mod();
        return;
    }
    cv_begin(240, SPEC_H, C_BLACK);
    for (b = 0; b < 8u; b++)             /* octaves: 80, 160 .. 10240 Hz */
        cv_rect((int32_t)((float)(b + 1u) * ((float)SMG_DISP / 8.643856f) * 4.0f), 6, 1, SPEC_H - 6, C_LINE);
    for (b = 0; b < SMG_DISP; b++) {
        float m = eng.disp[b], db = m > 1e-7f ? 6.0206f * fm_log2f(m) : -140.0f;
        int32_t h = (int32_t)((db + 72.0f) * ((float)(SPEC_H - 8) / 66.0f));
        h = clamp(h, 0, SPEC_H - 8);
        if (h > 0)
            cv_rect((int32_t)b * 4, SPEC_H - h, 3, h, h > SPEC_H - 30 ? C_WHITE : C_HI);
    }
    if (slot_sample(set.slot) && eng.smp.len) {
        int32_t x = (int32_t)(eng.scan / (float)eng.smp.len * 238.0f), x0 = (int32_t)(unit(mod_v[P_POSITION]) * 238.0f);
        cv_rect(0, 0, 240, 2, C_LINE);
        cv_rect(x0, 0, 2, 4, C_GRAY);
        cv_rect(clamp(x, 0, 238), 0, 2, 4, freeze ? C_WHITE : C_HI);
    }
    cv_blit(0, SPEC_Y);
}

static void draw_params(void)
{
    char t[16];
    uint32_t k, now = plat_ms();
    const uint8_t *pp = page_params();
    cv_begin(240, 98, C_BLACK);
    {                                    /* this page's name, and a dot for each page this source has */
        int32_t x = 236;
        cv_text(4, 0, &FONT_XS, PAGE_NAME[ui.page], C_HI);
        for (k = NPAGE; k-- > 0;)
            if (page_ok(k)) {
                x -= 8;
                cv_rect(x, 4, 5, 5, k == ui.page ? C_HI : C_LINE);
            }
    }
    for (k = 0; k < 4u; k++) {           /* K1 .. K4: name, value, a bar */
        int32_t x = 4 + (int32_t)(k % 2u) * 120, y = 14 + (int32_t)(k / 2u) * 33, w;
        uint32_t p = pp[k];
        int lit = ui.touched == k && now - ui.touched_at < 600u;
        cv_text(x, y, &FONT_XS, PDEF[p].name, lit ? C_WHITE : C_GRAY);
        value_text(p, t);
        cv_text(x, y + 12, &FONT_B, t, lit ? C_WHITE : C_HI);
        w = (int32_t)set.v[p] * 108 / PDEF[p].max;
        cv_rect(x, y + 29, 108, 2, C_LINE);
        cv_rect(x, y + 29, w, 2, lit ? C_WHITE : C_HI);
        if (mod_v[p] != (float)set.v[p]) {   /* modulated: where it is now */
            int32_t m = (int32_t)(mod_v[p] * 108.0f / (float)PDEF[p].max);
            cv_rect(x + clamp(m, 0, 106) - 1, y + 26, 3, 7, C_WHITE);
        }
    }
    {   /* the foot: octave, voices, the audio load */
        char f[40] = "OCT ";
        t[0] = set.octave < 0 ? '-' : '+';
        t[1] = (char)('0' + (set.octave < 0 ? -set.octave : set.octave));
        t[2] = 0;
        text_cat(f, t);
        text_cat(f, "   VOICES ");
        int_text(smg_active(&eng), t);
        text_cat(f, t);
        text_cat(f, "   CPU ");
        int_text((int32_t)plat_cpu_pct(), t);
        text_cat(f, t);
        text_cat(f, "%");
        cv_text(4, 81, &FONT_XS, f, C_DIM);
    }
    cv_blit(0, PAR_Y);
}

void app_ui_init(void)
{
    touch_on(1);
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.redraw = 1;
    ui.touched = 0xFF;
}

void app_frame(void)
{
    static uint32_t frames_seen, last_t, n;
    uint32_t now = plat_ms();
    input();
    speak();
    if (ui.msg_until && now >= ui.msg_until) {
        ui.msg_until = 0;
        ui.redraw = 1;
    }
    if (ui.touched != 0xFF && now - ui.touched_at > 650u) {
        ui.touched = 0xFF;
        ui.redraw = 1;
    }
    if (ui.redraw) {
        ui.redraw = 0;
        draw_header();
        draw_params();
        ui.redraw_spec = 1;
    }
    /* the spectrum at every new frame (5 to 40 a second), and the foot's numbers twice a second */
    if (eng.frames != frames_seen || ui.redraw_spec) {
        frames_seen = eng.frames;
        ui.redraw_spec = 0;
        draw_spectrum();
    }
    if (now - last_t > ((set.v[P_LFO(0, 2)] || set.v[P_LFO(1, 2)] || set.v[P_MENV + 2]) ? 60u : 500u)) {
        last_t = now;                    /* (faster while something is modulated: the markers move) */
        if (++n & 1u || set.v[P_LFO(0, 2)] || set.v[P_LFO(1, 2)] || set.v[P_MENV + 2])
            draw_params();
    }
}

#ifdef PLAT_HOST
int app_expect(const char *what, int *got)
{
    if (!strcmp(what, "slot"))
        *got = set.slot;
    else if (!strcmp(what, "page"))
        *got = ui.page;
    else if (!strcmp(what, "preset"))
        *got = set.preset;
    else if (!strcmp(what, "octave"))
        *got = set.octave;
    else if (!strcmp(what, "hold"))
        *got = hold;
    else if (!strcmp(what, "freeze"))
        *got = freeze;
    else if (!strcmp(what, "voices"))
        *got = smg_active(&eng);
    else if (!strcmp(what, "late"))
        *got = (int)eng.late;
    else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else if (!strcmp(what, "user_len"))   /* the loaded sample's length, 0: none */
        *got = user_sample() ? (int)user_sample()->len : 0;
    else if (!strcmp(what, "sysex_errors"))   /* sample-load commands answered with an error */
        *got = (int)up_errors;
    else if (!strncmp(what, "mod", 3))    /* modN: parameter N as played (modulated), in knob steps */
        *got = (int)(mod_v[atoi(what + 3) % NP] + 0.5f);
    else if (!strcmp(what, "src_len"))    /* the sample the engine scans */
        *got = (int)eng.smp.len;
    else if (!strncmp(what, "v", 1) && what[1] >= '0' && what[1] <= '9')   /* vN: parameter N's value */
        *got = set.v[atoi(what + 1) % NP];
    else
        return 0;
    return 1;
}

int app_host_cmd(const char *cmd, const char *a, const char *b, const char *c, const char *d)
{
    FILE *f;
    static uint8_t m[1024];
    uint32_t n = 0, count = 0;
    int ch;
    (void)b, (void)c, (void)d;
    if (strcmp(cmd, "sysex"))
        return 0;
    if (!(f = fopen(a, "rb"))) {
        printf("FAIL sysex %s: no such file\n", a);
        up_errors++;
        return 1;
    }
    while ((ch = fgetc(f)) != EOF) {     /* each F0 .. F7 to the app, as the main loop hands them */
        if (ch == 0xF0)
            n = 0;
        else if (ch == 0xF7) {
            if (!app_sysex(m, n))
                up_errors++;
            count++;
        } else if (n < sizeof m)
            m[n++] = (uint8_t)ch;
    }
    fclose(f);
    printf("  ok   sysex: %u messages from %s\n", count, a);
    return 1;
}
#endif
