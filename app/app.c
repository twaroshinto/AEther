/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 fm1-smuga contributors */
/* SMUGA on the FM-1: the panel, the screen, MIDI and the sample loader around the engine (dsp/smuga.c).
 * Part of fm1-base's unity build (base/device/unity.c, base/host/plat_host.c), after the platform: gfx.c, announce.c
 * and lib/touch.c are in scope. Structure from fm1-base's template.
 *
 * The panel:
 *   keys            notes (velocity from the keys); OCT- / OCT+ the octave
 *   K1 .. K4        the page's four parameters
 *   SEL, SELECT     the next page: SOURCE, SPACE, COLOR (EDIT, ENV and FX go straight to each)
 *   ALGORITHM       the source: PAD (harmonic clouds), CHOIR (the built-in sample), USER (a sample loaded over USB)
 *   PRESET          a factory preset
 *   PLAY            HOLD: the notes stay after the keys are let go (a new chord replaces them)
 *   REC             FREEZE: the sample's scan stops, the spectrum held
 *   HOME            all notes off (and HOLD off)
 *   SAVE            the settings kept across a power cycle
 * MIDI in (USB, TRS): notes on any channel, CC64 sustain, CC1 POSITION, CC71 SPREAD, CC74 TILT. Notes from the keys
 * go out over USB. */
#include "fastmath.h"
#include "smuga.h"
#include "smuga_source.h"                /* the built-in source (tools/gen_source.py, at build time) */

/* ------------------------------------------------------------- parameters --- */
enum { P_STRETCH, P_POSITION, P_SPREAD, P_SIZE, P_BRIGHT, P_BAND, P_INHARM, P_ODD,
       P_ATTACK, P_RELEASE, P_WIDTH, P_LEVEL, P_TILT, P_SUB, P_SHIMMER, NP };
typedef struct {
    const char *name;
    uint8_t max;                         /* the values are 0 .. max (127, or the sizes 0 .. 3) */
} pdef_t;
static const pdef_t PDEF[NP] = {
    {"STRETCH", 127}, {"POSITION", 127}, {"SPREAD", 127}, {"SIZE", SMG_NSIZE - 1u},
    {"BRIGHT", 127}, {"BAND", 127}, {"INHARM", 127}, {"ODD", 127},
    {"ATTACK", 127}, {"RELEASE", 127}, {"WIDTH", 127}, {"LEVEL", 127},
    {"TILT", 127}, {"SUB", 127}, {"SHIMMER", 127},
};

enum { SLOT_PAD, SLOT_CHOIR, SLOT_USER, NSLOT };
static const char *const SLOT_NAME[NSLOT] = {"PAD", "CHOIR", "USER"};
enum { PG_SOURCE, PG_SPACE, PG_COLOR, NPAGE };
static const char *const PAGE_NAME[NPAGE] = {"SOURCE", "SPACE", "COLOR"};
static const uint8_t PAGE_SAMPLE[4] = {P_STRETCH, P_POSITION, P_SPREAD, P_SIZE};
static const uint8_t PAGE_PAD[4] = {P_BRIGHT, P_BAND, P_INHARM, P_ODD};
static const uint8_t PAGE_SPACE[4] = {P_ATTACK, P_RELEASE, P_WIDTH, P_LEVEL};
static const uint8_t PAGE_COLOR[4] = {P_TILT, P_SUB, P_SHIMMER, P_SIZE};

typedef struct {
    const char *name;
    uint8_t slot;
    uint8_t v[NP];
} preset_t;
/*                                       STR POS SPR SZ  BRI BND INH ODD ATK REL WID LEV TLT SUB SHM */
static const preset_t PRESETS[] = {
    {"GLACIER", SLOT_CHOIR,             {52,   0,  0, 3, 64, 50,  0,  0, 80, 88, 100, 96, 64,  0,  0}},
    {"FOG", SLOT_CHOIR,                 {80,  30, 70, 3, 64, 50,  0,  0, 90, 96, 110, 96, 44, 40,  0}},
    {"HALO", SLOT_PAD,                  {52,   0,  0, 3, 80, 40,  0,  0, 85, 92, 110, 90, 64,  0, 50}},
    {"ORGAN DUST", SLOT_PAD,            {52,   0,  0, 2, 50, 18,  0, 100, 50, 70, 80, 96, 64, 30,  0}},
    {"BELL CLOUD", SLOT_PAD,            {52,   0,  0, 3, 90, 30, 90,  0, 60, 96, 110, 90, 70,  0, 20}},
    {"GRAIN", SLOT_CHOIR,               {30,  20, 20, 0, 64, 50,  0,  0, 20, 60, 70, 96, 64,  0,  0}},
    {"FROZEN", SLOT_CHOIR,              {127, 64, 90, 3, 64, 50,  0,  0, 100, 110, 127, 96, 50, 20, 30}},
    {"DEEP", SLOT_PAD,                  {52,   0,  0, 3, 25, 80,  0, 30, 95, 105, 100, 100, 40, 80,  0}},
};
#define NPRESET (sizeof PRESETS / sizeof PRESETS[0])

