/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
#include "pd_looper.h"

static const char *const LOOP_NAMES[PD_LOOP_PARAMS] = { "Loop Level" };

/* IMA ADPCM, 4 bits a sample, in blocks of PD_LOOPER_BLOCK: each block starts from a stored predictor and step
 * index, so playback can start at any block and an overdub re-encodes the block it passes through in place (the
 * decoder reads a sample before the encoder writes it). Re-encoding a decoded sample gives the same code back, so
 * an overdub of silence leaves the loop as it was. */
static const int16_t STEPS[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107,
    118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
};
static const int8_t INDEX_STEP[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };

typedef struct { int32_t pred, idx; } adpcm_t;

typedef struct {
    uint8_t *codes;            /* two samples a byte, the earlier in the low nibble */
    int16_t *bpred;            /* each block's starting predictor */
    uint8_t *bidx;             /* and step index */
    uint8_t *peaks;            /* each block's peak, for drawing: |sample| >> 6, at most 255 */
    uint32_t cap, len, pos, peak;
    adpcm_t dec, enc;
    int32_t level;             /* Q15 */
    int32_t gain, target;      /* playback gain, ramped by PD_UNIT / PD_LOOPER_FADE a sample */
    uint8_t state;
} looper_t;

static uint32_t blocks(uint32_t sr) { return (sr * PD_LOOPER_SECONDS + PD_LOOPER_BLOCK - 1u) / PD_LOOPER_BLOCK; }

static size_t loop_mem(uint32_t sr)
{
    return (size_t)(sr * PD_LOOPER_SECONDS + 1u) / 2u + blocks(sr) * (sizeof(int16_t) + 2u) + 4u;
}

static int32_t adpcm_step(adpcm_t *s, unsigned code)
{
    int32_t st = STEPS[s->idx], d = st >> 3;
    if (code & 4)
        d += st;
    if (code & 2)
        d += st >> 1;
    if (code & 1)
        d += st >> 2;
    s->pred = pd_clamp(code & 8 ? s->pred - d : s->pred + d, -32768, 32767);
    s->idx = pd_clamp(s->idx + INDEX_STEP[code & 7], 0, 88);
    return s->pred;
}

static unsigned adpcm_code(const adpcm_t *s, int32_t x)
{
    int32_t st = STEPS[s->idx], d = x - s->pred;
    unsigned code = 0;
    if (d < 0) {
        code = 8;
        d = -d;
    }
    if (d >= st) {
        code |= 4;
        d -= st;
    }
    if (d >= st >> 1) {
        code |= 2;
        d -= st >> 1;
    }
    if (d >= st >> 2)
        code |= 1;
    return code;
}

static unsigned get_code(const looper_t *l, uint32_t i) { return (l->codes[i >> 1] >> ((i & 1u) << 2)) & 15u; }

static void put_code(looper_t *l, uint32_t i, unsigned c)
{
    uint8_t *b = &l->codes[i >> 1];
    *b = i & 1u ? (uint8_t)((*b & 0x0F) | c << 4) : (uint8_t)((*b & 0xF0) | c);
}

/* the encoder at l->pos: a block's first sample stores its state; it returns what the decoder will read back */
static int32_t encode(looper_t *l, int32_t x)
{
    if (l->pos % PD_LOOPER_BLOCK == 0) {
        l->bpred[l->pos / PD_LOOPER_BLOCK] = (int16_t)l->enc.pred;
        l->bidx[l->pos / PD_LOOPER_BLOCK] = (uint8_t)l->enc.idx;
    }
    unsigned c = adpcm_code(&l->enc, pd_sat16(x));
    put_code(l, l->pos, c);
    return adpcm_step(&l->enc, c);
}

/* the decoder at l->pos; at a block's start an overdub takes the block's state too, so silence re-encodes the same */
static int32_t decode(looper_t *l)
{
    if (l->pos % PD_LOOPER_BLOCK == 0) {
        l->dec.pred = l->bpred[l->pos / PD_LOOPER_BLOCK];
        l->dec.idx = l->bidx[l->pos / PD_LOOPER_BLOCK];
        if (l->state == PD_LOOP_OVERDUB)
            l->enc = l->dec;
    }
    return adpcm_step(&l->dec, get_code(l, l->pos));
}

