/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Pendel's screens (memos/2026-10-28_ui_plan.md). The UI draws from a model, not from the engine: the app fills the
 * model (src/app/app.c) and the host fills it from fixtures, so a screen can be drawn without audio running.
 * Pictures before numbers: the loop is a tape, the source a scope, an effect its curve.
 *
 * The platform gives:  lcd_blit(x, y, w, h, px)  px: w x h RGB565, byte-swapped (the panel's order)
 *                      lcd_sync()                 the last blit has finished reading px */
#ifndef PD_UI_H
#define PD_UI_H

#include <stdint.h>

enum { UI_PAGE_HOME, UI_PAGE_LIST, UI_PAGES };
enum { UI_SRC_INSTRUMENT, UI_SRC_INPUT };
enum { UI_LOOP_EMPTY, UI_LOOP_REC, UI_LOOP_PLAY, UI_LOOP_DUB, UI_LOOP_STOP };   /* as pd_looper's states */
/* the picture on top of a list page; g[] holds its numbers (0 .. 32767 unless noted) */
enum {
    UI_G_NONE,
    UI_G_SCOPE,        /* the scope: g0 gate threshold (0 = none), g1 low-cut (0 = none) */
    UI_G_SYNTH,        /* one cycle of the wave (g0 wave 0..3), the filter curve (g1 cutoff, g2 resonance), the ADSR (g3..g6) */
    UI_G_DRIVE,        /* the transfer curve: g0 drive, g1 level */
    UI_G_DELAY,        /* the echoes: g0 time, g1 feedback */
    UI_G_FILTER,       /* the response: g0 cutoff, g1 resonance, g2 mode 0 LP 1 BP 2 HP */
    UI_G_GAIN,         /* g0 gain, 8192 = 1x */
};

enum { UI_KEY_SEL, UI_KEY_LOOP, UI_KEY_UPDN, UI_KEY_LR, UI_KEY_HOLD, UI_KEY_HOME, UI_KEY_STOP };   /* the footer's keycaps: PUSH, VOL+ (the tape), L/R (the buttons), KNOB, HOLD, HOME, VOL- (its stop) */

#define UI_ROWS 12
#define UI_SCOPE 96        /* scope points */
#define UI_WAVE 128        /* loop overview bins */

/* icons are IC_* (build/gen/ui_icons.h) plus one; 0 = none */
typedef struct {
    const char *label;
    char value[12];
    int32_t bar;           /* 0 .. 32767, -1 = no bar */
    uint8_t dim;           /* drawn inactive (an effect switched off) */
    uint8_t icon;          /* before the label */
    uint8_t vicon;         /* instead of the value (a choice: a wave, a filter mode) */
    uint8_t kind;          /* UI_ROW_*: how the value is drawn */
} ui_row_t;
enum { UI_ROW_VALUE, UI_ROW_SWITCH, UI_ROW_COLOR };   /* the value in words; OFF / ON as a switch; a palette's colours */

typedef struct {
    uint8_t page, focus;               /* HOME: 0..2 EFX1..3, 3 AMP, 4 the TAPE band, 5 the scope (IN); a list: the row */
    uint8_t palette;
    uint8_t source;
    uint8_t loop_state;
    uint32_t loop_len_ms, loop_cap_ms;
    uint16_t loop_pos;                 /* 0 .. 65535 across the loop (recording: across the room) */
    uint16_t loop_level;               /* 0 .. 32767 */
    uint8_t wave[UI_WAVE];             /* the loop's peaks, 0 .. 255, across the loop (recording: across the room) */
    uint16_t wave_n;                   /* bins that hold sound (recording: so far) */
    int8_t scope[UI_SCOPE];            /* the output, the latest cycle-ish */
    uint16_t in_level, out_level;      /* peak, 0 .. 32767 */
    uint16_t master;                   /* 0 .. 32767 */
    /* HOME's four cards (EFX1..3, AMP), as Felucca's: an icon and a name, the value in words, a gauge (-1: none) */
    const char *card_label[4];
    char card_value[4][12];
    uint8_t card_icon[4];              /* IC_* + 1, 0 = none */
    uint8_t card_dim[4];               /* an OFF slot */
    int32_t card_bar[4];
    uint8_t cpu_pct;
    uint8_t batt;                      /* the header's battery: 0..3 bars, 4 USB power, 0xFF not known */
    uint8_t head_slot;                 /* the header's cushion: 1..3 an EFX slot, 4 the AMP, 5 TAPE, 6 IN, 0 none (MENU: the cog) */
    char in_value[12];                 /* HOME's scope: the input's gain, as set ("1.0X") */
    int32_t in_bar;                    /* and its gauge, 0 .. 32767 */
    uint8_t midi_note;                 /* 0: none sounding */
    uint16_t bpm10;                    /* the tempo, in 1/10 BPM */
    uint8_t beat, beat_frac;           /* the beat in the bar (0 .. 3) and how far into it (0 .. 255) */
    /* a list page (SOURCE, FX, an effect) */
    const char *title;
    uint8_t title_icon;
    uint8_t graph, nrows;
    int32_t g[8];
    ui_row_t rows[UI_ROWS];
    /* the page menu (SEL held): a column of names over the page */
    uint8_t menu_open, menu_n, menu_sel;
    const char *menu[6];
    uint8_t menu_icon[6];
    /* the footer: only what matters here (often nothing), up to three hints, UI_KEY_* and words */
    uint8_t hint_key[3];
    const char *hint_act[3];
    uint8_t nhints;
} ui_model_t;

void ui_init(uint32_t palette);
void ui_palette(uint32_t i);              /* the colours, live */
const char *ui_palette_name(uint32_t i);
uint32_t ui_palette_count(void);
void ui_splash(const char *version);      /* the power-on screen, straight on the panel */
void ui_notice(const char *title, const char *line);   /* two centred lines, straight on the panel (the update) */
void ui_draw(const ui_model_t *m);    /* the whole screen (strips that did not change are not sent) */
void ui_redraw_all(void);             /* send every strip on the next ui_draw */

void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px);
void lcd_sync(void);

#endif