#define SET_MAGIC 0x31474D53u            /* "SMG1": the settings record; another magic loads a preset */
typedef struct {
    uint32_t magic;
    uint8_t v[NP];
    uint8_t slot, preset;
    int8_t octave;
    uint8_t rsv[3];
} settings_t;
static settings_t set;
static uint8_t set_dirty;

/* ------------------------------------------------------------------ engine --- */
static smg_t eng;
static float eng_ram[SMG_RAM_FLOATS] PLAT_POOL;
static uint8_t freeze, hold;

static float stretch_of(uint32_t v) { return v >= 127u ? 0.0f : fm_exp2f((float)v * (13.0f / 126.0f)); }
static float attack_of(uint32_t v) { return 0.01f * fm_exp2f((float)v * (11.0f / 127.0f)); }    /* 10 ms .. 20 s */
static float release_of(uint32_t v) { return 0.05f * fm_exp2f((float)v * (9.25f / 127.0f)); }   /* 50 ms .. 30 s */
static float band_of(uint32_t v) { float x = (float)v / 127.0f; return 1.0f + 299.0f * x * x; }  /* cents */
static float unit(uint32_t v) { return (float)v / 127.0f; }

/* the knobs into the engine's parameters (main loop; the engine takes a copy at each frame) */
static void apply(void)
{
    smg_params_t *p = &eng.p;
    const uint8_t *v = set.v;
    p->source = set.slot == SLOT_PAD ? SMG_SRC_PAD : SMG_SRC_SAMPLE;
    p->size = v[P_SIZE];
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
}

/* the loaded sample in the flash, checked: its header, 0 = none */
static const smp_hdr_t *user_sample(void)
{
    const smp_hdr_t *h = (const smp_hdr_t *)plat_data(SMP_HDR_OFF);
    if (!h || h->magic != SMP_MAGIC || !h->len || h->len * 2u > SMP_MAX_BYTES || h->rate < 4000u || h->rate > 96000u)
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
static void source_set(uint32_t slot)    /* audio interrupt */
{
    const smp_hdr_t *h = slot == SLOT_USER && !uploading ? user_sample() : 0;
    if (h)
        smg_set_sample(&eng, (const int16_t *)plat_data(SMP_DATA_OFF), h->len, h->rate);
    else
        smg_set_sample(&eng, smg_source, SMG_SOURCE_LEN, SMG_SOURCE_RATE);
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
        set.v[k] = p->v[k];
    set.preset = (uint8_t)(i % NPRESET);
    if (set.slot != SLOT_USER || p->slot == SLOT_PAD)   /* a sample preset keeps a loaded sample */
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
        set.slot = SLOT_CHOIR;
        preset_load(0);
    }
    if (set.slot == SLOT_USER && !user_sample())
        set.slot = SLOT_CHOIR;
    smg_init(&eng, eng_ram);
    apply();
    source_set(set.slot);
}

/* ------------------------------------------------------------- sample load --- */
/* SysEx F0 7D 53 4D cmd ... F7 (tools/smuga_load.py); every command answered F0 7D 53 4D 7F cmd status F7.
 *   01 BEGIN  len (samples, 4 x 7 bits), rate (3 x 7 bits), name (up to 15 ASCII)
 *   02 DATA   byte offset (4 x 7 bits), then the bytes packed 7 in 8 (the first of each 8 holds the next seven's top
 *             bits); in order, at most 512 bytes a message
 *   03 END    CRC-32 of the bytes (5 x 7 bits): checked against the flash, then the header written
 * Status: 0 ok, 1 not now, 2 too long, 3 out of order, 4 the flash refused, 5 CRC mismatch */
