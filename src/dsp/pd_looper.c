/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
#include "pd_looper.h"

static const char *const LOOP_NAMES[PD_LOOP_PARAMS] = { "Loop Level" };

typedef struct {
    int16_t *buf;
    uint32_t cap, len, pos;
    int32_t level;             /* Q15 */
    int32_t gain, target;      /* playback gain, ramped by PD_UNIT / PD_LOOPER_FADE a sample */
    uint8_t state;
} looper_t;

static size_t loop_mem(uint32_t sr) { return (size_t)sr * PD_LOOPER_SECONDS * sizeof(int16_t); }

static void loop_set(void *st, unsigned id, int32_t v)
{
    if (id == PD_LOOP_LEVEL)
        ((looper_t *)st)->level = pd_clamp(v, 0, PD_PARAM_MAX);
}

static void loop_reset(void *st)
{
    looper_t *l = st;
    l->len = l->pos = 0;
    l->gain = l->target = 0;
    l->state = PD_LOOP_EMPTY;
}

static void loop_init(void *st, void *mem, uint32_t sr)
{
    looper_t *l = st;
    l->buf = mem;
    l->cap = sr * PD_LOOPER_SECONDS;
    l->level = PD_PARAM_MAX;
    loop_reset(l);
}

/* fade the first and last PD_LOOPER_FADE samples of a fresh loop, so its seam does not click */
static void seal(looper_t *l)
{
    uint32_t n = l->len < 2 * PD_LOOPER_FADE ? l->len / 2 : PD_LOOPER_FADE;
    for (uint32_t i = 0; i < n; i++) {
        int32_t g = (int32_t)((i * PD_UNIT) / n);
        l->buf[i] = (int16_t)pd_mulq15(l->buf[i], g);
        l->buf[l->len - 1 - i] = (int16_t)pd_mulq15(l->buf[l->len - 1 - i], g);
    }
}

void pd_looper_press(void *st)
{
    looper_t *l = st;
    switch (l->state) {
    case PD_LOOP_EMPTY:
        l->len = l->pos = 0;
        l->state = PD_LOOP_RECORD;
        break;
    case PD_LOOP_RECORD:
        l->len = l->pos;
        if (!l->len) {
            l->state = PD_LOOP_EMPTY;
            break;
        }
        seal(l);
        l->pos = 0;
        l->gain = 0;
        l->target = PD_UNIT;
        l->state = PD_LOOP_PLAY;
        break;
    case PD_LOOP_PLAY:
        l->state = PD_LOOP_OVERDUB;
        break;
    case PD_LOOP_OVERDUB:
        l->state = PD_LOOP_PLAY;
        break;
    default:                                  /* stopped: play from the top */
        l->pos = 0;
        l->target = PD_UNIT;
        l->state = PD_LOOP_PLAY;
        break;
    }
}

void pd_looper_stop(void *st)
{
    looper_t *l = st;
    if (l->state == PD_LOOP_RECORD)
        pd_looper_press(l);                   /* close the loop first, then stop it */
    if (l->state != PD_LOOP_EMPTY) {
        l->target = 0;
        l->state = PD_LOOP_STOP;
    }
}

void pd_looper_clear(void *st) { loop_reset(st); }
int pd_looper_state(const void *st) { return ((const looper_t *)st)->state; }
uint32_t pd_looper_length(const void *st)
{
    const looper_t *l = st;
    return l->state == PD_LOOP_EMPTY || l->state == PD_LOOP_RECORD ? 0 : l->len;
}

const int16_t *pd_looper_peek(const void *st, uint32_t *len, uint32_t *pos, uint32_t *cap)
{
    const looper_t *l = st;
    *len = l->state == PD_LOOP_RECORD ? l->pos : l->len;
    *pos = l->pos;
    *cap = l->cap;
    return l->buf;
}

static void loop_process(void *st, const pd_sample *in, pd_sample *out, unsigned frames)
{
    looper_t *l = st;
    const int32_t step = PD_UNIT / PD_LOOPER_FADE;
    for (unsigned i = 0; i < frames; i++) {
        int32_t x = in[i];
        if (l->state == PD_LOOP_RECORD) {
            l->buf[l->pos] = (int16_t)pd_sat16(x);
            if (++l->pos == l->cap)           /* full: the loop closes itself */
                pd_looper_press(l);
            out[i] = x;
            continue;
        }
        if (l->gain < l->target)
            l->gain = l->gain + step > l->target ? l->target : l->gain + step;
        else if (l->gain > l->target)
            l->gain = l->gain - step < l->target ? l->target : l->gain - step;
        if (!l->len || (l->gain == 0 && l->state == PD_LOOP_STOP)) {
            out[i] = x;
            continue;
        }
        int32_t y = l->buf[l->pos];
        if (l->state == PD_LOOP_OVERDUB)
            l->buf[l->pos] = (int16_t)pd_sat16(y + x);
        out[i] = x + pd_mulq15(pd_mulq15(y, l->gain >> 1) * 2, l->level);
        if (++l->pos >= l->len)
            l->pos = 0;
    }
}

const PendelDSP PD_LOOPER = {
    "Looper", sizeof(looper_t), loop_mem, PD_LOOP_PARAMS, LOOP_NAMES,
    loop_init, loop_process, loop_reset, loop_set,
};
