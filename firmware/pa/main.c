/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* Pendel on the Pocket AMP: the board (pendel-bsp, boards/pocketamp) wired to the application (src/app).
 *
 *   audio   ALNK0 at 44118 Hz, 64 frames a half: the input's left channel (24-bit, not sign-extended) as mono 16-bit
 *           into app_audio, its stereo 16-bit out scaled by the volume pot and sent as 24-bit (<< 7, as FM-1: -6 dB)
 *   keys    six buttons and the encoder, mapped onto the app's six logical keys (KEYMAP below; provisional)
 *   screen  app_frame about 30 times a second; its strips go to the ST7789 through lcd_blit
 *   USB     MIDI into app_midi; CDC console (p status, u UBOOT, r reboot); the SysEx soft key enters UBOOT
 *   update  the M-UPGRADE entry (pendel-bsp chip/ota.c): a host (tools/pa_install.py, the web installer) sends a
 *           package, its update loader is staged at the top of VM [0xB8000, 0xBD000) and started through the update
 *           record at 0xBCF00; the loader writes the app area. Nothing else here writes the flash
 *
 * Two boots in a row that never reach 2 s drop into UBOOT, and so do a crash and a main loop that stops for 3 s (the
 * timer interrupt watches it): UBOOT can always put an image back. */
#include <stdint.h>
#define FELUCCA_CDC 1
#define FELUCCA_OTA 1                 /* the update entry (ota.c); it writes only its staging area */
#ifndef PENDEL_PKG
#define PENDEL_PKG 900                /* the package identity's number: a release is 9XY (X.Y), as Felucca's FM-1_9XY */
#endif
#define PD_STR2(x) #x
#define PD_STR(x) PD_STR2(x)
#define FELUCCA_ID "Pocket AMP_" PD_STR(PENDEL_PKG)   /* the model hosts check, and the package marker string */
#define FELUCCA_USB_MANUFACTURER "Hugelton"
#define FELUCCA_USB_PRODUCT "Pendel"
#define FELUCCA_USB_PID 0x000A        /* pid.codes test range (1209:0001..000F, not for products): Felucca has 0001 and
                                       * 0002, and macOS keys MIDI names on VID:PID. Pendel's own PID before a release */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_timer.h"
#include "pa_lcd_hw.h"
#include "pa_lcd_init.h"
#include "pa_input.h"
#include "pa_audio.h"
#include "fm1_adc.h"
#include "libc.c"
#include "usb.c"
/* the update entry: the loader staged at the top of VM (empty under the original firmware too), its record 256 bytes
 * below the area's end, where the SPL looks. The RAM flash driver may touch this area only */
#define OTA_AREA 0x000B8000u
#define OTA_AREA_LEN 0x5000u
#define OTA_LOADER_ID "ota-Pocket AMP_105"   /* the identity a staged M-VAVE loader answers (the record names it) */
#define OTA_LOADER_MARK "PENDEL-LOADER-01"
#define OTA_BOARD "pocketamp"
#define FL_RANGE_OK(off, n) FL_IN(off, n, OTA_AREA, OTA_AREA + OTA_AREA_LEN)
#include "fm1_flash.h"
#include "fm1_flash_ext.h"
#include "ota.c"

#include "app.h"

#ifndef PENDEL_PA_VOL
#define PENDEL_PA_VOL 1               /* read the volume pot (PA10, SARADC ch2) every 20 ms */
#endif
#ifndef PENDEL_PA_FRAME
#define PENDEL_PA_FRAME 1             /* draw the app's frames */
#endif

extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _rt_start[], _rt_end[], _rt_load[], _pool_start[], _pool_end[];

static volatile uint32_t ms, loop_beat;
static uint32_t boots_ok;
static uint32_t flash_jedec, flash_ok; /* the flash part's JEDEC id; 1 MiB: the update entry may stage */
static struct { uint32_t magic, pending, failed; } guard __attribute__((section(".noinit")));

/* The boot trace: how far each boot got and why it left for UBOOT, kept across resets in RAM below the stacks (app.ld
 * TRACE), and the previous boot's shown on the console ('p': prev_*). Finding the cold start's detour into UBOOT. */
