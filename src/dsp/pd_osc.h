/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Building blocks: a band-limited oscillator, an ADSR envelope, filters (state-variable, ladder, Sallen-Key). */
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

/* a trapezoidal (zero-delay feedback) state-variable filter: stable at any cutoff and damping */
typedef struct {
    int32_t low, band;         /* the two integrators' states */
    int32_t a1, a2, a3;        /* Q30, from the cutoff and the damping */
    int32_t q;                 /* Q15: damping (2 = none .. small = resonant) */
    uint8_t mode;
} pd_svf;

/* cutoff 0..PD_PARAM_MAX (20 Hz .. 20 kHz, exponential), resonance 0..PD_PARAM_MAX */
void pd_svf_set(pd_svf *s, int32_t cutoff, int32_t resonance);
void pd_svf_process(pd_svf *s, const pd_sample *in, pd_sample *out, unsigned frames);

/* a four-pole ladder low-pass (24 dB / oct), its feedback solved per sample and saturated; resonance 0 .. 4.0,
 * self-oscillating at the top; the bass it loses with resonance is half made up */
typedef struct {
    int32_t s[4];
    int32_t g, g4, c[4], inv;  /* Q30 */
    int32_t k;                 /* Q12 */
} pd_ladder;
void pd_ladder_set(pd_ladder *l, int32_t cutoff, int32_t resonance);
void pd_ladder_process(pd_ladder *l, const pd_sample *in, pd_sample *out, unsigned frames);

/* a Sallen-Key low-pass (12 dB / oct) whose feedback runs through a high-pass and a saturation, as in the
 * well-known semi-modular's; resonance 0 .. 2.0, self-oscillating at the top */
typedef struct {
    int32_t s[3];              /* the low-pass, the second low-pass, the feedback's high-pass */
    int32_t g;                 /* Q30 */
    int32_t alpha, b_lp, b_hp; /* Q29 */
    int32_t k;                 /* Q14 */
} pd_sk;
void pd_sk_set(pd_sk *f, int32_t cutoff, int32_t resonance);
void pd_sk_process(pd_sk *f, const pd_sample *in, pd_sample *out, unsigned frames);

#endif
