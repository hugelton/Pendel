/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* The looper at the end of the chain: one loop in RAM, record / play / overdub / stop / clear.
 * Its output is what comes in plus the loop. The loop is 16-bit mono, at most PD_LOOPER_SECONDS long. */
#ifndef PD_LOOPER_H
#define PD_LOOPER_H

#include "pd_dsp.h"

#ifndef PD_LOOPER_SECONDS
#define PD_LOOPER_SECONDS 2    /* 2 s at 44.1 kHz is 176 KB of the pool */
#endif
#define PD_LOOPER_FADE 64      /* samples faded at the loop's ends and when playback starts or stops */

enum { PD_LOOP_EMPTY, PD_LOOP_RECORD, PD_LOOP_PLAY, PD_LOOP_OVERDUB, PD_LOOP_STOP };
enum { PD_LOOP_LEVEL, PD_LOOP_PARAMS };

extern const PendelDSP PD_LOOPER;

/* the one-button cycle: empty -> record -> play -> overdub -> play ...; stopped -> play */
void pd_looper_press(void *state);
void pd_looper_stop(void *state);
void pd_looper_clear(void *state);
int pd_looper_state(const void *state);
uint32_t pd_looper_length(const void *state);   /* samples, 0 while empty or recording the first pass */
/* for drawing: the loop's samples, how many there are (while recording: so far), the play / record position, the room */
const int16_t *pd_looper_peek(const void *state, uint32_t *len, uint32_t *pos, uint32_t *cap);

#endif
