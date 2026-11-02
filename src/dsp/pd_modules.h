/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* The modules: a VA synth (a source) and the effects for the rack. */
#ifndef PD_MODULES_H
#define PD_MODULES_H

#include "pd_dsp.h"

/* VA synth: one oscillator, a filter with its own envelope amount, an ADSR, a level. Monophonic. */
enum {
    PD_SYN_WAVE, PD_SYN_TUNE, PD_SYN_CUTOFF, PD_SYN_RESONANCE, PD_SYN_ENV_AMOUNT,
    PD_SYN_ATTACK, PD_SYN_DECAY, PD_SYN_SUSTAIN, PD_SYN_RELEASE, PD_SYN_LEVEL, PD_SYN_PARAMS
};
extern const PendelDSP PD_SYNTH;
void pd_synth_note_on(void *state, int note, int velocity);
void pd_synth_note_off(void *state, int note);   /* note < 0: whatever is sounding */

enum { PD_GAIN_GAIN, PD_GAIN_PARAMS };                       /* 0 .. 4x, 8192 = 1x */
extern const PendelDSP PD_FX_GAIN;

enum { PD_DRV_DRIVE, PD_DRV_TONE, PD_DRV_LEVEL, PD_DRV_PARAMS };
extern const PendelDSP PD_FX_DRIVE;

enum { PD_FLT_CUTOFF, PD_FLT_RESONANCE, PD_FLT_MODE, PD_FLT_PARAMS };
extern const PendelDSP PD_FX_FILTER;

/* input conditioning, the source's buffer stage: gain (0 .. 8x, 4096 = 1x), a noise gate (0 = off),
 * a one-pole low-cut (0 = off, else 20 .. 400 Hz). At 1x with gate and low-cut off it passes bit for bit. */
enum { PD_IN_GAIN, PD_IN_GATE, PD_IN_LOWCUT, PD_IN_PARAMS };
extern const PendelDSP PD_INPUT;

/* echoes only (the rack's mix sets the dry part); up to 1 s */
enum { PD_DLY_TIME, PD_DLY_FEEDBACK, PD_DLY_PARAMS };
extern const PendelDSP PD_FX_DELAY;

#endif
