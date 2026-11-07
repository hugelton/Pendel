/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
#include "pd_modules.h"

#include "pd_osc.h"

/* a gain ramped across the block from *cur to target (both Q15, at most 4x): no zipper noise */
static void ramp_gain(pd_sample *buf, unsigned frames, int32_t *cur, int32_t target)
{
    int32_t g = *cur, step = (target - g) / (int32_t)frames;
    for (unsigned i = 0; i < frames; i++, g += step)
        buf[i] = (pd_clamp(buf[i], -65535, 65535) * (g >> 2)) >> 13;
    *cur = target;
}

/* ---------------------------------------------------------------- synth --- */

static const char *const SYN_NAMES[PD_SYN_PARAMS] = {
    "Wave", "Tune", "Cutoff", "Resonance", "Env Amount", "Attack", "Decay", "Sustain", "Release", "Level",
};

typedef struct {
    pd_osc osc;
    pd_env env;
    pd_svf svf;
    int32_t p[PD_SYN_PARAMS];
    int32_t note, vel, gain;
} synth_t;

static void syn_set(void *st, unsigned id, int32_t v)
{
    synth_t *s = st;
    if (id >= PD_SYN_PARAMS)
        return;
    s->p[id] = pd_clamp(v, 0, PD_PARAM_MAX);
    s->osc.wave = (uint8_t)((s->p[PD_SYN_WAVE] * PD_WAVE_COUNT) >> 15);
    pd_env_set(&s->env, s->p[PD_SYN_ATTACK], s->p[PD_SYN_DECAY], s->p[PD_SYN_SUSTAIN], s->p[PD_SYN_RELEASE]);
    /* tune: +-12 semitones around the middle */
    s->osc.inc = pd_pitch_inc((s->note << 8) + ((s->p[PD_SYN_TUNE] - 16384) * 3072 >> 14));
}

static void syn_reset(void *st)
{
    synth_t *s = st;
    s->env.stage = PD_ENV_IDLE;
    s->env.level = 0;
    s->svf.low = s->svf.band = 0;
    s->gain = 0;
}

static void syn_init(void *st, void *mem, uint32_t sr)
{
    static const int32_t DEF[PD_SYN_PARAMS] = { 0, 16384, 18000, 8000, 12000, 2000, 14000, 20000, 12000, 26000 };
    synth_t *s = st;
    (void)mem;
    (void)sr;
    s->note = 60;
    s->svf.mode = PD_SVF_LP;
    for (unsigned i = 0; i < PD_SYN_PARAMS; i++)
        syn_set(s, i, DEF[i]);
    syn_reset(s);
}

static void syn_process(void *st, const pd_sample *in, pd_sample *out, unsigned frames)
{
    synth_t *s = st;
    (void)in;
    if (s->env.stage == PD_ENV_IDLE && s->gain == 0) {
        for (unsigned i = 0; i < frames; i++)
            out[i] = 0;
        return;
    }
    int32_t amp = pd_env_block(&s->env, frames);
    pd_osc_render(&s->osc, out, frames);
    pd_svf_set(&s->svf, s->p[PD_SYN_CUTOFF] + pd_mulq15(amp, s->p[PD_SYN_ENV_AMOUNT]), s->p[PD_SYN_RESONANCE]);
    pd_svf_process(&s->svf, out, out, frames);
    /* amp * level * velocity, Q15 -> the ramp's Q15 (1x = 32768) */
    int32_t target = pd_mulq15(pd_mulq15(amp, s->p[PD_SYN_LEVEL]), s->vel << 8);
    ramp_gain(out, frames, &s->gain, target);
}

void pd_synth_note_on(void *st, int note, int velocity)
{
    synth_t *s = st;
    s->note = pd_clamp(note, 0, 127);
    s->vel = pd_clamp(velocity, 1, 127);
    syn_set(s, PD_SYN_TUNE, s->p[PD_SYN_TUNE]);
    pd_env_gate(&s->env, 1);
}

void pd_synth_note_off(void *st, int note)
{
    synth_t *s = st;
    if (note < 0 || note == s->note)
        pd_env_gate(&s->env, 0);
}

const PendelDSP PD_SYNTH = {
    "VA Synth", sizeof(synth_t), NULL, PD_SYN_PARAMS, SYN_NAMES,
    syn_init, syn_process, syn_reset, syn_set,
};

