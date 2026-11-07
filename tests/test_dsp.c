/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Host tests for the DSP layer. Each test checks numbers; some also write a WAV to listen to.
 *
 *   test_dsp [WAV_DIR]      (WAVs are skipped when no directory is given) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../host/wav.h"
#include "pd_effects.h"
#include "pd_looper.h"
#include "pd_metro.h"
#include "pd_modules.h"
#include "pd_osc.h"
#include "pd_rack.h"
#include "pd_tuner.h"

static int fails, checks;
static const char *wav_dir;

#define CHECK(cond, ...)                                                                     \
    do {                                                                                     \
        checks++;                                                                            \
        if (!(cond)) {                                                                       \
            fails++;                                                                         \
            printf("  FAIL %s:%d: ", __FILE__, __LINE__);                                    \
            printf(__VA_ARGS__);                                                             \
            printf("\n");                                                                    \
        }                                                                                    \
    } while (0)

#define SECONDS(s) ((unsigned)(PD_FS * (s)) / PD_BLOCK * PD_BLOCK)   /* whole blocks */

static uint8_t pool[360 * 1024];             /* the size of Felucca's .pool on the same chip */

static int16_t *mono_buf(unsigned n) { return calloc(n, sizeof(int16_t)); }

static void save(const char *name, const int16_t *s, unsigned frames, unsigned ch)
{
    if (!wav_dir)
        return;
    char path[512];
    snprintf(path, sizeof path, "%s/%s", wav_dir, name);
    pd_wav w;
    if (pd_wav_open(&w, path, PD_FS, ch)) {
        printf("  (cannot write %s)\n", path);
        return;
    }
    pd_wav_write(&w, s, frames);
    pd_wav_close(&w);
    printf("  wrote %s\n", path);
}

/* rising zero crossings a second */
static unsigned crossings(const int16_t *s, unsigned n)
{
    unsigned c = 0;
    for (unsigned i = 1; i < n; i++)
        c += s[i - 1] < 0 && s[i] >= 0;
    return c;
}

static double rms(const pd_sample *s, unsigned n)
{
    double a = 0;
    for (unsigned i = 0; i < n; i++)
        a += (double)s[i] * s[i];
    return n ? __builtin_sqrt(a / n) : 0;
}

/* --------------------------------------------------------------- tests --- */

static void test_osc(void)
{
    static const char *const names[PD_WAVE_COUNT] = { "saw", "square", "triangle", "sine" };
    unsigned n = SECONDS(1);
    int16_t *all = mono_buf(n * PD_WAVE_COUNT);
    for (unsigned w = 0; w < PD_WAVE_COUNT; w++) {
        pd_osc o = { .inc = pd_pitch_inc(69 << 8), .wave = (uint8_t)w };
        int16_t *s = all + w * n;
        int32_t peak = 0;
        for (unsigned i = 0; i < n; i += PD_BLOCK) {
            pd_sample b[PD_BLOCK];
            unsigned k = n - i < PD_BLOCK ? n - i : PD_BLOCK;
            pd_osc_render(&o, b, k);
            for (unsigned j = 0; j < k; j++) {
                s[i + j] = (int16_t)pd_sat16(b[j] / 2);
                peak = abs(b[j]) > peak ? abs(b[j]) : peak;
            }
        }
        unsigned c = crossings(s, n);
        CHECK(c >= 439 && c <= 441, "%s at A4: %u crossings a second", names[w], c);
        CHECK(peak <= 40000, "%s peak %d", names[w], (int)peak);
    }
    save("osc_saw_square_tri_sine.wav", all, n * PD_WAVE_COUNT, 1);
    free(all);

    /* pitch: an octave up doubles the increment, within the interpolation's rounding */
    uint32_t a = pd_pitch_inc(57 << 8), b = pd_pitch_inc(69 << 8);
    CHECK(b / 2 - a < 4 || a - b / 2 < 4, "octave: %u vs %u", (unsigned)a, (unsigned)b);
}

static void test_env(void)
{
    pd_env e = { 0 };
    pd_env_set(&e, 0, 0, 16384, 0);          /* 1 ms segments, half sustain level */
    pd_env_gate(&e, 1);
    int32_t amp = 0;
    for (int i = 0; i < 20; i++)
        amp = pd_env_block(&e, PD_BLOCK);
    CHECK(e.stage == PD_ENV_SUSTAIN, "stage %d after 20 blocks", e.stage);
    CHECK(amp > 8000 && amp < 8500, "sustain amplitude %d (half level, squared)", (int)amp);
    pd_env_gate(&e, 0);
    for (int i = 0; i < 4; i++)
        amp = pd_env_block(&e, PD_BLOCK);
    CHECK(e.stage == PD_ENV_IDLE && amp == 0, "release: stage %d amp %d", e.stage, (int)amp);

    /* a 1 s attack (param where PD_TIME is ~1 s) takes about a second */
    pd_env_set(&e, 192 * 128, 0, PD_PARAM_MAX, 0);
    pd_env_gate(&e, 1);
    unsigned blocks = 0;
    while (e.stage == PD_ENV_ATTACK && blocks < 100000) {
        pd_env_block(&e, PD_BLOCK);
        blocks++;
    }
    double sec = (double)blocks * PD_BLOCK / PD_FS;
    CHECK(sec > 0.9 && sec < 1.1, "1 s attack took %.3f s", sec);
}

static double filter_gain(int mode, int32_t cutoff, int32_t hz)
{
    pd_svf s = { .mode = (uint8_t)mode };
    pd_svf_set(&s, cutoff, 0);
    pd_osc o = { .inc = (uint32_t)((double)hz / PD_FS * 4294967296.0), .wave = PD_WAVE_SINE };
    pd_sample in[PD_BLOCK], out[PD_BLOCK];
    double ri = 0, ro = 0;
    for (unsigned i = 0; i < SECONDS(0.5) / PD_BLOCK; i++) {
        pd_osc_render(&o, in, PD_BLOCK);
        pd_svf_process(&s, in, out, PD_BLOCK);
        if (i > 100) {
            ri += rms(in, PD_BLOCK);
            ro += rms(out, PD_BLOCK);
        }
    }
    return ro / ri;
}

static void test_filter(void)
{
    /* cutoff 8000 is ~108 Hz; 24000 is ~3.4 kHz */
    double lp_hi = filter_gain(PD_SVF_LP, 8000, 5000), lp_lo = filter_gain(PD_SVF_LP, 24000, 100);
    double hp_hi = filter_gain(PD_SVF_HP, 8000, 5000), hp_lo = filter_gain(PD_SVF_HP, 24000, 100);
    CHECK(lp_hi < 0.01, "LP 108 Hz passes 5 kHz at %.4f", lp_hi);
    CHECK(lp_lo > 0.9 && lp_lo < 1.1, "LP 3.4 kHz passes 100 Hz at %.3f", lp_lo);
    CHECK(hp_hi > 0.9 && hp_hi < 1.1, "HP 108 Hz passes 5 kHz at %.3f", hp_hi);
    CHECK(hp_lo < 0.01, "HP 3.4 kHz passes 100 Hz at %.4f", hp_lo);

    /* full resonance on a full-scale square stays bounded (UBSan catches an overflow) */
    pd_svf s = { .mode = PD_SVF_BP };
    pd_svf_set(&s, 16000, PD_PARAM_MAX);
    pd_osc o = { .inc = pd_pitch_inc(40 << 8), .wave = PD_WAVE_SQUARE };
    pd_sample b[PD_BLOCK];
    int32_t peak = 0;
    for (unsigned i = 0; i < SECONDS(1) / PD_BLOCK; i++) {
        pd_osc_render(&o, b, PD_BLOCK);
        pd_svf_process(&s, b, b, PD_BLOCK);
        for (unsigned j = 0; j < PD_BLOCK; j++)
            peak = abs(b[j]) > peak ? abs(b[j]) : peak;
    }
    CHECK(peak <= 65535, "resonant peak %d", (int)peak);

    /* little resonance and a cutoff of 7 kHz and up (where a Chamberlin SVF runs away): a low-pass passes 1 kHz */
    for (int32_t c = 27700; c <= PD_PARAM_MAX; c += 1010) {
        pd_svf t = { .mode = PD_SVF_LP };
        pd_svf_set(&t, c, 1638);
        o = (pd_osc){ .inc = (uint32_t)(1000.0 / PD_FS * 4294967296.0), .wave = PD_WAVE_SINE };
        double ri = 0, ro = 0;
        for (unsigned i = 0; i < SECONDS(0.5) / PD_BLOCK; i++) {
            pd_sample in[PD_BLOCK];
            pd_osc_render(&o, in, PD_BLOCK);
            pd_svf_process(&t, in, b, PD_BLOCK);
            if (i > 100) {
                ri += rms(in, PD_BLOCK);
                ro += rms(b, PD_BLOCK);
            }
        }
        CHECK(ro / ri > 0.97 && ro / ri < 1.03, "LP at %d, resonance 5%%, passes 1 kHz at %.3f", (int)c, ro / ri);
    }
}