/* keep the block's peak; at its end (or the loop's), store it */
static void track_peak(looper_t *l, int32_t y, uint32_t end)
{
    uint32_t v = (uint32_t)(y < 0 ? -y : y) >> 6;
    if (v > l->peak)
        l->peak = v;
    if ((l->pos + 1u) % PD_LOOPER_BLOCK == 0 || l->pos + 1u == end) {
        l->peaks[l->pos / PD_LOOPER_BLOCK] = (uint8_t)(l->peak > 255u ? 255u : l->peak);
        l->peak = 0;
    }
}

static void loop_set(void *st, unsigned id, int32_t v)
{
    if (id == PD_LOOP_LEVEL)
        ((looper_t *)st)->level = pd_clamp(v, 0, PD_PARAM_MAX);
}

static void loop_reset(void *st)
{
    looper_t *l = st;
    l->len = l->pos = l->peak = 0;
    l->gain = l->target = 0;
    l->state = PD_LOOP_EMPTY;
}

static void loop_init(void *st, void *mem, uint32_t sr)
{
    looper_t *l = st;
    uint32_t nb = blocks(sr);
    l->cap = sr * PD_LOOPER_SECONDS;
    l->bpred = mem;                                       /* (the arena aligns mem) */
    l->codes = (uint8_t *)(l->bpred + nb);
    l->bidx = l->codes + (l->cap + 1u) / 2u;
    l->peaks = l->bidx + nb;
    l->level = PD_PARAM_MAX;
    loop_reset(l);
}

void pd_looper_press(void *st)
{
    looper_t *l = st;
    switch (l->state) {
    case PD_LOOP_EMPTY:
        l->len = l->pos = l->peak = 0;
        l->enc.pred = l->enc.idx = 0;
        l->state = PD_LOOP_RECORD;
        break;
    case PD_LOOP_RECORD:
        l->len = l->pos;
        if (!l->len) {
            l->state = PD_LOOP_EMPTY;
            break;
        }
        if (l->len % PD_LOOPER_BLOCK)                     /* the last block's peak, so far */
            l->peaks[(l->len - 1u) / PD_LOOPER_BLOCK] = (uint8_t)(l->peak > 255u ? 255u : l->peak);
        l->pos = l->peak = 0;
        l->gain = 0;
        l->target = PD_UNIT;
        l->state = PD_LOOP_PLAY;
        break;
    case PD_LOOP_PLAY:
        l->enc = l->dec;                                  /* the overdub carries on from the decoder */
        l->state = PD_LOOP_OVERDUB;
        break;
    case PD_LOOP_OVERDUB:
        l->state = PD_LOOP_PLAY;
        break;
    default:                                  /* stopped: play from the top */
        l->pos = l->peak = 0;
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

const uint8_t *pd_looper_peek(const void *st, uint32_t *len, uint32_t *pos, uint32_t *cap)
{
    const looper_t *l = st;
    *len = l->state == PD_LOOP_RECORD ? l->pos : l->len;
    *pos = l->pos;
    *cap = l->cap;
    return l->peaks;
}

/* the loop's ends fade in and out over PD_LOOPER_FADE samples, so its seam does not click */
static int32_t seam(const looper_t *l, int32_t y)
{
    uint32_t n = l->len < 2 * PD_LOOPER_FADE ? l->len / 2 : PD_LOOPER_FADE, i = l->pos;
    if (i >= n && i < l->len - n)
        return y;
    if (i >= n)
        i = l->len - 1u - i;
    return pd_mulq15(y, (int32_t)((i * PD_UNIT) / n));
}

static void loop_process(void *st, const pd_sample *in, pd_sample *out, unsigned frames)
{
    looper_t *l = st;
    const int32_t step = PD_UNIT / PD_LOOPER_FADE;
    for (unsigned i = 0; i < frames; i++) {
        int32_t x = in[i];
        if (l->state == PD_LOOP_RECORD) {
            track_peak(l, encode(l, x), l->cap);
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
        int32_t y = decode(l);
        track_peak(l, l->state == PD_LOOP_OVERDUB ? encode(l, y + x) : y, l->len);
        out[i] = x + pd_mulq15(pd_mulq15(seam(l, y), l->gain >> 1) * 2, l->level);
        if (++l->pos >= l->len)
            l->pos = 0;
    }
}

const PendelDSP PD_LOOPER = {
    "Looper", sizeof(looper_t), loop_mem, PD_LOOP_PARAMS, LOOP_NAMES,
    loop_init, loop_process, loop_reset, loop_set,
};
