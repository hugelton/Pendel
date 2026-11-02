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
#include "pd_modules.h"
#include "pd_osc.h"
#include "pd_rack.h"

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

int main(int argc, char **argv)
{
    wav_dir = argc > 1 ? argv[1] : NULL;
    static const struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        { "oscillator", test_osc }, { "envelope", test_env }, { "filter", test_filter },
        { "delay", test_delay },    { "rack", test_rack },    { "input", test_input },
        { "looper", test_looper },
        { "synth sweep", test_synth_sweep },
        { "engine", test_engine },
        { "effects", test_effects },
        { "amp", test_amp },
    };
    printf("Pendel DSP tests at %u Hz, %d-frame blocks\n", PD_FS, PD_BLOCK);
    for (size_t i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        printf("%s\n", tests[i].name);
        tests[i].fn();
    }
    printf("%d of %d checks failed\n", fails, checks);
    return fails != 0;
}
