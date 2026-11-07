/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* The DSP module interface and the fixed-point helpers every module shares.
 *
 * Samples are pd_sample (int32_t) in Q15: 32767 is full scale. Buffers may run above full scale between
 * modules (headroom); only the final output is saturated to 16 bits. No floating point, no 64-bit
 * multiplies (see felucca-bsp docs/CPU.md), no allocation after init. */
#ifndef PD_DSP_H
#define PD_DSP_H

#include <stddef.h>
#include <stdint.h>

#include "pd_tables.h"

typedef int32_t pd_sample;

#define PD_BLOCK 32            /* most frames a process call is given */
#define PD_UNIT 32768          /* 1.0 in Q15 */
#define PD_PARAM_MAX 32767     /* every parameter is set as 0..PD_PARAM_MAX; the module maps it */

/* A DSP module. `state` is state_size bytes, `mem` is mem_size(rate) bytes (delay lines and the like),
 * both taken from the arena at init. process() works in place (in == out) as well as out of place;
 * a source (a synth) ignores `in`. set_param() may divide and is called between blocks, never inside one. */
typedef struct PendelDSP {
    const char *name;
    unsigned state_size;
    size_t (*mem_size)(uint32_t sample_rate);                 /* NULL: no extra memory */
    unsigned num_params;
    const char *const *param_names;
    void (*init)(void *state, void *mem, uint32_t sample_rate);
    void (*process)(void *state, const pd_sample *in, pd_sample *out, unsigned frames);
    void (*reset)(void *state);
    void (*set_param)(void *state, unsigned id, int32_t value);
} PendelDSP;

/* ---------------------------------------------------------------- arena --- */
/* Memory for modules, handed out once at start-up from a static pool (the BSP's .pool on hardware). */
typedef struct {
    uint8_t *base;
    size_t size, used;
} pd_arena;

void pd_arena_init(pd_arena *a, void *base, size_t size);
void *pd_arena_alloc(pd_arena *a, size_t n);              /* 8-byte aligned, zeroed; NULL when full */

/* A module and its state, made from the arena. Returns 0, or -1 when the arena is full. */
typedef struct {
    const PendelDSP *dsp;
    void *state;
} pd_node;

int pd_node_make(pd_node *n, pd_arena *a, const PendelDSP *dsp, uint32_t sample_rate);

/* ------------------------------------------------------------- helpers --- */
static inline int32_t pd_clamp(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }
static inline int32_t pd_sat16(int32_t v) { return pd_clamp(v, -32768, 32767); }
/* a * b >> 15 for |a| < 2^16 and |b| <= 2^15 (the product stays in 32 bits) */
static inline int32_t pd_mulq15(int32_t a, int32_t b) { return (a * b) >> 15; }

/* sine of a 32-bit phase, Q15, linearly interpolated */
static inline int32_t pd_sine(uint32_t ph)
{
    uint32_t i = ph >> 22;
    int32_t a = PD_SINE[i], b = PD_SINE[i + 1];
    return a + (((b - a) * (int32_t)((ph >> 7) & 0x7FFFu)) >> 15);
}

/* a 0..PD_PARAM_MAX parameter looked up in a 257-point table, interpolated */
static inline int32_t pd_lut16(const int16_t *t, int32_t p)
{
    int32_t i = p >> 7, f = p & 127;
    return t[i] + (((t[i + 1] - t[i]) * f) >> 7);
}
static inline uint32_t pd_lut32(const uint32_t *t, int32_t p)
{
    int32_t i = p >> 7;
    uint32_t a = t[i], b = t[i + 1];
    return a + (((b - a) * (uint32_t)(p & 127)) >> 7);   /* steps below 2^25 (PD_TIME's are ~16k) */
}

/* floor(sqrt(x)), bit by bit (no division) */
static inline uint32_t pd_isqrt(uint32_t x)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > x)
        b >>= 2;
    for (; b; b >>= 2) {
        if (x >= r + b) {
            x -= r + b;
            r = (r >> 1) + b;
        } else
            r >>= 1;
    }
    return r;
}

#endif
