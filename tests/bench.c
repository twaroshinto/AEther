/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 fm1-aether contributors
 * AI-generated code (Claude, Anthropic): no warranty, use at your own risk (see NOTICE). */
/* What a frame costs, stage by stage, on this machine: the engine compiled in (its static steps timed one by one),
 * driven as app_render drives it, block by block. The FM-1's core is slower and its ratios differ, but where the
 * time goes and how even the blocks are is what this shows.
 *   cc -O2 -ffp-contract=off -Iapp/dsp -Ibuild/test/gen tests/bench.c app/dsp/va.c -lm -o build/test/bench */
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "../app/dsp/aether.c"
#include "va.h"
#include "aether_source.h"

#define BLOCK 256u
static float ram[AET_RAM_FLOATS];
static aet_t eng;
static double st_ns[S_DONE + 1], st_units[S_DONE + 1];

static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}

/* work() with each step timed by the stage it ran in */
static void work_timed(aet_t *e, uint32_t budget)
{
    uint32_t spent = 0;
    while (e->st != S_DONE && spent < budget) {
        uint32_t st = e->st;
        double t0 = now_ns();
        e->units = 0;
        step(e);
        st_ns[st] += now_ns() - t0;
        st_units[st] += e->units;
        spent += e->units;
        e->units_frame += e->units;
    }
}

static void render_timed(aet_t *e, float *l, float *r, uint32_t n)   /* aet_render, with work_timed */
{
    uint32_t i = 0;
    uint32_t est = e->units_est > e->units_bound ? e->units_est : e->units_bound;
    uint32_t budget = (uint32_t)((float)est * (float)n * 1.25f / (float)e->play_len) + CHUNK;
    work_timed(e, budget);
    while (i < n) {
        uint32_t take = e->play_len - e->play_pos, k;
        if (take > n - i)
            take = n - i;
        for (k = 0; k < take; k++) {
            l[i + k] = ceiling(e->play[0][e->play_pos + k]);
            r[i + k] = ceiling(e->play[1][e->play_pos + k]);
        }
        i += take;
        e->play_pos += take;
        if (e->play_pos >= e->play_len) {
            if (e->st != S_DONE) {
                e->late++;
                work_timed(e, 0xFFFFFFFFu);
            }
            swap(e);
        }
    }
}

static const char *ST_NAME[] = {"WIN", "LOAD", "AREV", "AFFT", "MAG", "SBR1", "SBR2", "SPR1", "SPR2", "CLEAR",
                                "VOICE", "PHASE", "SREV", "SFFT", "DONE"};

static void run(const char *name, int va_src, float secs)
{
    static float l[BLOCK], r[BLOCK];
    va_t va;
    uint32_t blocks = (uint32_t)(secs * 44100.0f / BLOCK), b, k;
    double total = 0, peak = 0, block_ns = BLOCK * 1e9 / 44100.0, frames0 = eng.frames;
    memset(st_ns, 0, sizeof st_ns);
    memset(st_units, 0, sizeof st_units);
    va_init(&va);
    for (b = 0; b < blocks; b++) {
        double t0 = now_ns(), dt;
        if (va_src) {
            va_render(&va, l, BLOCK);
            aet_live_push(&eng, l, BLOCK);
            eng.p.lp_hz = va.fc_now;
        }
        render_timed(&eng, l, r, BLOCK);
        dt = now_ns() - t0;
        if (b > blocks / 4u) {           /* (after the voices have come in) */
            total += dt;
            if (dt > peak)
                peak = dt;
        }
    }
    {
        double fr = (double)(eng.frames - frames0), all = 0;
        for (k = 0; k <= S_DONE; k++)
            all += st_ns[k];
        printf("%s: n %u, %u frames, late %u\n", name, eng.n, (unsigned)fr, eng.late);
        printf("  block: mean %.1f %%, peak %.1f %% of real time (this machine)\n",
               100.0 * total / (blocks - blocks / 4u - 1u) / block_ns, 100.0 * peak / block_ns);
        for (k = 0; k <= S_DONE; k++)
            if (st_ns[k] > 0)
                printf("  %-6s %5.1f %%  %7.0f units/frame  %5.2f ns/unit\n", ST_NAME[k], 100.0 * st_ns[k] / all,
                       st_units[k] / fr, st_units[k] ? st_ns[k] / st_units[k] : 0.0);
    }
}

static void setup(int src)
{
    int32_t i;
    aet_init(&eng, ram);
    aet_set_sample(&eng, aet_builtin[0].data, aet_builtin[0].len, aet_builtin[0].rate, AET_FMT_ADPCM);
    aet_set_env(&eng, aet_builtin[0].env, aet_builtin[0].env_frames, aet_builtin[0].env_hop);
    eng.p.source = src;
    eng.p.size = AET_SIZE_8K;
    eng.p.spread = 0.4f;                 /* as GLACIER: SPREAD and TILT moving, SHIMMER from an envelope */
    eng.p.shimmer = 0.4f;
    eng.p.tilt = -0.2f;
    eng.p.eq_type[2] = AET_EQ_HISHELF;   /* and a high shelf, as ten presets have */
    eng.p.eq_hz[2] = 5000.0f;
    eng.p.eq_db[2] = -4.0f;
    eng.p.eq_oct[2] = 1.0f;
    for (i = 0; i < 8; i++)
        aet_note_on(&eng, 48 + i * 4, 0.8f);
}

int main(void)
{
    setup(AET_SRC_SAMPLE);
    run("SAMPLE (CHOIR, 8 voices, SPREAD, SHIMMER, EQ)", 0, 20.0f);
    setup(AET_SRC_LIVE);
    eng.p.lp_reso = 0.3f;
    run("VA (LIVE, 8 voices, SPREAD, SHIMMER, EQ, low-pass)", 1, 20.0f);
    return 0;
}
