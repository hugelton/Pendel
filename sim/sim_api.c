/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* The simulator's side of the app: the screen as RGBA for a canvas, buffers the AudioWorklet fills and reads.
 * Built to WebAssembly by the Makefile (make sim); sim/worklet.js calls these. */
#include <stdint.h>

#include "app.h"

static uint8_t rgba[240 * 240 * 4];
static int16_t in_buf[1024], out_buf[2048];
static uint32_t dirty;

void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    for (uint32_t r = 0; r < h && y + r < 240; r++)
        for (uint32_t c = 0; c < w && x + c < 240; c++) {
            uint16_t v = px[r * w + c];
            uint16_t p = (uint16_t)(v >> 8 | v << 8);
            uint8_t *d = &rgba[((y + r) * 240 + x + c) * 4];
            d[0] = (uint8_t)((p >> 11) * 255 / 31);
            d[1] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
            d[2] = (uint8_t)((p & 31) * 255 / 31);
            d[3] = 255;
        }
    dirty = 1;
}
void lcd_sync(void) {}

void sim_init(uint32_t palette) { app_init(palette); }
int16_t *sim_in(void) { return in_buf; }
int16_t *sim_out(void) { return out_buf; }
uint8_t *sim_screen(void) { return rgba; }
void sim_audio(uint32_t frames) { app_audio(in_buf, out_buf, frames > 1024u ? 1024u : frames); }
void sim_keys(uint32_t down, uint32_t now) { app_keys((uint8_t)down, now); }
void sim_volume(int32_t dir) { app_master_step(dir); }
void sim_midi(uint32_t st, uint32_t d1, uint32_t d2) { app_midi((uint8_t)st, (uint8_t)d1, (uint8_t)d2); }
uint32_t sim_frame(uint32_t now)
{
    dirty = 0;
    app_frame(now);
    return dirty;
}