/* ----------------------------------------------------------------- gain --- */

static const char *const GAIN_NAMES[PD_GAIN_PARAMS] = { "Gain" };

typedef struct {
    int32_t target, cur;
} gain_t;

static void gain_set(void *st, unsigned id, int32_t v)
{
    if (id == PD_GAIN_GAIN)
        ((gain_t *)st)->target = pd_clamp(v, 0, PD_PARAM_MAX) * 4;
}

static void gain_reset(void *st) { ((gain_t *)st)->cur = ((gain_t *)st)->target; }

static void gain_init(void *st, void *mem, uint32_t sr)
{
    (void)mem;
    (void)sr;
    gain_set(st, PD_GAIN_GAIN, 8192);
    gain_reset(st);
}

static void gain_process(void *st, const pd_sample *in, pd_sample *out, unsigned frames)
{
    gain_t *g = st;
    for (unsigned i = 0; i < frames; i++)
        out[i] = in[i];
    ramp_gain(out, frames, &g->cur, g->target);
}

const PendelDSP PD_FX_GAIN = {
    "Gain", sizeof(gain_t), NULL, PD_GAIN_PARAMS, GAIN_NAMES,
    gain_init, gain_process, gain_reset, gain_set,
};

/* ---------------------------------------------------------------- drive --- */

static const char *const DRV_NAMES[PD_DRV_PARAMS] = { "Drive", "Tone", "Level" };

typedef struct {
    int32_t pre;               /* Q8: 1x .. 32x */
    int32_t tone;              /* one-pole coefficient, Q15 */
    int32_t vol, level, gain;  /* Q15: LEVEL as set; with the make-up; as ramped */
    int32_t lp;
} drive_t;

/* the drive's make-up: 0.72 / sqrt(pre), so turning DRIVE up thickens the sound more than it raises it
 * (about level with the dry at -8 dBFS peaks and the defaults) */
static void drv_level(drive_t *d)
{
    int32_t root = (int32_t)pd_isqrt((uint32_t)d->pre << 8);          /* Q8, 16 .. 362 */
    int32_t mk = (23593 * 256) / (root < 16 ? 16 : root);              /* Q15, 0.72 / sqrt(pre) */
    d->level = (d->vol * mk) >> 15;
}

static void drv_set(void *st, unsigned id, int32_t v)
{
    drive_t *d = st;
    v = pd_clamp(v, 0, PD_PARAM_MAX);
    if (id == PD_DRV_DRIVE) {
        d->pre = 256 + ((v * 31) >> 7);
        drv_level(d);
    } else if (id == PD_DRV_TONE)
        d->tone = 1500 + ((v * 31267) >> 15);
    else if (id == PD_DRV_LEVEL) {
        d->vol = v;
        drv_level(d);
    }
}

static void drv_reset(void *st)
{
    drive_t *d = st;
    d->lp = 0;
    d->gain = d->level;
}

static void drv_init(void *st, void *mem, uint32_t sr)
{
    (void)mem;
    (void)sr;
    drv_set(st, PD_DRV_DRIVE, 12000);
    drv_set(st, PD_DRV_TONE, 20000);
    drv_set(st, PD_DRV_LEVEL, 20000);
    drv_reset(st);
}

static void drv_process(void *st, const pd_sample *in, pd_sample *out, unsigned frames)
{
    drive_t *d = st;
    int32_t lp = d->lp;
    for (unsigned i = 0; i < frames; i++) {
        int32_t x = pd_clamp((pd_clamp(in[i], -65535, 65535) * d->pre) >> 8, -32767, 32767);
        int32_t y = x - pd_mulq15(pd_mulq15(pd_mulq15(x, x), x), 10923);   /* x - x^3/3: peaks at 2/3 */
        lp += pd_mulq15(((y * 3) >> 1) - lp, d->tone);
        out[i] = lp;
    }
    d->lp = lp;
    ramp_gain(out, frames, &d->gain, d->level);
}

const PendelDSP PD_FX_DRIVE = {
    "Drive", sizeof(drive_t), NULL, PD_DRV_PARAMS, DRV_NAMES,
    drv_init, drv_process, drv_reset, drv_set,
};

/* --------------------------------------------------------------- filter --- */

