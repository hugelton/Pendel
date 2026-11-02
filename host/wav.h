/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* 16-bit PCM WAV files on the host. */
#ifndef PD_WAV_H
#define PD_WAV_H

#include <stdint.h>
#include <stdio.h>

typedef struct {
    FILE *f;
    uint32_t frames;
    uint32_t rate;
    unsigned channels;
} pd_wav;

int pd_wav_open(pd_wav *w, const char *path, uint32_t rate, unsigned channels);   /* 0 or -1 */
void pd_wav_write(pd_wav *w, const int16_t *frames, uint32_t n);                 /* interleaved */
void pd_wav_close(pd_wav *w);

#endif