enum { PH_CSTART = 1, PH_CLEARED, PH_APP, PH_LCD, PH_FRAME, PH_USB, PH_AUDIO, PH_LOOP, PH_HEALTHY };
enum { WHY_NONE, WHY_GUARD, WHY_FAULT, WHY_STALL, WHY_SOFTKEY, WHY_CONSOLE };
#define TRACE_MAGIC 0x50545231u       /* "PTR1" */
typedef struct {
    uint32_t magic, boots, phase, why, why_ms;
    uint32_t p3_rst, rst_src, wdt_con;
    uint32_t clk_sys, clk_mux, pll1_con0, pll1_con1, t4_con;
    uint32_t loops_per_ms;            /* a busy loop's count in 1 ms of TIMER4: the CPU's speed */
    uint32_t guard_in;                /* the boot guard as this boot found it: failed << 8 | pending */
} trace_t;
static volatile trace_t trace __attribute__((section(".trace")));
static trace_t prev_trace;            /* the boot before this one, as it left */
static void trace_why(uint32_t why);

/* ---- keys, left to right (pa_input.h bits): 0 PH6 the parameter encoder's push, 1 PH8 HOME, 2 PH9 VOL-, 3 PA0 VOL+,
 * 4 PA1 LEFT, 5 PA3 RIGHT. The push selects, HOME goes home, LEFT / RIGHT move the focus, the encoder changes the
 * value (its detents go straight to app_knob), VOL- / VOL+ step the master level (repeating while held). ---- */
enum { PK_PUSH, PK_HOME, PK_VDN, PK_VUP, PK_LEFT, PK_RIGHT };
static const uint8_t KEYMAP[PA_KEYS] = { APP_K_SEL, APP_K_HOME, 0, 0, APP_K_UP, APP_K_DN };
#define SPLASH_MS 700u                /* the power-on screen stays this long */
/* the battery's thresholds on PC10 (SARADC ch9, 0..1023): a 1/2 divider on a 3.3 V reference is assumed, so 3.55 /
 * 3.75 / 3.95 V. PROVISIONAL: PC10 reads 617 on USB power (4.0 V); to check against a discharging unit */
#define BATT_1 550u
#define BATT_2 581u
#define BATT_3 612u
static uint32_t batt_raw;
#define VOL_REPEAT_MS 120u
static volatile int32_t enc_steps;
#define ENC_LOG 256u                  /* the encoder's last line changes, for the console's 'e': state << 14 | 100 us since the last */
static volatile uint16_t enc_log[ENC_LOG];
static volatile uint32_t enc_log_n, isr_gap_max;

/* ---- audio ---- */
#define HALF_FRAMES 64u
#define HALF_WORDS (HALF_FRAMES * 2u)
static int32_t abuf_out[2u * HALF_WORDS] __attribute__((aligned(4)));
static int32_t abuf_in[2u * HALF_WORDS] __attribute__((aligned(4)));
static int16_t in16[HALF_FRAMES], out16[2u * HALF_FRAMES];
static volatile uint32_t a_halves, a_us_max, a_us_last;
static volatile int32_t vol_q15;      /* the volume pot, 0..32767 */
static uint32_t f_us_last, f_us_max, f_count, midi_count;
static uint32_t blit_us, blit_n, f_blit_us, f_blit_n;   /* the LCD's share of a frame: time and strips sent */
static uint32_t boot_t0, boot_us[6];
static uint32_t boot_t4_con, boot_t4_cnt;   /* TIMER4 as the boot loader left it: running since it started? */  /* boot phases, us from fm1_cstart: app_init, LCD, 1st frame, USB, audio, loop */
static uint32_t inj_key, inj_hold, cur_inj;    /* a key pressed from the console: held for inj_hold ms (8 2 4 6 5 0, + long) */
extern void isr_alnk0(void);

void fm1_alnk0_irq(void)
{
    uint8_t p = pa_audio_pending();
    if (p & PA_AUDIO_IN)
        pa_audio_ack_in();
    if (p & PA_AUDIO_OUT) {
        uint32_t t0 = fm1_ticks(), i, us;
        int32_t *o = &abuf_out[pa_audio_free_half() * HALF_WORDS];
        const int32_t *in = &abuf_in[pa_audio_in_half() * HALF_WORDS];
        int32_t v = vol_q15;
        for (i = 0; i < HALF_FRAMES; i++)
            in16[i] = (int16_t)(((int32_t)((uint32_t)in[2u * i] << 8) >> 8) >> 8);
        app_audio(in16, out16, HALF_FRAMES);
        for (i = 0; i < 2u * HALF_FRAMES; i++)
            o[i] = ((out16[i] * v) >> 15) << 7;
        pa_audio_ack_out();
        a_halves++;
        us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
        a_us_last = us;
        if (us > a_us_max)
            a_us_max = us;
    }
}