/* the Filter effect in a mode: its gain for a sine at hz, its peak on a full-scale square at 55 Hz */
static double flt_gain(int mode, int32_t cutoff, int32_t res, double hz, int32_t *peak)
{
    static int64_t st[64];
    CHECK(PD_FX_FILTER.state_size <= sizeof st, "filter state %u B", PD_FX_FILTER.state_size);
    memset(st, 0, sizeof st);
    PD_FX_FILTER.init(st, NULL, PD_FS);
    PD_FX_FILTER.set_param(st, PD_FLT_MODE, (2 * mode + 1) * 32768 / (2 * PD_FLT_MODES));
    PD_FX_FILTER.set_param(st, PD_FLT_CUTOFF, cutoff);
    PD_FX_FILTER.set_param(st, PD_FLT_RESONANCE, res);
    pd_osc o = { .inc = (uint32_t)(hz / PD_FS * 4294967296.0), .wave = PD_WAVE_SINE };
    pd_sample in[PD_BLOCK], out[PD_BLOCK];
    double ri = 0, ro = 0;
    for (unsigned i = 0; i < SECONDS(0.5) / PD_BLOCK; i++) {
        pd_osc_render(&o, in, PD_BLOCK);
        PD_FX_FILTER.process(st, in, out, PD_BLOCK);
        if (i > 100) {
            ri += rms(in, PD_BLOCK);
            ro += rms(out, PD_BLOCK);
        }
    }
    if (peak) {
        PD_FX_FILTER.reset(st);
        o = (pd_osc){ .inc = pd_pitch_inc(33 << 8), .wave = PD_WAVE_SQUARE };
        *peak = 0;
        for (unsigned i = 0; i < SECONDS(0.3) / PD_BLOCK; i++) {
            pd_osc_render(&o, in, PD_BLOCK);
            for (unsigned j = 0; j < PD_BLOCK; j++)
                in[j] = in[j] < 0 ? -32768 : 32767;
            PD_FX_FILTER.process(st, in, out, PD_BLOCK);
            for (unsigned j = 0; j < PD_BLOCK; j++)
                *peak = abs(out[j]) > *peak ? abs(out[j]) : *peak;
        }
    }
    return ro / ri;
}

static void test_filter_modes(void)
{
    static const char *const NAME[] = { "LADDER", "SALLEN" };
    for (int m = PD_FLT_LADDER; m <= PD_FLT_SK; m++) {
        const char *n = NAME[m - PD_FLT_LADDER];
        double pass = flt_gain(m, 24000, 0, 100, NULL), stop = flt_gain(m, 8000, 0, 5000, NULL);
        CHECK(pass > 0.9 && pass < 1.1, "%s 3.4 kHz passes 100 Hz at %.3f", n, pass);
        CHECK(stop < (m == PD_FLT_LADDER ? 0.0005 : 0.005), "%s 108 Hz passes 5 kHz at %.5f", n, stop);
        /* the resonance peaks at the cutoff: 1 kHz through a 1 kHz filter, little and much resonance */
        double lo = flt_gain(m, 20500, 3000, 1000, NULL), hi = flt_gain(m, 20500, 26000, 1000, NULL);
        CHECK(hi > lo * 1.5, "%s resonance: %.3f at 5%%, %.3f at 80%%", n, lo, hi);
        /* every cutoff and resonance, a full-scale square: bounded (UBSan catches an overflow) */
        int32_t worst = 0;
        for (int32_t c = 0; c <= PD_PARAM_MAX; c += 4681)
            for (int32_t r = 0; r <= PD_PARAM_MAX; r += 8191) {
                int32_t p;
                double g = flt_gain(m, c, r, 440, &p);
                CHECK(g == g && g < 40, "%s at %d / %d: gain %.2f", n, (int)c, (int)r, g);
                worst = p > worst ? p : worst;
            }
        CHECK(worst <= 65535, "%s peak %d", n, (int)worst);
        /* little resonance and a high cutoff: 1 kHz passes, nothing runs away */
        for (int32_t c = 27700; c <= PD_PARAM_MAX; c += 1010) {
            double g = flt_gain(m, c, 1638, 1000, NULL);
            CHECK(g > (m == PD_FLT_LADDER ? 0.85 : 0.9) && g < 1.1, "%s at %d, resonance 5%%, passes 1 kHz at %.3f", n,
                  (int)c, g);                                     /* (the ladder loses some with resonance) */
        }
    }
    /* full resonance rings on by itself after a click */
    for (int m = PD_FLT_LADDER; m <= PD_FLT_SK; m++) {
        static int64_t st[64];
        memset(st, 0, sizeof st);
        PD_FX_FILTER.init(st, NULL, PD_FS);
        PD_FX_FILTER.set_param(st, PD_FLT_MODE, (2 * m + 1) * 32768 / (2 * PD_FLT_MODES));
        PD_FX_FILTER.set_param(st, PD_FLT_CUTOFF, 20500);
        PD_FX_FILTER.set_param(st, PD_FLT_RESONANCE, PD_PARAM_MAX);
        pd_sample b[PD_BLOCK];
        double e = 0;
        for (unsigned i = 0; i < SECONDS(1) / PD_BLOCK; i++) {
            for (unsigned j = 0; j < PD_BLOCK; j++)
                b[j] = i == 0 && j == 0 ? 16000 : 0;
            PD_FX_FILTER.process(st, b, b, PD_BLOCK);
            if (i > SECONDS(0.8) / PD_BLOCK)
                e = rms(b, PD_BLOCK);
        }
        CHECK(e > 100, "%s at full resonance: rms %.1f after 1 s", NAME[m - PD_FLT_LADDER], e);
    }
}

