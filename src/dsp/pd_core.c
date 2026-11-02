/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
#include "pd_dsp.h"

void pd_arena_init(pd_arena *a, void *base, size_t size)
{
    a->base = base;
    a->size = size;
    a->used = 0;
}

void *pd_arena_alloc(pd_arena *a, size_t n)
{
    size_t at = (a->used + 7u) & ~(size_t)7u;            /* 8: pointers on a 64-bit host */
    if (n > a->size || at > a->size - n)
        return NULL;
    a->used = at + n;
    for (size_t i = 0; i < n; i++)                       /* no libc: the layer is freestanding */
        a->base[at + i] = 0;
    return a->base + at;
}

int pd_node_make(pd_node *n, pd_arena *a, const PendelDSP *dsp, uint32_t sample_rate)
{
    size_t m = dsp->mem_size ? dsp->mem_size(sample_rate) : 0;
    void *state = pd_arena_alloc(a, dsp->state_size);
    void *mem = m ? pd_arena_alloc(a, m) : NULL;
    if (!state || (m && !mem))
        return -1;
    dsp->init(state, mem, sample_rate);
    n->dsp = dsp;
    n->state = state;
    return 0;
}
