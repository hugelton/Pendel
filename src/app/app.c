/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Pendel, the application (memos/2026-10-28_ui_plan.md).
 *
 * An effects unit with a tape, its HOME the chain top to bottom: four cards, EFX1, EFX2, EFX3 (each OFF or an effect)
 * and AMP; the TAPE band; the scope, where the input's gain is set. The input (gain, gate and low cut: MENU) runs
 * through EFX1..3, the amp, then the tape.
 * Keys (Felucca's manners): UP / DN (the LEFT / RIGHT buttons) move the focus, L / R (the knob) set its value, both
 * repeating when held (faster after a second); SEL (the knob's push) acts when let go: on HOME it opens the card in
 * focus; on the TAPE band it records, closes, overdubs (on the press when it starts or closes a take: timing), held
 * 0.7 s it stops, 2 s it clears. L+R: the
 * default. HOME: a tap goes home (or closes MENU), held 0.7 s it opens MENU. LOOP (MIDI, tests): the tape's key.
 * Pages: HOME, an EFX slot, AMP, MENU. */
#include "app.h"

#include "pd_effects.h"
#include "pd_looper.h"
#include "pd_modules.h"
#include "pd_rack.h"
#include "ui_icons.h"

#ifndef APP_POOL
#define APP_POOL __attribute__((section(".pool")))
#endif
#define ARENA_BYTES (320u * 1024u)

/* ------------------------------------------------------- parameters --- */

enum { F_PCT, F_CHOICE, F_SEMI, F_MS, F_HZ, F_X4, F_X8, F_OFFPCT, F_RATE, F_PITCH, F_BITS };

typedef struct {
    const char *name;
    int32_t def;
    uint8_t fmt, choices;
    const char *const *labels;
    uint8_t icon;                /* IC_* */
    const uint8_t *vicons;       /* a choice: its icons, IC_* */
} param_t;

typedef struct {
    const char *name;            /* short, for the cards and lists */
    uint8_t icon;
    const PendelDSP *dsp;
    uint8_t graph, np;
    param_t p[10];
} kind_t;

static const char *const WAVES[] = { "SAW", "SQUARE", "TRI", "SINE" };
static const char *const MODES[] = { "LP", "BP", "HP" };
static const uint8_t WAVE_IC[] = { IC_SAW, IC_SQUARE, IC_TRI, IC_SINE };
static const uint8_t MODE_IC[] = { IC_LP, IC_BP, IC_HP };
static const char *const AMP_MODELS[PD_AMP_MODELS] = { "OFF", "CLEAN", "CRUNCH", "LEAD" };
static const char *const OFF_ON[] = { "OFF", "ON" };

enum { K_SYNTH, K_INPUT, K_DRIVE, K_DELAY, K_FILTER, K_GAIN,
       K_FUZZ, K_WAH, K_CRUSH, K_FLANGER, K_PHASER, K_TREMOLO, K_WOBBLE, K_NOISE, K_PITCH, K_ROOM, K_AMP, K_COUNT };
static const kind_t KINDS[K_COUNT] = {
    { "SYNTH", IC_SYNTH, &PD_SYNTH, UI_G_SYNTH, PD_SYN_PARAMS,
      { { "WAVE", 0, F_CHOICE, 4, WAVES, IC_SAW, WAVE_IC }, { "TUNE", 16384, F_SEMI, 0, 0, IC_TUNE, 0 },
        { "CUTOFF", 18000, F_HZ, 0, 0, IC_CUTOFF, 0 }, { "RESONANCE", 8000, F_PCT, 0, 0, IC_RESO, 0 },
        { "ENV AMOUNT", 12000, F_PCT, 0, 0, IC_ENV, 0 }, { "ATTACK", 2000, F_PCT, 0, 0, IC_ATTACK, 0 },
        { "DECAY", 14000, F_PCT, 0, 0, IC_DECAY, 0 }, { "SUSTAIN", 20000, F_PCT, 0, 0, IC_SUSTAIN, 0 },
        { "RELEASE", 12000, F_PCT, 0, 0, IC_RELEASE, 0 }, { "LEVEL", 26000, F_PCT, 0, 0, IC_LEVEL, 0 } } },
    { "INPUT", IC_INPUT, &PD_INPUT, UI_G_SCOPE, PD_IN_PARAMS,
      { { "GAIN", 4096, F_X8, 0, 0, IC_AMP, 0 }, { "GATE", 0, F_OFFPCT, 0, 0, IC_GATE, 0 },
        { "LOW CUT", 0, F_OFFPCT, 0, 0, IC_LOWCUT, 0 } } },
    { "DRIVE", IC_DRIVE, &PD_FX_DRIVE, UI_G_DRIVE, PD_DRV_PARAMS,
      { { "DRIVE", 12000, F_PCT, 0, 0, IC_DRIVE, 0 }, { "TONE", 20000, F_PCT, 0, 0, IC_TONE, 0 },
        { "LEVEL", 20000, F_PCT, 0, 0, IC_LEVEL, 0 } } },
    { "DELAY", IC_DELAY, &PD_FX_DELAY, UI_G_DELAY, PD_DLY_PARAMS,
      { { "TIME", 12000, F_MS, 0, 0, IC_TIME, 0 }, { "FEEDBACK", 12000, F_PCT, 0, 0, IC_FEEDBACK, 0 } } },
    { "FILTER", IC_FILTER, &PD_FX_FILTER, UI_G_FILTER, PD_FLT_PARAMS,
      { { "CUTOFF", 20000, F_HZ, 0, 0, IC_CUTOFF, 0 }, { "RESONANCE", 6000, F_PCT, 0, 0, IC_RESO, 0 },
        { "MODE", 0, F_CHOICE, 3, MODES, IC_FILTER, MODE_IC } } },
    { "GAIN", IC_GAIN, &PD_FX_GAIN, UI_G_GAIN, PD_GAIN_PARAMS, { { "GAIN", 8192, F_X4, 0, 0, IC_GAIN, 0 } } },
    /* pd_effects.c: two values each, the scope above them */
    { "FUZZ", IC_FUZZ, &PD_FX_FUZZ, UI_G_SCOPE, PD_FUZZ_PARAMS,
      { { "FUZZ", 16000, F_PCT, 0, 0, IC_FUZZ, 0 }, { "BIAS", 6000, F_PCT, 0, 0, IC_BIAS, 0 } } },
    { "WAH", IC_WAH, &PD_FX_WAH, UI_G_SCOPE, PD_WAH_PARAMS,
      { { "SENS", 20000, F_PCT, 0, 0, IC_ENV, 0 }, { "RESONANCE", 20000, F_PCT, 0, 0, IC_RESO, 0 } } },
    { "CRUSH", IC_CRUSH, &PD_FX_CRUSH, UI_G_SCOPE, PD_CRUSH_PARAMS,
      { { "BITS", 16384, F_BITS, 0, 0, IC_CRUSH, 0 }, { "DOWNSAMPLE", 6000, F_PCT, 0, 0, IC_STEPS, 0 } } },
    { "FLANGER", IC_FLANGER, &PD_FX_FLANGER, UI_G_SCOPE, PD_FLG_PARAMS,
      { { "RATE", 9000, F_RATE, 0, 0, IC_RATE, 0 }, { "FEEDBACK", 16000, F_PCT, 0, 0, IC_FEEDBACK, 0 } } },
    { "PHASER", IC_PHASER, &PD_FX_PHASER, UI_G_SCOPE, PD_PHS_PARAMS,
      { { "RATE", 9000, F_RATE, 0, 0, IC_RATE, 0 }, { "DEPTH", 20000, F_PCT, 0, 0, IC_DEPTH, 0 } } },
    { "TREMOLO", IC_TREMOLO, &PD_FX_TREMOLO, UI_G_SCOPE, PD_TRM_PARAMS,
      { { "RATE", 19000, F_RATE, 0, 0, IC_RATE, 0 }, { "DEPTH", 20000, F_PCT, 0, 0, IC_DEPTH, 0 } } },
    { "WOBBLE", IC_WOBBLE, &PD_FX_WOBBLE, UI_G_SCOPE, PD_WOB_PARAMS,
      { { "RATE", 12000, F_RATE, 0, 0, IC_RATE, 0 }, { "DEPTH", 8000, F_PCT, 0, 0, IC_DEPTH, 0 } } },
    { "NOISE", IC_NOISE, &PD_FX_NOISE, UI_G_SCOPE, PD_NOISE_PARAMS,
      { { "HISS", 12000, F_PCT, 0, 0, IC_NOISE, 0 }, { "CRACKLE", 12000, F_PCT, 0, 0, IC_PROB, 0 } } },
    { "PITCH", IC_PITCH, &PD_FX_PITCH, UI_G_SCOPE, PD_PIT_PARAMS,
      { { "PITCH", 24576, F_PITCH, 0, 0, IC_PITCH, 0 }, { "FEEDBACK", 0, F_PCT, 0, 0, IC_FEEDBACK, 0 } } },
    { "ROOM", IC_ROOM, &PD_FX_ROOM, UI_G_SCOPE, PD_ROOM_PARAMS,
      { { "SIZE", 16384, F_PCT, 0, 0, IC_SIZE, 0 }, { "DECAY", 18000, F_PCT, 0, 0, IC_DECAY, 0 } } },
    /* the AMP card's, after EFX3 (pd_effects.c): MODEL OFF passes the sound */
    { "AMP", IC_AMP, &PD_FX_AMP, UI_G_SCOPE, PD_AMP_PARAMS,
      { { "MODEL", PD_AMP_CLEAN, F_CHOICE, PD_AMP_MODELS, AMP_MODELS, IC_AMP, 0 },
        { "GAIN", 12000, F_PCT, 0, 0, IC_DRIVE, 0 }, { "BASS", 16384, F_PCT, 0, 0, IC_LP, 0 },
        { "MID", 16384, F_PCT, 0, 0, IC_BP, 0 }, { "TREBLE", 16384, F_PCT, 0, 0, IC_HP, 0 },
        { "LEVEL", 16384, F_PCT, 0, 0, IC_LEVEL, 0 }, { "CAB", 1, F_CHOICE, 2, OFF_ON, IC_SPEAKER, 0 } } },
};

/* a module the app made: its kind and its parameters as set */
typedef struct {
    uint8_t kind;
    pd_node node;
    int32_t v[10];
} unit_t;

/* ------------------------------------------------------------- state --- */

static uint8_t pool[ARENA_BYTES] APP_POOL;
static pd_arena arena;
static pd_engine eng;
static unit_t synth_u, input_u;
static unit_t fx_u[PD_RACK_SLOTS];        /* in rack order: fx_u[i] is eng.fx.slot[i] */
static unit_t spare[PD_RACK_SLOTS * 2];   /* effects no slot holds, kept for reuse (their memory stays in the arena) */
static unsigned nspare;
static ui_model_t mdl;

enum { P_HOME, P_SOURCE, P_FXEDIT, P_MENU, P_COUNT };

/* the three EFX slots: the rack's slots 0..2, always there; an OFF slot is bypassed (its last effect kept) */
#define NSLOT 3u
#define SK_OFF 0xFFu
static uint8_t slot_kind[NSLOT];          /* SK_OFF or K_* */
static const char *const SLOT_NAMES[NSLOT] = { "EFX1", "EFX2", "EFX3" };
#define AMP_SLOT 3u                       /* the rack's slot 3 (fx_u[3]): the AMP card, always there */
#define TAPE_FOCUS 4u                     /* HOME: EFX1..3, AMP, then the TAPE band */
#define SCOPE_FOCUS 5u                    /* and the scope: the input's gain (the push: MENU's input rows) */
#define amp_model() ((unsigned)fx_u[AMP_SLOT].v[PD_AMP_MODEL])
/* an EFX slot's page: TYPE, MIX, then the effect's values; the AMP's: its values alone */
#define EDIT_FIRST() (ui.edit == AMP_SLOT ? 0u : 2u)
static struct {
    uint8_t page, home_focus, row[P_COUNT];
    uint8_t edit;                         /* P_FXEDIT: the slot */
    uint8_t edit_row[AMP_SLOT + 1u];      /* each slot's page (EFX1..3, AMP): its row, kept */
    uint8_t menu, menu_sel;
    uint8_t palette;                      /* MENU > COLOR */
    int32_t master, loop_level;
    uint8_t note;
    uint32_t bpm10;                       /* the tempo (1/10 BPM): MENU > TEMPO (120), or a loop's */
    uint32_t loop_len, loop_beats;        /* the loop the tempo was taken from, and its beats */
} ui;

/* audio side: a ring of the latest output for the scope, the block peaks */
#define RING 1024u
static int16_t ring_buf[RING];
static volatile uint32_t ring_w;
static volatile uint16_t peak_in, peak_out;
static volatile uint32_t clock_s;           /* samples since start: the free-running beat */

/* -------------------------------------------------------------- units --- */

static void unit_set(unit_t *u, unsigned i, int32_t v)
{
    const param_t *p = &KINDS[u->kind].p[i];
    if (p->choices) {                      /* a choice: stored as its index, set as the middle of its range */
        v = v < 0 ? 0 : v >= p->choices ? p->choices - 1 : v;
        u->v[i] = v;
        u->node.dsp->set_param(u->node.state, i, (int32_t)(((2 * v + 1) * 32768) / (2 * p->choices)));
        return;
    }
    u->v[i] = v < 0 ? 0 : v > PD_PARAM_MAX ? PD_PARAM_MAX : v;
    u->node.dsp->set_param(u->node.state, i, u->v[i]);
}

static int unit_make(unit_t *u, unsigned kind)
{
    u->kind = (uint8_t)kind;
    if (pd_node_make(&u->node, &arena, KINDS[kind].dsp, PD_FS))
        return -1;
    for (unsigned i = 0; i < KINDS[kind].np; i++)
        unit_set(u, i, KINDS[kind].p[i].def);
    return 0;
}

/* the next rack slot, at start-up (EFX1..3, AMP) */
static int fx_add(unsigned kind)
{
    unsigned n = eng.fx.count;
    unit_t u;
    if (n >= PD_RACK_SLOTS || unit_make(&u, kind))
        return -1;
    eng.fx.slot[n].node = u.node;
    eng.fx.slot[n].on = 1;
    eng.fx.slot[n].mix = PD_MIX_WET;
    eng.fx.count = n + 1;
    fx_u[n] = u;
    return (int)n;
}

/* a slot's TYPE: OFF bypasses it; an effect comes from the spares or is made (its memory stays in the arena) */
static const uint8_t SLOT_TYPES[] = {                     /* in a pedalboard's order */
    SK_OFF, K_DRIVE, K_FUZZ, K_FILTER, K_WAH, K_CRUSH, K_PHASER, K_FLANGER, K_TREMOLO, K_WOBBLE, K_PITCH,
    K_DELAY, K_ROOM, K_NOISE, K_GAIN,
};
#define NTYPES (sizeof SLOT_TYPES)

static int slot_set(unsigned i, unsigned kind)
{
    unit_t u;
    if (kind == SK_OFF) {
        slot_kind[i] = SK_OFF;
        eng.fx.slot[i].on = 0;
        return 0;
    }
    if (fx_u[i].kind != kind) {
        unsigned j;
        for (j = 0; j < nspare; j++)
            if (spare[j].kind == kind)
                break;
        if (j < nspare) {
            u = spare[j];
            spare[j] = spare[--nspare];
            u.node.dsp->reset(u.node.state);
        } else if (unit_make(&u, kind)) {
            return -1;                                   /* no room in the arena: the slot stays as it was */
        }
        if (nspare < PD_RACK_SLOTS * 2)
            spare[nspare++] = fx_u[i];
        fx_u[i] = u;
        eng.fx.slot[i].node = u.node;
    }
    slot_kind[i] = (uint8_t)kind;
    eng.fx.slot[i].on = 1;
    return 0;
}

static unsigned type_index(unsigned i)
{
    for (unsigned t = 0; t < NTYPES; t++)
        if (SLOT_TYPES[t] == slot_kind[i])
            return t;
    return 0;
}

/* ------------------------------------------------------------ format --- */

static char *put_s(char *d, const char *s)
{
    while (*s)
        *d++ = *s++;
    *d = 0;
    return d;
}
static char *put_u(char *d, uint32_t v)
{
    char t[10];
    int n = 0;
    do {
        t[n++] = (char)('0' + v % 10u);
        v /= 10u;
    } while (v);
    while (n)
        *d++ = t[--n];
    *d = 0;
    return d;
}
static char *put_x(char *d, uint32_t hundredths)          /* 150 -> "1.50x" */
{
    d = put_u(d, hundredths / 100u);
    *d++ = '.';
    *d++ = (char)('0' + hundredths / 10u % 10u);
    *d++ = (char)('0' + hundredths % 10u);
    return put_s(d, "x");
}
static uint32_t pct(int32_t v) { return ((uint32_t)v * 100u + 16383u) / 32767u; }

/* the cutoff scale: 20 Hz * 2^(10 v / 32767), in whole Hz (2^frac by a cubic, within 0.1 %) */
static uint32_t cutoff_hz(int32_t v)
{
    uint32_t oct = (uint32_t)v * 10u / 32767u, f = (uint32_t)v * 10u % 32767u * 65536u / 32767u;   /* frac, Q16 */
    uint32_t m = 65536u + ((f * 45426u) >> 16) + ((((f * f) >> 16) * 15743u) >> 16) + ((((((f * f) >> 16) * f) >> 16) * 4367u) >> 16);
    return (20u << oct) * (m >> 6) >> 10;
}

static void format(char *d, const param_t *p, int32_t v)
{
    switch (p->fmt) {
    case F_CHOICE:
        put_s(d, p->labels[v]);
        break;
    case F_SEMI: {
        int32_t s = ((v - 16384) * 12 + (v >= 16384 ? 8192 : -8192)) / 16384;
        if (s > 0)
            *d++ = '+';
        else if (s < 0)
            *d++ = '-';
        put_s(put_u(d, (uint32_t)(s < 0 ? -s : s)), " ST");
        break;
    }
    case F_MS:
        put_s(put_u(d, 1u + (uint32_t)v * 999u / 32767u), " MS");
        break;
    case F_HZ: {
        uint32_t hz = cutoff_hz(v);
        if (hz >= 1000u) {
            d = put_u(d, hz / 1000u);
            *d++ = '.';
            *d++ = (char)('0' + hz / 100u % 10u);
            put_s(d, " KHZ");
        } else {
            put_s(put_u(d, hz), " HZ");
        }
        break;
    }
    case F_RATE: {                                    /* PD_LFO_INC's rate, as the effect runs it */
        uint32_t i = (uint32_t)v >> 7, c = PD_LFO_CHZ[i] + ((PD_LFO_CHZ[i + (i < 256u)] - PD_LFO_CHZ[i]) * ((uint32_t)v & 127u) >> 7);
        d = put_u(d, c / 100u);
        *d++ = '.';
        if (c < 1000u)
            *d++ = (char)('0' + c / 10u % 10u);
        if (c < 100u)
            *d++ = (char)('0' + c % 10u);
        put_s(d, " HZ");
        break;
    }
    case F_PITCH: {
        int32_t s = pd_pitch_semis(v);
        if (s > 0)
            *d++ = '+';
        else if (s < 0)
            *d++ = '-';
        put_s(put_u(d, (uint32_t)(s < 0 ? -s : s)), " ST");
        break;
    }
    case F_BITS:
        put_s(put_u(d, 16u - ((uint32_t)v * 14u + 16383u) / 32768u), " BIT");
        break;
    case F_X4:
        put_x(d, (uint32_t)v * 400u / 32767u);
        break;
    case F_X8:
        put_x(d, (uint32_t)v * 800u / 32767u);
        break;
    case F_OFFPCT:
        if (!v) {
            put_s(d, "OFF");
            break;
        }
        /* fall through */
    default:
        put_s(put_u(d, pct(v)), "%");
        break;
    }
}

/* -------------------------------------------------------------- pages --- */

/* Pendel is an effects rack: the source is the input. APP_SOURCE_SELECT 1 brings back the choice of the synthesizer
 * (its code stays): a SOURCE row on that page, and MIDI notes. */
#define SRC_ROW (APP_SOURCE_SELECT ? 1u : 0u)      /* the source page's rows before the parameters */

static unit_t *src_unit(void) { return eng.source == PD_SRC_INPUT ? &input_u : &synth_u; }

/* MENU (HOME held): the settings, a list like Felucca's: the knob sets the selected row's value, live */
enum { MR_IN_GAIN, MR_IN_GATE, MR_IN_LOWCUT, MR_COLOR, MR_MASTER, MR_TAPE, MR_TEMPO, MR_VERSION, MR_CPU, MENU_ROWS };
#define MENU_HOLD_MS 700u
#define TEMPO_MIN 400u                             /* MENU > TEMPO, in 1/10 BPM: 40 .. 240, 1 BPM a step */
#define TEMPO_MAX 2400u

static unsigned page_rows(void)
{
    switch (ui.page) {
    case P_SOURCE: return SRC_ROW + KINDS[src_unit()->kind].np;
    case P_FXEDIT:
        if (ui.edit == AMP_SLOT)
            return KINDS[K_AMP].np;
        return slot_kind[ui.edit] == SK_OFF ? 1u : 2u + KINDS[slot_kind[ui.edit]].np;   /* TYPE, MIX, params */
    case P_MENU: return MENU_ROWS;
    default: return 0;
    }
}

static void tape_level_add(int32_t d)
{
    ui.loop_level = ui.loop_level + d < 0 ? 0 : ui.loop_level + d > 32767 ? 32767 : ui.loop_level + d;
    eng.looper.dsp->set_param(eng.looper.state, PD_LOOP_LEVEL, ui.loop_level);
}

/* one step of L/R on the focused thing: dir -1 / +1, big = the repeat's later, faster steps */
static void adjust(int dir, int big)
{
    int32_t step = big ? 2048 : 512;
    unsigned r = ui.row[ui.page];
    unit_t *u = NULL;
    unsigned pi = 0;
    switch (ui.page) {
    case P_HOME:                                         /* the focused card: its effect's first value, the AMP's gain, the TAPE level */
        if (ui.home_focus < NSLOT) {
            if (slot_kind[ui.home_focus] == SK_OFF)
                return;
            u = &fx_u[ui.home_focus];
            pi = 0;
            break;
        }
        if (ui.home_focus == AMP_SLOT) {
            if (amp_model() == PD_AMP_OFF)
                return;
            u = &fx_u[AMP_SLOT];
            pi = PD_AMP_GAIN;
            break;
        }
        if (ui.home_focus == SCOPE_FOCUS) {
            u = &input_u;
            pi = PD_IN_GAIN;
            break;
        }
        if (ui.home_focus == TAPE_FOCUS)
            tape_level_add(dir * step);
        return;
    case P_MENU:
        if (r <= MR_IN_LOWCUT) {
            u = &input_u;
            pi = r - MR_IN_GAIN;
            break;
        }
        if (r == MR_COLOR) {
            ui.palette = (uint8_t)((ui.palette + ui_palette_count() + (uint32_t)(dir > 0 ? 1 : -1)) % ui_palette_count());
            ui_palette(ui.palette);
        } else if (r == MR_MASTER) {
            app_master_step(dir);
        } else if (r == MR_TAPE) {
            tape_level_add(dir * step);
        } else if (r == MR_TEMPO && !ui.loop_len) {       /* (a loop holds its own tempo) */
            int32_t b = (int32_t)ui.bpm10 / 10 * 10 + dir * (big ? 50 : 10);
            ui.bpm10 = (uint32_t)(b < (int32_t)TEMPO_MIN ? TEMPO_MIN : b > (int32_t)TEMPO_MAX ? TEMPO_MAX : b);
        }
        return;
    case P_SOURCE:
        if (SRC_ROW && !r) {
            pd_engine_select(&eng, eng.source == PD_SRC_INPUT ? PD_SRC_INSTRUMENT : PD_SRC_INPUT);
            ui.row[P_SOURCE] = 0;
            return;
        }
        u = src_unit();
        pi = r - SRC_ROW;
        break;
    case P_FXEDIT:
        if (ui.edit == AMP_SLOT) {
            u = &fx_u[AMP_SLOT];
            pi = r;
            break;
        }
        if (r == 0) {                                    /* TYPE: in SLOT_TYPES' order (no wrap) */
            int t = (int)type_index(ui.edit) + (dir > 0 ? 1 : -1);
            if (t >= 0 && t < (int)NTYPES)
                slot_set(ui.edit, SLOT_TYPES[t]);
        } else if (slot_kind[ui.edit] == SK_OFF) {
            return;
        } else if (r == 1) {
            int32_t m = eng.fx.slot[ui.edit].mix + dir * step;
            eng.fx.slot[ui.edit].mix = m < 0 ? 0 : m > PD_MIX_WET ? PD_MIX_WET : m;
        } else if (r < 2u + KINDS[slot_kind[ui.edit]].np) {
            u = &fx_u[ui.edit];
            pi = r - 2u;
            break;
        }
        return;
    default:
        return;
    }
    if (KINDS[u->kind].p[pi].choices)
        unit_set(u, pi, u->v[pi] + dir);
    else
        unit_set(u, pi, u->v[pi] + dir * step);
}

static void reset_value(void)
{
    unsigned r = ui.row[ui.page];
    if (ui.page == P_HOME && ui.home_focus < NSLOT && slot_kind[ui.home_focus] != SK_OFF) {
        unit_set(&fx_u[ui.home_focus], 0, KINDS[slot_kind[ui.home_focus]].p[0].def);
    } else if (ui.page == P_HOME && ui.home_focus == SCOPE_FOCUS) {
        unit_set(&input_u, PD_IN_GAIN, KINDS[K_INPUT].p[PD_IN_GAIN].def);
    } else if (ui.page == P_HOME && ui.home_focus == AMP_SLOT && amp_model() != PD_AMP_OFF) {
        unit_set(&fx_u[AMP_SLOT], PD_AMP_GAIN, KINDS[K_AMP].p[PD_AMP_GAIN].def);
    } else if (ui.page == P_FXEDIT && ui.edit == AMP_SLOT) {
        if (r != PD_AMP_MODEL)                           /* (L+R on MODEL does not switch the amp off) */
            unit_set(&fx_u[AMP_SLOT], r, KINDS[K_AMP].p[r].def);
    } else if (ui.page == P_HOME && ui.home_focus == TAPE_FOCUS) {
        ui.loop_level = 32767;
        eng.looper.dsp->set_param(eng.looper.state, PD_LOOP_LEVEL, ui.loop_level);
    } else if (ui.page == P_SOURCE && r >= SRC_ROW) {
        unit_set(src_unit(), r - SRC_ROW, KINDS[src_unit()->kind].p[r - SRC_ROW].def);
    } else if (ui.page == P_FXEDIT && slot_kind[ui.edit] != SK_OFF && r == 1u) {
        eng.fx.slot[ui.edit].mix = PD_MIX_WET;
    } else if (ui.page == P_FXEDIT && slot_kind[ui.edit] != SK_OFF && r >= 2u && r < 2u + KINDS[slot_kind[ui.edit]].np) {
        unit_set(&fx_u[ui.edit], r - 2u, KINDS[slot_kind[ui.edit]].p[r - 2u].def);
    } else if (ui.page == P_MENU && r <= MR_IN_LOWCUT) {
        unit_set(&input_u, r, KINDS[K_INPUT].p[r].def);
    } else if (ui.page == P_MENU && r == MR_TEMPO && !ui.loop_len) {
        ui.bpm10 = 1200;
    }
}

static void go(unsigned page)
{
    ui.page = (uint8_t)page;
    unsigned n = page_rows();
    if (n && ui.row[page] >= n)
        ui.row[page] = (uint8_t)(n - 1u);
}

static void select_tap(void)
{
    unsigned r = ui.row[ui.page];
    switch (ui.page) {
    case P_HOME:                                         /* a card's page: its slot or the AMP; the scope: MENU (the TAPE
                                                          * band takes the push itself: app_keys) */
        if (ui.home_focus <= AMP_SLOT) {
            ui.edit = ui.home_focus;
            ui.row[P_FXEDIT] = ui.edit_row[ui.edit];
            go(P_FXEDIT);
        } else if (ui.home_focus == SCOPE_FOCUS) {
            ui.row[P_MENU] = MR_IN_GAIN;
            go(P_MENU);
        }
        break;
    case P_FXEDIT:
        break;
    case P_SOURCE:
        if (SRC_ROW && r == 0)
            adjust(1, 0);
        break;
    default:
        break;
    }
}

static void move_focus(int dir)
{
    if (ui.page == P_HOME) {
        int f = (int)ui.home_focus + dir;
        ui.home_focus = (uint8_t)(f < 0 ? 0 : f > (int)SCOPE_FOCUS ? (int)SCOPE_FOCUS : f);
        return;
    }
    unsigned n = page_rows();
    int r = (int)ui.row[ui.page] + dir;
    if (n)
        ui.row[ui.page] = (uint8_t)(r < 0 ? 0 : r >= (int)n ? (int)n - 1 : r);
    if (ui.page == P_FXEDIT)
        ui.edit_row[ui.edit] = ui.row[P_FXEDIT];
}

/* ------------------------------------------------------------ buttons --- */

#define HOLD_MS 400u
#define LOOP_STOP_MS 700u
#define LOOP_CLEAR_MS 2000u
#define REPEAT_MS 70u
#define FAST_MS 1000u


static struct {
    uint8_t down;
    uint32_t t0[6], next[6];
    uint8_t used, held;                   /* a bit per key: took part in a combo / its hold fired */
    uint8_t loop_acted;                   /* the tape's key acted on the press */
    uint8_t tape_keys;                    /* the keys working the tape this press: LOOP, or the push on the TAPE band */
    uint32_t home_t0;                     /* HOME: when it went down, and whether its hold (MENU) fired */
    uint8_t home_held;
} k;

static unsigned bit_index(uint8_t b) { unsigned i = 0; while (!(b & 1u)) { b >>= 1; i++; } return i; }

void app_keys(uint8_t down, uint32_t now)
{
    uint8_t pressed = down & (uint8_t)~k.down, released = k.down & (uint8_t)~down;
    unsigned i;

    if (pressed & APP_K_HOME) {                              /* HOME: a tap goes home (or closes MENU), a hold opens MENU */
        k.home_t0 = now;
        k.home_held = 0;
    }
    if ((down & APP_K_HOME) && !k.home_held && now - k.home_t0 >= MENU_HOLD_MS) {
        k.home_held = 1;
        ui.menu = 0;
        go(ui.page == P_MENU ? P_HOME : P_MENU);
    }
    if ((released & APP_K_HOME) && !k.home_held) {
        ui.menu = 0;
        go(P_HOME);
    }
    for (i = 0; i < 6u; i++) {
        uint8_t b = (uint8_t)(1u << i);
        if (!(pressed & b))
            continue;
        k.t0[i] = now;
        k.next[i] = now + HOLD_MS;
        k.used &= (uint8_t)~b;
        k.held &= (uint8_t)~b;
        if ((b == APP_K_L || b == APP_K_R) && (down & APP_K_L) && (down & APP_K_R)) {
            reset_value();                                   /* L + R: the default */
            k.used |= APP_K_L | APP_K_R;
            continue;
        }
        if ((b == APP_K_L || b == APP_K_R) && (k.down & APP_K_LOOP)) {
            int32_t l = ui.loop_level + (b == APP_K_R ? 2048 : -2048);
            ui.loop_level = l < 0 ? 0 : l > 32767 ? 32767 : l;
            eng.looper.dsp->set_param(eng.looper.state, PD_LOOP_LEVEL, ui.loop_level);
            k.used |= APP_K_LOOP | b;
            continue;
        }
        k.tape_keys &= (uint8_t)~b;
        if (b == APP_K_LOOP || (b == APP_K_SEL && !(k.down & ~b) && ui.page == P_HOME && ui.home_focus == TAPE_FOCUS))
            k.tape_keys |= b;
        if (b == APP_K_UP || b == APP_K_DN)
            move_focus(b == APP_K_UP ? -1 : 1);
        else if (b == APP_K_L || b == APP_K_R)
            adjust(b == APP_K_R ? 1 : -1, 0);
        else if (k.tape_keys & b) {                          /* the tape: a tap records, closes, overdubs; held 0.7 s
                                                              * it stops, 2 s it clears */
            int st = pd_looper_state(eng.looper.state);
            k.loop_acted = st == PD_LOOP_EMPTY || st == PD_LOOP_RECORD;
            if (k.loop_acted)
                pd_looper_press(eng.looper.state);           /* starting or closing a take: on the beat */
        } else if (b == APP_K_SEL && (k.down & ~b))
            k.used |= b;
    }

    for (i = 0; i < 6u; i++) {                               /* held */
        uint8_t b = (uint8_t)(1u << i);
        if (!(down & b) || (k.used & b))
            continue;
        uint32_t t = now - k.t0[i];
        if ((b & (APP_K_UP | APP_K_DN | APP_K_L | APP_K_R)) && !ui.menu && (int32_t)(now - k.next[i]) >= 0) {
            k.next[i] = now + REPEAT_MS;
            if (b == APP_K_UP || b == APP_K_DN)
                move_focus(b == APP_K_UP ? -1 : 1);
            else
                adjust(b == APP_K_R ? 1 : -1, t >= FAST_MS);
        }
        if ((k.tape_keys & b) && !k.loop_acted) {
            if (t >= LOOP_CLEAR_MS && !(k.held & 0x80u)) {
                k.held |= 0x80u | b;
                pd_looper_clear(eng.looper.state);
            } else if (t >= LOOP_STOP_MS && !(k.held & b)) {
                k.held |= b;
                pd_looper_stop(eng.looper.state);
            }
        }
    }

    for (i = 0; i < 6u; i++) {                               /* let go */
        uint8_t b = (uint8_t)(1u << i);
        if (!(released & b))
            continue;
        if (k.tape_keys & b) {
            if (!k.loop_acted && !(k.used & b) && !(k.held & b))
                pd_looper_press(eng.looper.state);
            k.held &= (uint8_t)~0x80u;
            k.tape_keys &= (uint8_t)~b;
        } else if (b == APP_K_SEL && !(k.used & b) && !(k.held & b)) {
            select_tap();
        }
    }
    k.down = down;
}

void app_master_step(int dir)
{
    int32_t v = ui.master + dir * 1024;
    ui.master = v < 0 ? 0 : v > 32767 ? 32767 : v;
    eng.master = ui.master;
}

/* the knob: a detent is an L / R step; detents in one direction less than ACC_GAP ms apart take more (Felucca's
 * KNOB ACCEL: 25 ms x2, 16 ms x3, 12 ms x4, 10 ms x5 .. 6 ms or less x8); a choice always moves by one */
#define ACC_GAP 40u
#define ACC_RATE 50u
static struct { uint32_t t; int dir; } knob;

static int focus_is_choice(void)                         /* a list of names (TYPE, MODE, COLOR): one step a detent */
{
    unsigned r = ui.row[ui.page];
    const unit_t *u = 0;
    unsigned pi = 0;
    if (ui.page == P_FXEDIT && ui.edit == AMP_SLOT) {
        u = &fx_u[AMP_SLOT];
        pi = r;
    } else if (ui.page == P_FXEDIT) {
        if (r == 0)
            return 1;
        if (slot_kind[ui.edit] != SK_OFF && r >= 2u) {
            u = &fx_u[ui.edit];
            pi = r - 2u;
        }
    } else if (ui.page == P_HOME && ui.home_focus < NSLOT && slot_kind[ui.home_focus] != SK_OFF) {
        u = &fx_u[ui.home_focus];
    } else if (ui.page == P_MENU) {
        return r == MR_COLOR;
    }
    return u && pi < KINDS[u->kind].np && KINDS[u->kind].p[pi].choices;
}

void app_knob(int32_t detents, uint32_t now)
{
    int dir = detents > 0 ? 1 : -1;
    int32_t n = detents < 0 ? -detents : detents;
    while (n--) {
        uint32_t gap = now - knob.t, mult = 1;
        if (knob.dir == dir && gap < ACC_GAP)
            mult = gap ? ACC_RATE / gap : 8u;
        mult = mult < 1u ? 1u : mult > 8u ? 8u : mult;
        if (focus_is_choice())
            mult = 1;
        knob.t = now;
        knob.dir = dir;
        if (k.down & APP_K_SEL)
            k.used |= APP_K_SEL;                          /* (a turn while the push is held: the push does not act) */
        while (mult--)
            adjust(dir, 0);
    }
}

void app_power(int32_t level, int usb) { mdl.batt = (uint8_t)(usb ? 4 : level < 0 ? 0xFF : level > 3 ? 3 : level); }

/* --------------------------------------------------------------- MIDI --- */

void app_midi(uint8_t st, uint8_t d1, uint8_t d2)
{
    if (!APP_SOURCE_SELECT)
        return;                                  /* no synthesizer to play */
    if ((st & 0xF0u) == 0x90u && d2) {
        pd_synth_note_on(synth_u.node.state, d1, d2);
        ui.note = d1;
    } else if ((st & 0xF0u) == 0x80u || ((st & 0xF0u) == 0x90u && !d2)) {
        pd_synth_note_off(synth_u.node.state, d1);
        if (ui.note == d1)
            ui.note = 0;
    }
}

/* -------------------------------------------------------------- audio --- */

void app_audio(const int16_t *in, int16_t *out, unsigned frames)
{
    uint32_t pi = 0, po = 0;
    while (frames) {
        unsigned n = frames < PD_BLOCK ? frames : PD_BLOCK;
        pd_engine_process(&eng, in, out, n);
        for (unsigned i = 0; i < n; i++) {
            uint32_t a = (uint32_t)(in ? (in[i] < 0 ? -in[i] : in[i]) : 0), b = (uint32_t)(out[2 * i] < 0 ? -out[2 * i] : out[2 * i]);
            if (a > pi) pi = a;
            if (b > po) po = b;
            ring_buf[(ring_w + i) & (RING - 1u)] = out[2 * i];
        }
        ring_w += n;
        clock_s += n;
        if (in)
            in += n;
        out += 2 * n;
        frames -= n;
    }
    if (pi > peak_in) peak_in = (uint16_t)(pi > 32767u ? 32767u : pi);
    if (po > peak_out) peak_out = (uint16_t)(po > 32767u ? 32767u : po);
}

/* -------------------------------------------------------------- model --- */

static void fill_scope(void)
{
    uint32_t end = ring_w, start = end - RING + 2u * UI_SCOPE * 2u, t, i;
    /* the latest rising zero crossing that leaves a whole window after it: a still picture of a steady tone */
    for (t = end - 2u * UI_SCOPE * 2u; t > start; t--)
        if (ring_buf[(t - 1u) & (RING - 1u)] < 0 && ring_buf[t & (RING - 1u)] >= 0)
            break;
    if (t <= start)
        t = end - 2u * UI_SCOPE * 2u;
    for (i = 0; i < UI_SCOPE; i++) {
        int32_t v = ring_buf[(t + i * 4u) & (RING - 1u)] >> 8;
        mdl.scope[i] = (int8_t)(v > 127 ? 127 : v < -127 ? -127 : v);
    }
}

/* a / b as 0 .. 65535 for a <= b, in 32 bits (b up to 2^24) */
static uint32_t frac16(uint32_t a, uint32_t b)
{
    unsigned s = 0;
    if (!b)
        return 0;
    while ((b >> s) > 0xFFFFu)
        s++;
    a >>= s;
    b >>= s;
    return a >= b ? 65535u : (a << 16) / b;
}

static void fill_loop(void)
{
    uint32_t len, pos, cap, span, b;
    const int16_t *s = pd_looper_peek(eng.looper.state, &len, &pos, &cap);
    int st = pd_looper_state(eng.looper.state);
    mdl.loop_state = (uint8_t)st;
    mdl.loop_len_ms = len * 1000u / PD_FS;                  /* (88 k samples * 1000 fits in 32 bits) */
    mdl.loop_cap_ms = cap * 1000u / PD_FS;
    mdl.loop_level = (uint16_t)ui.loop_level;
    span = st == PD_LOOP_RECORD ? cap : len;                 /* recording: the ring is the room, filling up */
    mdl.loop_pos = (uint16_t)frac16(pos, span);
    mdl.wave_n = st == PD_LOOP_EMPTY || !span ? 0 : (uint16_t)(len * UI_WAVE / span);
    for (b = 0; b < mdl.wave_n; b++) {
        uint32_t a = b * span / UI_WAVE, e = (b + 1u) * span / UI_WAVE, j, pk = 0;
        uint32_t stride = (e - a) / 48u + 1u;
        if (e > len)
            e = len;
        for (j = a; j < e; j += stride) {
            uint32_t v = (uint32_t)(s[j] < 0 ? -s[j] : s[j]);
            if (v > pk)
                pk = v;
        }
        pk >>= 6;                                            /* 0 .. 511, then a soft knee: quiet takes stay visible */
        mdl.wave[b] = (uint8_t)(pk > 255u ? 255u : pk);
    }
}

/* the tempo: MENU > TEMPO, until a closed loop sets it, taken as 1, 2, 4 .. 64 beats, whichever lands in 70 .. 140
 * BPM; the beat runs with the loop's position, or free at the tempo when there is no loop */
static void fill_tempo(void)
{
    uint32_t len, pos, cap, spb, t;
    int st = pd_looper_state(eng.looper.state);
    pd_looper_peek(eng.looper.state, &len, &pos, &cap);
    if (st != PD_LOOP_EMPTY && st != PD_LOOP_RECORD && len && len != ui.loop_len) {
        uint32_t n = 1;
        while (n < 64u && n * 600u * PD_FS / len < 700u)
            n <<= 1;
        ui.loop_len = len;
        ui.loop_beats = n;
        ui.bpm10 = n * 600u * PD_FS / len;
    }
    if (st == PD_LOOP_EMPTY)
        ui.loop_len = 0;
    if (ui.loop_len && st != PD_LOOP_RECORD) {
        spb = ui.loop_len / ui.loop_beats;
        t = pos;
    } else {
        spb = 600u * PD_FS / (ui.bpm10 ? ui.bpm10 : 1200u);
        t = clock_s;
    }
    if (!spb)
        spb = 1;
    mdl.bpm10 = (uint16_t)ui.bpm10;
    mdl.beat = (uint8_t)(t / spb % 4u);
    mdl.beat_frac = (uint8_t)(t % spb * 255u / spb);
}

static void hints(uint8_t k0, const char *a0, uint8_t k1, const char *a1, uint8_t k2, const char *a2)
{
    mdl.hint_key[0] = k0;
    mdl.hint_act[0] = a0;
    mdl.hint_key[1] = k1;
    mdl.hint_act[1] = a1;
    mdl.hint_key[2] = k2;
    mdl.hint_act[2] = a2;
    mdl.nhints = 3;
}

static const char *loop_next(void)
{
    static const char *const N[] = { "REC", "PLAY", "DUB", "PLAY", "PLAY" };
    return N[mdl.loop_state % 5u];
}

static void row(ui_row_t *r, const char *label, uint8_t ic, int32_t bar, uint8_t vic)
{
    r->label = label;
    r->value[0] = 0;
    r->bar = bar;
    r->dim = 0;
    r->icon = ic;                                    /* (icons in the model are IC_* + 1, 0 = none) */
    r->vicon = vic;
    r->kind = UI_ROW_VALUE;
}

static void row_param(ui_row_t *r, const unit_t *u, unsigned i)
{
    const param_t *p = &KINDS[u->kind].p[i];
    row(r, p->name, (uint8_t)(p->icon + 1u), p->choices ? -1 : u->v[i], p->vicons ? (uint8_t)(p->vicons[u->v[i]] + 1u) : 0);
    if (!p->vicons)
        format(r->value, p, u->v[i]);
    if (p->labels == OFF_ON)
        r->kind = UI_ROW_SWITCH;
}

static void graph_of(const unit_t *u)
{
    const int32_t *v = u->v;
    mdl.graph = KINDS[u->kind].graph;
    switch (u->kind) {
    case K_SYNTH:
        mdl.g[0] = v[PD_SYN_WAVE];
        mdl.g[1] = v[PD_SYN_CUTOFF];
        mdl.g[2] = v[PD_SYN_RESONANCE];
        mdl.g[3] = v[PD_SYN_ATTACK];
        mdl.g[4] = v[PD_SYN_DECAY];
        mdl.g[5] = v[PD_SYN_SUSTAIN];
        mdl.g[6] = v[PD_SYN_RELEASE];
        break;
    case K_INPUT:
        mdl.g[0] = (v[PD_IN_GATE] * (v[PD_IN_GATE] >> 2)) >> 15;
        mdl.g[1] = v[PD_IN_LOWCUT];
        break;
    default:
        for (unsigned i = 0; i < 8u; i++)
            mdl.g[i] = i < KINDS[u->kind].np && mdl.graph != UI_G_SCOPE ? v[i] : 0;   /* (the scope's g: the gate) */
        break;
    }
}

static void no_hints(void) { mdl.nhints = 0; }

static void fill_page(void)
{
    unsigned r, n = 0;
    mdl.graph = UI_G_NONE;
    mdl.nrows = 0;
    mdl.title = 0;
    mdl.title_icon = 0;
    no_hints();
    mdl.head_slot = ui.page == P_HOME ? (uint8_t)(ui.home_focus + 1u) : ui.page == P_FXEDIT ? (uint8_t)(ui.edit + 1u)
                  : 0u;                                                   /* 1..3 EFX, 4 AMP, 5 TAPE, 6 IN */
    switch (ui.page) {
    case P_HOME: {
        ui_row_t t;
        mdl.page = UI_PAGE_HOME;
        mdl.focus = ui.home_focus;
        for (r = 0; r < NSLOT; r++) {                    /* EFX1..3: the effect's name and first value; OFF: the slot */
            unsigned kd = slot_kind[r];
            if (kd == SK_OFF) {
                mdl.card_label[r] = SLOT_NAMES[r];
                put_s(mdl.card_value[r], "OFF");
                mdl.card_icon[r] = IC_FX + 1u;
                mdl.card_dim[r] = 1;
                mdl.card_bar[r] = -1;
            } else {
                row_param(&t, &fx_u[r], 0);
                mdl.card_label[r] = KINDS[kd].name;
                put_s(mdl.card_value[r], t.value);
                mdl.card_icon[r] = (uint8_t)(KINDS[kd].icon + 1u);
                mdl.card_dim[r] = 0;
                mdl.card_bar[r] = KINDS[kd].p[0].choices ? -1 : fx_u[r].v[0];
            }
        }
        {                                                /* the scope's: the input's gain */
            ui_row_t t;
            row_param(&t, &input_u, PD_IN_GAIN);
            put_s(mdl.in_value, t.value);
            mdl.in_bar = input_u.v[PD_IN_GAIN];
        }
        mdl.card_icon[AMP_SLOT] = IC_AMP + 1u;           /* AMP: the model, its gain (OFF: dim) */
        if (amp_model() == PD_AMP_OFF) {
            mdl.card_label[AMP_SLOT] = "AMP";
            put_s(mdl.card_value[AMP_SLOT], "OFF");
            mdl.card_dim[AMP_SLOT] = 1;
            mdl.card_bar[AMP_SLOT] = -1;
        } else {
            mdl.card_label[AMP_SLOT] = AMP_MODELS[amp_model()];
            put_s(put_u(mdl.card_value[AMP_SLOT], pct(fx_u[AMP_SLOT].v[PD_AMP_GAIN])), "%");
            mdl.card_dim[AMP_SLOT] = 0;
            mdl.card_bar[AMP_SLOT] = fx_u[AMP_SLOT].v[PD_AMP_GAIN];
        }
        if (ui.home_focus != TAPE_FOCUS) {
            hints(UI_KEY_UPDN, "SELECT", UI_KEY_LR, "VALUE", UI_KEY_SEL, "OPEN");
        } else if (mdl.loop_state == UI_LOOP_EMPTY) {    /* the TAPE band: PUSH what comes next, HOLD stop (stopped:
                                                          * clear), KNOB its level */
            hints(UI_KEY_SEL, loop_next(), UI_KEY_LR, "LEVEL", 0, 0);
            mdl.nhints = 2;
        } else {
            hints(UI_KEY_SEL, loop_next(), UI_KEY_HOLD, mdl.loop_state == UI_LOOP_STOP ? "CLEAR" : "STOP", UI_KEY_LR,
                  "LEVEL");
        }
        return;
    }
    case P_MENU: {
        static const char *const NAMES[MENU_ROWS] = { "INPUT GAIN", "GATE", "LOW CUT", "COLOR", "MASTER", "TAPE LEVEL",
                                                      "TEMPO", "VERSION", "CPU" };
        static const uint8_t ICONS[MENU_ROWS] = { IC_AMP, IC_GATE, IC_LOWCUT, IC_COLOR, IC_SPEAKER, IC_LOOP, IC_TIME,
                                                  IC_INFO, IC_INFO };
        mdl.title = "MENU";
        mdl.title_icon = IC_MENU + 1u;
        for (r = 0; r < MENU_ROWS; r++)
            row(&mdl.rows[r], NAMES[r], (uint8_t)(ICONS[r] + 1u), r == MR_MASTER ? ui.master : r == MR_TAPE ? ui.loop_level : -1, 0);
        for (r = MR_IN_GAIN; r <= MR_IN_LOWCUT; r++) {
            ui_row_t t;
            row_param(&t, &input_u, r - MR_IN_GAIN);
            put_s(mdl.rows[r].value, t.value);
            mdl.rows[r].bar = input_u.v[r - MR_IN_GAIN];
        }
        put_s(mdl.rows[MR_COLOR].value, ui_palette_name(ui.palette));
        mdl.rows[MR_COLOR].kind = UI_ROW_COLOR;
        put_s(put_u(mdl.rows[MR_MASTER].value, pct(ui.master)), "%");
        put_s(put_u(mdl.rows[MR_TAPE].value, pct(ui.loop_level)), "%");
        put_s(put_u(mdl.rows[MR_TEMPO].value, (ui.bpm10 + 5u) / 10u), ui.loop_len ? " LOOP" : " BPM");
        put_s(mdl.rows[MR_VERSION].value, PENDEL_VERSION);
        put_s(put_u(mdl.rows[MR_CPU].value, mdl.cpu_pct), "%");
        n = MENU_ROWS;
        hints(UI_KEY_HOME, "CLOSE", UI_KEY_UPDN, "ROW", UI_KEY_LR, "VALUE");
        break;
    }
    case P_SOURCE: {
        unit_t *u = src_unit();
        mdl.title = KINDS[u->kind].name;
        mdl.title_icon = (uint8_t)(KINDS[u->kind].icon + 1u);
        if (SRC_ROW)
            row(&mdl.rows[n++], "SOURCE", 0, -1, (uint8_t)(KINDS[u->kind].icon + 1u));
        for (r = 0; r < KINDS[u->kind].np && n < UI_ROWS; r++)
            row_param(&mdl.rows[n++], u, r);
        graph_of(u);
        break;
    }
    case P_FXEDIT: {                                     /* an EFX slot: TYPE, MIX, the effect's values */
        unsigned kd = ui.edit < NSLOT ? slot_kind[ui.edit] : SK_OFF;
        if (ui.edit == AMP_SLOT) {                       /* the AMP: its values, MODEL first */
            mdl.title = "AMP";
            mdl.title_icon = IC_AMP + 1u;
            for (r = 0; r < KINDS[K_AMP].np && n < UI_ROWS; r++)
                row_param(&mdl.rows[n++], &fx_u[AMP_SLOT], r);
            graph_of(&fx_u[AMP_SLOT]);
            hints(UI_KEY_HOME, "BACK", UI_KEY_UPDN, "ROW", UI_KEY_LR, "VALUE");
            break;
        }
        pd_slot *sl = &eng.fx.slot[ui.edit];
        int32_t mix = sl->mix > 32767 ? 32767 : sl->mix;
        mdl.title = SLOT_NAMES[ui.edit];
        mdl.title_icon = (uint8_t)((kd == SK_OFF ? IC_FX : KINDS[kd].icon) + 1u);
        row(&mdl.rows[n], "TYPE", IC_FX + 1u, -1, kd == SK_OFF ? 0 : (uint8_t)(KINDS[kd].icon + 1u));
        put_s(mdl.rows[n++].value, kd == SK_OFF ? "OFF" : KINDS[kd].name);
        if (kd != SK_OFF) {
            row(&mdl.rows[n], "MIX", IC_MIX + 1u, mix, 0);
            put_s(put_u(mdl.rows[n++].value, pct(mix)), "%");
            for (r = 0; r < KINDS[kd].np && n < UI_ROWS; r++)
                row_param(&mdl.rows[n++], &fx_u[ui.edit], r);
            graph_of(&fx_u[ui.edit]);
        }
        mdl.rows[0].vicon = 0;                           /* TYPE: the name in words */
        hints(UI_KEY_HOME, "BACK", UI_KEY_UPDN, "ROW", UI_KEY_LR, "VALUE");
        break;
    }
    default:
        break;
    }
    mdl.page = UI_PAGE_LIST;
    mdl.nrows = (uint8_t)n;
    mdl.focus = ui.row[ui.page];
}

void app_frame(uint32_t now)
{
    (void)now;
    mdl.source = eng.source == PD_SRC_INPUT ? UI_SRC_INPUT : UI_SRC_INSTRUMENT;
    fill_loop();
    fill_tempo();
    fill_scope();
    /* meters: the peak since the last frame, falling no faster than ~1/8 a frame */
    mdl.in_level = peak_in > mdl.in_level ? peak_in : (uint16_t)(mdl.in_level - (mdl.in_level >> 3));
    mdl.out_level = peak_out > mdl.out_level ? peak_out : (uint16_t)(mdl.out_level - (mdl.out_level >> 3));
    peak_in = peak_out = 0;
    mdl.master = (uint16_t)ui.master;
    mdl.midi_note = ui.note;
    mdl.cpu_pct = eng.load.budget ? (uint8_t)(eng.load.peak * 100u / eng.load.budget) : 0;
    eng.load.peak = 0;
    fill_page();
    mdl.menu_open = 0;
    ui_draw(&mdl);
}

const ui_model_t *app_model(void) { return &mdl; }

/* --------------------------------------------------------------- init --- */

static void zero(void *p, unsigned n)
{
    uint8_t *b = p;
    while (n--)
        *b++ = 0;
}

void app_init(uint32_t palette)
{
    zero(&eng, sizeof eng);
    zero(&ui, sizeof ui);
    zero(&k, sizeof k);
    zero(&mdl, sizeof mdl);
    zero(fx_u, sizeof fx_u);
    zero(ring_buf, sizeof ring_buf);
    nspare = 0;
    ring_w = 0;
    pd_arena_init(&arena, pool, sizeof pool);
    pd_engine_init(&eng, &arena, PD_FS);
    synth_u.kind = K_SYNTH;
    synth_u.node = eng.instrument;
    input_u.kind = K_INPUT;
    input_u.node = eng.buffer;
    for (unsigned i = 0; i < KINDS[K_SYNTH].np; i++)
        unit_set(&synth_u, i, KINDS[K_SYNTH].p[i].def);
    for (unsigned i = 0; i < KINDS[K_INPUT].np; i++)
        unit_set(&input_u, i, KINDS[K_INPUT].p[i].def);
    if (!APP_SOURCE_SELECT)
        pd_engine_select(&eng, PD_SRC_INPUT);
    fx_add(K_DRIVE);                                     /* EFX1 DRIVE, EFX2 DELAY, EFX3 OFF (a GAIN, bypassed) */
    fx_add(K_DELAY);
    fx_add(K_GAIN);
    fx_add(K_AMP);                                       /* the AMP card: rack slot 3, after EFX3 */
    slot_kind[0] = K_DRIVE;
    slot_kind[1] = K_DELAY;
    slot_set(2, SK_OFF);
    eng.fx.slot[1].mix = 9000;
    ui.master = eng.master = 26214;
    ui.loop_level = 32767;
    ui.bpm10 = 1200;
    clock_s = 0;
    ui.page = P_HOME;
    ui.home_focus = 0;
    ui.palette = (uint8_t)(palette % ui_palette_count());
    mdl.batt = 0xFF;
    ui_init(palette);
}
