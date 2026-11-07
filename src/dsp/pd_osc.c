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

/* n / d in Q30 for 0 <= n <= d < 2^31, by long division (the chip has no 64-bit divide); d = 0 gives 1.0 */
static int32_t div_q30(uint32_t n, uint32_t d)
{
    if (!d || n >= d)
        return 1 << 30;
    uint32_t q = 0;
    for (int b = 0; b < 30; b++) {
        n <<= 1;
        q <<= 1;
        if (n >= d) {
            n -= d;
            q |= 1u;
        }
    }
    return (int32_t)q;
}

static int64_t cutoff_g(int32_t cutoff) { return pd_lut32(PD_SVF_G, pd_clamp(cutoff, 0, PD_PARAM_MAX)); }   /* tan(pi fc / fs), Q24 */

void pd_svf_set(pd_svf *s, int32_t cutoff, int32_t resonance)
{
    int64_t g = cutoff_g(cutoff);
    s->q = 65536 - ((pd_clamp(resonance, 0, PD_PARAM_MAX) * 62259) >> 15);   /* damping 2.0 .. 0.1 */
    uint32_t d = (uint32_t)(65536 + ((g * (g + ((int64_t)s->q << 9))) >> 32));   /* 1 + g (g + k), Q16: < 2^22 */
    int64_t a1 = div_q30(65536, d);
    int64_t a2 = (g * a1) >> 24;
    s->a1 = (int32_t)a1;
    s->a2 = (int32_t)a2;
    s->a3 = (int32_t)((g * a2) >> 24);
}

/* inside the filters a sample carries 8 more bits (x 256), so a low cutoff's tiny steps are not lost to rounding */
#define F_SH 8
#define F_LIM (65535 << F_SH)                  /* the outputs' limit (+-2.0); the states' is twice that */
#define F_IN(x) (pd_clamp((x), -65535, 65535) * (1 << F_SH))
#define F_OUT(x) ((x) >> F_SH)

static inline int32_t flim(int64_t x) { return (int32_t)(x < -F_LIM ? -F_LIM : x > F_LIM ? F_LIM : x); }
static inline int32_t slim(int64_t x) { return (int32_t)(x < -2 * F_LIM ? -2 * F_LIM : x > 2 * F_LIM ? 2 * F_LIM : x); }

void pd_svf_process(pd_svf *s, const pd_sample *in, pd_sample *out, unsigned frames)
{
    int32_t ic1 = s->band, ic2 = s->low;
    int64_t a1 = s->a1, a2 = s->a2, a3 = s->a3, q2 = s->q >> 1;
    for (unsigned i = 0; i < frames; i++) {
        int32_t x = F_IN(in[i]);
        int64_t v3 = (int64_t)x - ic2;
        int32_t band = flim((a1 * ic1 + a2 * v3) >> 30);
        int32_t low = flim(ic2 + ((a2 * ic1 + a3 * v3) >> 30));
        ic1 = slim(2 * (int64_t)band - ic1);
        ic2 = slim(2 * (int64_t)low - ic2);
        out[i] = F_OUT(s->mode == PD_SVF_LP ? low : s->mode == PD_SVF_BP ? band : flim(x - ((q2 * band) >> 14) - low));
    }
    s->band = ic1;
    s->low = ic2;
}

/* the feedback filters' saturation, on the filters' scale: tanh-like, unity slope at 0, +-2.0 from +-3.0 on
 * (2 (t - 4/27 t^3), t = x / 2) */
static int32_t sat2(int64_t x)
{
    const int64_t one = (int64_t)32768 << F_SH;
    int64_t t = (x < -3 * one ? -3 * one : x > 3 * one ? 3 * one : x) / 2;
    int64_t t3 = (((t * t) >> (15 + F_SH)) * t) >> (15 + F_SH);
    return (int32_t)(2 * (t - ((t3 * 4855) >> 15)));
}

/* a one-pole TPT stage: y = G (x - s) + s, s = 2 y - s; G Q30 */
static inline int32_t one_pole(int32_t *s, int64_t G, int32_t x)
{
    int32_t y = flim(*s + ((G * ((int64_t)x - *s)) >> 30));
    *s = slim(2 * (int64_t)y - *s);
    return y;
}

