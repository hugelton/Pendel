/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Pendel's screens. Flat, as Felucca: SURF cards on BG, no rules, Inter Tight S/M/L, the palette's tokens (gfx.c).
 * Pictures first: the loop is a tape (its waveform passing a head), the source a scope (or, for the synth, its wave,
 * filter and envelope), each effect its curve; numbers sit under them.
 * The screen is drawn strip by strip: every draw function works in screen coordinates and draw_screen runs once
 * per 40-row strip with cv_oy shifting it (what falls outside the strip is clipped). */
#include <stddef.h>
#include <stdint.h>

#include "ui.h"

#ifdef UI_LINT                         /* the host's layout check (tests/ui_render.c): text boxes in screen rows */
void ui_lint_text(int32_t x0, int32_t y0, int32_t x1, int32_t y1, const char *s, uint32_t flags);
void ui_lint_cover(int32_t x0, int32_t y0, int32_t x1, int32_t y1);      /* what was drawn there is hidden now */
#define LINT_COVER(x0, y0, x1, y1) ui_lint_cover(x0, (y0) - cv_oy, x1, (y1) - cv_oy)
void ui_lint_cell(int32_t x0, int32_t y0, int32_t x1, int32_t y1);      /* a card, panel or row: on the grid */
#define LINT_CELL(x0, y0, x1, y1) ui_lint_cell(x0, y0, x1, y1)
#define GFX_HOOK_TEXT(x0, y0, x1, y1, s, flags) ui_lint_text(x0, (y0) - cv_oy, x1, (y1) - cv_oy, s, flags)
#define GFX_HOOK_BLIT(x, y, r0) ((void)0)
#define GFX_HOOK_BEGIN() ((void)0)
#define GFX_HOOK_PIXELS(n) ((void)0)
#else
#define LINT_COVER(x0, y0, x1, y1) ((void)0)
#define LINT_CELL(x0, y0, x1, y1) ((void)0)
#endif

#define CV_MAX (240u * 40u)       /* the canvas: a 240 x 40 strip, 19 KB (the Pocket AMP's RAM; draw_screen) */
#include "gfx.c"

#include "ui_icons.h"

/* The grid: 4 px. Margins 12, header 0 .. 31, footer 208 .. 239, rows 20, cards and panels on the grid. Few words:
 * icons name things, pictures show them; the footer only says what a button does when it matters. */
#define U 4
#define W 240
#define H 240
#define STRIP 40
/* Felucca's layout (firmware/src/ui.c): header 0..24, four cards 28..72 (57 px at x 3 + 59 c), the panel 76..198,
 * the footer 202..240 (two rows: the keys' hints at +2, names at +19); BG between them */
#define HEAD_H 24
#define CARD_Y 28
#define CARD_W 57
#define CARD_H 44
#define CARD_X(c) (3 + 59 * (int32_t)(c))
#define PANEL_X 3
#define PANEL_W 234
#define PANEL_Y 76
#define PANEL_H 122
#define FOOT_Y 202
#define MARGIN (3 * U)
#define ROW_H (5 * U)

static const uint8_t KEY_KC[] = { KC_SEL, KC_LOOP, KC_UPDN, KC_LR, KC_HOLD, KC_HOME };

static int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ---------------------------------------------------------- strings --- */

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
static char *put_sec(char *d, uint32_t ms)          /* 1850 -> "1.85" */
{
    uint32_t cs = (ms + 5u) / 10u;
    d = put_u(d, cs / 100u);
    *d++ = '.';
    *d++ = (char)('0' + cs / 10u % 10u);
    *d++ = (char)('0' + cs % 10u);
    *d = 0;
    return d;
}
static char *put_pct(char *d, uint32_t q15) { return put_s(put_u(d, (q15 * 100u + 16383u) / 32767u), "%"); }
static const char *note_name(uint8_t n, char *b)
{
    static const char *const NN[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
    char *d = put_s(b, NN[n % 12u]);
    if (n < 12u)
        put_s(d, "-1");
    else
        put_u(d, n / 12u - 1u);
    return b;
}

/* ------------------------------------------------------------ states --- */

static const char *loop_word(uint8_t st)
{
    static const char *const WORDS[] = { "EMPTY", "REC", "PLAY", "DUB", "STOP" };
    return WORDS[st < 5u ? st : 0u];
}

static uint16_t loop_colour(uint8_t st)
{
    return st == UI_LOOP_REC ? T_REC : st == UI_LOOP_DUB ? T_ACCENT : st == UI_LOOP_PLAY ? T_THEME : T_DIM;
}

/* ------------------------------------------------------------- maths --- */

static uint32_t isqrt(uint32_t v)
{
    uint32_t r = 0, b = 1u << 30;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else {
            r >>= 1;
        }
        b >>= 2;
    }
    return r;
}

/* a sine-like cycle for pictures, t 0 .. 65535 (two parabolas: no table needed here) */
static int32_t sin_pic(int32_t t)
{
    int32_t x = t & 0x7FFF, y = ((x * (32768 - x)) >> 15) * 4;   /* 4 u (1 - u), u = x / 32768 */
    return t & 0x8000 ? -y : y;
}

/* ------------------------------------------------------------- tape --- */
/* The looper as a tape, drawn flat: the loop's own waveform passing a head through a window of tape. */

