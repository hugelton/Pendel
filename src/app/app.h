/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Pendel, the application: the engine, the six buttons, the pages, the screen. The same code runs on the
 * Pocket AMP (the BSP calls these from its audio interrupt and main loop) and in the browser simulator (sim/).
 *
 *   app_init(palette)                 once
 *   app_audio(in, out_lr, frames)     the audio interrupt: mono 16-bit in (NULL: silence), stereo 16-bit out
 *   app_keys(down, now_ms)            every ~10 ms: which buttons are held (APP_K_* bits)
 *   app_master_step(dir)              a volume button press (and its repeats)
 *   app_knob(detents, now_ms)         the PARAM knob's detents since the last call
 *   app_power(level, usb)             the battery, about once a second
 *   app_midi(status, d1, d2)          a MIDI message
 *   app_frame(now_ms)                 the main loop, ~30 times a second: fills the model and draws the screen
 */
#ifndef PD_APP_H
#define PD_APP_H

#include <stdint.h>

#include "ui.h"

#ifndef PENDEL_VERSION
#define PENDEL_VERSION "dev"               /* a release build sets it: PENDEL_RELEASE=1.0.0-beta firmware/pa/build.sh */
#endif

#ifndef APP_SOURCE_SELECT
#define APP_SOURCE_SELECT 0                /* 1: the synthesizer as a second source (its code stays; Pendel is input only) */
#endif

/* LOOP (VOL+ on the Pocket AMP) works the tape from any page: record, close, overdub, play; STOP (VOL-) stops it, or
 * plays it again from the top when stopped, held 2 s it clears */
enum { APP_K_UP = 1, APP_K_DN = 2, APP_K_L = 4, APP_K_R = 8, APP_K_SEL = 16, APP_K_LOOP = 32, APP_K_HOME = 64,
       APP_K_STOP = 128 };

void app_init(uint32_t palette);
void app_audio(const int16_t *in, int16_t *out_lr, unsigned frames);
void app_keys(uint8_t down, uint32_t now_ms);
void app_midi(uint8_t status, uint8_t d1, uint8_t d2);
void app_master_step(int dir);
void app_knob(int32_t detents, uint32_t now_ms);   /* the PARAM knob turned: detents (+ clockwise), as the L / R keys
                                                    * would, faster turns taking bigger steps (Felucca's KNOB ACCEL) */
void app_power(int32_t level, int usb);            /* the battery for the header: level 0..3 (-1 unknown), USB power */         /* the volume buttons: the master level one step up (+1) or down (-1) */
void app_frame(uint32_t now_ms);
const ui_model_t *app_model(void);

#endif