void fm1_timer5_irq(void)             /* 10 kHz: ms, USB at 2 kHz, buttons every 10 ms, the encoder at 1 kHz */
{
    static uint32_t sub, last, acc, k, ab = 3u, dir, deep;
    uint32_t t = fm1_ticks();
    fm1_timer5_ack();
    static uint32_t since;
    if (last && t - last > isr_gap_max)
        isr_gap_max = t - last;
    acc += t - last;
    last = t;
    if (since < 0x3FFFu)
        since++;
    while (acc >= 1000u * FM1_TICKS_PER_US) {
        acc -= 1000u * FM1_TICKS_PER_US;
        ms++;
    }
    if (++sub == 5u) {
        sub = 0;
        usb_poll();
    }
    if (++k == 100u) {
        k = 0;
        pa_input_poll();
    }
    if (ms - loop_beat > 3000u) {     /* the main loop stopped: into UBOOT, where an image can be put back */
        trace_why(WHY_STALL);
        pa_audio_stop();
        usb_detach();
        fm1_enter_uboot();
    }
    {                                 /* the encoder, 10 kHz: a detent per trip out of rest (both lines high) and back */
        uint32_t n = FM1_PR(FM1_PC, FM1_IN) & 3u;     /* bit 0 PC0, bit 1 PC1 */
        if (n != ab) {
            enc_log[enc_log_n++ % ENC_LOG] = (uint16_t)(n << 14 | since);
            since = 0;
            if (ab == 3u)                 /* leaving rest: PC0 falls first clockwise (10), PC1 first anticlockwise (01) */
                dir = n == 2u ? 1u : n == 1u ? 2u : 0u;
            if (n == 0u)
                deep = 1;                 /* both low: past the middle of the detent */
            if (n == 3u) {                /* back at rest: one detent, if it went through (on this unit anticlockwise
                                           * comes back the way it went, 11 01 00 01 11: no quadrature count works) */
                if (deep && dir)
                    enc_steps += dir == 1u ? 1 : -1;
                dir = 0;
                deep = 0;
            }
            ab = n;
        }
    }
}
extern void isr_timer5(void);

/* ---- screen ---- */
static void lcd_cmd(uint8_t c, const uint8_t *d, uint32_t n)
{
    static uint8_t buf[16];
    uint32_t i;
    pa_lcd_send_cmd(c);
    pa_lcd_wait();
    pa_lcd_deselect();
    if (!n)
        return;
    for (i = 0; i < n && i < sizeof buf; i++)
        buf[i] = d[i];
    pa_lcd_send_data(buf, n);
    pa_lcd_wait();
    pa_lcd_deselect();
}

static void lcd_init(void)
{
    uint32_t i;
    pa_lcd_hw_init();
    fm1_delay_ms(50);
    lcd_cmd(0x01, 0, 0);              /* SWRESET */
    fm1_delay_ms(150);
    for (i = 0; i < sizeof PA_LCD_INIT / sizeof PA_LCD_INIT[0]; i++) {
        if (PA_LCD_INIT[i].cmd == PA_LCD_INIT_DELAY)
            fm1_delay_ms(PA_LCD_INIT[i].len);
        else
            lcd_cmd(PA_LCD_INIT[i].cmd, PA_LCD_INIT[i].data, PA_LCD_INIT[i].len);
    }
}

/* the app's strips (RGB565, already byte-swapped for the panel) */
void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t t0 = fm1_ticks();
    uint8_t a[4];
    a[0] = 0; a[1] = (uint8_t)x; a[2] = 0; a[3] = (uint8_t)(x + w - 1u);
    lcd_cmd(0x2A, a, 4);
    a[1] = (uint8_t)y; a[3] = (uint8_t)(y + h - 1u);
    lcd_cmd(0x2B, a, 4);
    lcd_cmd(0x2C, 0, 0);
    pa_lcd_send_data(px, w * h * 2u);
    pa_lcd_wait();
    pa_lcd_deselect();
    blit_us += (fm1_ticks() - t0) / FM1_TICKS_PER_US;
    blit_n++;
}
void lcd_sync(void) {}