static void test_delay(void)
{
    pd_arena a;
    pd_arena_init(&a, pool, sizeof pool);
    pd_node d;
    CHECK(pd_node_make(&d, &a, &PD_FX_DELAY, PD_FS) == 0, "delay fits the pool");
    CHECK(a.used >= PD_FS * 2, "delay line in the arena (%zu bytes)", a.used);
    d.dsp->set_param(d.state, PD_DLY_TIME, 16384);
    d.dsp->set_param(d.state, PD_DLY_FEEDBACK, 16384);
    d.dsp->reset(d.state);
    unsigned expect = 1u + 16384u * (PD_FS - 2u) / PD_PARAM_MAX;

    unsigned n = SECONDS(2), first = 0, second = 0;
    for (unsigned i = 0; i < n; i += PD_BLOCK) {
        pd_sample b[PD_BLOCK] = { 0 };
        if (i == 0)
            b[0] = 20000;
        d.dsp->process(d.state, b, b, PD_BLOCK);
        for (unsigned j = 0; j < PD_BLOCK; j++)
            if (b[j]) {
                if (!first)
                    first = i + j;
                else if (!second && i + j > first + 10)
                    second = i + j;
            }
    }
    CHECK(first == expect, "first echo at %u, expected %u", first, expect);
    CHECK(second == 2 * expect, "second echo at %u, expected %u", second, 2 * expect);
}

static void test_rack(void)
{
    pd_arena a;
    pd_arena_init(&a, pool, sizeof pool);
    pd_rack r = { 0 };
    int drive = pd_rack_add(&r, &a, &PD_FX_DRIVE, PD_FS);
    int filter = pd_rack_add(&r, &a, &PD_FX_FILTER, PD_FS);
    CHECK(drive == 0 && filter == 1, "slots %d %d", drive, filter);

    pd_osc o = { .inc = pd_pitch_inc(52 << 8), .wave = PD_WAVE_SAW };
    pd_sample src[PD_BLOCK], b[PD_BLOCK];
    pd_osc_render(&o, src, PD_BLOCK);

    r.slot[0].on = r.slot[1].on = 0;         /* all off: bit exact */
    memcpy(b, src, sizeof b);
    pd_rack_process(&r, b, PD_BLOCK);
    CHECK(!memcmp(b, src, sizeof b), "rack with everything off changes the signal");

    r.slot[0].on = r.slot[1].on = 1;         /* on, mix 0: bit exact */
    r.slot[0].mix = r.slot[1].mix = 0;
    memcpy(b, src, sizeof b);
    pd_rack_process(&r, b, PD_BLOCK);
    CHECK(!memcmp(b, src, sizeof b), "rack at mix 0 changes the signal");

    r.slot[0].mix = r.slot[1].mix = PD_MIX_WET;
    memcpy(b, src, sizeof b);
    pd_rack_process(&r, b, PD_BLOCK);
    CHECK(memcmp(b, src, sizeof b), "rack fully wet leaves the signal as it was");

    pd_rack_move(&r, 0, 1);
    CHECK(r.slot[0].node.dsp == &PD_FX_FILTER && r.slot[1].node.dsp == &PD_FX_DRIVE, "move did not swap");

    /* the arena refuses what does not fit, and the rack refuses a ninth slot */
    pd_arena small;
    static uint8_t tiny[64];
    pd_arena_init(&small, tiny, sizeof tiny);
    pd_rack r2 = { 0 };
    CHECK(pd_rack_add(&r2, &small, &PD_FX_DELAY, PD_FS) == -1, "a 1 s delay fit in 64 bytes");
    for (int i = 0; i < PD_RACK_SLOTS; i++)
        pd_rack_add(&r2, &a, &PD_FX_GAIN, PD_FS);
    CHECK(pd_rack_add(&r2, &a, &PD_FX_GAIN, PD_FS) == -1, "a ninth slot was accepted");
}

static uint32_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec);
}

/* a plucked string (Karplus-Strong) standing in for a guitar at the input */
static void pluck(int16_t *out, unsigned n, unsigned period, uint32_t seed)
{
    int16_t line[1024];
    for (unsigned i = 0; i < period; i++) {
        seed = seed * 1664525u + 1013904223u;
        line[i] = (int16_t)((int32_t)(seed >> 16) - 32768) / 3;
    }
    for (unsigned i = 0, p = 0; i < n; i++) {
        unsigned q = p + 1 == period ? 0 : p + 1;
        out[i] = line[p];
        line[p] = (int16_t)(((int32_t)line[p] + line[q]) * 255 / 512);
        p = q;
    }
}

static void test_input(void)
{
    pd_arena a;
    pd_arena_init(&a, pool, sizeof pool);
    pd_node n;
    pd_node_make(&n, &a, &PD_INPUT, PD_FS);
    pd_osc o = { .inc = pd_pitch_inc(57 << 8), .wave = PD_WAVE_SINE };
    pd_sample src[PD_BLOCK], b[PD_BLOCK];

    pd_osc_render(&o, src, PD_BLOCK);        /* neutral: bit exact */
    for (unsigned j = 0; j < PD_BLOCK; j++)
        src[j] += 1;                          /* odd values too */
    memcpy(b, src, sizeof b);
    n.dsp->process(n.state, b, b, PD_BLOCK);
    CHECK(!memcmp(b, src, sizeof b), "neutral input stage changes the signal");

    n.dsp->set_param(n.state, PD_IN_GAIN, 8192);   /* 2x, after the ramp block */
    n.dsp->process(n.state, b, b, PD_BLOCK);
    memcpy(b, src, sizeof b);
    n.dsp->process(n.state, b, b, PD_BLOCK);
    int dbl = 1;
    for (unsigned j = 0; j < PD_BLOCK; j++)
        dbl &= b[j] == 2 * src[j];
    CHECK(dbl, "2x gain is not exact");
    n.dsp->set_param(n.state, PD_IN_GAIN, 4096);
    n.dsp->reset(n.state);

    /* gate: a quiet hum (-50 dBFS) is shut, a played note (-10 dBFS) passes */
    n.dsp->set_param(n.state, PD_IN_GATE, 8000);
    double quiet = 0, loud = 0;
    for (unsigned i = 0; i < SECONDS(0.5) / PD_BLOCK; i++) {
        pd_osc_render(&o, b, PD_BLOCK);
        for (unsigned j = 0; j < PD_BLOCK; j++)
            b[j] /= 300;
        n.dsp->process(n.state, b, b, PD_BLOCK);
        if (i > 200)
            quiet += rms(b, PD_BLOCK);
    }
    for (unsigned i = 0; i < SECONDS(0.5) / PD_BLOCK; i++) {
        pd_osc_render(&o, b, PD_BLOCK);
        for (unsigned j = 0; j < PD_BLOCK; j++)
            b[j] /= 3;
        n.dsp->process(n.state, b, b, PD_BLOCK);
        if (i > 200)
            loud += rms(b, PD_BLOCK);
    }
    CHECK(quiet == 0, "gate left the hum at %.1f", quiet);
    CHECK(loud > 0, "gate shut a played note");
    n.dsp->set_param(n.state, PD_IN_GATE, 0);

    /* low-cut: a DC offset is gone after a few time constants */
    n.dsp->set_param(n.state, PD_IN_LOWCUT, PD_PARAM_MAX);
    pd_sample dc[PD_BLOCK];
    for (unsigned i = 0; i < SECONDS(0.2) / PD_BLOCK; i++) {
        for (unsigned j = 0; j < PD_BLOCK; j++)
            dc[j] = 8000;
        n.dsp->process(n.state, dc, dc, PD_BLOCK);
    }
    CHECK(abs(dc[PD_BLOCK - 1]) < 80, "DC left after the low-cut: %d", (int)dc[PD_BLOCK - 1]);
}

