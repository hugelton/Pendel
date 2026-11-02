/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
#include "wav.h"

static void le(FILE *f, uint32_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        fputc((int)((v >> (8 * i)) & 0xFF), f);
}

static void header(pd_wav *w)
{
    uint32_t data = w->frames * w->channels * 2u;
    fseek(w->f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, w->f);
    le(w->f, 36u + data, 4);
    fwrite("WAVEfmt ", 1, 8, w->f);
    le(w->f, 16, 4);
    le(w->f, 1, 2);                              /* PCM */
    le(w->f, w->channels, 2);
    le(w->f, w->rate, 4);
    le(w->f, w->rate * w->channels * 2u, 4);
    le(w->f, w->channels * 2u, 2);
    le(w->f, 16, 2);
    fwrite("data", 1, 4, w->f);
    le(w->f, data, 4);
}

int pd_wav_open(pd_wav *w, const char *path, uint32_t rate, unsigned channels)
{
    w->f = fopen(path, "wb");
    if (!w->f)
        return -1;
    w->frames = 0;
    w->channels = channels;
    w->rate = rate;
    header(w);
    return 0;
}

void pd_wav_write(pd_wav *w, const int16_t *frames, uint32_t n)
{
    for (uint32_t i = 0; i < n * w->channels; i++)
        le(w->f, (uint16_t)frames[i], 2);
    w->frames += n;
}

void pd_wav_close(pd_wav *w)
{
    header(w);
    fclose(w->f);
    w->f = NULL;
}