static void fm1_fault(const fm1_crash_t *c)   /* a crash: into UBOOT (the record stays in .noinit) */
{
    (void)c;
    trace_why(WHY_FAULT);
    pa_audio_stop();
    usb_detach();
    fm1_enter_uboot();
}

/* ---- serial ---- */
static void put(char c)
{
    if (!cdc.dtr || co_w - co_r >= CO_N)
        return;
    cdc_out[co_w % CO_N] = (uint8_t)c;
    RING_PUBLISH();
    co_w++;
}
static void puts_(const char *s) { while (*s) put(*s++); }
static void putd(const char *k, uint32_t v)
{
    char b[10];
    uint32_t n = 0;
    puts_(k);
    do { b[n++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (n)
        put(b[--n]);
    put(' ');
}

static void status(void)
{
    putd("ms=", ms);
    putd("halves=", a_halves);
    putd("audio_us=", a_us_last);
    putd("audio_us_max=", a_us_max);
    putd("budget_us=", HALF_FRAMES * 1000000u / 44118u);
    putd("frame_us=", f_us_last);
    putd("frame_us_max=", f_us_max);
    putd("frames=", f_count);
    putd("blit_us=", f_blit_us);
    putd("strips=", f_blit_n);
    {
        uint32_t i;
        putd("prev_boots=", prev_trace.boots);
        putd("prev_phase=", prev_trace.phase);
        putd("prev_why=", prev_trace.why);
        putd("prev_why_ms=", prev_trace.why_ms);
        putd("prev_p3_rst=", prev_trace.p3_rst);
        putd("prev_rst_src=", prev_trace.rst_src);
        putd("prev_clk_sys=", prev_trace.clk_sys);
        putd("prev_pll1=", prev_trace.pll1_con0);
        putd("prev_loops_ms=", prev_trace.loops_per_ms);
        putd("prev_guard_in=", prev_trace.guard_in);
        putd("guard_in=", trace.guard_in);
        putd("p3_rst=", trace.p3_rst);
        putd("rst_src=", trace.rst_src);
        putd("clk_sys=", trace.clk_sys);
        putd("pll1=", trace.pll1_con0);
        putd("loops_ms=", trace.loops_per_ms);
        putd("t4_con=", boot_t4_con);
        putd("t4_at_start_us=", boot_t4_cnt / FM1_TICKS_PER_US);
        puts_("boot_us=");
        for (i = 0; i < 6u; i++) {
            putd("", boot_us[i]);
        }
    }
    putd("midi=", midi_count);
    putd("flash=", flash_jedec);
    putd("vol=", (uint32_t)vol_q15);
    putd("batt_raw=", batt_raw);
    putd("keys=", pa_in.down);
    putd("enc=", (uint32_t)enc_steps);
    putd("pc_in=", FM1_PR(FM1_PC, FM1_IN) & 0xFFFFu);  /* raw pins: finding the encoder's lines */
    putd("pa_in=", FM1_PR(FM1_PA, FM1_IN) & 0xFFFFu);
    putd("ph_in=", FM1_PR(FM1_PH, FM1_IN) & 0xFFFFu);
    putd("lcd_timeouts=", pa_lcd_timeouts);
    {
        const ui_model_t *m = app_model();
        putd("page=", m->page);
        putd("focus=", m->focus);
        putd("source=", m->source);
        putd("in=", m->in_level);
        putd("out=", m->out_level);
        putd("cpu=", m->cpu_pct);
        if (m->title) { puts_("title="); puts_(m->title); put(' '); }
    }
    puts_("\r\n");
}

static void trace_why(uint32_t why)
{
    if (trace.why == WHY_NONE) {
        trace.why = why;
        trace.why_ms = ms;
    }
}

static void leave(int uboot)
{
    puts_(uboot ? "UBOOT\r\n" : "reboot\r\n");
    fm1_delay_ms(30);
    pa_audio_stop();
    usb_detach();
    if (uboot)
        fm1_enter_uboot();
    fm1_reboot();
}

static void command(char c)
{
    switch (c) {
    case 'p': status(); break;
    case 'u': trace_why(WHY_CONSOLE); leave(1); break;
    case 'r': leave(0); break;
    case 'e': {                       /* the encoder's line changes since the last 'e' (state:100us), and the timer's longest gap */
        uint32_t i, n = enc_log_n, from = n > ENC_LOG ? n - ENC_LOG : 0;
        putd("enc_changes=", n);
        putd("isr_gap_max_us=", isr_gap_max / FM1_TICKS_PER_US);
        puts_("\r\n");
        for (i = from; i < n; i++) {
            uint16_t v = enc_log[i % ENC_LOG];
            put((char)('0' + (v >> 14)));     /* 0..3: bit 0 PC0, bit 1 PC1 */
            put(':');
            putd("", v & 0x3FFFu);
        }
        puts_("\r\n");
        enc_log_n = 0;
        isr_gap_max = 0;
        break;
    }
    case '8': inj_key = APP_K_UP; break;
    case '2': inj_key = APP_K_DN; break;
    case '4': inj_key = APP_K_L; break;
    case '6': inj_key = APP_K_R; break;
    case '5': inj_key = APP_K_SEL; break;
    case '0': inj_key = APP_K_LOOP; break;
    case '7': inj_key = APP_K_HOME; break;
    case '+': inj_hold = 900u; break;   /* the next key is a long press */
    case 'a': {                       /* every SARADC channel once, 0..1023 (9999: no answer) */
        uint32_t ch;
        int32_t v;
        for (ch = 0; ch < 16u; ch++) {
            v = fm1_adc_read(ch);
            put('a'); put("0123456789ABCDEF"[ch]);
            putd("=", v < 0 ? 9999u : (uint32_t)v);
        }
        puts_("\r\n");
        break;
    }
    case 'z': a_us_max = 0; f_us_max = 0; puts_("max cleared\r\n"); break;
    case 'h': puts_("Pendel: p status, a ADC, z clear the maxima, 8 2 4 6 5 0 7: LEFT RIGHT, knob down up, PUSH, tape, HOME (+ long), u UBOOT, r reboot\r\n"); break;
    }
}

static void key_inject(uint32_t now)  /* one console key at a time: down for its hold, then up for 60 ms */
{
    static uint32_t key, until, gap;
    if (!key && inj_key && (int32_t)(now - gap) >= 0) {
        key = inj_key;
        inj_key = 0;
        until = now + (inj_hold ? inj_hold : 60u);
        inj_hold = 0;
    }
    if (key && (int32_t)(now - until) >= 0) {
        key = 0;
        gap = now + 60u;
    }
    cur_inj = key;
}

static void midi_drain(void)
{
    while (mi_r != mi_w) {
        uint32_t pkt = midi_in_q[mi_r % MQ], cin = pkt & 0xFu;
        mi_r++;
        if (cin >= 0x8u && cin <= 0xEu)
            midi_count++, app_midi((uint8_t)(pkt >> 8), (uint8_t)(pkt >> 16) & 0x7Fu, (uint8_t)(pkt >> 24) & 0x7Fu);
    }
    if (midi_in_overflow)
        midi_in_overflow = 0;
}

/* ---- the update entry's hooks (ota.c): its clock, the main loop's heartbeat while it waits on the host, the flash
 * (the staging area only), its progress on the screen, the commit (the record into RAM, a core reset) ---- */
static uint32_t ota_now_ms(void) { return ms; }
static void ota_idle(void)
{
    loop_beat = ms;
    fm1_wdt_feed();
}
static int ota_erase(uint32_t off)
{
    uint32_t took;
    if (!FL_RANGE_OK(off, 0x1000u) || (off & 0xFFFu))
        return -8;
    return fl_erase4k(off, &took);
}
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!FL_RANGE_OK(off, n))
        return -8;
    return fl_write(off, p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n)
{
    uint32_t f = irq_save();
    int rc = FL_FAR(fl_read_ram)(off, p, n);
    irq_restore(f);
    return rc;
}
static void ota_show(uint32_t step, int32_t code)
{
    static const char *const STEP[] = { "", "PACKAGE", "CHECK HEAD", "LOADER", "CONFIRM", "RESTART" };
    char b[16] = "FAILED  -";
    uint32_t k = 9, v = (uint32_t)-code, t;
    if (step < 9u) {
        ui_notice("UPDATE", STEP[step < 6u ? step : 0]);
        return;
    }
    if (v >= 10u)
        b[k++] = (char)('0' + v / 10u % 10u);
    b[k++] = (char)('0' + v % 10u);
    b[k] = 0;
    ui_notice("UPDATE", code == 1 ? "DRY RUN OK" : b);
    for (t = ms; ms - t < 1500u;)
        ota_idle();
}
static void ota_commit(const uint8_t *parm)
{
    guard.pending = 0;                /* an intended reset, not a failed boot */
    pa_audio_stop();
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);           /* the record into RAM, a core reset: the SPL starts the staged loader */
}

