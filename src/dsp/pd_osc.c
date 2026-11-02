/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
#include "pd_osc.h"

/* ---------------------------------------------------------- oscillator --- */

uint32_t pd_pitch_inc(int32_t pitch)
{
    pitch = pd_clamp(pitch, 0, 127 << 8);
    uint32_t n = (uint32_t)pitch >> 8, f = (uint32_t)pitch & 255u;
    if (n == 127)
        return PD_NOTE_INC[127];
    uint32_t a = PD_NOTE_INC[n], b = PD_NOTE_INC[n + 1];
    return a + ((b - a) >> 8) * f;
}

/* a / b in Q15 for a < b, without 64-bit arithmetic: both are shifted so b fits in 16 bits */
static int32_t frac_q15(uint32_t a, uint32_t b)
{
    unsigned s = b >> 16 ? 16u - (unsigned)__builtin_clz(b) : 0u;
    return (int32_t)(((a >> s) << 15) / (b >> s));
}

/* polyBLEP residual at a step of size 2 (full scale) at phase 0, Q15 */
static int32_t blep(uint32_t ph, uint32_t inc)
{
    if (ph < inc) {                          /* just after the step: -(1 - x)^2 */
        int32_t x = PD_UNIT - frac_q15(ph, inc);
        return -pd_mulq15(x, x);
    }
    if (ph + inc < inc) {                    /* just before it (ph + inc wraps): y^2 */
        int32_t y = frac_q15(ph + inc, inc);
        return pd_mulq15(y, y);
    }
    return 0;
}

void pd_osc_render(pd_osc *o, pd_sample *out, unsigned frames)
{
    uint32_t ph = o->phase, inc = o->inc;
    switch (o->wave) {
    case PD_WAVE_SAW:
        for (unsigned i = 0; i < frames; i++, ph += inc)
            out[i] = (int32_t)(ph >> 16) - 32768 - blep(ph, inc);
        break;
    case PD_WAVE_SQUARE:
        for (unsigned i = 0; i < frames; i++, ph += inc)
            out[i] = (ph < 0x80000000u ? 32767 : -32768) + blep(ph, inc) - blep(ph ^ 0x80000000u, inc);
        break;
    case PD_WAVE_TRIANGLE:                   /* plain: its harmonics fall at 12 dB/oct, little to alias */
        for (unsigned i = 0; i < frames; i++, ph += inc) {
            int32_t t = (int32_t)(ph >> 16);
            out[i] = t < 32768 ? 2 * t - 32768 : 98303 - 2 * t;
        }
        break;
    default:
        for (unsigned i = 0; i < frames; i++, ph += inc)
            out[i] = pd_sine(ph);
        break;
    }
    o->phase = ph;
}

/* ------------------------------------------------------------ envelope --- */

static int32_t env_step(int32_t time)
{
    return (int32_t)(PD_ENV_FULL / pd_lut32(PD_TIME, pd_clamp(time, 0, PD_PARAM_MAX)));
}

void pd_env_set(pd_env *e, int32_t attack, int32_t decay, int32_t sustain, int32_t release)
{
    e->a_step = env_step(attack);
    e->d_step = env_step(decay);
    e->r_step = env_step(release);
    e->sustain = pd_clamp(sustain, 0, PD_PARAM_MAX) << 15;
}

void pd_env_gate(pd_env *e, int on)
{
    if (on)
        e->stage = PD_ENV_ATTACK;            /* from where it is: no click on a retrigger */
    else if (e->stage != PD_ENV_IDLE)
        e->stage = PD_ENV_RELEASE;
}

int32_t pd_env_block(pd_env *e, unsigned frames)
{
    int32_t n = (int32_t)frames;
    switch (e->stage) {
    case PD_ENV_ATTACK:
        e->level += e->a_step * n;
        if (e->level >= PD_ENV_FULL) {
            e->level = PD_ENV_FULL;
            e->stage = PD_ENV_DECAY;
        }
        break;
    case PD_ENV_DECAY:
        e->level -= e->d_step * n;
        if (e->level <= e->sustain) {
            e->level = e->sustain;
            e->stage = PD_ENV_SUSTAIN;
        }
        break;
    case PD_ENV_SUSTAIN:
        e->level = e->sustain;
        break;
    case PD_ENV_RELEASE:
        e->level -= e->r_step * n;
        if (e->level <= 0) {
            e->level = 0;
            e->stage = PD_ENV_IDLE;
        }
        break;
    default:
        e->level = 0;
        break;
    }
    int32_t l = e->level >> 15;
    return (l * l) >> 15;
}

/* -------------------------------------------------------------- filter --- */

void pd_svf_set(pd_svf *s, int32_t cutoff, int32_t resonance)
{
    s->f = pd_lut16(PD_SVF_F, pd_clamp(cutoff, 0, PD_PARAM_MAX));
    s->q = 65536 - ((pd_clamp(resonance, 0, PD_PARAM_MAX) * 62259) >> 15);   /* damping 2.0 .. 0.1 */
}

#define SVF_LIM 65535                         /* keeps every product below 2^31 */

void pd_svf_process(pd_svf *s, const pd_sample *in, pd_sample *out, unsigned frames)
{
    int32_t low = s->low, band = s->band, f = s->f, q2 = s->q >> 1;
    for (unsigned i = 0; i < frames; i++) {
        low = pd_clamp(low + pd_mulq15(f, band), -SVF_LIM, SVF_LIM);
        int32_t high = pd_clamp(in[i] - low - ((q2 * band) >> 14), -SVF_LIM, SVF_LIM);
        band = pd_clamp(band + pd_mulq15(f, high), -SVF_LIM, SVF_LIM);
        out[i] = s->mode == PD_SVF_LP ? low : s->mode == PD_SVF_BP ? band : high;
    }
    s->low = low;
    s->band = band;
}