static void test_looper(void)
{
    pd_arena a;
    pd_arena_init(&a, pool, sizeof pool);
    pd_node l;
    CHECK(pd_node_make(&l, &a, &PD_LOOPER, PD_FS) == 0, "looper fits the pool (%zu bytes)", a.used);
    pd_osc o = { .inc = pd_pitch_inc(69 << 8), .wave = PD_WAVE_SINE };
    pd_sample b[PD_BLOCK];
    unsigned rec = SECONDS(0.5);

    pd_sample *src = calloc(rec, sizeof *src);
    pd_looper_press(l.state);                /* record half a second of A4 */
    CHECK(pd_looper_state(l.state) == PD_LOOP_RECORD, "not recording");
    for (unsigned i = 0; i < rec; i += PD_BLOCK) {
        pd_osc_render(&o, b, PD_BLOCK);
        memcpy(src + i, b, sizeof b);
        l.dsp->process(l.state, b, b, PD_BLOCK);
    }
    pd_looper_press(l.state);
    CHECK(pd_looper_state(l.state) == PD_LOOP_PLAY && pd_looper_length(l.state) == rec,
          "state %d length %u, expected play and %u", pd_looper_state(l.state), (unsigned)pd_looper_length(l.state), rec);

    /* playback with silence in: the loop repeats with its period; the seam has no jump */
    pd_sample *out = calloc(3 * rec, sizeof *out);
    for (unsigned i = 0; i < 3 * rec; i += PD_BLOCK) {
        pd_sample z[PD_BLOCK] = { 0 };
        l.dsp->process(l.state, z, out + i, PD_BLOCK);
    }
    int same = 1;
    for (unsigned i = rec; i < 2 * rec && same; i++)
        same = out[i] == out[i + rec];
    CHECK(same, "second and third passes differ");
    int32_t jump = 0;
    for (unsigned i = rec - 4; i < rec + 4; i++)
        jump = abs(out[i] - out[i - 1]) > jump ? abs(out[i] - out[i - 1]) : jump;
    CHECK(jump < 2000, "seam jump %d", (int)jump);
    double es = 0, ss = 0;                   /* the ADPCM loop against what went in, away from the faded seam */
    for (unsigned i = PD_LOOPER_FADE; i < rec - PD_LOOPER_FADE; i++) {
        double d = (double)out[rec + i] - src[i];
        es += d * d;
        ss += (double)src[i] * src[i];
    }
    CHECK(es < ss * 1e-3, "loop SNR %.1f dB, expected over 30", 10 * log10(ss / (es > 0 ? es : 1)));
    printf("  loop SNR %.1f dB (4-bit ADPCM), %u s\n", 10 * log10(ss / (es > 0 ? es : 1)), PD_LOOPER_SECONDS);

    /* overdub doubles the loop with the same signal in phase */
    pd_looper_press(l.state);
    CHECK(pd_looper_state(l.state) == PD_LOOP_OVERDUB, "not overdubbing");
    int32_t before = 0, after = 0;
    for (unsigned i = 0; i < rec; i += PD_BLOCK) {
        pd_sample z[PD_BLOCK] = { 0 };
        l.dsp->process(l.state, z, b, PD_BLOCK);
        for (unsigned j = 0; j < PD_BLOCK; j++)
            before = abs(b[j]) > before ? abs(b[j]) : before;
    }
    pd_looper_press(l.state);
    for (unsigned i = 0; i < rec; i += PD_BLOCK) {
        pd_sample z[PD_BLOCK] = { 0 };
        l.dsp->process(l.state, z, b, PD_BLOCK);
        for (unsigned j = 0; j < PD_BLOCK; j++)
            after = abs(b[j]) > after ? abs(b[j]) : after;
    }
    CHECK(after == before, "overdub of silence changed the loop (%d -> %d)", (int)before, (int)after);

    pd_looper_stop(l.state);                 /* stop fades out, then the input passes alone */
    for (unsigned i = 0; i < 4 * PD_BLOCK; i += PD_BLOCK) {
        pd_sample z[PD_BLOCK] = { 0 };
        l.dsp->process(l.state, z, b, PD_BLOCK);
    }
    int silent = 1;
    for (unsigned j = 0; j < PD_BLOCK; j++)
        silent &= b[j] == 0;
    CHECK(silent && pd_looper_state(l.state) == PD_LOOP_STOP, "stop left sound");
    pd_looper_clear(l.state);
    CHECK(pd_looper_state(l.state) == PD_LOOP_EMPTY && pd_looper_length(l.state) == 0, "clear");

    /* recording past the end closes the loop at full length */
    pd_looper_press(l.state);
    for (unsigned i = 0; i < SECONDS(PD_LOOPER_SECONDS) + 2 * PD_BLOCK; i += PD_BLOCK) {
        pd_osc_render(&o, b, PD_BLOCK);
        l.dsp->process(l.state, b, b, PD_BLOCK);
    }
    CHECK(pd_looper_state(l.state) == PD_LOOP_PLAY && pd_looper_length(l.state) == PD_FS * PD_LOOPER_SECONDS,
          "full: state %d length %u", pd_looper_state(l.state), (unsigned)pd_looper_length(l.state));
    free(out);
    free(src);
}

static void test_engine(void)
{
    pd_arena a;
    pd_arena_init(&a, pool, sizeof pool);
    pd_engine e = { 0 };
    CHECK(pd_engine_init(&e, &a, PD_FS) == 0, "engine init (arena %zu of %zu)", a.used, a.size);
    CHECK(pd_engine_init(&(pd_engine){ 0 }, &a, 48000) == -1, "engine accepted a rate the tables are not for");

    /* input selected, rack empty, looper empty: the input passes bit for bit */
    pd_engine_select(&e, PD_SRC_INPUT);
    unsigned n = SECONDS(0.5);
    int16_t *in = mono_buf(n), *out = mono_buf(2 * n);
    pluck(in, n, 200, 1);
    for (unsigned i = 0; i < n; i += PD_BLOCK)
        pd_engine_process(&e, in + i, out + 2 * i, PD_BLOCK);
    int same = 1;
    for (unsigned i = PD_BLOCK; i < n && same; i++)  /* the first block crossfades from the instrument */
        same = out[2 * i] == in[i] && out[2 * i + 1] == in[i];
    CHECK(same, "dry input path is not bit exact");
    free(in);
    free(out);

    /* a performance: guitar through drive and delay, looped, then the synth over the loop; timed */
    pd_rack_add(&e.fx, &a, &PD_FX_DRIVE, PD_FS);
    int dly = pd_rack_add(&e.fx, &a, &PD_FX_DELAY, PD_FS);
    CHECK(dly == 1, "delay slot %d (arena %zu of %zu)", dly, a.used, a.size);
    if (dly == 1)
        e.fx.slot[1].mix = 10000;

    n = SECONDS(8);
    in = mono_buf(n);
    out = mono_buf(2 * n);
    static const unsigned periods[] = { 168, 200, 224, 252 };   /* roughly E3 .. C4 */
    unsigned bar = SECONDS(0.5);
    for (unsigned k = 0; k < 4; k++)
        pluck(in + k * bar, bar, periods[k], 7 + k);
    static const int melody[] = { 69, 72, 76, 79, 81, 79, 76, 72 };
    e.load.now = now_ns;
    e.load.budget = (uint32_t)(1000000000ull * PD_BLOCK / PD_FS);
    uint64_t total = 0;
    unsigned blocks = 0, step = SECONDS(0.25);
    pd_looper_press(e.looper.state);                       /* record the 2 s riff */
    for (unsigned i = 0; i < n; i += PD_BLOCK, blocks++) {
        if (i == 4 * bar) {
            pd_looper_press(e.looper.state);               /* close: it plays */
            pd_engine_select(&e, PD_SRC_INSTRUMENT);
        }
        if (i >= 4 * bar && (i - 4 * bar) % step < PD_BLOCK)
            pd_synth_note_on(e.instrument.state, melody[((i - 4 * bar) / step) % 8], 100);
        else if (i >= 4 * bar && (i - 4 * bar) % step == step / 2 / PD_BLOCK * PD_BLOCK)
            pd_synth_note_off(e.instrument.state, -1);
        pd_engine_process(&e, in + i, out + 2 * i, PD_BLOCK);
        total += e.load.last;
    }
    CHECK(pd_looper_length(e.looper.state) == 4 * bar, "loop length %u", (unsigned)pd_looper_length(e.looper.state));
    printf("  host load: %.2f%% of real time a block on average, peak %.2f%%\n",
           100.0 * (double)total / blocks / e.load.budget, 100.0 * e.load.peak / e.load.budget);
    printf("  arena: %zu of %zu bytes\n", a.used, a.size);
    save("engine_loop_and_synth.wav", out, n, 2);
    free(in);
    free(out);
}

