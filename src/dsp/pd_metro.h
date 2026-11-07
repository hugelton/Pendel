/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* The metronome: a beat clock at a tempo (or following a loop's position) and its click, a short sine burst, higher
 * on the bar's first beat when ACCENT is on. The clock runs whether the click sounds or not: the header's beat dots
 * read it. Timing in 1/256 sample, so a tempo's beats do not drift. */
#ifndef PD_METRO_H
#define PD_METRO_H

#include "pd_dsp.h"

#define PD_METRO_BEATS_MAX 8
#define PD_METRO_CLICK 2048    /* samples a click lasts (46 ms) */

typedef struct {
    uint32_t spb_q8;           /* samples a beat, Q8 */
    uint32_t pos_q8;           /* how far into the beat, Q8 */
    uint32_t ticks;            /* beats since the start */
    uint8_t beat, beats;       /* the beat in the bar (0 = the first), beats in a bar (1 .. PD_METRO_BEATS_MAX) */
    uint8_t on, accent;        /* the click sounds; the first beat higher and louder */
    uint8_t start;             /* the next sample is the bar's first beat */
    int32_t level;             /* the click's level, Q15 */
    /* the click sounding */
    uint32_t ph, inc;
    int32_t env, amp;          /* Q15 */
    uint32_t age;              /* samples since the last beat */
} pd_metro;

void pd_metro_init(pd_metro *m);
void pd_metro_tempo(pd_metro *m, uint32_t bpm10);            /* 1/10 BPM, 10 .. 3000 BPM; keeps its place */
void pd_metro_beats(pd_metro *m, unsigned beats);
void pd_metro_restart(pd_metro *m);                           /* the next sample is beat 1 */
/* follow a loop: its position and its samples a beat place the clock (call once a block, before process) */
void pd_metro_follow(pd_metro *m, uint32_t pos, uint32_t spb);
/* one block: the clock moves on, out[] gets the click (0 when none, or off) */
void pd_metro_process(pd_metro *m, pd_sample *out, unsigned frames);
/* how far into the beat, 0 .. 255 */
uint8_t pd_metro_frac(const pd_metro *m);

#endif
