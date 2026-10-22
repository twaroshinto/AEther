/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 fm1-aether contributors */
/* AETHER on the FM-1: the panel, the screen, MIDI and the sample loader around the engine (dsp/aether.c).
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
#include "aether.h"
#include "va.h"
#include "aether_source.h"                /* the built-in samples (tools/gen_source.py, at build time) */

/* ------------------------------------------------------------- parameters --- */
#define NLFO 3u                          /* LFO 1-3 */
#define NMENV 2u                         /* MOD ENV 1-2 */
#define NMX 8u                           /* the modulation matrix's slots */
enum { P_STRETCH, P_POSITION, P_SPREAD, P_SIZE, P_BRIGHT, P_BAND, P_INHARM, P_ODD,
       P_ATTACK, P_RELEASE, P_WIDTH, P_LEVEL, P_TILT, P_SUB, P_SHIMMER, P_SHAPE, P_CUTOFF, P_RESO, P_MOTION,
       /* HARM */
       P_H_COUNT, P_H_SLOPE, P_H_ODD, P_H_BAND, P_H_FOCUS, P_H_BOOST, P_H_WIDTH, P_H_VEL,
       P_HENV0,                          /* ENV A-D: TARGET, ATTACK, DECAY, SUSTAIN each */
       P_LFO0 = P_HENV0 + 4 * AET_HENV,  /* LFO 1-3: RATE, SHAPE, FADE, SYNC each */
       P_MENV0 = P_LFO0 + 4 * NLFO,      /* MOD ENV 1-2: ATTACK, DECAY, SUSTAIN, RELEASE each */
       P_MX0 = P_MENV0 + 4 * NMENV,      /* the matrix: SOURCE, DEST, AMOUNT a slot */
       P_MSLOT = P_MX0 + 3 * NMX,        /* the slot the MATRIX page shows */
       P_CORE2,                          /* SETUP: the second core (at the next power-on) */
       NP };
#define P_HENV(k, i) (P_HENV0 + 4 * (k) + (i))
#define P_LFO(k, i) (P_LFO0 + 4 * (k) + (i))
#define P_MENV(k, i) (P_MENV0 + 4 * (k) + (i))
#define P_MX(s, i) (P_MX0 + 3 * (s) + (i))
#define P_NONE 0xFFu                     /* an empty knob on a page */
enum { LFO_SINE, LFO_TRI, LFO_SAW_UP, LFO_SAW_DOWN, LFO_SQUARE, LFO_SH, LFO_DRIFT, NLFO_SHAPE };
static const char *const LFO_SHAPE_NAME[NLFO_SHAPE] = {"SINE", "TRIANGLE", "SAW UP", "SAW DOWN", "SQUARE",
                                                      "RANDOM", "DRIFT"};
/* the matrix's sources: the LFOs and envelopes, and what the player does */
enum { MS_OFF, MS_LFO1, MS_LFO2, MS_LFO3, MS_ENV1, MS_ENV2, MS_VEL, MS_WHEEL, MS_PRESS, MS_RAND, MS_KEY, NMS };
static const char *const MS_NAME[NMS] = {"OFF", "LFO 1", "LFO 2", "LFO 3", "ENV 1", "ENV 2", "VELOCITY", "WHEEL",
                                         "PRESSURE", "RANDOM", "KEY"};
/* ... and what they may move: these parameters (their knobs' values, so every mapping holds) */
enum { D_OFF, D_STRETCH, D_POSITION, D_SPREAD, D_TILT, D_SUB, D_SHIMMER, D_WIDTH, D_LEVEL, D_BRIGHT, D_BAND,
       D_INHARM, D_ODD, D_SHAPE, D_CUTOFF, D_RESO, D_SLOPE, D_EVENODD, D_FOCUS, D_BOOST, D_HBAND, D_RATE1, D_RATE2,
       D_RATE3, NDEST };
static const uint8_t MOD_DEST[NDEST] = {P_NONE, P_STRETCH, P_POSITION, P_SPREAD, P_TILT, P_SUB, P_SHIMMER, P_WIDTH,
                                        P_LEVEL, P_BRIGHT, P_BAND, P_INHARM, P_ODD, P_SHAPE, P_CUTOFF, P_RESO,
                                        P_H_SLOPE, P_H_ODD, P_H_FOCUS, P_H_BOOST, P_H_BAND, P_LFO(0, 0), P_LFO(1, 0),
                                        P_LFO(2, 0)};
static const char *const DEST_NAME[NDEST] = {"OFF", "STRETCH", "POSITION", "SPREAD", "TILT", "SUB", "SHIMMER",
                                             "WIDTH", "LEVEL", "BRIGHT", "BAND", "INHARM", "ODD", "SHAPE", "CUTOFF",
                                             "RESO", "SLOPE", "EVEN/ODD", "FOCUS", "BOOST", "H BAND", "LFO1 RATE",
                                             "LFO2 RATE", "LFO3 RATE"};
