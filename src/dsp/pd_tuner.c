/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
#include "pd_tuner.h"

/* the decimation filter: two one-poles at about 1.5 kHz (a = w / (fs + w), w = 2 pi 1500), Q12 */
#define LP_A ((int32_t)(4096u * 9425u / (PD_FS + 9425u)))
#define LEVEL_MIN 128          /* the quietest look that is measured: its peak after the filter, of 32767 (-48 dBFS) */
#define SCALE 1023             /* a look is scaled to this peak: a difference squared, summed over the window, fits
                                * in 32 bits (2046^2 * 512 < 2^31) */
#define THR_NUM 18             /* YIN's threshold, 0.18: a dip below it is a period */
#define THR_DEN 100
#define LOG2_C0 198663         /* log2(8.1758 Hz), MIDI note 0, Q16 */

void pd_tuner_reset(pd_tuner *t)
{
    t->lp1 = t->lp2 = t->acc = 0;
    t->k = 0;
    t->fill = 0;
    t->ready = 0;
}

void pd_tuner_feed(pd_tuner *t, int16_t *buf, const int16_t *in, unsigned frames)
{
    int32_t lp1 = t->lp1, lp2 = t->lp2, acc = t->acc;
    unsigned k = t->k, fill = t->fill;
    int full = t->ready;
    for (unsigned i = 0; i < frames; i++) {
        int32_t x = (in ? in[i] : 0) * 16;                   /* Q4: |x| < 2^19, (x - lp) * LP_A < 2^31 */
        lp1 += ((x - lp1) * LP_A) >> 12;
        lp2 += ((lp1 - lp2) * LP_A) >> 12;                   /* (it runs on while the buffer waits: no step when it
                                                              * starts again) */
        acc += lp2;
        if (++k < PD_TUNER_DEC)
            continue;
        k = 0;
        if (!full) {
            buf[fill++] = (int16_t)pd_sat16(acc / (16 * PD_TUNER_DEC));
            if (fill >= PD_TUNER_N)
                full = 1;
        }
        acc = 0;
    }
    t->lp1 = lp1;
    t->lp2 = lp2;
    t->acc = acc;
    t->k = (uint8_t)k;
    t->fill = (uint16_t)fill;
    if (full && !t->ready)
        t->ready = 1;
}

/* the difference function at lag tau over the window (x has PD_TUNER_W + tau samples), every `stride`-th sample of it */
static uint32_t diff_by(const int16_t *x, unsigned tau, unsigned stride)
{
    uint32_t s = 0;
    for (unsigned j = 0; j < PD_TUNER_W; j += stride) {
        int32_t d = x[j] - x[j + tau];
        s += (uint32_t)(d * d);
    }
    return s;
}
static uint32_t diff(const int16_t *x, unsigned tau) { return diff_by(x, tau, 1); }

/* the vertex of the parabola through (-1, a), (0, b), (1, c), in Q16 (-0.5 .. 0.5 when b is the least), no 64-bit
 * divide: both terms shifted into 15 bits first */
static int32_t vertex_q16(uint32_t a, uint32_t b, uint32_t c)
{
    int64_t num = (int64_t)a - (int64_t)c, den = 2 * ((int64_t)a + (int64_t)c - 2 * (int64_t)b);
    if (den <= 0)
        return 0;
    while (den > 0x7FFF || num > 0x7FFF || num < -0x7FFF) {
        num >>= 1;
        den >>= 1;
    }
    if (den <= 0)
        return 0;
    int32_t v = (int32_t)(((int32_t)num * 65536) / (int32_t)den);
    return pd_clamp(v, -32768, 32768);
}

int32_t pd_log2_q16(uint32_t v)
{
    if (!v)
        return 0;
    int e = 31 - __builtin_clz(v);
    uint32_t m = e > 30 ? v >> (e - 30) : v << (30 - e);   /* the mantissa, 1 .. 2 in Q30 */
    int32_t r = e << 16;
    for (int b = 15; b >= 0; b--) {                         /* a bit a squaring */
        m = (uint32_t)(((uint64_t)m * m) >> 30);
        if (m >= 2u << 30) {
            m >>= 1;
            r |= 1 << b;
        }
    }
    return r;
}