/* the clip rows of the strip being drawn */
static void strip_rows(int32_t *y0, int32_t *y1)
{
    if (*y0 < -cv_oy) *y0 = -cv_oy;
    if (*y1 > (int32_t)cv_h - cv_oy) *y1 = (int32_t)cv_h - cv_oy;
}

/* the window of tape: the loop's waveform scrolling past the head at the centre (what has played to its left), the
 * whole loop across the width; while recording, the take so far runs up to the head and the rest is blank tape */
static void tape_strip(int32_t x, int32_t y, int32_t w, int32_t h, const ui_model_t *m, uint16_t fg, uint16_t rest,
                       uint16_t under)
{
    int32_t c, half = w / 2, cy = y + h / 2, n = m->wave_n;
    int32_t head = (int32_t)((uint32_t)m->loop_pos * UI_WAVE >> 16);
    int wrap = m->loop_state != UI_LOOP_REC && n == UI_WAVE;
    cv_rrect(x, y, w, h, 3, T_RAISE, under);
    cv_rect(x, y + 2, w, 1, T_LINE);                         /* the tape's edges */
    cv_rect(x, y + h - 3, w, 1, T_LINE);
    for (c = 0; c < w; c++) {
        int32_t off = c - half, bin = head + off * UI_WAVE / w, a;
        if (wrap)
            bin = ((bin % UI_WAVE) + UI_WAVE) % UI_WAVE;
        if (bin < 0 || bin >= n || m->loop_state == UI_LOOP_EMPTY)
            continue;
        a = m->wave[bin] * (h / 2 - 4) / 255;
        cv_rect(x + c, cy - a, 1, 2 * a + 1, off < 0 ? fg : rest);
    }
    cv_rect(x + half - 1, y, 2, h, T_ACCENT);                /* the head */
    cv_rect(x + half - 3, y, 6, 2, T_ACCENT);
    cv_rect(x + half - 3, y + h - 2, 6, 2, T_ACCENT);
}

/* ------------------------------------------------------------- parts --- */

static void bar(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t v, uint16_t fill, uint16_t under)
{
    int32_t f = (int32_t)((uint32_t)w * (v > 32767u ? 32767u : v) / 32767u);
    cv_rrect(x, y, w, h, h / 2, T_RAISE, under);
    if (f >= h)
        cv_rrect(x, y, f, h, h / 2, fill, T_RAISE);
}

static void meter(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t v, uint16_t under)
{
    int32_t hot = w * 94 / 100, f = (int32_t)((uint32_t)w * v / 32767u);
    bar(x, y, hot, h, v * 100u / 94u, T_THEME, under);
    if (f > hot)
        cv_rect(x + hot, y, f - hot, h, T_REC);
}

/* ------------------------------------------------- anti-aliased lines --- */
/* Lines are drawn by their distance: a pixel is covered by how far its centre lies inside the stroke, and blended
 * over what is already on the canvas. Coordinates in 1/16 px (a pixel's centre at +8). */

static void blend(int32_t x, int32_t y, uint16_t c, int32_t a16)
{
    uint16_t *p;
    y += cv_oy;
    if (a16 <= 0 || (uint32_t)x >= cv_w || (uint32_t)y >= cv_h)
        return;
    p = &cv_px[(uint32_t)y * cv_w + (uint32_t)x];
    *p = swap16(a16 >= 16 ? c : ux_mix(swap16(*p), c, a16 * 100 / 16));
}

/* the distance from p to the segment a-b, all in 1/16 px (products stay in 32 bits on a 240 px screen) */
static int32_t seg_dist(int32_t px, int32_t py, int32_t ax, int32_t ay, int32_t bx, int32_t by)
{
    int32_t dx = bx - ax, dy = by - ay, ux = px - ax, uy = py - ay, l2 = dx * dx + dy * dy, t = ux * dx + uy * dy, cr;
    if (!l2 || t <= 0)
        return (int32_t)isqrt((uint32_t)(ux * ux + uy * uy));
    if (t >= l2) {
        int32_t vx = px - bx, vy = py - by;
        return (int32_t)isqrt((uint32_t)(vx * vx + vy * vy));
    }
    cr = ux * dy - uy * dx;
    return (cr < 0 ? -cr : cr) / (int32_t)isqrt((uint32_t)l2);
}

/* a polyline `width` (1/16 px) wide through n points. Each pixel is drawn once, by the segment nearest to it (ties:
 * the earlier), so the joints do not darken */
static void aa_poly(const int32_t *xs, const int32_t *ys, uint32_t n, int32_t width, uint16_t c)
{
    int32_t half = width / 2;
    uint32_t i;
    for (i = 0; i + 1u < n; i++) {
        int32_t x0 = (xs[i] < xs[i + 1] ? xs[i] : xs[i + 1]) - half - 16, x1 = (xs[i] > xs[i + 1] ? xs[i] : xs[i + 1]) + half + 16;
        int32_t y0 = (ys[i] < ys[i + 1] ? ys[i] : ys[i + 1]) - half - 16, y1 = (ys[i] > ys[i + 1] ? ys[i] : ys[i + 1]) + half + 16;
        int32_t x, y;
        x0 >>= 4, x1 >>= 4, y0 >>= 4, y1 >>= 4;
        y1++;
        strip_rows(&y0, &y1);
        for (y = y0; y < y1; y++)
            for (x = x0; x <= x1; x++) {
                int32_t px = x * 16 + 8, py = y * 16 + 8, d = seg_dist(px, py, xs[i], ys[i], xs[i + 1], ys[i + 1]);
                if (d > half + 8)
                    continue;
                if (i && seg_dist(px, py, xs[i - 1], ys[i - 1], xs[i], ys[i]) <= d)
                    continue;
                if (i + 2u < n && seg_dist(px, py, xs[i + 1], ys[i + 1], xs[i + 2], ys[i + 2]) < d)
                    continue;
                blend(x, y, c, half + 8 - d);
            }
    }
}