static void app_main(void)
{
    uint32_t last_key = 0, last_frame = 0, last_vol = 0, last_batt = 0;
    int32_t enc_seen = 0, batt_iir = 0;
#define BOOT_MARK(i) (boot_us[i] = (fm1_ticks() - boot_t0) / FM1_TICKS_PER_US)
    app_init(0);
    BOOT_MARK(0);
    trace.phase = PH_APP;
    lcd_init();
    BOOT_MARK(1);
    trace.phase = PH_LCD;
    ui_splash(PENDEL_VERSION);        /* Felucca's power-on screen; the app's frames start at SPLASH_MS */
    lcd_cmd(0x29, 0, 0);              /* DISPON after the first picture */
    BOOT_MARK(2);
    trace.phase = PH_FRAME;
    pa_input_init();
    (void)pa_board_inputs();          /* PC0 / PC1 (the encoder), PA10, PC10: inputs */
    FM1_PR(FM1_PC, FM1_PU) |= 3u;     /* the encoder's lines pulled up: without, PC1 came back high tens of ms late and a
                                       * fast turn left it low (only PC0 moving, no detent counted) */
    vol_q15 = 16384;
    {
        uint32_t f = irq_save();
        flash_jedec = FL_FAR(fl_jedec_ram)();
        irq_restore(f);
    }
    flash_ok = fm1_flash_size_from_jedec(flash_jedec) == 0x100000u;
    if (flash_ok)
        ota_boot_cleanup();           /* a staging area an update left behind */
    usb_start();
    BOOT_MARK(3);
    trace.phase = PH_USB;
    pa_audio_init(abuf_out, abuf_in, HALF_WORDS, isr_alnk0, 3);
    BOOT_MARK(4);
    trace.phase = PH_AUDIO;
    loop_beat = ms;
    fm1_timer5_start(isr_timer5, 4);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    BOOT_MARK(5);
    trace.phase = PH_LOOP;
    for (;;) {
        uint32_t now = ms;
        loop_beat = now;
        fm1_wdt_feed();
        usb_retry(now);
        if (usb.uboot_req) {          /* the SysEx soft key */
            trace_why(WHY_SOFTKEY);
            leave(1);
        }
        ota_service();                /* the M-UPGRADE handshake */
        if (usb.ota_req) {            /* the M-UPGRADE upgrade command */
            int32_t v = vol_q15;
            usb.ota_req = 0;
            vol_q15 = 0;              /* silence: an erase stops the interrupts and the DMA repeats its half */
            fm1_delay_ms(5);
            if (flash_ok)
                ota_session();        /* returns only if nothing was committed */
            vol_q15 = v;
            ui_redraw_all();
        }
        if (!boots_ok && now > 2000u) {
            boots_ok = 1;
            trace.phase = PH_HEALTHY;
            guard.pending = 0;
            guard.failed = 0;
        }
        while (ci_r != ci_w) {
            char c = (char)cdc_in[ci_r % CI_N];
            ci_r++;
            command(c);
        }
        midi_drain();
        if (PENDEL_PA_VOL && now - last_vol >= 20u) {  /* PA10: 0..1023 -> 0..32767, a little dead band at both ends */
            int32_t v = fm1_adc_read(2);
            last_vol = now;
            if (v >= 0) {
                v = (v - 8) * 33;
                vol_q15 = v < 0 ? 0 : v > 32767 ? 32767 : v;
            }
        }
        if (now - last_batt >= 1000u) {  /* the battery: PC10 (SARADC ch9), a slow average, about once a second */
            int32_t v = fm1_adc_read(9);
            last_batt = now;
            if (v >= 0) {
                batt_iir = batt_iir ? batt_iir + (v * 16 - batt_iir) / 8 : v * 16;
                batt_raw = (uint32_t)(batt_iir / 16);
                app_power(batt_raw >= BATT_3 ? 3 : batt_raw >= BATT_2 ? 2 : batt_raw >= BATT_1 ? 1 : 0,
                          usb.config && !usb.suspended);
            }
        }
        if (now - last_key >= 10u) {
            uint32_t i, down = 0;
            int32_t e = enc_steps;
            last_key = now;
            for (i = 0; i < PA_KEYS; i++)
                if (pa_in.down & (1u << i))
                    down |= KEYMAP[i];
            if (e != enc_seen) {      /* the knob: its detents straight to the app (no key taps: a fast turn keeps up) */
                app_knob(e - enc_seen, now);
                enc_seen = e;
            }
            key_inject(now);
            {                         /* VOL- / VOL+: a step on the press, then every VOL_REPEAT_MS after 400 ms */
                static uint32_t vt0, vnext;
                uint32_t v = pa_in.down & ((1u << PK_VDN) | (1u << PK_VUP));
                static uint32_t vlast;
                if (v && !vlast) {
                    vt0 = now;
                    vnext = now + 400u;
                    app_master_step(v & (1u << PK_VUP) ? 1 : -1);
                } else if (v && (int32_t)(now - vnext) >= 0) {
                    vnext = now + VOL_REPEAT_MS;
                    app_master_step(v & (1u << PK_VUP) ? 1 : -1);
                }
                (void)vt0;
                vlast = v;
            }
            app_keys((uint8_t)(down | cur_inj), now);
        }
        if (PENDEL_PA_FRAME && now >= SPLASH_MS && now - last_frame >= 33u) {
            uint32_t t0 = fm1_ticks(), us;
            last_frame = now;
            blit_us = blit_n = 0;
            app_frame(now);
            f_blit_us = blit_us;
            f_blit_n = blit_n;
            us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
            f_us_last = us;
            if (us > f_us_max)
                f_us_max = us;
            f_count++;
        }
    }
}

