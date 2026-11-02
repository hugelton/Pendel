/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Plays the app on the host: scripted button presses, a plucked string at the input, MIDI notes, 10 ms at a
 * time, and takes screenshots on the way (PPM; tools/ppm2png.py makes the PNGs and a contact sheet).
 * Every screenshot goes through the layout check: no text ellipsised, off the screen or over another text.
 *
 *   ui_render OUTDIR */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "app.h"
#include "pd_dsp.h"

static uint16_t fb[240 * 240];

void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    for (uint32_t r = 0; r < h && y + r < 240; r++)
        for (uint32_t c = 0; c < w && x + c < 240; c++) {
            uint16_t v = px[r * w + c];
            fb[(y + r) * 240 + x + c] = (uint16_t)(v >> 8 | v << 8);   /* the panel's byte order back */
        }
}
void lcd_sync(void) {}

/* ------------------------------------------------------- layout check --- */

typedef struct { int32_t x0, y0, x1, y1; uint32_t flags; char s[32]; } box_t;
static box_t boxes[256];
static unsigned nbox;
static int lint_total;

void ui_lint_text(int32_t x0, int32_t y0, int32_t x1, int32_t y1, const char *s, uint32_t flags)
{
    for (unsigned i = 0; i < nbox; i++)
        if (boxes[i].x0 == x0 && boxes[i].y0 == y0 && !strncmp(boxes[i].s, s, sizeof boxes[i].s - 1)) {
            boxes[i].flags |= flags & 1u;
            return;
        }
    if (nbox < sizeof boxes / sizeof boxes[0]) {
        box_t *b = &boxes[nbox++];
        *b = (box_t){ x0, y0, x1, y1, flags & 5u, { 0 } };
        strncpy(b->s, s, sizeof b->s - 1);
    }
}

/* a box drawn over: the texts under it are out of the check (screen rows; called once per strip) */
void ui_lint_cover(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    unsigned j = 0;
    for (unsigned i = 0; i < nbox; i++)
        if (!(boxes[i].x0 < x1 && x0 < boxes[i].x1 && boxes[i].y0 < y1 && y0 < boxes[i].y1))
            boxes[j++] = boxes[i];
    nbox = j;
}

/* a structural cell (card, panel) must sit on Felucca's layout (firmware/src/ui.c): the cards at x 3 + 59 c, 57 wide,
 * y 28..72; the panel 3..237 x 76..198; a list's panel from 28 or 76 to 198: once per screen, reported */
static int cells_off;
static char cell_note[160];
static int on_layout(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    int xs = (x0 - 3) % 59 == 0 && x0 >= 3 && x0 <= 180 && (x1 == x0 + 57 || (x0 == 3 && x1 == 237));
    int ys = (y0 == 28 && (y1 == 72 || y1 == 198)) || (y0 == 76 && y1 == 198)
          || (y0 == 76 && y1 == 135) || (y0 == 139 && y1 == 198);        /* HOME: the TAPE band, the scope */
    if (x0 == 4 && x1 == 236)                                             /* a list's rows: Felucca's MENU panel */
        return (y0 == 28 || y0 == 76) && y1 > y0 && y1 <= 198;
    return xs && ys;
}
void ui_lint_cell(int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    if (!on_layout(x0, y0, x1, y1)) {
        if (!cells_off)
            snprintf(cell_note, sizeof cell_note, "(%d,%d)-(%d,%d)", x0, y0, x1, y1);
        cells_off++;
    }
}