#define STROKE 28          /* graphs: 1.75 px */

/* a polyline through n points spread across w, about the line yc */
static void plot(int32_t x, int32_t yc, int32_t w, const int32_t *pts, uint32_t n, uint16_t c)
{
    int32_t xs[UI_SCOPE], ys[UI_SCOPE];
    uint32_t i;
    if (n > UI_SCOPE)
        n = UI_SCOPE;
    for (i = 0; i < n; i++) {
        xs[i] = x * 16 + 8 + (int32_t)(i * (uint32_t)w * 16u / (n - 1u));
        ys[i] = (yc + pts[i]) * 16 + 8;
    }
    aa_poly(xs, ys, n, STROKE, c);
}

/* an icon (IC_*) of size s (12, 16, 24) at (x, y), fg on bg; for the lint it is a box like a text */
static void icon(int32_t x, int32_t y, uint32_t id, int32_t s, uint16_t fg, uint16_t bg)
{
    const uint8_t *d = s == 24 ? IC24_DATA : s == 16 ? IC16_DATA : IC12_DATA;
    const uint8_t *k = (s == 24 ? IC24_INK : s == 16 ? IC16_INK : IC12_INK) + id * 4u;
    if (id >= IC_COUNT)
        return;
    cv_alpha(x, y, (uint32_t)s, (uint32_t)s, d + id * (uint32_t)(s * s / 2), ramp(fg, bg));
    GFX_HOOK_TEXT(x + k[0], y + k[1] + cv_oy, x + k[2], y + k[3] + cv_oy, "(icon)", 4u);
    (void)k;
}
/* .. its ink centred in the box x, y, w, h */
static void icon_in(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t id, int32_t s, uint16_t fg, uint16_t bg)
{
    const uint8_t *k = (s == 24 ? IC24_INK : s == 16 ? IC16_INK : IC12_INK) + (id < IC_COUNT ? id : 0u) * 4u;
    icon(x + HALF_UP(w - (k[2] - k[0])) - k[0], y + HALF_UP(h - (k[3] - k[1])) - k[1], id, s, fg, bg);
}

static const uint8_t LOOP_IC[] = { IC_EMPTY, IC_REC, IC_PLAY, IC_DUB, IC_STOP };

/* the header: the page's icon and name at the left; at the right, on every page, the tempo, the beat (four dots, the
 * one now lit and fading through its beat) and the loop's state */
/* the header (0..24), Felucca's three slots, every element's ink centred on row 12:
 *   left    the cushion: what is in focus (1..3 EFX, A the amp, T the tape, I the input) as Felucca's track number, a
 *           rounded square in THEME with the figure in INK; the REC mark after it while the tape records or overdubs
 *           (MENU: Felucca's menu head instead, the cog in THEME and the title, no centre group)
 *   centre  the tape's state, the metronome (four dots, the beat lit) and the BPM, one group centred on x 120
 *   right   the master level and the battery (USB power: the bolt, in OK green) */