typedef struct {
    const char *name;
    uint8_t max;                         /* the values are 0 .. max (127 mostly) */
} pdef_t;
#define HENV_DEF {"TARGET", AET_NHT - 1u}, {"ATTACK", 127}, {"DECAY", 127}, {"SUSTAIN", 127}
#define LFO_DEF {"RATE", 127}, {"SHAPE", NLFO_SHAPE - 1u}, {"FADE", 127}, {"SYNC", 1}
#define MENV_DEF {"ATTACK", 127}, {"DECAY", 127}, {"SUSTAIN", 127}, {"RELEASE", 127}
#define MX_DEF {"SOURCE", NMS - 1u}, {"DEST", NDEST - 1u}, {"AMOUNT", 127}
static const pdef_t PDEF[NP] = {
    {"STRETCH", 127}, {"POSITION", 127}, {"SPREAD", 127}, {"SIZE", AET_NSIZE - 1u},
    {"BRIGHT", 127}, {"BAND", 127}, {"INHARM", 127}, {"ODD", 127},
    {"ATTACK", 127}, {"RELEASE", 127}, {"WIDTH", 127}, {"LEVEL", 127},
    {"TILT", 127}, {"SUB", 127}, {"SHIMMER", 127},
    {"SHAPE", 127}, {"CUTOFF", 127}, {"RESO", 127}, {"MOTION", 127},
    {"HARMONICS", AET_HARM_MAX - 1u}, {"SLOPE", 127}, {"EVEN/ODD", 127}, {"H BAND", 127},
    {"FOCUS", 31}, {"BOOST", 127}, {"FOCUS W", 127}, {"VEL>TONE", 127},
    HENV_DEF, HENV_DEF, HENV_DEF, HENV_DEF,
    LFO_DEF, LFO_DEF, LFO_DEF,
    MENV_DEF, MENV_DEF,
    MX_DEF, MX_DEF, MX_DEF, MX_DEF, MX_DEF, MX_DEF, MX_DEF, MX_DEF,
    {"SLOT", NMX - 1u}, {"2ND CORE", 1},
};
/* the factory values; a preset changes some of them */
static const uint8_t PDEFAULT[NP] = {
    52, 0, 0, 3, 64, 50, 0, 0, 80, 90, 110, 96, 64, 0, 0, 0, 80, 40, 30,
    23, 40, 64, 30, 3, 64, 30, 64,
    0, 50, 80, 64, 0, 50, 80, 64, 0, 50, 80, 64, 0, 50, 80, 64,
    40, LFO_SINE, 0, 0, 60, LFO_DRIFT, 0, 0, 20, LFO_TRI, 0, 0,     /* LFO 1-3 */
    40, 80, 64, 80, 90, 100, 127, 100,                              /* MOD ENV 1-2 */
    0, 0, 64, 0, 0, 64, 0, 0, 64, 0, 0, 64, 0, 0, 64, 0, 0, 64, 0, 0, 64,
    MS_WHEEL, D_POSITION, 127,                                      /* slot 8: the mod wheel scans the sample */
    0, 0,
};

/* the sources: PAD, VA, HARM, the built-in samples (aet_builtin[], SLOT_SMP0 on), USER */
enum { SLOT_PAD, SLOT_VA, SLOT_HARM, SLOT_SMP0, SLOT_CHOIR = SLOT_SMP0, SLOT_GLASS, SLOT_STRING, SLOT_WIND, SLOT_PIANO,
       SLOT_BOWED, SLOT_FLUTE, SLOT_ORGAN, SLOT_KALIMBA, SLOT_BRASS, SLOT_DRONE, SLOT_RAIN, SLOT_BIRDS,
       SLOT_WHISPER, SLOT_GAMELAN, SLOT_TAPE };   /* in gen_source.py's order */
#define SLOT_USER (SLOT_SMP0 + AET_NBUILTIN)
#define NSLOT (SLOT_USER + 1u)
static int slot_sample(uint32_t s) { return s >= SLOT_SMP0; }   /* a sample, built-in or loaded */
static const char *slot_name(uint32_t s)
{
    return s == SLOT_PAD ? "PAD" : s == SLOT_VA ? "VA" : s == SLOT_HARM ? "HARM" :
           s < SLOT_USER ? aet_builtin[s - SLOT_SMP0].name : "USER";
}
enum { PG_SOURCE, PG_TONE, PG_ENVA, PG_ENVB, PG_ENVC, PG_ENVD, PG_SPACE, PG_LFO1, PG_LFO2, PG_LFO3, PG_MENV1,
       PG_MENV2, PG_MATRIX, PG_COLOR, PG_SETUP, NPAGE };