void fm1_cstart(void)
{
    uint32_t *s, *d;
    uint32_t t0, con = FM1_T4_CON, cnt = FM1_T4_CNT, i, n;
    trace_t pt;
    volatile uint32_t spin;
    fm1_reset_reason();               /* first thing: why the chip started */
    fm1_time_init();
    t0 = fm1_ticks();                 /* (kept until .bss is cleared) */
    pt = *(trace_t *)&trace;          /* the previous boot's trace, before this one overwrites it */
    if (trace.magic != TRACE_MAGIC) {
        trace.magic = TRACE_MAGIC;
        trace.boots = 0;
    }
    trace.boots++;
    trace.phase = PH_CSTART;
    trace.why = WHY_NONE;
    trace.why_ms = 0;
    trace.p3_rst = fm1_boot.p3_rst;
    trace.rst_src = fm1_boot.rst_src;
    trace.wdt_con = fm1_boot.wdt_con;
    trace.clk_sys = *(volatile uint32_t *)0x10014u;
    trace.clk_mux = *(volatile uint32_t *)0x1000Cu;
    trace.pll1_con0 = *(volatile uint32_t *)0x119A8u;
    trace.pll1_con1 = *(volatile uint32_t *)0x119ACu;
    trace.t4_con = con;
    n = 0;                            /* the CPU's speed: a counted loop for 1 ms of the crystal's timer */
    i = fm1_ticks();
    while (fm1_ticks() - i < 1000u * FM1_TICKS_PER_US) {
        spin = n;
        n++;
    }
    (void)spin;
    trace.loops_per_ms = n;
    fm1_wdt_arm(0x0D);
    if (guard.magic != 0x504E4431u) { /* "PND1" */
        guard.magic = 0x504E4431u;
        guard.failed = 0;
        guard.pending = 0;
    }
    trace.guard_in = guard.failed << 8 | guard.pending;
    if (fm1_boot.p3_rst & 0x11u) {    /* a power-on or the power key's long press: the user's, not a crash loop. Unlike
                                       * the FM-1 the Pocket AMP has a battery and SRAM outlives its power-off, so the
                                       * count left there sent the next power-on to UBOOT (~45 s) */
        guard.failed = 0;
        guard.pending = 0;
    }
    if (guard.pending)
        guard.failed++;
    guard.pending = 1;
    if (guard.failed >= 2u) {
        guard.failed = 0;
        guard.pending = 0;
        trace_why(WHY_GUARD);
        fm1_enter_uboot();
    }
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (d = _pool_start; d < _pool_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;
    boot_t0 = t0;
    prev_trace = pt;
    trace.phase = PH_CLEARED;
    boot_t4_con = con;
    boot_t4_cnt = cnt;
    fm1_mailbox_clear();              /* as Felucca: a boot-loader request left by the soft key must not outlive this
                                       * boot (SRAM keeps it through a short power-off) */
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    app_main();
    for (;;)
        ;
}