static void header(const ui_model_t *m, uint32_t ic, const char *title)
{
    char b[8];
    int32_t i, gw, x;
    if (!m->head_slot && title) {                            /* MENU, as Felucca's (ui_menu.c menu_head): the icon in
                                                              * THEME, the title in M at 30; no tape group */
        icon_in(8, 0, 16, HEAD_H, ic ? ic - 1u : IC_MENU, 16, T_THEME, T_BG);
        cv_text(30, CAP_IN(M, HEAD_H), &AF_M, title, T_TEXT);
        if (m->loop_state == UI_LOOP_REC || m->loop_state == UI_LOOP_DUB)
            icon_in(30 + text_w(&AF_M, title) + 6, 0, 12, HEAD_H, m->loop_state == UI_LOOP_REC ? IC_REC : IC_DUB, 12,
                    T_REC, T_BG);
        goto right;
    }
    if (m->head_slot) {
        char d[2] = { m->head_slot < 4u ? (char)('0' + m->head_slot) : m->head_slot == 4u ? 'A' : m->head_slot == 5u ? 'T' : 'I', 0 };
        cv_rrect(4, 4, 16, 16, -4, T_THEME, T_BG);
        cv_text_in(4, 4 + CAP_IN(S, 16), 16, &AF_S, d, T_INK, T_THEME);
    } else if (ic) {
        icon_in(4, 0, 16, HEAD_H, ic - 1u, 16, T_TEXT, T_BG);
    }
    if (m->loop_state == UI_LOOP_REC || m->loop_state == UI_LOOP_DUB)
        icon_in(23, 0, 12, HEAD_H, m->loop_state == UI_LOOP_REC ? IC_REC : IC_DUB, 12, T_REC, T_BG);
    put_u(b, (m->bpm10 + 5u) / 10u);
    gw = 16 + 6 + 4 * 8 - 2 + 4 + text_w(&AF_M, b);         /* state, metronome (4 dots), BPM */
    x = 120 - gw / 2;
    icon_in(x, 0, 16, HEAD_H, LOOP_IC[m->loop_state], 16,
            m->loop_state == UI_LOOP_EMPTY ? T_DIM : loop_colour(m->loop_state), T_BG);
    x += 16 + 6;
    for (i = 0; i < 4; i++) {                                /* the beat */
        int32_t on = i == m->beat;
        uint16_t c = on ? ux_mix(T_ACCENT, T_DIM, m->beat_frac * 100 / 255) : T_RAISE;
        cv_rrect(x + i * 8, HEAD_H / 2 - 3, 6, 6, -3, i == 0 && !on ? T_DIM : c, T_BG);
    }
    x += 4 * 8 - 2 + 4;
    cv_text(x, CAP_IN(M, HEAD_H), &AF_M, b, T_TEXT);
right:
    {
        char v[8];
        int32_t xr = 208;
        if (m->batt != 0xFFu) {                              /* the battery, a 24 px cell at 214 (Felucca's) */
            static const uint8_t BI[5] = { IC_BAT1, IC_BAT1, IC_BAT3, IC_BAT4, IC_BATCHG };
            uint32_t lv = m->batt > 4u ? 4u : m->batt;
            icon_in(214, 0, 24, HEAD_H, BI[lv], 24, lv == 4u ? T_OK : lv <= 1u ? T_WARN : T_THEME, T_BG);
        } else {
            xr = 236;
        }
        put_u(v, ((uint32_t)m->master * 100u + 16383u) / 32767u);
        xr = cv_text_r(xr, CAP_IN(M, HEAD_H), &AF_M, v, T_MID, T_BG);
        icon_in(xr - 12 - 3, 0, 12, HEAD_H, IC_SPEAKER, 12, T_MID, T_BG);
    }
}

/* the footer (202..240): row 1 the keys' hints (8..232), row 2 a name in THEME left (its icon before it) and the
 * page's title in MID right, as Felucca's */
static void footer(const ui_model_t *m, uint32_t ic, const char *name, const char *right)
{
    khint_t h[3];
    uint32_t i;
    int32_t x = 8;
    for (i = 0; i < m->nhints && i < 3u; i++) {
        h[i].key = KEY_KC[m->hint_key[i] % sizeof KEY_KC];
        h[i].act = m->hint_act[i];
    }
    if (i)
        cv_key_row(8, 232, FOOT_Y + 2, h, i, 0xFFu, T_BG);
    if (ic) {
        icon_in(x, FOOT_Y + 19 + AF_S_CAP_Y, 12, AF_S_CAP_H, ic - 1u, 12, T_MID, T_BG);
        x += 12 + 5;
    }
    if (name)
        cv_text_fit(x, FOOT_Y + 19, &AF_S, name, T_THEME, T_BG, 150);
    if (right)
        cv_text_r(232, FOOT_Y + 19, &AF_S, right, T_MID, T_BG);
}

/* ------------------------------------------------------------ graphs --- */

/* the filter's response on a log frequency axis (the cutoff scale itself across), top = 0 dB */
static void filter_curve(int32_t x, int32_t y, int32_t w, int32_t h, int32_t cutoff, int32_t res, int32_t mode,
                         uint16_t c)
{
    int32_t pts[48], i, xc = cutoff * 47 / 32767, peak = res * h / 2 / 32767, base = h / 3;
    for (i = 0; i < 48; i++) {
        int32_t d = i - xc, ad = d < 0 ? -d : d, v;
        int32_t bump = ad < 4 ? peak * (4 - ad) / 4 : 0;
        if (mode == 1)                                        /* band: falls both ways */
            v = base + ad * 3 - bump;
        else if ((mode == 0 && d <= 0) || (mode == 2 && d >= 0))
            v = base - bump;                                  /* the pass band */
        else
            v = base + ad * 4 - bump;                         /* the stop band, 12 dB / oct */
        pts[i] = clampi(v, 0, h - 2);
    }
    plot(x, y, w, pts, 48, c);
}

static void adsr_curve(int32_t x, int32_t y, int32_t w, int32_t h, const int32_t *g, uint16_t c)
{
    /* attack, decay, release take up to a quarter each by their time; sustain holds a quarter */
    int32_t a = 2 + g[0] * (w / 4) / 32767, d = 2 + g[1] * (w / 4) / 32767, r = 2 + g[3] * (w / 4) / 32767;
    int32_t s = h - 2 - g[2] * (h - 4) / 32767, top = 1, bot = h - 2, xs = x + a + d + w / 4;
    int32_t px[5] = { x, x + a, x + a + d, xs, xs + r }, py[5] = { y + bot, y + top, y + s, y + s, y + bot }, i;
    for (i = 0; i < 5; i++) {
        px[i] = px[i] * 16 + 8;
        py[i] = py[i] * 16 + 8;
    }
    aa_poly(px, py, 5, STROKE, c);
}