static const char *const PAGE_NAME[NPAGE] = {"SOURCE", "TONE", "ENV A", "ENV B", "ENV C", "ENV D", "SPACE", "LFO 1",
                                             "LFO 2", "LFO 3", "MOD ENV 1", "MOD ENV 2", "MATRIX", "COLOR", "SETUP"};
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
    uint8_t ov[40][2];                   /* {parameter + 1, value}, then zeros */
} preset_t;
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
    {"HARM BLOOM", SLOT_HARM, {O(P_H_COUNT, 31), O(P_H_SLOPE, 30), O(P_HENV(0, 0), AET_HT_HIGH), O(P_HENV(0, 1), 105),
                               O(P_HENV(0, 3), 127), O(P_HENV(1, 0), 1), O(P_HENV(1, 1), 20), O(P_HENV(1, 2), 95),
                               O(P_HENV(1, 3), 70), O(P_ATTACK, 70),
                               O(P_MX(0, 0), MS_LFO1), O(P_MX(0, 1), D_TILT), O(P_MX(0, 2), 100),
                               O(P_MX(1, 0), MS_ENV2), O(P_MX(1, 1), D_SLOPE), O(P_MX(1, 2), 20)}},
    {"HARM BELL", SLOT_HARM, {O(P_H_COUNT, 23), O(P_H_SLOPE, 25), O(P_H_FOCUS, 4), O(P_H_BOOST, 100), O(P_H_WIDTH, 15),
                              O(P_HENV(0, 0), AET_HT_HIGH), O(P_HENV(0, 1), 5), O(P_HENV(0, 2), 70), O(P_HENV(0, 3), 0),
                              O(P_HENV(1, 0), 3), O(P_HENV(1, 1), 5), O(P_HENV(1, 2), 90), O(P_HENV(1, 3), 20),
                              O(P_ATTACK, 10), O(P_RELEASE, 100)}},
    {"HARM VOWELS", SLOT_HARM, {O(P_H_COUNT, 40), O(P_H_SLOPE, 35), O(P_H_FOCUS, 6), O(P_H_BOOST, 110),
                                O(P_H_WIDTH, 25), O(P_LFO(0, 0), 25), O(P_LFO(0, 1), LFO_TRI),
                                O(P_MX(0, 0), MS_LFO1), O(P_MX(0, 1), D_FOCUS), O(P_MX(0, 2), 110),
                                O(P_MX(1, 0), MS_LFO2), O(P_MX(1, 1), D_BOOST), O(P_MX(1, 2), 84),
                                O(P_MX(2, 0), MS_LFO3), O(P_MX(2, 1), D_RATE1), O(P_MX(2, 2), 90)}},
    {"HARM HOLLOW", SLOT_HARM, {O(P_H_COUNT, 31), O(P_H_SLOPE, 45), O(P_H_ODD, 127), O(P_H_BAND, 50),
                                O(P_HENV(0, 0), AET_HT_HIGH), O(P_HENV(0, 1), 110), O(P_HENV(0, 3), 100), O(P_SUB, 40)}},
    {"DRIFT FOG", SLOT_CHOIR, {O(P_STRETCH, 80), O(P_SPREAD, 40), O(P_LFO(0, 0), 10), O(P_LFO(0, 1), LFO_DRIFT),
                               O(P_LFO(1, 0), 30), O(P_LFO(1, 1), LFO_SINE),
                               O(P_MX(0, 0), MS_LFO1), O(P_MX(0, 1), D_POSITION), O(P_MX(0, 2), 100),
                               O(P_MX(1, 0), MS_LFO2), O(P_MX(1, 1), D_TILT), O(P_MX(1, 2), 90)}},
    {"EVOLVER", SLOT_HARM, {O(P_H_COUNT, 47), O(P_H_SLOPE, 30), O(P_H_FOCUS, 7), O(P_H_BOOST, 100), O(P_H_WIDTH, 20),
                            O(P_H_BAND, 40), O(P_ATTACK, 85), O(P_RELEASE, 105), O(P_WIDTH, 120),
                            O(P_LFO(0, 0), 18), O(P_LFO(0, 1), LFO_TRI), O(P_LFO(0, 2), 80),
                            O(P_LFO(1, 0), 12), O(P_LFO(1, 1), LFO_DRIFT), O(P_LFO(2, 0), 70), O(P_LFO(2, 1), LFO_SINE),
                            O(P_MENV(0, 0), 95), O(P_MENV(0, 1), 110), O(P_MENV(0, 2), 40),
                            O(P_MX(0, 0), MS_LFO1), O(P_MX(0, 1), D_FOCUS), O(P_MX(0, 2), 120),
                            O(P_MX(1, 0), MS_LFO2), O(P_MX(1, 1), D_SLOPE), O(P_MX(1, 2), 90),
                            O(P_MX(2, 0), MS_ENV1), O(P_MX(2, 1), D_SHIMMER), O(P_MX(2, 2), 110),
                            O(P_MX(3, 0), MS_VEL), O(P_MX(3, 1), D_BOOST), O(P_MX(3, 2), 90),
                            O(P_MX(4, 0), MS_LFO3), O(P_MX(4, 1), D_WIDTH), O(P_MX(4, 2), 84),
                            O(P_MX(5, 0), MS_LFO2), O(P_MX(5, 1), D_RATE1), O(P_MX(5, 2), 100)}},
    {"BREATHING GLASS", SLOT_GLASS, {O(P_STRETCH, 90), O(P_ATTACK, 80), O(P_RELEASE, 100), O(P_WIDTH, 120),
                                     O(P_LFO(0, 0), 15), O(P_LFO(0, 1), LFO_SINE), O(P_LFO(0, 2), 90),
                                     O(P_LFO(1, 0), 35), O(P_LFO(1, 1), LFO_SH),
                                     O(P_MENV(1, 0), 110), O(P_MENV(1, 1), 115), O(P_MENV(1, 2), 30),
                                     O(P_MX(0, 0), MS_ENV2), O(P_MX(0, 1), D_SPREAD), O(P_MX(0, 2), 120),
                                     O(P_MX(1, 0), MS_LFO1), O(P_MX(1, 1), D_POSITION), O(P_MX(1, 2), 100),
                                     O(P_MX(2, 0), MS_PRESS), O(P_MX(2, 1), D_SHIMMER), O(P_MX(2, 2), 127),
                                     O(P_MX(3, 0), MS_LFO2), O(P_MX(3, 1), D_TILT), O(P_MX(3, 2), 76),
                                     O(P_MX(4, 0), MS_RAND), O(P_MX(4, 1), D_POSITION), O(P_MX(4, 2), 90)}},
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

#define SET_MAGIC 0x31544541u            /* "AET1": the settings record; another magic loads a preset */
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
static aet_t eng;
static float eng_ram[AET_RAM_FLOATS] PLAT_POOL;
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
/* The modulation matrix: eight slots, each a source (an LFO, an envelope, what the player does), a destination
 * (one of NDEST parameters) and an amount (-100 .. +100 % of that parameter's range). Slots add up: one source may
 * move many parameters, and one parameter take many sources. Worked out once an audio block, in the audio
 * interrupt; the knobs keep their values, the engine plays mod_v. */
static float mod_v[NP];                  /* every parameter's value as played: the knob's plus its modulation */
static float msrc[NMS];                  /* the sources now: LFOs -1 .. 1, envelopes 0 .. 1, ... */
static struct {
    float ph, out, held, drift, drift_to, since;
    uint32_t rng;
} lfo[NLFO] = {{0, 0, 0, 0, 0, 0, 0x1234567u}, {0.25f, 0, 0, 0, 0, 0, 0x89ABCDEu}, {0.5f, 0, 0, 0, 0, 0, 0x2468ACEu}};
static struct {
    float level;
    uint8_t stage;                       /* 0 released, 1 attack, 2 decay and sustain */
} menv[NMENV];
static volatile uint32_t note_trig;      /* note-ons (keys and MIDI): the envelopes start again, SYNC LFOs too */
static uint32_t note_seen;
static volatile float last_vel = 0.8f, last_key, wheel, key_press, midi_press;
static float note_rand;
static uint32_t rand_rng = 0x5EEDu;

static float rnd_unit(uint32_t *r)       /* -1 .. 1 */
{
    *r ^= *r << 13;
    *r ^= *r >> 17;
    *r ^= *r << 5;
    return (float)(*r >> 8) / 8388608.0f - 1.0f;
}

static void note_heard(uint32_t note, uint32_t vel)   /* a note-on, for VELOCITY, KEY and the envelopes */
{
    last_vel = (float)vel / 127.0f;
    last_key = fm_clampf(((float)note - 60.0f) / 24.0f, -1.0f, 1.0f);
    note_trig++;
}