void pd_ladder_set(pd_ladder *l, int32_t cutoff, int32_t resonance)
{
    int64_t g = cutoff_g(cutoff), G = div_q30((uint32_t)g, (uint32_t)(g + (1 << 24)));   /* g / (1 + g) */
    int64_t G2 = (G * G) >> 30, G3 = (G2 * G) >> 30, G4 = (G3 * G) >> 30, H = (1 << 30) - G;
    l->k = pd_clamp(resonance, 0, PD_PARAM_MAX) >> 1;        /* 0 .. 4.0, Q12 */
    l->g = (int32_t)G;
    l->g4 = (int32_t)G4;
    l->c[0] = (int32_t)((G3 * H) >> 30);                     /* the stages' states, as they reach the output */
    l->c[1] = (int32_t)((G2 * H) >> 30);
    l->c[2] = (int32_t)((G * H) >> 30);
    l->c[3] = (int32_t)H;
    l->inv = div_q30(1u << 26, (uint32_t)((1 << 26) + ((l->k * G4) >> 16)));   /* 1 / (1 + k G^4): k G^4 < 4 */
}

void pd_ladder_process(pd_ladder *l, const pd_sample *in, pd_sample *out, unsigned frames)
{
    int64_t G = l->g, k = l->k;
    for (unsigned i = 0; i < frames; i++) {
        int32_t x = F_IN(in[i]);
        int64_t s = (int64_t)l->c[0] * l->s[0] + (int64_t)l->c[1] * l->s[1] + (int64_t)l->c[2] * l->s[2]
                  + (int64_t)l->c[3] * l->s[3];
        int64_t y4 = ((((int64_t)l->g4 * x + s) >> 30) * l->inv) >> 30;   /* the output the feedback solves for */
        int32_t y = sat2(x - ((k * y4) >> 12));
        for (int j = 0; j < 4; j++)
            y = one_pole(&l->s[j], G, y);
        out[i] = F_OUT(flim(y + ((k * y) >> 13)));             /* (1 + k / 2): some of the bass back */
    }
}

void pd_sk_set(pd_sk *f, int32_t cutoff, int32_t resonance)
{
    int64_t g = cutoff_g(cutoff), G = div_q30((uint32_t)g, (uint32_t)(g + (1 << 24)));
    int64_t K = pd_clamp(resonance, 0, PD_PARAM_MAX);         /* 0 .. 2.0, Q14 */
    int64_t KGH = (((K * G) >> 14) * ((1 << 30) - G)) >> 30;  /* K G (1 - G) <= 0.5, Q30 */
    int64_t inv1g = div_q30(1u << 24, (uint32_t)(g + (1 << 24)));          /* 1 / (1 + g), Q30 */
    f->g = (int32_t)G;
    f->k = (int32_t)K;
    f->alpha = div_q30(1u << 29, (uint32_t)((1 << 30) - KGH));              /* 1 / (1 - K G (1 - G)), Q29: 1 .. 2 */
    f->b_lp = (int32_t)(((((K * ((1 << 30) - G)) >> 14) >> 1) * inv1g) >> 30);   /* K (1 - G) / (1 + g), Q29 */
    f->b_hp = (int32_t)-(inv1g >> 1);                                     /* -1 / (1 + g), Q29 */
}

void pd_sk_process(pd_sk *f, const pd_sample *in, pd_sample *out, unsigned frames)
{
    int64_t G = f->g;
    for (unsigned i = 0; i < frames; i++) {
        int32_t y1 = one_pole(&f->s[0], G, F_IN(in[i]));
        int64_t fb = ((int64_t)f->b_lp * f->s[1] + (int64_t)f->b_hp * f->s[2]) >> 29;
        int32_t u = sat2(((y1 + fb) * f->alpha) >> 29);
        int32_t y = one_pole(&f->s[1], G, u);
        one_pole(&f->s[2], G, (int32_t)(((int64_t)f->k * y) >> 14));   /* the feedback's high-pass */
        out[i] = F_OUT(y);
    }
}
