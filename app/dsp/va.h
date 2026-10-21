/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 fm1-smuga contributors */
/* SMUGA's VA source: one virtual-analog voice that runs all the time at C4 and feeds the engine's LIVE source
 * (smg_live_push). The engine smears its newest window and moves it to each key's pitch, so one cheap monophonic
 * voice plays every note of a chord. Because the phases are thrown away, what matters is the spectrum: the wave's
 * harmonics, the filter's slope and resonant peak, and how they move (MOTION).
 *
 *   wave    an anti-aliased (polyBLEP) saw morphing into a square, then into a narrowing pulse
 *   filter  a resonant 12 dB low-pass (a trapezoidal state-variable filter), driven gently
 *   motion  a slow LFO on the cutoff with a random drift, the cloud's own life
 * Float only, no libc (fastmath.h): about 40 operations a sample. */
#pragma once
#include <stdint.h>

typedef struct {
    float shape;                         /* 0 saw .. 0.5 square .. 1 narrow pulse */
    float cutoff;                        /* 0 .. 1: 60 Hz .. 18 kHz */
    float reso;                          /* 0 .. 1 */
    float motion;                        /* 0 .. 1: the LFO's depth and speed */
    float ph, ic1, ic2;                  /* the oscillator, the filter's state */
    float lfo, drift, drift_to;
    uint32_t rng, n;
} va_t;

void va_init(va_t *v);
void va_render(va_t *v, float *out, uint32_t n);   /* n samples at 44.1 kHz, about -6 dBFS */
float va_cutoff_hz(float cutoff);        /* the knob's value in Hz (for the screen) */