static void wave_cycle(int32_t x, int32_t y, int32_t w, int32_t h, int32_t wave, uint16_t c)
{
    int32_t pts[33], i, a = h / 2 - 2;
    for (i = 0; i <= 32; i++) {
        int32_t t = i * 2048 > 65535 ? 65535 : i * 2048, v;
        if (wave == 0)
            v = t - 32768;                                    /* saw */
        else if (wave == 1)
            v = t < 32768 ? 32767 : -32767;                   /* square */
        else if (wave == 2)
            v = t < 32768 ? 2 * t - 32768 : 98303 - 2 * t;    /* triangle */
        else
            v = sin_pic(t);
        pts[i] = -v * a / 32768;
    }
    plot(x, y + h / 2, w, pts, 33, c);
}

/* the drive's transfer: input -1 .. 1 across, output up, the same integer maths as the DSP */
static void drive_curve(int32_t x, int32_t y, int32_t w, int32_t h, int32_t drive, int32_t level, uint16_t c)
{
    int32_t pts[41], i, pre = 256 + ((drive * 31) >> 7);
    for (i = 0; i <= 40; i++) {
        int32_t in = (i - 20) * 32767 / 20, v = clampi((in * pre) >> 8, -32767, 32767);
        int32_t o = v - (((((v * v) >> 15) * v) >> 15) * 10923 >> 15);
        o = (((o * 3) >> 1) * (level >> 3)) >> 12;
        pts[i] = -o * (h / 2 - 2) / 32768;
    }
    cv_rect(x, y + h / 2, w, 1, T_LINE);
    cv_rect(x + w / 2, y, 1, h, T_LINE);
    plot(x, y + h / 2, w, pts, 41, c);
}

/* echoes over a 2 s window: the dry hit, then one bar each `time` (1 ms .. 1 s), each fb times the last */
static void delay_taps(int32_t x, int32_t y, int32_t w, int32_t h, int32_t time, int32_t fb, uint16_t c, uint16_t dry)
{
    int32_t t = 1 + time * 999 / 32767, amp = 32767, k, fbq = (fb * 29491) >> 15;
    cv_rect(x, y + h - 1, w, 1, T_LINE);
    cv_rect(x, y + 2, 3, h - 3, dry);
    for (k = 1; k * t <= 2000 && amp > 600; k++) {
        int32_t bx = x + k * t * (w - 4) / 2000, bh;
        if (k > 1)
            amp = (amp * fbq) >> 15;
        bh = amp * (h - 3) / 32767;
        if (bh > 0)
            cv_rect(bx, y + h - 1 - bh, 3, bh, c);
        if (!fbq)
            break;
    }
}

static void scope(int32_t x, int32_t y, int32_t w, int32_t h, const int8_t *s, uint16_t c)
{
    int32_t pts[UI_SCOPE], i;
    for (i = 0; i < UI_SCOPE; i++)
        pts[i] = -s[i] * (h / 2 - 2) / 128;
    cv_rect(x, y + h / 2, w, 1, T_LINE);
    plot(x, y + h / 2, w, pts, UI_SCOPE, c);
}

/* --------------------------------------------------------------- HOME --- */
/* Felucca's home, the chain top to bottom: four cards (EFX1, EFX2, EFX3, AMP: an icon and a name in S, the value in M,
 * a 3 px gauge), the TAPE band (its state, length and level as a card's; the loop passing the head) and the scope (the output; the input's gain and level over it), the two the same height, the footer.
 * LEFT / RIGHT pick a card, the band or the scope (its own colour), the knob sets its value, the push opens it (on the
 * band the push works the tape). */
#define TAPE_Y PANEL_Y
#define TAPE_H 59
#define SCOPE_Y (TAPE_Y + TAPE_H + 4)             /* the cards' 4 px gap */
#define SCOPE_H (PANEL_Y + PANEL_H - SCOPE_Y)

/* HOME's focus: the card, band or scope a faint THEME over SURF (FOCUS_PCT), its name and gauge in ACCENT (Felucca's
 * hot card), the value as it is unfocused */
#define FOCUS_PCT 20
static uint16_t focus_bg(void) { return ux_mix(T_SURF, T_THEME, FOCUS_PCT); }

static void card(int32_t c, int focus, const ui_model_t *m)
{
    int32_t x = CARD_X(c), lx = x + 5;
    int dim = m->card_dim[c];
    uint16_t bg = focus ? focus_bg() : T_SURF;        /* the card in focus: its own colour, no frame */
    uint16_t lc = focus ? T_ACCENT : dim ? T_DIM : T_MID, vc = dim ? T_DIM : T_THEME;
    cv_rrect(x, CARD_Y, CARD_W, CARD_H, 4, bg, T_BG);
    LINT_CELL(x, CARD_Y, x + CARD_W, CARD_Y + CARD_H);
    if (m->card_icon[c] && m->card_label[c] && 5 + 12 + 2 + text_w(&AF_S, m->card_label[c]) <= CARD_W - 2) {
        icon_in(lx, CARD_Y + 3 + AF_S_CAP_Y, 12, AF_S_CAP_H, m->card_icon[c] - 1u, 12, lc, bg);
        lx += 12 + 2;                                   /* (Felucca: the icon only where it fits with the label) */
    }
    if (m->card_label[c])
        cv_text_fit(lx, CARD_Y + 3, &AF_S, m->card_label[c], lc, bg, x + CARD_W - 2 - lx);
    if (text_w(&AF_M, m->card_value[c]) <= CARD_W - 9)    /* the value in M, in S if it would not fit (Felucca) */
        cv_text_on(x + 5, CARD_Y + 17, &AF_M, m->card_value[c], vc, bg);   /* (its edges blended into the card's colour) */
    else
        cv_text_fit(x + 5, CARD_Y + 20, &AF_S, m->card_value[c], vc, bg, CARD_W - 8);
    if (m->card_bar[c] >= 0) {
        int32_t gw = CARD_W - 10, w = m->card_bar[c] * gw / 32767;
        cv_rect(x + 5, CARD_Y + 37, gw, 3, T_RAISE);
        cv_rect(x + 5, CARD_Y + 37, w, 3, focus ? T_ACCENT : dim ? T_DIM : T_THEME);
    }
}

