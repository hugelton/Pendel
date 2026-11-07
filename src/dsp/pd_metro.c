/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
#include "pd_metro.h"

#include "pd_osc.h"

#define NOTE_BEAT (84 << 8)    /* C6, 1 kHz */
#define NOTE_ONE (96 << 8)     /* C7, 2 kHz: the first beat, accented */
#define DECAY_SHIFT 9          /* the click's envelope: falls by 1/512 a sample (12 ms to 1/e) */

void pd_metro_init(pd_metro *m)
{
    *m = (pd_metro){ 0 };
    m->beats = 4;
    m->accent = 1;
    m->level = 16384;
    pd_metro_tempo(m, 1200);
}

void pd_metro_tempo(pd_metro *m, uint32_t bpm10)
{
    bpm10 = bpm10 < 100u ? 100u : bpm10 > 30000u ? 30000u : bpm10;
    uint32_t whole = 600u * PD_FS / bpm10, rest = 600u * PD_FS % bpm10;   /* (600 * 48000 < 2^32) */
    m->spb_q8 = (whole << 8) + (rest << 8) / bpm10;           /* (a beat at 10 BPM: 2^22 samples Q8 < 2^31) */
    if (m->pos_q8 >= m->spb_q8)
        m->pos_q8 = m->spb_q8 - 1u;
}

void pd_metro_beats(pd_metro *m, unsigned beats)
{
    m->beats = (uint8_t)(beats < 1u ? 1u : beats > PD_METRO_BEATS_MAX ? PD_METRO_BEATS_MAX : beats);
    if (m->beat >= m->beats)
        m->beat = 0;
}

void pd_metro_restart(pd_metro *m)
{
    m->start = 1;
}

static void click(pd_metro *m)
{
    int one = m->beat == 0 && m->accent;
    m->inc = pd_pitch_inc(one ? NOTE_ONE : NOTE_BEAT);
    m->ph = 0;
    m->env = 32767;
    m->amp = one ? 32767 : m->accent ? 20000 : 26000;
    m->age = 0;
}

void pd_metro_follow(pd_metro *m, uint32_t pos, uint32_t spb)
{
    if (!spb)
        return;
    uint32_t b = pos / spb % m->beats;
    if (spb > 0xFFFFFFu)
        spb = 0xFFFFFFu;                                      /* (Q8 in 32 bits) */
    m->spb_q8 = spb << 8;
    m->pos_q8 = pos % spb << 8;
    if (b != m->beat) {                                       /* the loop came round (its length not a whole bar) */
        m->beat = (uint8_t)b;
        if (m->age < PD_BLOCK * 2u && b == 0)                 /* the click just sounded, as another beat: as the first */
            click(m);
    }
}

void pd_metro_process(pd_metro *m, pd_sample *out, unsigned frames)
{
    uint32_t pos = m->pos_q8, spb = m->spb_q8 ? m->spb_q8 : 256u;
    for (unsigned i = 0; i < frames; i++) {
        if (m->start) {
            m->start = 0;
            pos = 0;
            m->beat = 0;
            m->ticks++;
            click(m);
        } else if ((pos += 256u) >= spb) {
            pos -= spb;
            if (pos >= spb)
                pos = 0;
            m->beat = (uint8_t)(m->beat + 1u >= m->beats ? 0u : m->beat + 1u);
            m->ticks++;
            click(m);
        }
        int32_t s = 0;
        if (m->env) {
            s = pd_mulq15(pd_mulq15(pd_sine(m->ph), m->env), pd_mulq15(m->amp, m->level));
            m->ph += m->inc;
            m->env -= (m->env >> DECAY_SHIFT) + 1;
            if (m->env < 0 || m->age >= PD_METRO_CLICK)
                m->env = 0;
        }
        if (m->age < 0xFFFFFFFFu)
            m->age++;
        out[i] = m->on ? s : 0;
    }
    m->pos_q8 = pos;
}

uint8_t pd_metro_frac(const pd_metro *m)
{
    uint32_t spb = m->spb_q8 ? m->spb_q8 : 256u, p = m->pos_q8 < spb ? m->pos_q8 : spb - 1u;
    unsigned s = spb >> 23 ? 8u : 0u;                         /* (p * 255 in 32 bits) */
    return (uint8_t)((p >> s) * 255u / ((spb >> s) ? (spb >> s) : 1u));
}
