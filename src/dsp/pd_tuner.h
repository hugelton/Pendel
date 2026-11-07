/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* The tuner: the input's pitch, as a note, an octave and cents.
 *
 * The audio side (pd_tuner_feed) low-passes the input, takes every PD_TUNER_DEC-th sample (11 kHz) and fills a
 * buffer of PD_TUNER_N; then it waits. The main loop (pd_tuner_detect) finds the period in it with YIN's cumulative
 * mean normalised difference, then refines it on a multiple of the period as far out as the buffer reaches (the
 * parabola's error divided by the number of periods), and lets the audio side fill the buffer again. All integer:
 * 32-bit sums, 64-bit products, no 64-bit divide. The buffer is the caller's (the app lends it the scope's ring). */
#ifndef PD_TUNER_H
#define PD_TUNER_H

#include "pd_dsp.h"

#define PD_TUNER_DEC 4         /* PD_FS / 4: 11 kHz */
#define PD_TUNER_N 1024        /* decimated samples a look: 93 ms */
#define PD_TUNER_W 512         /* the difference function's window */
#define PD_TUNER_MAXLAG 440    /* the longest period searched: 25 Hz (below B0) */

typedef struct {
    int32_t lp1, lp2;          /* the decimation filter, Q4 */
    int32_t acc;               /* the samples summed towards the next decimated one */
    uint8_t k;                 /* how many so far */
    uint16_t fill;             /* decimated samples in the buffer */
    volatile uint8_t ready;    /* the buffer is full: the main loop's to read until pd_tuner_next */
} pd_tuner;

typedef struct {
    int32_t c16;               /* the pitch above MIDI note 0 (8.18 Hz), in 1/16 cent */
    uint8_t note;              /* the nearest MIDI note */
    int16_t cents10;           /* and the offset from it, in 1/10 cent, -500 .. 499 */
    uint32_t hz10;             /* the frequency in 1/10 Hz */
} pd_pitch;

void pd_tuner_reset(pd_tuner *t);
/* the audio side: frames of input at PD_FS into buf (PD_TUNER_N int16); does nothing while the buffer is ready */
void pd_tuner_feed(pd_tuner *t, int16_t *buf, const int16_t *in, unsigned frames);
/* the main loop: when the buffer is ready, look at it (scaling it in place) and start the next; returns 1 with p
 * filled when a pitch was found, 0 when there is none (silence, noise), -1 when no buffer was ready */
int pd_tuner_poll(pd_tuner *t, int16_t *buf, pd_pitch *p);
/* the analysis alone, on PD_TUNER_N decimated samples (scaled in place); 1 with p filled, or 0 */
int pd_tuner_detect(int16_t *buf, pd_pitch *p);

/* log2(v) in Q16, v > 0 (0: 0) */
int32_t pd_log2_q16(uint32_t v);

#endif