static int lint(const char *name)
{
    int n = 0;
    for (unsigned i = 0; i < nbox; i++) {
        box_t *a = &boxes[i];
        if (a->flags & 1u)
            printf("  LINT %s: \"%s\" ellipsised\n", name, a->s), n++;
        if (a->x0 < 0 || a->y0 < 0 || a->x1 > 240 || a->y1 > 240)
            printf("  LINT %s: \"%s\" off the screen (%d,%d)-(%d,%d)\n", name, a->s, a->x0, a->y0, a->x1, a->y1), n++;
        for (unsigned j = i + 1; j < nbox; j++) {
            box_t *b = &boxes[j];
            if (a->x0 < b->x1 && b->x0 < a->x1 && a->y0 < b->y1 && b->y0 < a->y1)
                printf("  LINT %s: \"%s\" overlaps \"%s\"\n", name, a->s, b->s), n++;
        }
    }
    if (cells_off) {
        printf("  LINT %s: %d cells off the layout, e.g. %s\n", name, cells_off, cell_note);
        n++;
        cells_off = 0;
    }
    printf("  %-28s %2u texts and icons\n", name, nbox);
    return n;
}

/* ------------------------------------------------------------- player --- */

static const char *outdir;
static const char *palname;
static uint32_t now;
static uint8_t keys;
static int16_t line[1024];
static unsigned lp, period;
static int pluck_on;

/* a plucked string (Karplus-Strong) at the input */
static void pluck(unsigned per, uint32_t seed)
{
    period = per;
    lp = 0;
    for (unsigned i = 0; i < per; i++) {
        seed = seed * 1664525u + 1013904223u;
        line[i] = (int16_t)(((int32_t)(seed >> 16) - 32768) / 3);
    }
    pluck_on = 1;
}

/* 10 ms of audio and a button poll; a frame every 30 ms, and one at the end (the one a shot takes) */
static void run(uint32_t ms)
{
    int16_t in[441], out[882];
    for (uint32_t t = 0; t < ms; t += 10) {
        for (unsigned i = 0; i < 441; i++) {
            if (pluck_on) {
                unsigned q = lp + 1 == period ? 0 : lp + 1;
                in[i] = line[lp];
                line[lp] = (int16_t)(((int32_t)line[lp] + line[q]) * 255 / 512);
                lp = q;
            } else {
                in[i] = 0;
            }
        }
        app_audio(in, out, 441);
        now += 10;
        app_keys(keys, now);
        if (now % 30u == 0)
            app_frame(now);
    }
    nbox = 0;
    app_frame(now);
}

static void press(uint8_t k, uint32_t hold_ms)
{
    keys |= k;
    run(hold_ms);
    keys &= (uint8_t)~k;
    run(20);
}
static void tap(uint8_t k) { press(k, 60); }

/* HOME, then a card (or the band, the scope): focus it with DN from the first; open_card also pushes */
static void focus_card(unsigned c)
{
    tap(APP_K_HOME);
    for (unsigned i = 0; i < 6; i++)
        tap(APP_K_UP);
    for (unsigned i = 0; i < c; i++)
        tap(APP_K_DN);
}
static void open_card(unsigned c)
{
    focus_card(c);
    tap(APP_K_SEL);
    run(40);
}
enum { CARD_EFX1, CARD_EFX2, CARD_EFX3, CARD_AMP, CARD_TAPE };

