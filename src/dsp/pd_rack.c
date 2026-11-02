/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
#include "pd_rack.h"

#include "pd_looper.h"
#include "pd_modules.h"

int pd_rack_add(pd_rack *r, pd_arena *a, const PendelDSP *dsp, uint32_t sample_rate)
{
    if (r->count >= PD_RACK_SLOTS)
        return -1;
    pd_slot *s = &r->slot[r->count];
    if (pd_node_make(&s->node, a, dsp, sample_rate))
        return -1;
    s->mix = PD_MIX_WET;
    s->on = 1;
    return (int)r->count++;
}

void pd_rack_move(pd_rack *r, unsigned from, unsigned to)
{
    if (from >= r->count || to >= r->count || from == to)
        return;
    pd_slot s = r->slot[from];
    if (from < to)
        for (unsigned i = from; i < to; i++)
            r->slot[i] = r->slot[i + 1];
    else
        for (unsigned i = from; i > to; i--)
            r->slot[i] = r->slot[i - 1];
    r->slot[to] = s;
}

void pd_rack_remove(pd_rack *r, unsigned at)
{
    if (at >= r->count)
        return;
    for (unsigned i = at; i + 1 < r->count; i++)
        r->slot[i] = r->slot[i + 1];
    r->count--;
}

void pd_rack_process(pd_rack *r, pd_sample *buf, unsigned frames)
{
    pd_sample dry[PD_BLOCK];
    for (unsigned k = 0; k < r->count; k++) {
        pd_slot *s = &r->slot[k];
        if (!s->on || s->mix <= 0)
            continue;
        if (s->mix >= PD_MIX_WET) {
            s->node.dsp->process(s->node.state, buf, buf, frames);
            continue;
        }
        for (unsigned i = 0; i < frames; i++)
            dry[i] = buf[i];
        s->node.dsp->process(s->node.state, buf, buf, frames);
        for (unsigned i = 0; i < frames; i++)
            buf[i] = dry[i] + ((pd_clamp(buf[i] - dry[i], -65535, 65535) * (s->mix >> 1)) >> 14);
    }
}

/* --------------------------------------------------------------- engine --- */

int pd_engine_init(pd_engine *e, pd_arena *a, uint32_t sample_rate)
{
    if (sample_rate != PD_FS)                /* the tables are made for one rate */
        return -1;
    e->master = PD_UNIT;
    e->source = e->prev = PD_SRC_INSTRUMENT;
    if (pd_node_make(&e->instrument, a, &PD_SYNTH, sample_rate) || pd_node_make(&e->buffer, a, &PD_INPUT, sample_rate))
        return -1;
    return pd_node_make(&e->looper, a, &PD_LOOPER, sample_rate);
}

void pd_engine_select(pd_engine *e, unsigned source)
{
    if (source >= PD_SOURCES || source == e->source)
        return;
    if (e->source == PD_SRC_INSTRUMENT)
        pd_synth_note_off(e->instrument.state, -1);   /* -1: whatever is sounding */
    e->source = (uint8_t)source;
}

static void render_source(pd_engine *e, unsigned src, const int16_t *in, pd_sample *out, unsigned frames)
{
    if (src == PD_SRC_INSTRUMENT) {
        e->instrument.dsp->process(e->instrument.state, NULL, out, frames);
        return;
    }
    for (unsigned i = 0; i < frames; i++)
        out[i] = in ? in[i] : 0;
    if (e->buffer.dsp)
        e->buffer.dsp->process(e->buffer.state, out, out, frames);
}

void pd_engine_process(pd_engine *e, const int16_t *in, int16_t *out_lr, unsigned frames)
{
    pd_sample a[PD_BLOCK];
    uint32_t t0 = e->load.now ? e->load.now() : 0;

    render_source(e, e->source, in, a, frames);
    if (e->prev != e->source) {              /* crossfade the old source out over this block */
        pd_sample b[PD_BLOCK];
        render_source(e, e->prev, in, b, frames);
        for (unsigned i = 0; i < frames; i++) {
            int32_t g = (int32_t)((i * PD_UNIT) / frames);
            a[i] = pd_mulq15(pd_clamp(a[i], -65535, 65535) >> 1, g) + pd_mulq15(pd_clamp(b[i], -65535, 65535) >> 1, PD_UNIT - g);
            a[i] *= 2;                       /* halves above keep each product in 32 bits */
        }
        e->prev = e->source;
    }
    pd_rack_process(&e->fx, a, frames);
    e->looper.dsp->process(e->looper.state, a, a, frames);
    for (unsigned i = 0; i < frames; i++) {
        int32_t m = (pd_clamp(a[i], -65535, 65535) * (e->master >> 2)) >> 13;
        out_lr[2 * i] = out_lr[2 * i + 1] = (int16_t)pd_sat16(m);
    }

    if (e->load.now) {
        pd_load *l = &e->load;
        l->last = l->now() - t0;
        if (l->last > l->peak)
            l->peak = l->last;
        if (l->budget && l->last > l->budget)
            l->over++;
    }
}