static struct {
    uint32_t len, rate, next;            /* samples; Hz; the next byte expected */
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
    if (n < 11u)
        return 1;
    up.len = rd7(p + 4, 4);
    up.rate = rd7(p + 8, 3);
    if (!up.len || up.len * 2u > SMP_MAX_BYTES || up.rate < 4000u || up.rate > 96000u)
        return 2;
    for (k = 0; k < 15u && 11u + k < n; k++)
        up.name[k] = (char)p[11u + k];
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
    if (off + m > up.len * 2u)
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
    if (!uploading || n < 9u || up.next != up.len * 2u)
        return 1;
    want = rd7(p + 4, 5);
    uploading = 0;
    if (!d || crc32_add(0, d, up.len * 2u) != want) {
        app_say("LOAD", "FAILED");
        return 5;
    }
    h.magic = SMP_MAGIC;
    h.rate = up.rate;
    h.len = up.len;
    h.crc = want;
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
            float s = stretch_of(v);
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
        tenths_text(p == P_ATTACK ? attack_of(v) : release_of(v), b);
        text_cat(b, " S");
        break;
    case P_BAND:
        int_text((int32_t)(band_of(v) + 0.5f), b);
        text_cat(b, " CT");
        break;
    case P_TILT: {
        int32_t db = (int32_t)((2.0f * unit(v) - 1.0f) * 60.0f);   /* tenths of dB per octave */
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
        int_text((int32_t)(unit(v) * 100.0f + 0.5f), b);
        text_cat(b, "%");
        break;
    }
}

static const uint8_t *page_params(void)
{
    if (ui.page == PG_SPACE)
        return PAGE_SPACE;
    if (ui.page == PG_COLOR)
        return PAGE_COLOR;
    return set.slot == SLOT_PAD ? PAGE_PAD : PAGE_SAMPLE;
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
        text_cat(t, SLOT_NAME[set.slot]);
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
    apply();
    ev_push(Q_SOURCE, slot, 0);
    if (slot == SLOT_USER)
        app_say("SOURCE", ((const smp_hdr_t *)plat_data(SMP_HDR_OFF))->name);
    else
        app_say("SOURCE", SLOT_NAME[slot]);
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
        case B_SEL: page_set(ui.page + 1u); break;
        case B_EDIT: page_set(PG_SOURCE); break;
        case B_ENV: page_set(PG_SPACE); break;
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
        page_set((uint32_t)((int32_t)ui.page + NPAGE * 4 + e));
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
              1u << (ui.page == PG_SOURCE ? B_EDIT : ui.page == PG_SPACE ? B_ENV : B_FX), keys);
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
        x = cv_text(x + 10, 10, &FONT_XS, SLOT_NAME[set.slot], C_HI);
        cv_text(x + 8, 10, &FONT_XS, PRESETS[set.preset].name, C_GRAY);
        if (hold)
            cv_text(196, 2, &FONT_XS, "HOLD", C_HI);
        if (freeze && set.slot != SLOT_PAD)
            cv_text(196, 16, &FONT_XS, "FREEZE", C_WHITE);
    }
    cv_blit(0, 0);
}

/* the spectrum of the last frame, log frequency (40 Hz .. 16 kHz), -72 .. -6 dB; the sample's scan along the top */
static void draw_spectrum(void)
{
    uint32_t b;
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
    if (set.slot != SLOT_PAD && eng.smp.len) {
        int32_t x = (int32_t)(eng.scan / (float)eng.smp.len * 238.0f), x0 = (int32_t)(unit(set.v[P_POSITION]) * 238.0f);
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
    for (k = 0; k < NPAGE; k++) {        /* the pages, this one lit */
        int32_t x = 4 + (int32_t)k * 60;
        cv_text(x, 0, &FONT_XS, PAGE_NAME[k], k == ui.page ? C_HI : C_DIM);
    }
    for (k = 0; k < 4u; k++) {           /* K1 .. K4: name, value, a bar */
        int32_t x = 4 + (int32_t)(k % 2u) * 120, y = 16 + (int32_t)(k / 2u) * 38, w;
        uint32_t p = pp[k];
        int lit = ui.touched == k && now - ui.touched_at < 600u;
        cv_text(x, y, &FONT_XS, PDEF[p].name, lit ? C_WHITE : C_GRAY);
        value_text(p, t);
        cv_text(x, y + 12, &FONT_B, t, lit ? C_WHITE : C_HI);
        w = (int32_t)set.v[p] * 108 / PDEF[p].max;
        cv_rect(x, y + 31, 108, 2, C_LINE);
        cv_rect(x, y + 31, w, 2, lit ? C_WHITE : C_HI);
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
        cv_text(4, 83, &FONT_XS, f, C_DIM);
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
    if (now - last_t > 500u) {
        last_t = now;
        if (++n & 1u)
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