static void shot(const char *name)
{
    char path[512], full[96];
    snprintf(full, sizeof full, "%s_%s", palname, name);
    lint_total += lint(full);
    nbox = 0;
    snprintf(path, sizeof path, "%s/%s.ppm", outdir, full);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n240 240\n255\n");
    for (int i = 0; i < 240 * 240; i++) {
        uint16_t c = fb[i];
        uint8_t rgb[3] = { (uint8_t)((c >> 11) * 255 / 31), (uint8_t)(((c >> 5) & 63) * 255 / 63),
                           (uint8_t)((c & 31) * 255 / 31) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static int checks_failed;
#define EXPECT(cond, ...)                                     \
    do {                                                      \
        if (!(cond)) {                                        \
            printf("  FAIL %s: ", palname);                   \
            printf(__VA_ARGS__);                              \
            printf("\n");                                     \
            checks_failed++;                                  \
        }                                                     \
    } while (0)

static void session(void)
{
    now = 0;
    keys = 0;
    pluck_on = 0;

    nbox = 0;                                      /* (the last palette's screens are not under it) */
    ui_splash("0.1");                              /* the power-on screen */
    shot("00_splash");
    app_power(2, 0);                               /* the battery: two bars of three */
    pluck(200, 3);
    run(200);
    EXPECT(app_model()->bpm10 == 1200, "default tempo %u", app_model()->bpm10);
    EXPECT(app_model()->batt == 2, "battery %d", app_model()->batt);
    EXPECT(app_model()->page == UI_PAGE_HOME && app_model()->source == UI_SRC_INPUT, "not HOME on the input");
    shot("01_home");

    /* EFX1: DRIVE, its first value up with the knob from HOME (slow: one step a detent; a flick: up to 8), then
     * its page */
    {
        char before[12], slow[12];
        snprintf(before, sizeof before, "%s", app_model()->card_value[0]);
        for (int i = 0; i < 4; i++) {
            app_knob(1, now);
            run(100);
        }
        snprintf(slow, sizeof slow, "%s", app_model()->card_value[0]);
        for (int i = 0; i < 6; i++) {
            app_knob(1, now);
            run(8);
        }
        run(40);
        EXPECT(strcmp(before, slow) != 0 && strcmp(slow, app_model()->card_value[0]) != 0,
               "knob: %s -> %s -> %s", before, slow, app_model()->card_value[0]);
        printf("  the knob: %s, 4 slow detents %s, 6 fast detents %s\n", before, slow, app_model()->card_value[0]);
    }
    app_power(-1, 1);                              /* USB power: the bolt */
    run(40);
    shot("02_home_drive_up");
    open_card(CARD_EFX1);
    EXPECT(app_model()->page == UI_PAGE_LIST && app_model()->graph == UI_G_DRIVE, "EFX1 not DRIVE");
    tap(APP_K_DN), tap(APP_K_DN);                  /* DRIVE */
    shot("03_efx1_drive");

    /* EFX3: OFF -> FILTER, its resonance up */
    open_card(CARD_EFX3);
    EXPECT(app_model()->nrows == 1, "EFX3 not OFF (%d rows)", app_model()->nrows);
    shot("04_efx3_off");
    tap(APP_K_R), tap(APP_K_R), tap(APP_K_R);      /* TYPE: DRIVE, FUZZ, FILTER */
    EXPECT(app_model()->graph == UI_G_FILTER, "EFX3 not FILTER (graph %d)", app_model()->graph);
    tap(APP_K_DN), tap(APP_K_DN), tap(APP_K_DN);   /* RESONANCE */
    for (int i = 0; i < 30; i++)
        tap(APP_K_R);
    shot("05_efx3_filter");

    /* EFX2: DELAY's feedback */
    open_card(CARD_EFX2);
    EXPECT(app_model()->graph == UI_G_DELAY, "EFX2 not DELAY");
    for (int i = 0; i < 3; i++)
        tap(APP_K_DN);                             /* FEEDBACK */
    for (int i = 0; i < 16; i++)
        tap(APP_K_R);
    shot("06_efx2_delay");

    /* AMP: CLEAN -> LEAD, its gain up; the HOME card follows */
    open_card(CARD_AMP);
    EXPECT(app_model()->page == UI_PAGE_LIST && app_model()->nrows == 7, "AMP page: %d rows", app_model()->nrows);
    EXPECT(app_model()->focus == 0, "AMP page opened at row %d (its own row: 0)", app_model()->focus);
    tap(APP_K_R), tap(APP_K_R);                    /* MODEL: CRUNCH, LEAD */
    EXPECT(!strcmp(app_model()->rows[0].value, "LEAD"), "AMP model %s", app_model()->rows[0].value);
    tap(APP_K_DN);
    for (int i = 0; i < 8; i++)
        tap(APP_K_R);                              /* GAIN */
    shot("06b_amp_lead");
    tap(APP_K_HOME);
    run(20);
    EXPECT(!strcmp(app_model()->card_label[CARD_AMP], "LEAD"), "AMP card %s", app_model()->card_label[CARD_AMP]);

    /* the scope: the input's gain on the knob, the push opens MENU at INPUT GAIN */
    for (int i = 0; i < 5; i++)
        tap(APP_K_DN);
    char was[12];
    EXPECT(app_model()->focus == 5 && !strcmp(app_model()->in_value, "1.00x"), "scope: focus %d, IN %s",
           app_model()->focus, app_model()->in_value);
    strcpy(was, app_model()->in_value);
    for (int i = 0; i < 4; i++)
        tap(APP_K_R);
    run(20);
    EXPECT(strcmp(app_model()->in_value, was) != 0, "IN gain did not move (%s)", app_model()->in_value);
    shot("06c_home_input");
    tap(APP_K_SEL);
    run(20);
    EXPECT(app_model()->page == UI_PAGE_LIST && app_model()->focus == 0 && !strcmp(app_model()->title, "MENU"),
           "the push did not open MENU at INPUT GAIN");
    tap(APP_K_HOME);
    run(20);

    /* TAPE, on HOME's band: the push records a take (four plucks, about 1.6 s), closes it, overdubs */
    focus_card(CARD_TAPE);
    EXPECT(app_model()->page == UI_PAGE_HOME && app_model()->focus == CARD_TAPE, "not on the TAPE band");
    EXPECT(app_model()->nhints == 2 && app_model()->hint_key[0] == UI_KEY_LOOP, "the TAPE band does not name VOL+");
    press(APP_K_SEL, 40);
    static const unsigned per[] = { 168, 200, 224, 252 };
    for (int i = 0; i < 4; i++) {
        pluck(per[i], 7u + (unsigned)i);
        run(i == 1 ? 200 : 400);
        if (i == 1) {
            EXPECT(app_model()->loop_state == UI_LOOP_REC && app_model()->wave_n > 0, "not recording");
            shot("07_tape_recording");
            run(200);
        }
    }
    press(APP_K_SEL, 40);                          /* close: it plays */
    pluck_on = 0;
    run(700);
    EXPECT(app_model()->loop_state == UI_LOOP_PLAY && app_model()->loop_len_ms > 1500 && app_model()->loop_len_ms < 1800,
           "loop: state %d, %u ms", app_model()->loop_state, (unsigned)app_model()->loop_len_ms);
    {   /* the take sets the tempo: a whole number of beats in 70 .. 140 BPM */
        unsigned bpm10 = app_model()->bpm10, ms = app_model()->loop_len_ms;
        unsigned beats = (bpm10 * ms + 300000u) / 600000u;
        EXPECT(bpm10 >= 700 && bpm10 <= 1400 && beats && (beats & (beats - 1)) == 0,
               "tempo %u.%u BPM over a %u ms loop", bpm10 / 10, bpm10 % 10, ms);
        printf("  the take: %u ms -> %u.%u BPM, %u beats\n", ms, bpm10 / 10, bpm10 % 10, beats);
    }
    shot("08_tape_playing");
    tap(APP_K_SEL);                                /* overdub */
    pluck(134, 99);
    run(500);
    EXPECT(app_model()->loop_state == UI_LOOP_DUB, "not overdubbing (%d)", app_model()->loop_state);
    shot("09_tape_dub");
    tap(APP_K_SEL);
    pluck_on = 0;
    run(300);

    tap(APP_K_HOME);
    run(200);
    shot("10_home_playing");

    focus_card(CARD_TAPE);
    press(APP_K_SEL, 900);                         /* held 0.7 s: stop */
    EXPECT(app_model()->loop_state == UI_LOOP_STOP, "not stopped (%d)", app_model()->loop_state);
    tap(APP_K_HOME);
    run(100);

    /* the tape's keys from another page (EFX1 open): VOL- plays the stopped tape from the top, stops it; VOL+ overdubs,
     * plays; VOL- held 2 s clears; VOL+ records and closes a new take */
    focus_card(0);
    tap(APP_K_SEL);
    run(100);
    EXPECT(app_model()->page != UI_PAGE_HOME, "EFX1 did not open");
    tap(APP_K_STOP);
    run(100);
    EXPECT(app_model()->loop_state == UI_LOOP_PLAY, "VOL- did not play the stopped tape (%d)", app_model()->loop_state);
    tap(APP_K_LOOP);
    run(100);
    EXPECT(app_model()->loop_state == UI_LOOP_DUB, "VOL+ did not overdub (%d)", app_model()->loop_state);
    tap(APP_K_LOOP);
    run(100);
    EXPECT(app_model()->loop_state == UI_LOOP_PLAY, "VOL+ did not end the overdub (%d)", app_model()->loop_state);
    tap(APP_K_STOP);
    run(100);
    EXPECT(app_model()->loop_state == UI_LOOP_STOP, "VOL- did not stop (%d)", app_model()->loop_state);
    tap(APP_K_LOOP);
    run(100);
    EXPECT(app_model()->loop_state == UI_LOOP_PLAY, "VOL+ did not play the stopped tape (%d)", app_model()->loop_state);
    press(APP_K_STOP, 2200);                       /* held 2 s: stops at once, then clears */
    EXPECT(app_model()->loop_state == UI_LOOP_EMPTY, "VOL- held did not clear (%d)", app_model()->loop_state);
    tap(APP_K_LOOP);
    run(300);
    EXPECT(app_model()->loop_state == UI_LOOP_REC, "VOL+ did not record (%d)", app_model()->loop_state);
    tap(APP_K_LOOP);
    run(100);
    EXPECT(app_model()->loop_state == UI_LOOP_PLAY, "VOL+ did not close the take (%d)", app_model()->loop_state);
    press(APP_K_STOP, 2200);
    EXPECT(app_model()->loop_state == UI_LOOP_EMPTY, "not cleared (%d)", app_model()->loop_state);
    tap(APP_K_HOME);
    run(100);
    shot("11_home_cleared");

    press(APP_K_HOME, 800);                        /* HOME held: MENU */
    run(100);
    EXPECT(app_model()->page == UI_PAGE_LIST && app_model()->nrows == 9, "MENU not open (page %d rows %d)",
           app_model()->page, app_model()->nrows);
    tap(APP_K_DN);                                 /* GATE */
    for (int i = 0; i < 12; i++)
        tap(APP_K_R);
    shot("12_menu");
    for (int i = 0; i < 5; i++)                    /* TEMPO: whole BPM, a step each, with no loop */
        tap(APP_K_DN);
    tap(APP_K_R);
    run(100);
    unsigned b0 = app_model()->bpm10;
    for (int i = 0; i < 3; i++)
        tap(APP_K_R);
    run(100);
    EXPECT(app_model()->bpm10 == b0 + 30u && b0 % 10u == 0, "TEMPO: %u then %u", b0, app_model()->bpm10);
    shot("13_menu_tempo");
    tap(APP_K_HOME);                               /* a tap closes it */
    run(100);
    EXPECT(app_model()->page == UI_PAGE_HOME, "MENU not closed");
}

int main(int argc, char **argv)
{
    static const struct { const char *name; uint8_t palette; } PAL[] = { { "grey", 0 }, { "amber", 2 }, { "paper", 6 } };
    outdir = argc > 1 ? argv[1] : ".";
    for (unsigned p = 0; p < sizeof PAL / sizeof PAL[0]; p++) {
        palname = PAL[p].name;
        app_init(PAL[p].palette);
        session();
    }
    printf("%d layout problems, %d session checks failed\n", lint_total, checks_failed);
    return lint_total != 0 || checks_failed != 0;
}