static float lfo_wave(uint32_t shape, float ph, uint32_t k)
{
    switch (shape) {
    case LFO_SINE: return fm_sinf(FM_TWO_PI * ph);
    case LFO_TRI: return ph < 0.5f ? 4.0f * ph - 1.0f : 3.0f - 4.0f * ph;
    case LFO_SAW_UP: return 2.0f * ph - 1.0f;
    case LFO_SAW_DOWN: return 1.0f - 2.0f * ph;
    case LFO_SQUARE: return ph < 0.5f ? 1.0f : -1.0f;
    case LFO_SH: return lfo[k].held;
    default: return lfo[k].drift;
    }
}

static int any_gate(void)                /* a note held (keys, HOLD, MIDI): the envelopes' gate */
{
    uint32_t v;
    for (v = 0; v < AET_VOICES; v++)
        if (eng.voice[v].note >= 0 && eng.voice[v].gate)
            return 1;
    return 0;
}

static void mod_step(float dt)           /* audio interrupt: dt seconds since the last */
{
    uint32_t k, gate = (uint32_t)any_gate(), trig = note_trig != note_seen;
    note_seen = note_trig;
    if (trig)
        note_rand = rnd_unit(&rand_rng);
    for (k = 0; k < NLFO; k++) {
        const uint8_t *v = &set.v[P_LFO(k, 0)];
        float ph, fade;
        int wrapped;
        if (trig && v[3]) {              /* SYNC: from the start at each note */
            lfo[k].ph = 0.0f;
            lfo[k].held = rnd_unit(&lfo[k].rng);
        }
        if (trig)
            lfo[k].since = 0.0f;
        lfo[k].since += dt;
        ph = lfo[k].ph + lfo_hz(mod_v[P_LFO(k, 0)]) * dt;   /* (its RATE as modulated: last block's) */
        wrapped = ph >= 1.0f;
        if (wrapped)
            ph -= (float)(uint32_t)ph;
        lfo[k].ph = ph;
        if (wrapped) {
            lfo[k].held = rnd_unit(&lfo[k].rng);
            lfo[k].drift_to = rnd_unit(&lfo[k].rng);
        }
        lfo[k].drift += (lfo[k].drift_to - lfo[k].drift) * fm_minf(1.0f, dt * lfo_hz(mod_v[P_LFO(k, 0)]) * 4.0f);
        fade = v[2] ? fm_minf(1.0f, lfo[k].since / attack_of((float)v[2])) : 1.0f;   /* FADE: in after each note */
        lfo[k].out = lfo_wave(v[1], ph, k) * fade;
        msrc[MS_LFO1 + k] = lfo[k].out;
    }
    for (k = 0; k < NMENV; k++) {        /* ADSR, gated by the notes held; each note-on starts it again */
        const uint8_t *v = &set.v[P_MENV(k, 0)];
        float sus = unit((float)v[2]);
        if (trig)
            menv[k].stage = 1;
        if (!gate)
            menv[k].stage = 0;
        if (menv[k].stage == 1) {
            menv[k].level += dt / attack_of((float)v[0]);
            if (menv[k].level >= 1.0f) {
                menv[k].level = 1.0f;
                menv[k].stage = 2;
            }
        } else if (menv[k].stage == 2) {
            menv[k].level -= dt / release_of((float)v[1]) * (1.0f - sus);
            if (menv[k].level < sus)
                menv[k].level = sus;
        } else {
            menv[k].level -= dt / release_of((float)v[3]);
            if (menv[k].level < 0.0f)
                menv[k].level = 0.0f;
        }
        msrc[MS_ENV1 + k] = menv[k].level;
    }
    msrc[MS_OFF] = 0.0f;
    msrc[MS_VEL] = last_vel;
    msrc[MS_WHEEL] = wheel;
    msrc[MS_PRESS] = fm_maxf(key_press, midi_press);
    msrc[MS_RAND] = note_rand;
    msrc[MS_KEY] = last_key;
    for (k = 0; k < NP; k++)
        mod_v[k] = (float)set.v[k];
    for (k = 0; k < NMX; k++) {
        const uint8_t *m = &set.v[P_MX(k, 0)];
        uint32_t d = m[1] < NDEST ? MOD_DEST[m[1]] : P_NONE;
        if (m[0] && m[0] < NMS && d < NP)
            mod_v[d] += bipolar((float)m[2]) * msrc[m[0]] * (float)PDEF[d].max;
    }
    for (k = 0; k < NP; k++)
        mod_v[k] = fm_clampf(mod_v[k], 0.0f, (float)PDEF[k].max);
}

/* the overload guard: the platform tells (app_overload) when a block took over 90 % of its time; each time (at most
 * every 250 ms) one step more is shed: SUB and SHIMMER first, then voices, down to two; three quiet seconds give
 * one step back (app_frame) */
static volatile uint8_t shed;
static volatile uint32_t shed_at;
static const uint8_t SHED_CAP[7] = {0, 0, 6, 5, 4, 3, 2};
void app_overload(void)
{
    uint32_t now = plat_ms();
    if (shed < 6u && now - shed_at > 250u) {
        shed++;
        shed_at = now;
    }
}