static void test_synth_sweep(void)
{
    pd_arena a;
    pd_arena_init(&a, pool, sizeof pool);
    pd_node s;
    pd_node_make(&s, &a, &PD_SYNTH, PD_FS);
    unsigned n = SECONDS(4);
    int16_t *out = mono_buf(n);
    s.dsp->set_param(s.state, PD_SYN_RESONANCE, 26000);
    s.dsp->set_param(s.state, PD_SYN_ENV_AMOUNT, 0);
    s.dsp->set_param(s.state, PD_SYN_SUSTAIN, PD_PARAM_MAX);
    pd_synth_note_on(s.state, 45, 127);
    int32_t peak = 0;
    for (unsigned i = 0; i < n; i += PD_BLOCK) {
        pd_sample b[PD_BLOCK];
        s.dsp->set_param(s.state, PD_SYN_CUTOFF, (int32_t)((uint64_t)i * PD_PARAM_MAX / n));
        s.dsp->process(s.state, NULL, b, PD_BLOCK);
        for (unsigned j = 0; j < PD_BLOCK; j++) {
            out[i + j] = (int16_t)pd_sat16(b[j]);
            peak = abs(b[j]) > peak ? abs(b[j]) : peak;
        }
    }
    CHECK(peak > 3000 && peak <= 65535, "sweep peak %d", (int)peak);
    save("synth_filter_sweep.wav", out, n, 1);
    free(out);
}

/* pd_effects.c: every value at its ends and middle, fed past full scale (the sanitizer stops on any 32-bit overflow);
 * bounded output, quiet in silence, small memory; then what each does, measured */
static void fx_run(pd_node *n, const pd_sample *in, pd_sample *out, unsigned frames)
{
    for (unsigned o = 0; o < frames; o += PD_BLOCK)
        n->dsp->process(n->state, in + o, out + o, PD_BLOCK);
}

static void test_effects(void)
{
    static const PendelDSP *const FX[] = {
        &PD_FX_FUZZ, &PD_FX_WAH, &PD_FX_CRUSH, &PD_FX_FLANGER, &PD_FX_PHASER,
        &PD_FX_TREMOLO, &PD_FX_WOBBLE, &PD_FX_NOISE, &PD_FX_PITCH, &PD_FX_ROOM,
    };
    static const int32_t V[3] = { 0, 16384, PD_PARAM_MAX };
    const unsigned n = SECONDS(0.5);
    pd_sample *loud = calloc(n, sizeof *loud), *quiet = calloc(n, sizeof *quiet), *out = calloc(n, sizeof *out);
    uint32_t r = 1;
    for (unsigned i = 0; i < n; i++) {                       /* a 110 Hz saw at 2x full scale, plus noise */
        r = r * 1664525u + 1013904223u;
        loud[i] = (pd_sample)((int32_t)((i * 110u * 2u * 65535u / PD_FS) % 131070u) - 65535 + (int32_t)(r >> 24) - 128);
        loud[i] = pd_clamp(loud[i], -65535, 65535);
    }
    for (size_t k = 0; k < sizeof FX / sizeof FX[0]; k++) {
        pd_arena a;
        pd_node nd;
        pd_arena_init(&a, pool, sizeof pool);
        CHECK(pd_node_make(&nd, &a, FX[k], PD_FS) == 0, "%s: made", FX[k]->name);
        CHECK(a.used <= 8192 + 256, "%s: %zu bytes", FX[k]->name, a.used);
        for (unsigned p0 = 0; p0 < 3; p0++)
            for (unsigned p1 = 0; p1 < 3; p1++) {
                int32_t peak = 0, qpeak = 0;
                FX[k]->set_param(nd.state, 0, V[p0]);
                FX[k]->set_param(nd.state, 1, V[p1]);
                FX[k]->reset(nd.state);
                fx_run(&nd, loud, out, n);
                for (unsigned i = 0; i < n; i++)
                    peak = out[i] > peak ? out[i] : -out[i] > peak ? -out[i] : peak;
                FX[k]->reset(nd.state);
                fx_run(&nd, quiet, out, n);
                for (unsigned i = n / 2; i < n; i++)         /* (after any ringing) */
                    qpeak = out[i] > qpeak ? out[i] : -out[i] > qpeak ? -out[i] : qpeak;
                CHECK(peak < 4 * 65536, "%s %d/%d: peak %d", FX[k]->name, (int)V[p0], (int)V[p1], (int)peak);
                CHECK(qpeak <= (FX[k] == &PD_FX_NOISE ? 32768 : 4), "%s %d/%d: %d in silence", FX[k]->name,
                      (int)V[p0], (int)V[p1], (int)qpeak);
            }
    }
    {   /* PITCH +12: twice the crossings of a 220 Hz sine; -12: half */
        pd_arena a;
        pd_node nd;
        pd_sample *sine = calloc(n, sizeof *sine);
        unsigned c[3];
        for (unsigned i = 0; i < n; i++)
            sine[i] = pd_sine((uint32_t)(i * (uint32_t)(220.0 / PD_FS * 4294967296.0))) / 2;
        pd_arena_init(&a, pool, sizeof pool);
        pd_node_make(&nd, &a, &PD_FX_PITCH, PD_FS);
        for (unsigned t = 0; t < 3; t++) {
            int16_t *m = mono_buf(n);
            nd.dsp->set_param(nd.state, PD_PIT_SEMI, V[t]);
            nd.dsp->reset(nd.state);
            fx_run(&nd, sine, out, n);
            for (unsigned i = 0; i < n; i++)                 /* the shifted voice alone: take the dry part out */
                m[i] = (int16_t)pd_sat16(out[i] - ((sine[i] * 18022) >> 15));
            c[t] = crossings(m + n / 2, n / 2);
            free(m);
        }
        CHECK(c[2] > c[1] * 17 / 10 && c[0] < c[1] * 6 / 10, "PITCH crossings -12 %u, 0 %u, +12 %u", c[0], c[1], c[2]);
        free(sine);
    }
    {   /* CRUSH at 16 bits and no hold passes */
        pd_arena a;
        pd_node nd;
        int32_t diff = 0;
        pd_arena_init(&a, pool, sizeof pool);
        pd_node_make(&nd, &a, &PD_FX_CRUSH, PD_FS);
        nd.dsp->set_param(nd.state, PD_CRUSH_BITS, 0);
        nd.dsp->set_param(nd.state, PD_CRUSH_DOWN, 0);
        fx_run(&nd, loud, out, n);
        for (unsigned i = 0; i < n; i++)
            diff |= out[i] != pd_sat16(loud[i]);
        CHECK(!diff, "CRUSH at 16 bits, 1x changes the sound");
    }
    {   /* the cost of each, at its middle values (relative: the host is not the chip) */
        for (size_t k = 0; k < sizeof FX / sizeof FX[0]; k++) {
            pd_arena a;
            pd_node nd;
            clock_t t0;
            pd_arena_init(&a, pool, sizeof pool);
            pd_node_make(&nd, &a, FX[k], PD_FS);
            t0 = clock();
            for (unsigned rep = 0; rep < 20; rep++)
                fx_run(&nd, loud, out, n);
            printf("  %-8s %5.1f ns a sample\n", FX[k]->name, (double)(clock() - t0) / CLOCKS_PER_SEC * 1e9 / (20.0 * n));
        }
    }
    if (wav_dir) {                                           /* each on a plucked 110 Hz saw */
        int16_t *m = mono_buf(n);
        for (unsigned i = 0; i < n; i++)
            loud[i] = (pd_sample)((((int32_t)((i * 110u * 2u * 32767u / PD_FS) % 65534u) - 32767) / 3) * (int32_t)(n - i) / (int32_t)n);
        for (size_t k = 0; k < sizeof FX / sizeof FX[0]; k++) {
            pd_arena a;
            pd_node nd;
            char name[64];
            pd_arena_init(&a, pool, sizeof pool);
            pd_node_make(&nd, &a, FX[k], PD_FS);
            fx_run(&nd, loud, out, n);
            for (unsigned i = 0; i < n; i++)
                m[i] = (int16_t)pd_sat16(out[i]);
            snprintf(name, sizeof name, "fx_%s.wav", FX[k]->name);
            save(name, m, n, 1);
        }
        free(m);
    }
    free(loud);
    free(quiet);
    free(out);
}

