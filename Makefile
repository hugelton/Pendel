# SPDX-License-Identifier: GPL-3.0-only
# Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
# Host build of the Pendel DSP layer: tests and WAV renders.
#
#   make            build and run the tests
#   make wav        the same, and write the WAVs to build/wav/
#   make FS=48000   tables for another rate
#   make sim        build the browser simulator (sim/pendel.wasm; serve sim/ over http)
#   make ui         render the screens to build/ui/*.png (fonts, palettes, keycaps generated from assets/)

FS ?= 44118
CC ?= cc
BUILD := build
CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion -Werror \
          -fsanitize=address,undefined -fno-sanitize-recover=undefined \
          -Isrc/dsp -I$(BUILD)
DSP := src/dsp/pd_core.c src/dsp/pd_osc.c src/dsp/pd_modules.c src/dsp/pd_effects.c src/dsp/pd_rack.c src/dsp/pd_looper.c

.PHONY: test wav ui sim clean
test: $(BUILD)/test_dsp
	$(BUILD)/test_dsp

wav: $(BUILD)/test_dsp
	mkdir -p $(BUILD)/wav
	$(BUILD)/test_dsp $(BUILD)/wav

$(BUILD)/pd_tables.h: tools/gen_tables.py Makefile
	mkdir -p $(BUILD)
	python3 tools/gen_tables.py --fs $(FS) $@

$(BUILD)/test_dsp: $(DSP) tests/test_dsp.c host/wav.c $(wildcard src/dsp/*.h) host/wav.h $(BUILD)/pd_tables.h
	$(CC) $(CFLAGS) -o $@ $(DSP) tests/test_dsp.c host/wav.c -lm

GEN := $(BUILD)/gen
UI_HDR := $(GEN)/ui_fonts.h $(GEN)/ui_palettes.h $(GEN)/ui_keycaps.h $(GEN)/ui_icons.h
UI_CFLAGS := -std=c11 -O2 -g -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined \
             -fno-sanitize-recover=undefined -DUI_POOL= -DAPP_POOL= -Isrc/ui -Isrc/app -Isrc/dsp -I$(BUILD) -I$(GEN)

$(GEN)/ui_fonts.h: tools/gen_aa_font.py tools/aa_raster.py assets/fonts/InterTight[wght].ttf
	mkdir -p $(GEN)
	python3 tools/gen_aa_font.py $@ --preset inter-tight
$(GEN)/ui_palettes.h: tools/gen_ui_palettes.py
	mkdir -p $(GEN)
	python3 tools/gen_ui_palettes.py $@
$(GEN)/ui_icons.h: tools/gen_icons.py tools/aa_raster.py assets/fonts/fukiai.ttf
	mkdir -p $(GEN)
	python3 tools/gen_icons.py $@
$(GEN)/ui_keycaps.h: tools/gen_aa_keycaps.py tools/aa_raster.py
	mkdir -p $(GEN)
	python3 tools/gen_aa_keycaps.py $@

APP := src/app/app.c src/ui/ui.c
$(BUILD)/ui_render: $(APP) $(DSP) src/ui/gfx.c src/ui/ui.h src/app/app.h tests/ui_render.c $(UI_HDR) $(BUILD)/pd_tables.h
	$(CC) $(UI_CFLAGS) -DUI_LINT -o $@ $(APP) $(DSP) tests/ui_render.c

ui: $(BUILD)/ui_render
	rm -rf $(BUILD)/ui && mkdir -p $(BUILD)/ui
	$(BUILD)/ui_render $(BUILD)/ui
	python3 tools/ppm2png.py $(BUILD)/ui

SIM_EXPORTS := _sim_init,_sim_in,_sim_out,_sim_screen,_sim_audio,_sim_keys,_sim_volume,_sim_midi,_sim_frame
sim/pendel.wasm: $(APP) $(DSP) src/ui/gfx.c src/ui/ui.h src/app/app.h sim/sim_api.c $(UI_HDR) $(BUILD)/pd_tables.h
	emcc -std=c11 -O2 -Wall -Wextra -Wno-unused-function -DUI_POOL= -DAPP_POOL= \
	     -Isrc/ui -Isrc/app -Isrc/dsp -I$(BUILD) -I$(GEN) \
	     --no-entry -sSTANDALONE_WASM -sEXPORTED_FUNCTIONS=$(SIM_EXPORTS) -sINITIAL_MEMORY=4194304 \
	     -sALLOW_MEMORY_GROWTH=0 -sSTACK_SIZE=131072 -o $@ $(APP) $(DSP) sim/sim_api.c

sim: sim/pendel.wasm

clean:
	rm -rf $(BUILD)