/* the played values into the engine's parameters: the audio interrupt only (the engine's own context), once a block */
static void map_params(void)
{
    aet_params_t *p = &eng.p;
    const float *v = mod_v;
    uint32_t k;
    p->source = set.slot == SLOT_PAD ? AET_SRC_PAD : set.slot == SLOT_VA ? AET_SRC_LIVE :
                set.slot == SLOT_HARM ? AET_SRC_HARM : AET_SRC_SAMPLE;
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
    p->voice_cap = SHED_CAP[shed];
    p->no_layers = shed > 0u;
    p->h_count = (uint8_t)(set.v[P_H_COUNT] + 1u);
    p->h_slope = 3.0f * unit(v[P_H_SLOPE]);
    p->h_odd = bipolar(v[P_H_ODD]);
    p->h_band = hband_of(v[P_H_BAND]);
    p->h_focus = v[P_H_FOCUS] + 1.0f;
    p->h_boost = 24.0f * bipolar(v[P_H_BOOST]);
    p->h_width = 0.4f + 5.6f * unit(v[P_H_WIDTH]);
    p->h_vel = unit(v[P_H_VEL]);
    for (k = 0; k < AET_HENV; k++) {
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
    return fmt == AET_FMT_ADPCM ? AET_ADPCM_SIZE(len) : fmt == AET_FMT_PCM16 ? 2u * len : 0u;
}
static int sample_size_ok(uint32_t len, uint32_t rate, uint32_t fmt, uint32_t bytes, uint32_t frames, uint32_t hop)
{
    return len && len <= SMP_MAX_BYTES * 2u && rate >= 4000u && rate <= 96000u && bytes <= SMP_MAX_BYTES &&
           audio_bytes(len, fmt) && bytes == audio_bytes(len, fmt) + frames * AET_SBR_BANDS && (!frames || hop);
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
    const aet_builtin_t *b = &aet_builtin[slot >= SLOT_SMP0 && slot < SLOT_USER ? slot - SLOT_SMP0 : 0u];
    if (h) {
        aet_set_sample(&eng, plat_data(SMP_DATA_OFF), h->len, h->rate, h->fmt);
        aet_set_env(&eng, plat_data(SMP_DATA_OFF) + audio_bytes(h->len, h->fmt), h->env_frames, h->env_hop);
    } else if (slot_sample(slot) || !eng.smp.len) {
        aet_set_sample(&eng, b->data, b->len, b->rate, AET_FMT_ADPCM);
        aet_set_env(&eng, b->env, b->env_frames, b->env_hop);
    }
}

/* ---------------------------------------------------------------- MIDI in --- */
static volatile int16_t cc_in[3] = {-1, -1, -1};   /* (unused), CC71, CC74 from the interrupt, for the main loop */
static uint32_t sus_on, sus_notes[4];    /* CC64: note-offs held back, by note */
static volatile uint32_t midi_notes_in;
static void midi_in(void)                /* audio interrupt */
{
    uint32_t pkt;
    int port;
    while (plat_midi_in(&pkt, &port)) {
        uint32_t st = (pkt >> 8) & 0xFFu, d1 = (pkt >> 16) & 0x7Fu, d2 = pkt >> 24 & 0x7Fu;
        if ((st & 0xF0u) == 0x90u && d2) {
            aet_note_on(&eng, (int32_t)d1, (float)d2 / 127.0f);
            note_heard(d1, d2);
            sus_notes[d1 >> 5] &= ~(1u << (d1 & 31u));
            midi_notes_in++;
        } else if ((st & 0xF0u) == 0x80u || (st & 0xF0u) == 0x90u) {
            if (sus_on)
                sus_notes[d1 >> 5] |= 1u << (d1 & 31u);
            else
                aet_note_off(&eng, (int32_t)d1);
        } else if ((st & 0xF0u) == 0xB0u) {
            if (d1 == 64u) {
                sus_on = d2 >= 64u;
                if (!sus_on) {
                    uint32_t k;
                    for (k = 0; k < 128u; k++)
                        if (sus_notes[k >> 5] >> (k & 31u) & 1u)
                            aet_note_off(&eng, (int32_t)k);
                    sus_notes[0] = sus_notes[1] = sus_notes[2] = sus_notes[3] = 0;
                }
            } else if (d1 == 1u) {           /* the mod wheel: the matrix's WHEEL */
                wheel = (float)d2 / 127.0f;
            } else if (d1 == 71u) {
                cc_in[1] = (int16_t)d2;
            } else if (d1 == 74u) {
                cc_in[2] = (int16_t)d2;
            } else if (d1 == 120u || d1 == 123u) {   /* all sound off, all notes off */
                aet_all_off(&eng);
            }
        } else if ((st & 0xF0u) == 0xD0u) {  /* channel pressure: the matrix's PRESSURE */
            midi_press = (float)d1 / 127.0f;
        } else if ((st & 0xF0u) == 0xA0u) {  /* poly pressure: the strongest key's */
            midi_press = (float)d2 / 127.0f;
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
        case Q_ON: aet_note_on(&eng, (int32_t)a, (float)b / 127.0f); break;
        case Q_OFF: aet_note_off(&eng, (int32_t)a); break;
        case Q_ALLOFF: aet_all_off(&eng); break;
        case Q_PANIC: aet_panic(&eng); break;
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
            aet_live_push(&eng, buf_l, m);
            eng.p.lp_hz = va.fc_now;
        }
        aet_render(&eng, buf_l, buf_r, m);
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
    aet_panic(&eng);
}

/* --------------------------------------------------------------- settings --- */
static void preset_load(uint32_t i)
{
    uint32_t k;
    const preset_t *p = &PRESETS[i % NPRESET];
    uint8_t core2 = set.v[P_CORE2];      /* (SETUP's choices are not a preset's) */
    for (k = 0; k < NP; k++)
        set.v[k] = PDEFAULT[k];
    for (k = 0; k < 40u && p->ov[k][0]; k++)
        set.v[p->ov[k][0] - 1u] = p->ov[k][1];
    set.v[P_CORE2] = core2;
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

/* ------------------------------------------------------------- second core --- */
static uint8_t core2_wanted_at_boot;     /* SETUP's choice when this power-on started */
#ifndef PLAT_HOST
#include "hw/core1.c"                    /* (vendored: the register access of the second core) */
static int core2_running(void) { return c1_state == C1_RUNNING; }
static const char *core2_state_text(void)
{
    static const char *const T[] = {"OFF", "RUNNING", "DID NOT START", "GIVEN UP: ONE CORE", "BARRED UNTIL POWER-OFF"};
    return T[c1_state < 5u ? c1_state : 0u];
}
void app_cstart(void) { core1_halt(); }   /* registers only: a second core a reset left running, held */
void app_fault(const fm1_crash_t *c)
{
    (void)c;
    core1_give_up(C1_GAVE_UP);           /* one core until power-off, whichever crashed */
}
#else
static int core2_running(void) { return 0; }
static const char *core2_state_text(void) { return set.v[P_CORE2] ? "NOT IN THE EMULATOR" : "OFF"; }
#endif

void app_init(void)
{
    if (plat_store_load(OBJ_SETTINGS, &set, sizeof set) != (int)sizeof set || !settings_ok()) {
        set.magic = SET_MAGIC;
        set.octave = 0;
        preset_load(0);
    }
    if (set.slot == SLOT_USER && !user_sample())
        set.slot = SLOT_CHOIR;
    aet_init(&eng, eng_ram);
    core2_wanted_at_boot = set.v[P_CORE2];
#ifndef PLAT_HOST
    if (set.v[P_CORE2] && core1_start() == 0)   /* (before the audio: core 1 starts at power-on only) */
        aet_set_par(&eng, core1_run, core1_wait);
#endif
    va_init(&va);
    mod_step(0.0f);
    map_params();
    source_set(set.slot);
}

/* ------------------------------------------------------------- sample load --- */
/* SysEx F0 7D 41 45 cmd ... F7 (tools/aether_load.py); every command answered F0 7D 41 45 7F cmd status F7.
 *   01 BEGIN  len (samples, 4 x 7 bits), rate (3 x 7 bits), format (AET_FMT_*: 0 PCM16, 1 ADPCM), bytes (4 x 7
 *             bits: the samples' 2 len or AET_ADPCM_SIZE(len), plus the envelope's), the SBR envelope's frames (3 x 7
 *             bits) and hop (2 x 7 bits; 0 0: none), name (up to 15 ASCII)
 *   02 DATA   byte offset (4 x 7 bits), then the bytes packed 7 in 8 (the first of each 8 holds the next seven's top
 *             bits); in order, at most 512 bytes a message
 *   03 END    CRC-32 of the bytes (5 x 7 bits): checked against the flash, then the header written
 * Status: 0 ok, 1 not now, 2 too long, 3 out of order, 4 the flash refused, 5 CRC mismatch */
static struct {
    uint32_t len, rate, fmt, bytes, next;   /* samples; Hz; AET_FMT_*; the data's size; the next byte expected */
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
    uint8_t f[8] = {0xF0, 0x7D, 0x41, 0x45, 0x7F, (uint8_t)cmd, (uint8_t)status, 0xF7};
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
    if (n < 4u || p[0] != 0x7Du || p[1] != 0x41u || p[2] != 0x45u)
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
    if (t == AET_HT_OFF)
        text_cat(b, "OFF");
    else if (t <= AET_HT_H32) {
        text_cat(b, "H");
        int_text((int32_t)t, b + 1);
    } else
        text_cat(b, G[(t - AET_HT_ODD) % 6u]);
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
        static const char *const MS[AET_NSIZE] = {"23 MS", "46 MS", "93 MS", "186 MS"};
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
        } else if (p >= P_LFO0 && p < P_MENV0) {    /* LFO 1-3: RATE, SHAPE, FADE, SYNC */
            uint32_t i = (p - P_LFO0) % 4u;
            if (i == 0u) {
                float hz = lfo_hz((float)v);
                if (hz < 1.0f) {
                    tenths_text(1.0f / hz, b);
                    text_cat(b, " S");
                } else {
                    tenths_text(hz, b);
                    text_cat(b, " HZ");
                }
            } else if (i == 1u) {
                text_cat(b, LFO_SHAPE_NAME[v < NLFO_SHAPE ? v : 0u]);
            } else if (i == 2u) {
                if (v == 0u)
                    text_cat(b, "OFF");
                else {
                    tenths_text(attack_of((float)v), b);
                    text_cat(b, " S");
                }
            } else {
                text_cat(b, v ? "KEY" : "FREE");
            }
        } else if (p >= P_MENV0 && p < P_MX0) {    /* MOD ENV 1-2: ATTACK, DECAY, SUSTAIN, RELEASE */
            uint32_t i = (p - P_MENV0) % 4u;
            if (i == 2u) {
                int_text((int32_t)(unit((float)v) * 100.0f + 0.5f), b);
                text_cat(b, "%");
            } else {
                tenths_text(i == 0u ? attack_of((float)v) : release_of((float)v), b);
                text_cat(b, " S");
            }
        } else if (p >= P_MX0 && p < P_MSLOT) {     /* the matrix: SOURCE, DEST, AMOUNT */
            uint32_t i = (p - P_MX0) % 3u;
            if (i == 0u)
                text_cat(b, MS_NAME[v < NMS ? v : 0u]);
            else if (i == 1u)
                text_cat(b, DEST_NAME[v < NDEST ? v : 0u]);
            else {
                int32_t pc = (int32_t)(bipolar((float)v) * 100.0f + (v >= 64u ? 0.5f : -0.5f));
                if (pc > 0)
                    text_cat(b, "+");
                int_text(pc, b + (pc > 0));
                text_cat(b, "%");
            }
        } else if (p == P_MSLOT) {
            int_text((int32_t)v + 1, b);
        } else if (p == P_CORE2) {
            text_cat(b, v ? "ON" : "OFF");
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
static int page_is_mod(uint32_t pg) { return pg >= PG_LFO1 && pg <= PG_MATRIX; }

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
    case PG_LFO1: case PG_LFO2: case PG_LFO3:
        for (k = 0; k < 4u; k++)
            pp[k] = (uint8_t)P_LFO(ui.page - PG_LFO1, k);
        return pp;
    case PG_MENV1: case PG_MENV2:
        for (k = 0; k < 4u; k++)
            pp[k] = (uint8_t)P_MENV(ui.page - PG_MENV1, k);
        return pp;
    case PG_MATRIX:                      /* SLOT, then that slot's SOURCE, DEST, AMOUNT */
        pp[0] = P_MSLOT;
        for (k = 0; k < 3u; k++)
            pp[k + 1u] = (uint8_t)P_MX(set.v[P_MSLOT] % NMX, k);
        return pp;
    case PG_SETUP:
        pp[0] = P_CORE2;
        pp[1] = pp[2] = pp[3] = P_NONE;
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
        for (k = 0; k < 4u && pp[k] != P_NONE; k++) {
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
    if (p == P_CORE2)                    /* (it starts only at power-on) */
        app_say(v[1] == 'N' ? "2ND CORE ON" : "2ND CORE OFF", "SAVE, THEN RESTART");
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
            note_heard(note, v ? v : 100u);
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
        case B_LFO: page_button(PG_LFO1, PG_MENV2); break;
        case B_GLO: page_set(PG_MATRIX); break;
        case B_SEQ: page_set(PG_SETUP); break;
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
            if ((e = plat_enc(KN[k])) != 0 && pp[k] != P_NONE) {
                /* the sizes step one a detent; the rest faster when turned fast */
                param_turn(pp[k], PDEF[pp[k]].max < 127u ? e : e * (e > 2 || e < -2 ? 3 : 1));
                ui.touched = (uint8_t)k;
                ui.touched_at = plat_ms();
            }
    }
    {   /* the keys' pressure (the matrix's PRESSURE): the strongest held key's */
        uint32_t pr = 0;
        for (k = 0; k < NKEYS; k++)
            if (keys >> k & 1u && touch_pressure(k) > pr)
                pr = touch_pressure(k);
        key_press = (float)pr / 127.0f;
    }
    {   /* CC71 SPREAD, CC74 TILT from MIDI in (CC1, the wheel, is the matrix's) */
        static const uint8_t CCP[3] = {P_POSITION, P_SPREAD, P_TILT};
        for (k = 1; k < 3u; k++) {
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
              1u << (ui.page <= PG_TONE ? B_EDIT : ui.page <= PG_SPACE ? B_ENV : ui.page <= PG_MENV2 ? B_LFO :
                     ui.page == PG_MATRIX ? B_GLO : ui.page == PG_COLOR ? B_FX : B_SEQ), keys);
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
        if (shed)                        /* the overload guard is shedding (SETUP tells more) */
            cv_text(160, 2, &FONT_XS, "LOAD", C_WHITE);
        if (freeze && set.slot != SLOT_PAD)
            cv_text(196, 16, &FONT_XS, "FREEZE", C_WHITE);
    }
    cv_blit(0, 0);
}

/* HARM's pages: its first 32 harmonics as bars (their levels before the envelopes, -48 .. 0 dB), those an envelope
 * moves marked (this page's in white), and on ENV A-D that envelope's shape */
static void draw_harmonics(void)
{
    const aet_params_t *p = &eng.p;
    uint32_t h, k, env = ui.page >= PG_ENVA && ui.page <= PG_ENVD ? (uint32_t)(ui.page - PG_ENVA) : 99u;
    float top = 0.0f, lv[33];
    cv_begin(240, SPEC_H, C_BLACK);
    for (h = 1; h <= 32u; h++) {
        lv[h] = h <= p->h_count ? aet_harm_level(p, h, 1.0f) : 0.0f;
        top = fm_maxf(top, lv[h]);
    }
    for (h = 1; h <= 32u; h++) {
        int32_t x = (int32_t)(h - 1u) * 15 / 2 + 1, ht;
        uint16_t c = C_HI;
        float db = lv[h] > 0.0f ? 6.0206f * fm_log2f(lv[h] / top) : -99.0f;
        for (k = 0; k < AET_HENV; k++)
            if (aet_harm_takes(p->h_target[k], h))
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
    if (env < AET_HENV && p->h_target[env]) {   /* the envelope: 0 .. its attack + decay + 1 s, top right */
        float span = p->h_att[env] + p->h_dec[env] + 1.0f;
        int32_t x0 = 120, w = 116, y0 = 2, hh = 34, i, py = y0 + hh;
        cv_rect(x0, y0 + hh, w, 1, C_LINE);
        for (i = 0; i <= w; i++) {
            int32_t y = y0 + hh - (int32_t)(aet_harm_env(p, env, span * (float)i / (float)w) * (float)hh);
            if (i)
                cv_line(x0 + i - 1, py, x0 + i, y, C_WHITE);
            py = y;
        }
    }
    cv_blit(0, SPEC_Y);
}

/* what a source moves, from the matrix, as lines "> FOCUS +73%" from y down (at most four) */
static void draw_targets(uint32_t src, int32_t y)
{
    uint32_t k, n = 0;
    char t[32];
    for (k = 0; k < NMX && n < 4u; k++) {
        const uint8_t *m = &set.v[P_MX(k, 0)];
        if (m[0] != src || !m[1])
            continue;
        t[0] = 0;
        text_cat(t, "> ");
        text_cat(t, DEST_NAME[m[1] % NDEST]);
        {
            char a[12];
            text_cat(t, " ");
            value_text(P_MX(k, 2), a);
            text_cat(t, a);
        }
        cv_text(4, y + (int32_t)n * 12, &FONT_XS, t, C_WHITE);
        n++;
    }
    if (!n)
        cv_text(4, y, &FONT_XS, "> NOTHING YET: SET IT ON THE MATRIX (GLO)", C_DIM);
}

/* LFO 1-3: one cycle of the wave and where it is now; MOD ENV 1-2: the ADSR and where it is; both: what they move */
static void draw_mod(void)
{
    int is_env = ui.page == PG_MENV1 || ui.page == PG_MENV2;
    uint32_t k = is_env ? ui.page - PG_MENV1 : ui.page - PG_LFO1;
    int32_t i, mid = 62, amp = 34, py = mid;
    const uint8_t *v = is_env ? &set.v[P_MENV(k, 0)] : &set.v[P_LFO(k, 0)];
    float a = 0, d = 0, sus = 0, r = 0, span = 1;
    cv_begin(240, SPEC_H, C_BLACK);
    if (is_env) {                        /* attack, decay, a held stretch, release, on one time scale */
        a = attack_of((float)v[0]);
        d = release_of((float)v[1]);
        sus = unit((float)v[2]);
        r = release_of((float)v[3]);
        span = a + d + r + 0.25f * (a + d + r);
        mid = SPEC_H - 6;
        amp = 64;
    }
    cv_rect(0, mid, 240, 1, C_LINE);
    for (i = 0; i <= 236; i++) {
        float ph = (float)i / 236.0f, y;
        if (is_env) {
            float t_ = ph * span, hold = 0.25f * (a + d + r);
            y = t_ < a ? t_ / a : t_ < a + d ? 1.0f - (1.0f - sus) * (t_ - a) / d : t_ < a + d + hold ? sus :
                fm_maxf(0.0f, sus * (1.0f - (t_ - a - d - hold) / r));
        } else {
            y = v[1] >= LFO_SH ? fm_sinf(FM_TWO_PI * ph * 3.0f) * 0.6f + fm_sinf(FM_TWO_PI * ph * 7.0f + 1.0f) * 0.3f
                               : lfo_wave(v[1], ph, k);
        }
        {
            int32_t yy = mid - (int32_t)(y * (float)amp);
            if (i)
                cv_line(i + 1, py, i + 2, yy, C_HI);
            py = yy;
        }
    }
    if (is_env) {
        int32_t yy = mid - (int32_t)(menv[k].level * (float)amp);
        cv_rect(2, yy, 236, 1, C_WHITE);
    } else {
        int32_t x = (int32_t)(lfo[k].ph * 236.0f) + 1, yy = mid - (int32_t)(lfo[k].out * (float)amp);
        cv_rect(x - 2, yy - 2, 5, 5, C_WHITE);
    }
    draw_targets(is_env ? MS_ENV1 + k : MS_LFO1 + k, 2);
    cv_blit(0, SPEC_Y);
}

/* MATRIX: the eight slots, SOURCE > DEST and AMOUNT, each with what it adds now; the one the knobs edit lit */
static void draw_matrix(void)
{
    uint32_t k, sel = set.v[P_MSLOT] % NMX;
    char t[16];
    cv_begin(240, SPEC_H, C_BLACK);
    for (k = 0; k < NMX; k++) {
        const uint8_t *m = &set.v[P_MX(k, 0)];
        int32_t y = 2 + (int32_t)k * 13, on = m[0] && m[1];
        uint16_t c = k == sel ? C_WHITE : on ? C_HI : C_DIM;
        if (k == sel)
            cv_rect(0, y - 1, 240, 12, C_LINE);
        t[0] = (char)('1' + k);
        t[1] = 0;
        cv_text(4, y, &FONT_XS, t, c);
        cv_text(16, y, &FONT_XS, MS_NAME[m[0] % NMS], c);
        cv_text(84, y, &FONT_XS, DEST_NAME[m[1] % NDEST], c);
        value_text(P_MX(k, 2), t);
        cv_text(162, y, &FONT_XS, on ? t : "", c);
        if (on) {                        /* what it adds now: a bar from the middle, -100 .. +100 % */
            float now = bipolar((float)m[2]) * msrc[m[0] % NMS];
            int32_t w = (int32_t)(now * 18.0f);
            cv_rect(220, y + 4, 1, 4, C_GRAY);
            cv_rect(w >= 0 ? 220 : 220 + w, y + 5, w >= 0 ? w + 1 : -w, 2, c);
        }
    }
    cv_blit(0, SPEC_Y);
}

/* SETUP: the second core, what it does now, and the overload guard */
static void draw_setup(void)
{
    char t[40];
    cv_begin(240, SPEC_H, C_BLACK);
    cv_text(4, 2, &FONT_XS, "2ND CORE (EXPERIMENTAL)", C_GRAY);
    cv_text(4, 16, &FONT_B, core2_state_text(), core2_running() ? C_HI : C_WHITE);
    cv_text(4, 38, &FONT_XS, set.v[P_CORE2] != core2_wanted_at_boot ? "SAVE, THEN SWITCH OFF AND ON" :
                             "IT STARTS AT POWER-ON ONLY", C_DIM);
    t[0] = 0;
    text_cat(t, "SPLIT FRAMES ");
    int_text((int32_t)eng.par_jobs, t + 13);
    cv_text(4, 54, &FONT_XS, t, C_DIM);
    t[0] = 0;
    text_cat(t, shed ? "OVERLOAD: SHEDDING, STEP " : "OVERLOAD GUARD: IDLE");
    if (shed) {
        char n[4];
        int_text(shed, n);
        text_cat(t, n);
    }
    cv_text(4, 78, &FONT_XS, t, shed ? C_WHITE : C_DIM);
    t[0] = 0;
    text_cat(t, "CPU ");
    int_text((int32_t)plat_cpu_pct(), t + 4);
    text_cat(t, "%");
    cv_text(4, 92, &FONT_XS, t, C_DIM);
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
    if (ui.page >= PG_LFO1 && ui.page <= PG_MENV2) {
        draw_mod();
        return;
    }
    if (ui.page == PG_MATRIX) {
        draw_matrix();
        return;
    }
    if (ui.page == PG_SETUP) {
        draw_setup();
        return;
    }
    cv_begin(240, SPEC_H, C_BLACK);
    for (b = 0; b < 8u; b++)             /* octaves: 80, 160 .. 10240 Hz */
        cv_rect((int32_t)((float)(b + 1u) * ((float)AET_DISP / 8.643856f) * 4.0f), 6, 1, SPEC_H - 6, C_LINE);
    for (b = 0; b < AET_DISP; b++) {
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
        if (p == P_NONE)
            continue;
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
        int_text(aet_active(&eng), t);
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
    {
        int moving = 0;
        uint32_t k;
        for (k = 0; k < NMX; k++)
            moving |= set.v[P_MX(k, 0)] && set.v[P_MX(k, 1)];
        if (now - last_t > (moving ? 60u : 500u)) {
            last_t = now;                /* (faster while something is modulated: the markers move) */
            if (++n & 1u || moving)
                draw_params();
        }
        if (shed && now - shed_at > 3000u) {   /* the overload guard: a step back after three quiet seconds */
            shed--;
            shed_at = now;
            ui.redraw = 1;
        }
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
        *got = aet_active(&eng);
    else if (!strcmp(what, "late"))
        *got = (int)eng.late;
    else if (!strcmp(what, "dirty"))
        *got = set_dirty;
    else if (!strcmp(what, "user_len"))   /* the loaded sample's length, 0: none */
        *got = user_sample() ? (int)user_sample()->len : 0;
    else if (!strcmp(what, "sysex_errors"))   /* sample-load commands answered with an error */
        *got = (int)up_errors;
    else if (!strcmp(what, "shed"))       /* the overload guard's step */
        *got = shed;
    else if (!strncmp(what, "src", 3) && what[3] >= '0' && what[3] <= '9')   /* srcN: matrix source N, x100 */
        *got = (int)(msrc[atoi(what + 3) % NMS] * 100.0f);
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
    if (!strcmp(cmd, "overload")) {      /* "overload": as the platform says when a block took over 90 % */
        app_overload();
        return 1;
    }
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