/* the period near `centre` (a multiple of it): walks down the valley from there, then the parabola; Q16, or 0 */
#define LAG_MAX (PD_TUNER_N - PD_TUNER_W - 1)
static uint32_t valley_q16(const int16_t *x, unsigned centre)
{
    unsigned l = centre;
    uint32_t d = diff(x, l), dl, dr;
    for (int step = 0; step < 6; step++) {
        if (l < 2 || l + 1 > LAG_MAX)
            return 0;
        dl = diff(x, l - 1);
        dr = diff(x, l + 1);
        if (dl < d && dl <= dr) {
            l--;
            d = dl;
        } else if (dr < d) {
            l++;
            d = dr;
        } else {
            return (uint32_t)((int32_t)(l << 16) + vertex_q16(dl, d, dr));
        }
    }
    return 0;                                                /* no valley there */
}

int pd_tuner_detect(int16_t *x, pd_pitch *p)
{
    int32_t sum = 0, peak = 0;
    unsigned i;
    for (i = 0; i < PD_TUNER_N; i++)
        sum += x[i];
    int32_t mean = sum / PD_TUNER_N;
    for (i = 0; i < PD_TUNER_N; i++) {
        int32_t v = x[i] - mean, a = v < 0 ? -v : v;
        if (a > peak)
            peak = a;
    }
    if (peak < LEVEL_MIN)
        return 0;
    int32_t s = (SCALE << 16) / peak;                         /* |v| <= peak: v * s <= SCALE << 16 */
    for (i = 0; i < PD_TUNER_N; i++)
        x[i] = (int16_t)(((x[i] - mean) * s) >> 16);

    /* YIN: the first lag whose normalised difference d(t) t / sum(d(1..t)) falls below the threshold, then on down
     * to the bottom of that dip; on every other sample of the window (half the time: the refinement below has the
     * whole window) */
    uint64_t cum = 0;
    uint32_t d1 = 0, d2 = 0, d = 0;
    unsigned tau, t0 = 0;
    int hit = 0;
    for (tau = 1; tau <= PD_TUNER_MAXLAG; tau++) {
        d = diff_by(x, tau, 2);
        cum += d;
        if (hit && d >= d1) {
            t0 = tau - 1;
            break;
        }
        if (!hit && tau >= 2 && (uint64_t)d * tau * THR_DEN < (uint64_t)THR_NUM * cum)
            hit = 1;
        d2 = d1;
        d1 = d;
    }
    if (!t0 || t0 < 2)
        return 0;
    uint32_t per = (uint32_t)((int32_t)(t0 << 16) + vertex_q16(d2, d1, d));   /* d: at t0 + 1 */

    /* the period again on twice as many periods each time, out to as many as the buffer holds */
    unsigned n = 1;
    for (;;) {
        unsigned nmax = (unsigned)(((uint32_t)(LAG_MAX - 7) << 16) / per);
        if (nmax <= n)
            break;
        unsigned m = 2 * n < nmax ? 2 * n : nmax;
        uint32_t v = valley_q16(x, (unsigned)((m * per + 32768u) >> 16));
        if (!v)
            break;
        per = v / m;
        n = m;
    }

    /* the pitch: log2 f = log2 fs - log2 (period at fs) */
    uint32_t pf = per * PD_TUNER_DEC;                         /* Q16, < 2^27 */
    int32_t l2 = pd_log2_q16(PD_FS) - pd_log2_q16(pf) + (16 << 16) - LOG2_C0;
    if (pf < (1u << 16) || l2 <= 0)
        return 0;
    int32_t c16 = (l2 * 1200) >> 12;                          /* 1/16 cent above note 0 */
    int32_t note = (c16 + 800) / 1600;
    if (note > 127)
        return 0;
    int32_t off = c16 - note * 1600;
    p->c16 = c16;
    p->note = (uint8_t)note;
    p->cents10 = (int16_t)((off * 10 + (off < 0 ? -8 : 8)) / 16);
    uint32_t fs16 = (uint32_t)PD_FS << 16;
    p->hz10 = fs16 / pf * 10u + fs16 % pf * 10u / pf;
    return 1;
}

int pd_tuner_poll(pd_tuner *t, int16_t *buf, pd_pitch *p)
{
    if (!t->ready)
        return -1;
    int r = pd_tuner_detect(buf, p);
    t->fill = 0;                                              /* (before ready: the audio side reads fill after it) */
    t->ready = 0;
    return r;
}