static const char *const FLT_NAMES[PD_FLT_PARAMS] = { "Cutoff", "Resonance", "Mode" };

typedef struct {
    pd_svf svf;
    pd_ladder ladder;
    pd_sk sk;
    int32_t cutoff, res;
    uint8_t mode;
} filter_t;

static void flt_reset(void *st)
{
    filter_t *f = st;
    f->svf.low = f->svf.band = 0;
    for (int i = 0; i < 4; i++)
        f->ladder.s[i] = 0;
    for (int i = 0; i < 3; i++)
        f->sk.s[i] = 0;
}

static void flt_set(void *st, unsigned id, int32_t v)
{
    filter_t *f = st;
    v = pd_clamp(v, 0, PD_PARAM_MAX);
    if (id == PD_FLT_CUTOFF)
        f->cutoff = v;
    else if (id == PD_FLT_RESONANCE)
        f->res = v;
    else if (id == PD_FLT_MODE && f->mode != (uint8_t)((v * PD_FLT_MODES) >> 15)) {
        f->mode = (uint8_t)((v * PD_FLT_MODES) >> 15);
        flt_reset(f);                                     /* the new filter starts from silence */
    }
    if (f->mode == PD_FLT_LADDER)
        pd_ladder_set(&f->ladder, f->cutoff, f->res);
    else if (f->mode == PD_FLT_SK)
        pd_sk_set(&f->sk, f->cutoff, f->res);
    else {
        f->svf.mode = f->mode;
        pd_svf_set(&f->svf, f->cutoff, f->res);
    }
}

static void flt_init(void *st, void *mem, uint32_t sr)
{
    (void)mem;
    (void)sr;
    flt_set(st, PD_FLT_CUTOFF, 20000);
    flt_set(st, PD_FLT_RESONANCE, 6000);
    flt_set(st, PD_FLT_MODE, 0);
}

static void flt_process(void *st, const pd_sample *in, pd_sample *out, unsigned frames)
{
    filter_t *f = st;
    if (f->mode == PD_FLT_LADDER)
        pd_ladder_process(&f->ladder, in, out, frames);
    else if (f->mode == PD_FLT_SK)
        pd_sk_process(&f->sk, in, out, frames);
    else {
        pd_svf_process(&f->svf, in, out, frames);
        if (f->mode != PD_FLT_LP)                          /* BP, HP: +9.5 dB, for the lows they take out */
            for (unsigned i = 0; i < frames; i++)
                out[i] = pd_clamp(out[i], -21845, 21845) * 3;
    }
}

const PendelDSP PD_FX_FILTER = {
    "Filter", sizeof(filter_t), NULL, PD_FLT_PARAMS, FLT_NAMES,
    flt_init, flt_process, flt_reset, flt_set,
};

/* ---------------------------------------------------------------- input --- */

static const char *const IN_NAMES[PD_IN_PARAMS] = { "Input Gain", "Gate", "Low Cut" };

typedef struct {
    int32_t gain, cur;         /* Q15, PD_UNIT = 1x, up to 8x */
    int32_t thr;               /* gate threshold, 0 = off */
    int32_t env, gate;         /* follower; gate gain Q15 */
    int32_t hp_c, hp_lp;       /* low-cut coefficient Q15 (0 = off) and its low-pass state */
} input_t;

static void in_set(void *st, unsigned id, int32_t v)
{
    input_t *n = st;
    v = pd_clamp(v, 0, PD_PARAM_MAX);
    if (id == PD_IN_GAIN)
        n->gain = v * 8;
    else if (id == PD_IN_GATE)
        n->thr = (v * (v >> 2)) >> 15;          /* 0 .. 8191 (-12 dBFS), finer at the bottom */
    else if (id == PD_IN_LOWCUT)               /* 2 pi fc / fs, Q15, fc = 20 .. 400 Hz */
        n->hp_c = v ? (int32_t)((uint32_t)(20 + ((v * 380) >> 15)) * 205887u / PD_FS) : 0;
}

static void in_reset(void *st)
{
    input_t *n = st;
    n->cur = n->gain;
    n->env = n->hp_lp = 0;
    n->gate = PD_UNIT;
}

static void in_init(void *st, void *mem, uint32_t sr)
{
    (void)mem;
    (void)sr;
    in_set(st, PD_IN_GAIN, 4096);
    in_reset(st);
}

