/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Building blocks: a band-limited oscillator, an ADSR envelope, a state-variable filter. */
#ifndef PD_OSC_H
#define PD_OSC_H

#include "pd_dsp.h"

/* ---------------------------------------------------------- oscillator --- */
enum { PD_WAVE_SAW, PD_WAVE_SQUARE, PD_WAVE_TRIANGLE, PD_WAVE_SINE, PD_WAVE_COUNT };

typedef struct {
    uint32_t phase, inc;
    int32_t tri;               /* the triangle's leaky integrator */
    uint8_t wave;
} pd_osc;

/* pitch in 1/256 semitone from MIDI note 0 (note 69 << 8 is A440) */
uint32_t pd_pitch_inc(int32_t pitch);
void pd_osc_render(pd_osc *o, pd_sample *out, unsigned frames);   /* writes Q15 */

/* ------------------------------------------------------------ envelope --- */
enum { PD_ENV_IDLE, PD_ENV_ATTACK, PD_ENV_DECAY, PD_ENV_SUSTAIN, PD_ENV_RELEASE };

typedef struct {
    int32_t level;             /* 0 .. PD_ENV_FULL */
    int32_t a_step, d_step, r_step, sustain;
    uint8_t stage;
} pd_env;

#define PD_ENV_FULL (1 << 30)

/* times and sustain as 0..PD_PARAM_MAX parameters (times: 1 ms .. 10 s) */
void pd_env_set(pd_env *e, int32_t attack, int32_t decay, int32_t sustain, int32_t release);
void pd_env_gate(pd_env *e, int on);
/* advances `frames` samples and returns the amplitude at the end, Q15 (level squared, for a curve that
 * sounds closer to exponential); the caller ramps across the block */
int32_t pd_env_block(pd_env *e, unsigned frames);

/* -------------------------------------------------------------- filter --- */
enum { PD_SVF_LP, PD_SVF_BP, PD_SVF_HP, PD_SVF_MODES };

typedef struct {
    int32_t low, band;
    int32_t f, q;              /* Q15: frequency coefficient, damping (2 = none .. small = resonant) */
    uint8_t mode;
} pd_svf;

/* cutoff 0..PD_PARAM_MAX (20 Hz .. 20 kHz, exponential), resonance 0..PD_PARAM_MAX */
void pd_svf_set(pd_svf *s, int32_t cutoff, int32_t resonance);
void pd_svf_process(pd_svf *s, const pd_sample *in, pd_sample *out, unsigned frames);

#endif
