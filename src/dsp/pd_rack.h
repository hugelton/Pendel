/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* The FX rack: effects in series, each with an on switch and a wet/dry mix, reorderable.
 * The engine: the synth and the audio input, each through a rack, mixed to the output. */
#ifndef PD_RACK_H
#define PD_RACK_H

#include "pd_dsp.h"

#define PD_RACK_SLOTS 8
#define PD_MIX_WET 32768       /* mix at which a slot's output is the effect alone */

typedef struct {
    pd_node node;
    int32_t mix;               /* Q15, 0 = dry .. PD_MIX_WET = wet */
    uint8_t on;
} pd_slot;

typedef struct {
    pd_slot slot[PD_RACK_SLOTS];
    unsigned count;
} pd_rack;

/* adds an effect at the end, on and fully wet; returns its index or -1 (rack or arena full) */
int pd_rack_add(pd_rack *r, pd_arena *a, const PendelDSP *dsp, uint32_t sample_rate);
void pd_rack_move(pd_rack *r, unsigned from, unsigned to);
/* takes a slot out (its memory stays in the arena: the app reuses a removed effect's node when it adds that kind again) */
void pd_rack_remove(pd_rack *r, unsigned at);
void pd_rack_process(pd_rack *r, pd_sample *buf, unsigned frames);   /* in place, frames <= PD_BLOCK */

/* --------------------------------------------------------------- engine --- */
/* One chain:  [source] -> [FX rack] -> [looper] -> out
 * The source is chosen: the instrument (the synth) or the audio input through its buffer stage (PD_INPUT).
 * Switching crossfades over a block; the instrument's note is released when it is switched away. */

enum { PD_SRC_INSTRUMENT, PD_SRC_INPUT, PD_SOURCES };

/* the time a block took against its budget, in the units of the counter the platform gives */
typedef struct {
    uint32_t (*now)(void);     /* NULL: not measured */
    uint32_t budget;           /* a block's real-time length in those units */
    uint32_t last, peak;
    uint32_t over;             /* blocks that took longer than the budget */
} pd_load;

typedef struct {
    pd_node instrument;
    pd_node buffer;            /* on the input: PD_INPUT (gain, gate, low-cut); dsp NULL = straight through */
    pd_rack fx;
    pd_node looper;
    uint8_t source, prev;
    int32_t master;            /* Q15, PD_UNIT = 1x */
    pd_load load;
} pd_engine;

int pd_engine_init(pd_engine *e, pd_arena *a, uint32_t sample_rate);
void pd_engine_select(pd_engine *e, unsigned source);
/* one block: mono 16-bit input in, interleaved stereo 16-bit out; frames <= PD_BLOCK */
void pd_engine_process(pd_engine *e, const int16_t *in, int16_t *out_lr, unsigned frames);

#endif