static void in_process(void *st, const pd_sample *in, pd_sample *out, unsigned frames)
{
    input_t *n = st;
    for (unsigned i = 0; i < frames; i++)
        out[i] = in[i];
    if (n->hp_c) {
        int32_t lp = n->hp_lp;
        for (unsigned i = 0; i < frames; i++) {
            int32_t x = pd_clamp(out[i], -65535, 65535);
            lp += pd_mulq15(x - lp, n->hp_c);
            out[i] = x - lp;
        }
        n->hp_lp = lp;
    }
    if (n->thr) {
        /* peak follower (tau ~23 ms); opens in ~1 ms above the threshold, closes in ~50 ms below half of it */
        int32_t env = n->env, g = n->gate;
        const int32_t open = PD_UNIT / 44, close = PD_UNIT / 2205;
        for (unsigned i = 0; i < frames; i++) {
            int32_t a = pd_clamp(out[i] < 0 ? -out[i] : out[i], 0, 65535);
            env -= env >> 10;
            if (a > env)
                env = a;
            if (env > n->thr)
                g = g + open > PD_UNIT ? PD_UNIT : g + open;
            else if (env < n->thr >> 1)
                g = g - close < 0 ? 0 : g - close;
            out[i] = pd_mulq15(pd_clamp(out[i], -65535, 65535) >> 1, g) * 2;
        }
        n->env = env;
        n->gate = g;
    }
    /* ramped like ramp_gain, with the room for 8x: exact at 1x (x * 4096 >> 12) */
    int32_t g = n->cur, step = (n->gain - g) / (int32_t)frames;
    for (unsigned i = 0; i < frames; i++, g += step)
        out[i] = (pd_clamp(out[i], -65535, 65535) * (g >> 3)) >> 12;
    n->cur = n->gain;
}

const PendelDSP PD_INPUT = {
    "Input", sizeof(input_t), NULL, PD_IN_PARAMS, IN_NAMES,
    in_init, in_process, in_reset, in_set,
};

/* ---------------------------------------------------------------- delay --- */

static const char *const DLY_NAMES[PD_DLY_PARAMS] = { "Time", "Feedback" };

typedef struct {
    int16_t *buf;
    uint32_t len, w;
    uint32_t cur, target;      /* delay in samples; cur glides to target one sample a sample (tape-like) */
    int32_t fb;                /* Q15, at most 0.9 */
} delay_t;

static size_t dly_mem(uint32_t sr) { return (size_t)sr * sizeof(int16_t); }

static void dly_set(void *st, unsigned id, int32_t v)
{
    delay_t *d = st;
    v = pd_clamp(v, 0, PD_PARAM_MAX);
    if (id == PD_DLY_TIME)
        d->target = 1u + (uint32_t)v * (d->len - 2u) / PD_PARAM_MAX;
    else if (id == PD_DLY_FEEDBACK)
        d->fb = (v * 29491) >> 15;
}

static void dly_reset(void *st)
{
    delay_t *d = st;
    for (uint32_t i = 0; i < d->len; i++)
        d->buf[i] = 0;
    d->cur = d->target;
}

static void dly_init(void *st, void *mem, uint32_t sr)
{
    delay_t *d = st;
    d->buf = mem;
    d->len = sr;
    dly_set(d, PD_DLY_TIME, 12000);
    dly_set(d, PD_DLY_FEEDBACK, 12000);
    d->cur = d->target;
}

static void dly_process(void *st, const pd_sample *in, pd_sample *out, unsigned frames)
{
    delay_t *d = st;
    uint32_t w = d->w, len = d->len, cur = d->cur;
    for (unsigned i = 0; i < frames; i++) {
        if (cur < d->target)
            cur++;
        else if (cur > d->target)
            cur--;
        uint32_t r = w >= cur ? w - cur : w + len - cur;
        int32_t echo = d->buf[r];
        d->buf[w] = (int16_t)pd_sat16(in[i] + pd_mulq15(echo, d->fb));
        out[i] = echo;
        if (++w == len)
            w = 0;
    }
    d->w = w;
    d->cur = cur;
}

const PendelDSP PD_FX_DELAY = {
    "Delay", sizeof(delay_t), dly_mem, PD_DLY_PARAMS, DLY_NAMES,
    dly_init, dly_process, dly_reset, dly_set,
};