static void tape_band(const ui_model_t *m, int focus)
{
    uint16_t bg = focus ? focus_bg() : T_SURF, lc = focus ? T_ACCENT : T_MID, dc = T_DIM;
    uint16_t vc = m->loop_state == UI_LOOP_EMPTY ? T_THEME : loop_colour(m->loop_state);
    int32_t lx = PANEL_X + 5;
    char len[12], *d;
    cv_rrect(PANEL_X, TAPE_Y, PANEL_W, TAPE_H, 4, bg, T_BG);
    LINT_CELL(PANEL_X, TAPE_Y, PANEL_X + PANEL_W, TAPE_Y + TAPE_H);
    icon_in(lx, TAPE_Y + 3 + AF_S_CAP_Y, 12, AF_S_CAP_H, IC_LOOP, 12, lc, bg);       /* as a card: name, state, gauge */
    cv_text_on(lx + 14, TAPE_Y + 3, &AF_S, "TAPE", lc, bg);
    if (text_w(&AF_M, loop_word(m->loop_state)) <= 54)
        cv_text_on(lx, TAPE_Y + 17, &AF_M, loop_word(m->loop_state), vc, bg);
    else
        cv_text_fit(lx, TAPE_Y + 20, &AF_S, loop_word(m->loop_state), vc, bg, 54);
    d = put_u(len, m->loop_len_ms / 1000u);                                            /* its length */
    *d++ = '.';
    *d++ = (char)('0' + m->loop_len_ms / 100u % 10u);
    *d++ = ' ';
    *d++ = 'S';
    *d = 0;
    cv_text_on(lx, TAPE_Y + 34, &AF_S, len, dc, bg);
    cv_rect(lx, TAPE_Y + TAPE_H - 9, 52, 3, T_RAISE);                                  /* its level */
    cv_rect(lx, TAPE_Y + TAPE_H - 9, 52 * m->loop_level / 32767, 3, focus ? T_ACCENT : T_THEME);
    tape_strip(PANEL_X + 64, TAPE_Y + 6, PANEL_W - 70, TAPE_H - 12, m, loop_colour(m->loop_state), T_DIM, bg);
}

static void home(const ui_model_t *m)
{
    int32_t i;
    header(m, IC_HOME + 1u, 0);
    for (i = 0; i < 4; i++)
        card(i, m->focus == i, m);
    tape_band(m, m->focus == 4);
    {   /* the scope: the output; over it the input, its gain (the knob's when in focus) and its level */
        int sf = m->focus == 5;
        uint16_t bg = sf ? focus_bg() : T_SURF, lc = sf ? T_ACCENT : T_MID, vc = T_THEME;
        int32_t lx = PANEL_X + 5, x;
        cv_rrect(PANEL_X, SCOPE_Y, PANEL_W, SCOPE_H, 4, bg, T_BG);
        LINT_CELL(PANEL_X, SCOPE_Y, PANEL_X + PANEL_W, SCOPE_Y + SCOPE_H);
        scope(PANEL_X + 5, SCOPE_Y + 18, PANEL_W - 10, SCOPE_H - 28, m->scope, T_THEME);
        icon_in(lx, SCOPE_Y + 3 + AF_S_CAP_Y, 12, AF_S_CAP_H, IC_INPUT, 12, lc, bg);
        x = cv_text_on(lx + 14, SCOPE_Y + 3, &AF_S, "IN", lc, bg);
        x = cv_text_on(x + 6, SCOPE_Y + 3, &AF_S, m->in_value, vc, bg);
        meter(x + 8, SCOPE_Y + 3 + AF_S_CAP_Y + AF_S_CAP_H / 2 - 1, PANEL_X + PANEL_W - 5 - (x + 8), 3, m->in_level, bg);
        meter(PANEL_X + 5, SCOPE_Y + SCOPE_H - 7, PANEL_W - 10, 2, m->out_level, bg);
    }
    if (m->focus < 4)
        footer(m, m->card_icon[m->focus], m->card_label[m->focus], "HOME");
    else if (m->focus == 4)
        footer(m, IC_LOOP + 1u, "TAPE", "HOME");
    else
        footer(m, IC_INPUT + 1u, "INPUT", "HOME");
}