/* the amp: every model at the ends of gain and tone, past full scale; OFF passes; quiet in silence; its cost */
/* the effects about level: plucked strings at -8 dBFS peaks through each at its defaults (the app's), the RMS within
 * 3.5 dB of the dry (not asked: the ones that colour the level by design, a tremolo, a room, and the high-pass, which
 * takes a guitar's body away at 1.4 kHz) */
static double pluck_at(unsigned i)
{
    static const double hz[] = { 82.41, 110, 146.83, 196, 246.94, 329.63, 164.8, 123.47 };
    unsigned note = i / (PD_FS / 2) % 8u;
    double t = (double)(i % (PD_FS / 2)) / PD_FS, s = 0;
    for (int h = 1; h <= 12; h++)
        s += sin(2 * M_PI * hz[note] * h * t) / h * exp(-t * (2.0 + h * 0.8));
    return 8000 * s * exp(-t * 3);
}

static void test_levels(void)
{
#define CH(v, n) ((int32_t)(((2 * (v) + 1) * 32768) / (2 * (n))))
    static const struct { const char *name; const PendelDSP *dsp; unsigned np; int32_t v[7]; } fx[] = {
        { "DRIVE", &PD_FX_DRIVE, 3, { 12000, 20000, 20000 } },
        { "FILTER LP", &PD_FX_FILTER, 3, { 20000, 6000, CH(0, PD_FLT_MODES) } },
        { "FILTER BP", &PD_FX_FILTER, 3, { 20000, 6000, CH(1, PD_FLT_MODES) } },
        { "FILTER LADDER", &PD_FX_FILTER, 3, { 20000, 6000, CH(3, PD_FLT_MODES) } },
        { "FILTER SALLEN", &PD_FX_FILTER, 3, { 20000, 6000, CH(4, PD_FLT_MODES) } },
        { "FUZZ", &PD_FX_FUZZ, 2, { 16000, 6000 } },
        { "FLANGER", &PD_FX_FLANGER, 2, { 9000, 16000 } },
        { "PHASER", &PD_FX_PHASER, 2, { 9000, 20000 } },
        { "AMP CLEAN", &PD_FX_AMP, 7, { CH(1, 4), 12000, 16384, 16384, 16384, 16384, CH(1, 2) } },
        { "AMP CRUNCH", &PD_FX_AMP, 7, { CH(2, 4), 12000, 16384, 16384, 16384, 16384, CH(1, 2) } },
        { "AMP LEAD", &PD_FX_AMP, 7, { CH(3, 4), 12000, 16384, 16384, 16384, 16384, CH(1, 2) } },
    };
#undef CH
    for (unsigned k = 0; k < sizeof fx / sizeof fx[0]; k++) {
        pd_arena a;
        pd_node n;
        pd_arena_init(&a, pool, sizeof pool);
        CHECK(pd_node_make(&n, &a, fx[k].dsp, PD_FS) == 0, "%s fits the pool", fx[k].name);
        for (unsigned p = 0; p < fx[k].np; p++)
            n.dsp->set_param(n.state, p, fx[k].v[p]);
        n.dsp->reset(n.state);
        double ei = 0, eo = 0;
        for (unsigned i = 0; i < SECONDS(4); i += PD_BLOCK) {
            pd_sample b[PD_BLOCK], d[PD_BLOCK];
            for (unsigned j = 0; j < PD_BLOCK; j++)
                b[j] = d[j] = (pd_sample)lrint(pluck_at(i + j));
            n.dsp->process(n.state, b, b, PD_BLOCK);
            if (i >= SECONDS(0.5))
                for (unsigned j = 0; j < PD_BLOCK; j++) {
                    ei += (double)d[j] * d[j];
                    eo += (double)b[j] * b[j];
                }
        }
        double db = 10 * log10(eo / ei);
        printf("  %-14s %+5.1f dB\n", fx[k].name, db);
        CHECK(fabs(db) < 3.5, "%s: %+.1f dB against the dry", fx[k].name, db);
    }
}

