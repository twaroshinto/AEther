/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 fm1-aether contributors
 * AI-generated code (Claude, Anthropic): no warranty, use at your own risk (see NOTICE). */
/* AETHER's VA source: one virtual-analog oscillator that runs all the time at C4 and feeds the engine's LIVE source
 * (aet_live_push). The engine smears its newest window and moves it to each key's pitch, so one cheap monophonic
 * voice plays every note of a chord. Because the phases are thrown away, only the spectrum matters.
 *
 * The filter is not here: it is the engine's spectral low-pass (aet_params_t lp_hz, lp_reso), applied after the
 * voices are summed. So its cutoff is the same for every key (in time, a C4 filter moved up with the note would make
 * high notes ever brighter), its slope is 24 dB/octave, and nothing is driven into distortion.
 *
 *   wave    an anti-aliased (polyBLEP) saw morphing into a square, then into a narrowing pulse
 *   motion  a slow LFO with a random drift on the cutoff (va_render gives the cutoff of the moment: fc_now)
 * Float only, no libc (fastmath.h): about 20 operations a sample. */
#pragma once
#include <stdint.h>

typedef struct {
    float shape;                         /* 0 saw .. 0.5 square .. 1 narrow pulse */
    float cutoff;                        /* 0 .. 1: 60 Hz .. 18 kHz */
    float motion;                        /* 0 .. 1: the LFO's depth and speed */
    float fc_now;                        /* out: the cutoff in Hz with MOTION, for the engine's lp_hz */
    float ph, lfo, drift, drift_to;
    uint32_t rng, n;
} va_t;

void va_init(va_t *v);
void va_render(va_t *v, float *out, uint32_t n);   /* n samples at 44.1 kHz, about -6 dBFS */
float va_cutoff_hz(float cutoff);        /* the knob's value in Hz (for the screen) */