/* --------------------------------------------------------------- list --- */
/* An EFX slot and MENU, as Felucca's MENU (ui_menu.c): the rows on a SURF panel (radius 8, 3 px in), a row's name at
 * x 16 and its value right-aligned at 224 in M; the selected row filled THEME (radius 6), its text in INK; 1 px rules
 * between the others, inset 6, none next to the selection. A slot's picture sits in the cards' band above. */

static void graph(const ui_model_t *m, int32_t x, int32_t y, int32_t w, int32_t h)
{
    const int32_t *g = m->g;
    cv_rrect(x, y, w, h, 4, T_SURF, T_BG);
    LINT_CELL(x, y, x + w, y + h);
    x += 8;
    y += 5;
    w -= 16;
    h -= 10;
    switch (m->graph) {
    case UI_G_SCOPE:
        scope(x, y, w, h, m->scope, T_THEME);
        if (g[0]) {                                           /* the gate's threshold, both sides */
            int32_t t = clampi(g[0] * (h / 2) / 8191, 1, h / 2);
            cv_rect(x, y + h / 2 - t, w, 1, T_ACCENT);
            cv_rect(x, y + h / 2 + t, w, 1, T_ACCENT);
        }
        break;
    case UI_G_SYNTH: {
        int32_t cw = (w - 16) / 3;
        wave_cycle(x, y, cw, h, g[0], T_THEME);
        filter_curve(x + cw + 8, y + 2, cw, h - 2, g[1], g[2], 0, T_ACCENT);
        adsr_curve(x + 2 * (cw + 8), y, cw, h, g + 3, T_THEME);
        break;
    }
    case UI_G_DRIVE:
        drive_curve(x + w / 2 - h, y, 2 * h, h, g[0], g[1], T_THEME);
        break;
    case UI_G_DELAY:
        delay_taps(x, y, w, h, g[0], g[1], T_THEME, T_DIM);
        break;
    case UI_G_FILTER:
        filter_curve(x, y + 2, w, h - 2, g[0], g[1], g[2], T_THEME);
        break;
    case UI_G_GAIN:
        bar(x, y + h / 2 - 4, w, 8, (uint32_t)g[0] / 4u, T_THEME, T_SURF);
        cv_rect(x + w / 4, y + h / 2 - 6, 1, 12, T_ACCENT);   /* 1x */
        break;
    default:
        break;
    }
}

/* Felucca's MENU rows (ui_menu.c): one SURF panel 4..236 (radius 8, 3 px in) as tall as its rows, 24 px rows 2 px
 * apart (6 rows: 1 px; 7: 21 px; 8 and more: 18 px), a 1 px rule in the gap (none beside the selection). */
#define LP_X 4
#define LP_W 232
#define LP_PAD 3
#define MR_X 8
#define MR_W 224
#define MR_LX 16
#define MR_VR 224

/* OFF / ON as Felucca's switch: a 26 x 14 cushion, its knob at the right when ON */
static void row_switch(int32_t y, int32_t h, int on, int sel)
{
    int32_t x = MR_VR - 26, ty = y + HALF_UP(h - 14);
    uint16_t under = sel ? T_THEME : T_SURF;
    uint16_t track = sel ? (on ? T_INK : ux_mix(T_THEME, T_INK, 35)) : on ? T_THEME : T_DIM;
    uint16_t knob = sel ? (on ? T_THEME : T_INK) : on ? T_INK : T_TEXT;
    cv_rrect(x, ty, 26, 14, -7, track, under);
    cv_rrect(on ? x + 14 : x + 2, ty + 2, 10, 10, -5, knob, track);
}

static void list_page(const ui_model_t *m)
{
    char pos[8], *d;
    int32_t y0 = m->graph ? PANEL_Y : CARD_Y, yend = PANEL_Y + PANEL_H, n = m->nrows, first = 0, vis, ph, i;
    int32_t rh = n > 7 ? 18 : n > 6 ? 21 : 24, gap = n <= 5 ? 2 : 1;
    header(m, m->title_icon, m->title);
    if (m->graph)
        graph(m, PANEL_X, CARD_Y, PANEL_W, CARD_H);
    vis = (yend - y0 - 2 * LP_PAD + gap) / (rh + gap);
    if (vis > n)
        vis = n;
    if (m->focus >= vis)
        first = m->focus - vis + 1;
    ph = (vis ? vis * (rh + gap) - gap : 0) + 2 * LP_PAD;
    cv_rrect(LP_X, y0, LP_W, ph, 8, T_SURF, T_BG);
    LINT_CELL(LP_X, y0, LP_X + LP_W, y0 + ph);
    for (i = first; i < n && i < first + vis; i++) {
        const ui_row_t *r = &m->rows[i];
        int32_t y = y0 + LP_PAD + (i - first) * (rh + gap), sel = i == m->focus;
        uint16_t under = sel ? T_THEME : T_SURF, lab = sel ? T_INK : r->dim ? T_DIM : T_TEXT;
        uint16_t val = sel ? T_INK : r->dim ? T_DIM : T_THEME;
        const aafont_t *vf = text_w(&AF_M, r->value) > 100 ? &AF_S : &AF_M;
        if (sel)
            cv_rrect(MR_X, y, MR_W, rh, 6, T_THEME, T_SURF);
        else if (i > first && i - 1 != m->focus)
            cv_rect(MR_X + 6, y - 1, MR_W - 12, 1, T_LINE);
        cv_text_fit(MR_LX, y + CAP_IN(M, rh), &AF_M, r->label, lab, under, 110);
        if (r->kind == UI_ROW_SWITCH) {
            row_switch(y, rh, r->value[0] == 'O' && r->value[1] == 'N', sel);
        } else if (r->kind == UI_ROW_COLOR) {        /* the palette's name and its five colours (BG SURF TEXT THEME ACCENT) */
            const uint16_t *tok = &T_BG;
            int32_t x0 = MR_VR - 74, sy = y + HALF_UP(rh - 12), c;
            cv_text_r(x0 - 6, y + CAP_IN(M, rh), &AF_M, r->value, val, under);
            for (c = 0; c < 5; c++) {
                cv_rrect(x0 + c * 15, sy, 12, 12, -3, T_RAISE, under);
                cv_rrect(x0 + 1 + c * 15, sy + 1, 10, 10, -2, tok[c], T_RAISE);
            }
        } else if (r->vicon) {
            icon_in(MR_VR - 16, y, 16, rh, r->vicon - 1u, 16, val, under);
        } else {
            cv_text_r(MR_VR, y + (vf == &AF_M ? CAP_IN(M, rh) : CAP_IN(S, rh)), vf, r->value, val, under);
        }
    }
    d = put_u(pos, (uint32_t)m->focus + 1u);
    *d++ = '/';
    put_u(d, m->nrows);
    footer(m, m->title_icon, m->title, pos);
}