static void test_amp(void)
{
    const unsigned n = SECONDS(0.5);
    pd_sample *loud = calloc(n, sizeof *loud), *quiet = calloc(n, sizeof *quiet), *out = calloc(n, sizeof *out);
    pd_arena a;
    pd_node nd;
    int32_t diff = 0;
    clock_t t0;
    for (unsigned i = 0; i < n; i++)
        loud[i] = (pd_sample)((int32_t)((i * 110u * 2u * 65535u / PD_FS) % 131070u) - 65535);
    pd_arena_init(&a, pool, sizeof pool);
    CHECK(pd_node_make(&nd, &a, &PD_FX_AMP, PD_FS) == 0 && a.used < 1024, "amp: %zu bytes", a.used);
    for (int32_t m = 0; m < PD_AMP_MODELS; m++)
        for (unsigned k = 0; k < 16u; k++) {
            int32_t peak = 0, qpeak = 0;
            nd.dsp->set_param(nd.state, PD_AMP_MODEL, (2 * m + 1) * 32768 / (2 * PD_AMP_MODELS));
            nd.dsp->set_param(nd.state, PD_AMP_GAIN, k & 1u ? PD_PARAM_MAX : 0);
            nd.dsp->set_param(nd.state, PD_AMP_BASS, k & 2u ? PD_PARAM_MAX : 0);
            nd.dsp->set_param(nd.state, PD_AMP_MID, k & 4u ? PD_PARAM_MAX : 0);
            nd.dsp->set_param(nd.state, PD_AMP_TREBLE, k & 8u ? PD_PARAM_MAX : 0);
            nd.dsp->set_param(nd.state, PD_AMP_LEVEL, PD_PARAM_MAX);
            nd.dsp->set_param(nd.state, PD_AMP_CAB, k & 1u ? PD_PARAM_MAX : 0);
            nd.dsp->reset(nd.state);
            fx_run(&nd, loud, out, n);
            for (unsigned i = 0; i < n; i++)
                peak = out[i] > peak ? out[i] : -out[i] > peak ? -out[i] : peak;
            nd.dsp->reset(nd.state);
            fx_run(&nd, quiet, out, n);
            for (unsigned i = n / 2; i < n; i++)
                qpeak = out[i] > qpeak ? out[i] : -out[i] > qpeak ? -out[i] : qpeak;
            CHECK(peak < 4 * 65536, "amp model %d set %u: peak %d", (int)m, k, (int)peak);
            CHECK(qpeak <= 4, "amp model %d set %u: %d in silence", (int)m, k, (int)qpeak);
        }
    nd.dsp->set_param(nd.state, PD_AMP_MODEL, 0);
    fx_run(&nd, loud, out, n);
    for (unsigned i = 0; i < n; i++)
        diff |= out[i] != loud[i];
    CHECK(!diff, "amp OFF changes the sound");
    nd.dsp->set_param(nd.state, PD_AMP_MODEL, 7 * 32768 / 8);       /* LEAD, gain at the top, the cabinet on */
    nd.dsp->set_param(nd.state, PD_AMP_GAIN, PD_PARAM_MAX);
    nd.dsp->set_param(nd.state, PD_AMP_CAB, PD_PARAM_MAX);
    t0 = clock();
    for (unsigned rep = 0; rep < 20; rep++)
        fx_run(&nd, loud, out, n);
    printf("  %-8s %5.1f ns a sample (LEAD, cabinet)\n", "Amp", (double)(clock() - t0) / CLOCKS_PER_SEC * 1e9 / (20.0 * n));
    if (wav_dir) {
        int16_t *m = mono_buf(n);
        for (int32_t md = 1; md < PD_AMP_MODELS; md++) {
            char name[32];
            for (unsigned i = 0; i < n; i++)
                loud[i] = (pd_sample)((((int32_t)((i * 110u * 2u * 32767u / PD_FS) % 65534u) - 32767) / 4) * (int32_t)(n - i) / (int32_t)n);
            nd.dsp->set_param(nd.state, PD_AMP_MODEL, (2 * md + 1) * 32768 / (2 * PD_AMP_MODELS));
            nd.dsp->set_param(nd.state, PD_AMP_GAIN, 20000);
            nd.dsp->reset(nd.state);
            fx_run(&nd, loud, out, n);
            for (unsigned i = 0; i < n; i++)
                m[i] = (int16_t)pd_sat16(out[i]);
            snprintf(name, sizeof name, "amp_%d.wav", (int)md);
            save(name, m, n, 1);
        }
        free(m);
    }
    free(loud);
    free(quiet);
    free(out);
}


/* ----------------------------------------------------------------- tuner --- */

/* feeds `n` samples of src through the tuner in blocks, as the audio does; the last look's result (and how many
 * looks found a pitch, of how many) */
static int tuner_listen(const int16_t *src, unsigned n, pd_pitch *last, unsigned *looks)
{
    static int16_t buf[PD_TUNER_N];
    pd_tuner t;
    pd_tuner_reset(&t);
    int found = 0;
    *looks = 0;
    for (unsigned i = 0; i + PD_BLOCK <= n; i += PD_BLOCK) {
        pd_tuner_feed(&t, buf, src + i, PD_BLOCK);
        pd_pitch p;
        int r = pd_tuner_poll(&t, buf, &p);
        if (r < 0)
            continue;
        if (++*looks == 1)
            continue;                        /* (the first look holds the filter's start) */
        if (r == 1) {
            found++;
            *last = p;
        }
    }
    return found;
}

static double cents_of(double hz) { return 1200.0 * log2(hz / 8.175798915643707); }

static void test_tuner(void)
{
    unsigned n = SECONDS(0.6);                 /* six looks */
    int16_t *s = mono_buf(n);
    pd_sample blk[PD_BLOCK];

    /* log2 */
    double worst_l = 0;
    for (uint32_t v = 1; v < 0x7FFFFFFFu; v = v * 3u + 7u) {
        double e = fabs(pd_log2_q16(v) / 65536.0 - log2((double)v));
        if (e > worst_l)
            worst_l = e;
    }
    CHECK(worst_l < 3.0 / 65536, "log2 off by %.6f", worst_l);

    /* E1 .. E6, every semitone, in tune and off (by +14 and -35 cents), sine and saw, loud and quiet */
    static const int detune[] = { 0, 37, -90 };     /* 1/256 semitone */
    static const char *const wname[] = { "sine", "saw" };
    for (int w = 0; w < 2; w++) {
        double worst = 0;
        int missed = 0, wrong = 0, tried = 0;
        for (int note = 28; note <= 88; note++)
            for (unsigned dt = 0; dt < 3; dt++)
                for (int loud = 0; loud < 2; loud++) {
                    pd_osc o = { .inc = pd_pitch_inc((note << 8) + detune[dt]),
                                 .wave = (uint8_t)(w ? PD_WAVE_SAW : PD_WAVE_SINE) };
                    int32_t amp = loud ? 12000 : 600;
                    for (unsigned i = 0; i < n; i += PD_BLOCK) {
                        pd_osc_render(&o, blk, PD_BLOCK);
                        for (unsigned j = 0; j < PD_BLOCK; j++)
                            s[i + j] = (int16_t)((blk[j] * amp) >> 15);
                    }
                    double hz = (double)o.inc * PD_FS / 4294967296.0, want = cents_of(hz);
                    pd_pitch p;
                    unsigned looks;
                    int f = tuner_listen(s, n, &p, &looks);
                    tried++;
                    if (f < (int)looks - 1) {
                        missed++;
                        continue;
                    }
                    double got = p.c16 / 16.0, e = fabs(got - want);
                    int near = (int)floor(want / 100.0 + 0.5);
                    if (p.note != near || e > 2.0 || fabs(p.hz10 / 10.0 - hz) > hz * 0.002 + 0.1) {
                        if (wrong < 4)
                            printf("  %s note %d %+d/256: got note %d %+.1f c (%.1f Hz), want %.2f c (%.2f Hz)\n",
                                   wname[w], note, detune[dt], p.note, p.cents10 / 10.0, p.hz10 / 10.0,
                                   want - near * 100, hz);
                        wrong++;
                    }
                    if (e > worst)
                        worst = e;
                }
        CHECK(!missed && !wrong, "%s: %d of %d missed, %d wrong", wname[w], missed, tried, wrong);
        printf("  %s E1..E6, %d tones: worst %.3f cents\n", wname[w], tried, worst);
    }

    /* silence, white noise, low-passed noise: no pitch */
    {
        uint32_t seed = 12345;
        int32_t lp = 0;
        pd_pitch p;
        unsigned looks;
        memset(s, 0, n * sizeof *s);
        CHECK(tuner_listen(s, n, &p, &looks) == 0 && looks > 3, "silence: a pitch");
        for (int kind = 0; kind < 3; kind++) {
            for (unsigned i = 0; i < n; i++) {
                seed = seed * 1664525u + 1013904223u;
                int32_t v = (int32_t)(seed >> 16) - 32768;
                lp += (v - lp) >> (kind == 2 ? 3 : 1);
                s[i] = (int16_t)(kind ? lp / 3 : v / 3);
            }
            int f = tuner_listen(s, n, &p, &looks);
            CHECK(f == 0, "noise %d: a pitch in %d of %u looks (note %d)", kind, f, looks - 1, p.note);
        }
    }

    /* the cost: a look at E2 and at noise (the most lags) */
    {
        pd_osc o = { .inc = pd_pitch_inc(40 << 8), .wave = PD_WAVE_SAW };
        static int16_t buf[PD_TUNER_N], keep[PD_TUNER_N];
        pd_pitch p;
        for (unsigned i = 0; i < PD_TUNER_N; i += PD_BLOCK) {
            pd_osc_render(&o, blk, PD_BLOCK);
            for (unsigned j = 0; j < PD_BLOCK; j++)
                keep[i + j] = (int16_t)(blk[j] >> 2);
        }
        uint32_t t0 = now_ns();
        for (int r = 0; r < 20; r++) {
            memcpy(buf, keep, sizeof buf);
            pd_tuner_detect(buf, &p);
        }
        printf("  a look at E2: %.0f us on the host\n", (now_ns() - t0) / 20.0 / 1000.0);
    }
    free(s);
}