/* ------------------------------------------------------------- driver --- */

void ui_init(uint32_t palette) { palette_set(palette % NPALETTES); }

/* The power-on splash, Felucca's (ui_draw.c draw_splash): a RAISE square (176) centred on the screen, square corners;
 * in it, in M, 16 px in from the left: at the top the name and the version, the first line's capitals 16 px down; at
 * the bottom the maker and "with community", the last baseline 16 px up. Drawn in strips, straight on the panel. */
static uint32_t strip_sig[H / STRIP];                  /* (ui_draw's: the strips as last sent) */
#define SPL_SQ 176
#define SPL_X0 ((W - SPL_SQ) / 2)
#define SPL_PAD 16
#define SPL_PITCH 20

void ui_splash(const char *version)
{
    static const char *const L[4] = { "Pendel", 0, "H\xFCgelton Instruments", "with community" };
    int32_t ty = SPL_X0 + SPL_PAD - AF_M_CAP_Y;                       /* the first line's top */
    int32_t by = SPL_X0 + SPL_SQ - SPL_PAD - AF_M_CAP_H - AF_M_CAP_Y; /* the last line's top */
    uint32_t y;
    for (y = 0; y < H; y += STRIP) {
        cv_begin(W, STRIP, T_BG);
        cv_oy = -(int32_t)y;
        cv_rect(SPL_X0, SPL_X0, SPL_SQ, SPL_SQ, T_RAISE);
        cv_text_on(SPL_X0 + SPL_PAD, ty, &AF_M, L[0], T_TEXT, T_RAISE);
        cv_text_on(SPL_X0 + SPL_PAD, ty + SPL_PITCH, &AF_M, version, T_TEXT, T_RAISE);
        cv_text_on(SPL_X0 + SPL_PAD, by - SPL_PITCH, &AF_M, L[2], T_TEXT, T_RAISE);
        cv_text_on(SPL_X0 + SPL_PAD, by, &AF_M, L[3], T_TEXT, T_RAISE);
        cv_oy = 0;
        cv_blit(0, y);
        strip_sig[y / STRIP] = 0;                                     /* the next frame redraws everything */
    }
    lcd_sync();
}
void ui_palette(uint32_t i) { palette_set(i % NPALETTES); }
const char *ui_palette_name(uint32_t i) { return UI_PALETTES[i % NPALETTES].name; }
uint32_t ui_palette_count(void) { return NPALETTES; }

static void draw_screen(const ui_model_t *m)
{
    if (m->page == UI_PAGE_LIST)
        list_page(m);
    else
        home(m);
}

/* a strip's pixels hashed (FNV-1a over its words): a strip that comes out as it was is not sent again (the panel's
 * SPI is most of a frame's time) */
static uint32_t strip_sig[H / STRIP];

static uint32_t strip_hash(void)
{
    const uint32_t *w = (const uint32_t *)(const void *)cv_px;
    uint32_t h = 2166136261u, i, n = cv_w * cv_h / 2u;
    for (i = 0; i < n; i++)
        h = (h ^ w[i]) * 16777619u;
    return h | 1u;                                      /* never 0: 0 means "not drawn yet" */
}

void ui_draw(const ui_model_t *m)
{
    uint32_t y;
    for (y = 0; y < H; y += STRIP) {
        uint32_t h;
        cv_begin(W, STRIP, T_BG);
        cv_oy = -(int32_t)y;
        draw_screen(m);
        cv_oy = 0;
        h = strip_hash();
        if (h != strip_sig[y / STRIP]) {
            strip_sig[y / STRIP] = h;
            cv_blit(0, y);
        }
    }
    lcd_sync();
}

void ui_redraw_all(void)                                /* the panel lost its picture (woke, re-initialised) */
{
    uint32_t i;
    for (i = 0; i < H / STRIP; i++)
        strip_sig[i] = 0;
}