/* -------------------------------------------------------------- metronome --- */

static void test_metro(void)
{
    static const uint32_t tempi[] = { 400, 1200, 1337, 2400 };
    pd_sample one, blk[PD_BLOCK];
    for (unsigned t = 0; t < 4; t++) {
        pd_metro m;
        pd_metro_init(&m);
        pd_metro_tempo(&m, tempi[t]);
        pd_metro_restart(&m);
        double spb = 600.0 * PD_FS / tempi[t], worst = 0;
        uint32_t last = 0, beats = 0;
        for (uint32_t i = 0; beats < 64; i++) {
            pd_metro_process(&m, &one, 1);
            if (m.ticks != last) {
                double e = fabs(i - beats * spb);
                if (e > worst)
                    worst = e;
                CHECK(m.beat == beats % 4, "tempo %u: beat %u is %d in the bar", tempi[t], beats, m.beat);
                last = m.ticks;
                beats++;
            }
        }
        CHECK(worst <= 1.0, "tempo %u.%u: a beat %.2f samples off", tempi[t] / 10, tempi[t] % 10, worst);
        printf("  %u.%u BPM: 64 beats, at most %.2f samples off\n", tempi[t] / 10, tempi[t] % 10, worst);
    }

    /* blocks: the same beats as sample by sample */
    {
        pd_metro a, b;
        pd_metro_init(&a);
        pd_metro_init(&b);
        pd_metro_tempo(&a, 1777);
        pd_metro_tempo(&b, 1777);
        a.on = b.on = 1;
        pd_metro_restart(&a);
        pd_metro_restart(&b);
        int same = 1;
        for (unsigned i = 0; i < SECONDS(4); i += PD_BLOCK) {
            pd_metro_process(&a, blk, PD_BLOCK);
            for (unsigned j = 0; j < PD_BLOCK; j++) {
                pd_metro_process(&b, &one, 1);
                same &= one == blk[j];
            }
        }
        CHECK(same && a.ticks == b.ticks && a.ticks > 10, "blocks and samples differ (%u, %u beats)", a.ticks, b.ticks);
    }

    /* the click: on the beat, the first beat higher and louder (ACCENT), its level, silent when off */
    {
        pd_metro m;
        pd_metro_init(&m);
        pd_metro_tempo(&m, 1200);
        pd_metro_beats(&m, 3);
        m.on = 1;
        pd_metro_restart(&m);
        unsigned spb = 600u * PD_FS / 1200u, n = spb * 6u;
        pd_sample *o = calloc(n + PD_BLOCK, sizeof *o);
        for (unsigned i = 0; i < n; i += PD_BLOCK)
            pd_metro_process(&m, o + i, PD_BLOCK);
        int32_t pk[6] = { 0 };
        unsigned zc[6] = { 0 };
        for (unsigned b = 0; b < 6; b++) {
            for (unsigned i = b * spb; i < b * spb + PD_METRO_CLICK && i < n; i++) {
                int32_t a = o[i] < 0 ? -o[i] : o[i];
                if (a > pk[b])
                    pk[b] = a;
                zc[b] += i > b * spb && o[i - 1] < 0 && o[i] >= 0;
            }
            CHECK(o[b * spb + spb / 2] == 0, "beat %u: sound between the clicks", b);
        }
        CHECK(pk[0] > pk[1] && pk[3] > pk[4] && pk[1] > 1000 && zc[0] > zc[1] * 3 / 2,
              "ACCENT: peaks %d %d, crossings %u %u", pk[0], pk[1], zc[0], zc[1]);
        printf("  ACCENT on 3: the first beat %d at %u crossings, the others %d at %u\n", pk[0], zc[0], pk[1], zc[1]);
        if (wav_dir) {
            int16_t *w = mono_buf(n);
            for (unsigned i = 0; i < n; i++)
                w[i] = (int16_t)pd_sat16(o[i]);
            save("metronome_3.wav", w, n, 1);
            free(w);
        }
        int32_t full = pk[1];
        m.level = 8192;
        pd_metro_restart(&m);
        memset(o, 0, n * sizeof *o);
        for (unsigned i = 0; i < spb * 2u; i += PD_BLOCK)
            pd_metro_process(&m, o + i, PD_BLOCK);
        int32_t q = 0;
        for (unsigned i = spb; i < spb + PD_METRO_CLICK; i++)
            q = o[i] > q ? o[i] : q;
        CHECK(q > full / 3 && q < full * 2 / 3, "LEVEL 25%%: %d against %d at 50%%", q, full);
        m.on = 0;
        uint32_t tk = m.ticks;
        int quiet = 1;
        for (unsigned i = 0; i < spb * 3u; i += PD_BLOCK) {
            pd_metro_process(&m, blk, PD_BLOCK);
            for (unsigned j = 0; j < PD_BLOCK; j++)
                quiet &= blk[j] == 0;
        }
        CHECK(quiet && m.ticks >= tk + 2, "off: %s, %u beats", quiet ? "silent" : "a click", m.ticks - tk);
        free(o);
    }

    /* following a loop: 7 beats of 10000 samples, 4 to the bar: the beat is the loop's, the bar starts again with it */
    {
        pd_metro m;
        pd_metro_init(&m);
        m.on = 1;
        uint32_t len = 70000, spb = 10000, pos = 0, ones = 0, bad = 0, accented = 0;
        uint8_t prev = 0;
        for (unsigned i = 0; i < len * 2u; i += PD_BLOCK) {
            pd_metro_follow(&m, pos, spb);
            uint32_t was = m.ticks;
            uint8_t b0 = m.beat;
            pd_metro_process(&m, blk, PD_BLOCK);
            if (m.beat == 0 && prev != 0) {
                ones++;
                accented += m.inc == pd_pitch_inc(96 << 8) && m.age < 2u * PD_BLOCK;
            }
            if (m.ticks == was && m.beat != b0)
                bad++;
            prev = m.beat;
            pos = (pos + PD_BLOCK) % len;
        }
        uint32_t want = pos / spb % 4u;
        pd_metro_follow(&m, pos, spb);
        CHECK(m.beat == want && ones == 3 && accented == 3 && !bad,
              "following: beat %d (want %u), %u first beats (%u accented), %u odd", m.beat, want, ones, accented, bad);
    }
}

int main(int argc, char **argv)
{
    wav_dir = argc > 1 ? argv[1] : NULL;
    static const struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        { "oscillator", test_osc }, { "envelope", test_env }, { "filter", test_filter }, { "filter modes", test_filter_modes },
        { "delay", test_delay },    { "rack", test_rack },    { "input", test_input },
        { "looper", test_looper },
        { "synth sweep", test_synth_sweep },
        { "engine", test_engine },
        { "effects", test_effects },
        { "amp", test_amp },
        { "levels", test_levels },
        { "tuner", test_tuner },
        { "metronome", test_metro },
    };
    printf("Pendel DSP tests at %u Hz, %d-frame blocks\n", PD_FS, PD_BLOCK);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        printf("%s\n", tests[i].name);
        tests[i].fn();
    }
    printf("%d of %d checks failed\n", fails, checks);
    return fails != 0;
}
