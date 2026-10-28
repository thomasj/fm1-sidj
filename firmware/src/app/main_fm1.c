/* SPDX-License-Identifier: GPL-3.0-only
 * Boot, audio ISR and main loop adapted from Felucca's main.c / audio.c, Copyright (C) 2026
 * Leo Kuroshita (@kurogedelic), Hugelton Instruments, by way of X0X's main_fm1.c
 * (charlesvestal/fm1-x0x). */
/* SiDJ on the FM-1: boot (WDT first, boot-loop guard, guards), LCD, input (TIMER5 10 kHz),
 * audio (ALNK0), USB; then the main loop: UI at ~60 frames/s, input polled in between. */
extern uint32_t _data_start[], _data_end[], _data_load[], _bss_start[], _bss_end[];
extern uint32_t _pool_start[], _pool_end[], _rt_start[], _rt_end[], _rt_load[];

/* --------------------------------------------------------------- audio --- */
#define HALF_FRAMES 512u                    /* I2S half buffer: 11.6 ms at 44.1 kHz (X0X's): a heavy */
#define HALF_WORDS (HALF_FRAMES * 2u)       /* moment is spread over twice the time (256 dropped out) */
#define REND_FRAMES 256u                    /* rendered in two blocks (USB audio's UA_HALF) */
static int32_t abuf[2u * HALF_WORDS] __attribute__((aligned(4)));

#define DBG_MAGIC 0x44424731u               /* "DBG1" */
struct dbg_s {
    uint32_t magic, halves, max_us, in_audio, late, timer_irqs, ui_frames;
    uint32_t last_us, cpu_q8, boots;
    uint32_t crash_seen;                    /* the crash count already reported at a boot */
} dbg __attribute__((section(".noinit")));
static uint8_t safe_mode;                   /* two failed boots: no audio, USB on (safe_main) */

#define UNITY_Q12 8192u                     /* eng_render's gain for the chip's 16 bits as 24: USB audio
                                             * gets the chip's own level, MASTER only the jack */

static uint32_t last_q8;                    /* the last render's load, 256 = the whole budget */
#if FELUCCA_UAC_STEMS
static int16_t stem_buf[REND_FRAMES * 3u];  /* the voices, for USB audio's four channels */
#endif

/* the load as last_q8 counts it: a half's render time, 256 = the whole half */
#define LOAD_Q8(pct) (((pct) * 256u + 50u) / 100u)
#define LOAD_BUSY_ON LOAD_Q8(85)            /* the combined-waveform averaging steps aside above */
#define LOAD_BUSY_OFF LOAD_Q8(75)           /* ... and comes back below */
#define LOAD_STEMS_MAX LOAD_Q8(90)          /* the USB voice channels sit out a half after one above */

static void render_block(int32_t *o)        /* ALNK0: one half, measured */
{
    uint32_t t0 = fm1_ticks(), us, budget = HALF_FRAMES * 1000000u / ENG_RATE, i, b;
    int32_t m = (int32_t)master_q12;
    dbg.in_audio = 1;
#if FELUCCA_UAC
    uac_render_start();
#endif
#if FELUCCA_UAC_STEMS
    /* the voices only while the computer records four channels: not for an RSID (its own
     * interrupts, often samples: too heavy, as for ASID and GLO), and not right after a half
     * near the budget's end (the stems sit out a half; the mix and the jack never drop) */
    eng_stem_out = uac_stems() && !eng_deck_off && last_q8 < LOAD_STEMS_MAX ? stem_buf : 0;
#endif
    for (b = 0; b < HALF_FRAMES; b += REND_FRAMES) {
        int32_t *ob = o + 2u * b;
        eng_render(ob, (int)REND_FRAMES, UNITY_Q12);
#if FELUCCA_UAC_STEMS
        uac_tap(ob, eng_stem_out, REND_FRAMES);   /* the computer records before MASTER */
#elif FELUCCA_UAC
        uac_tap(ob, 0, REND_FRAMES);
#endif
    }
    for (i = 0; i < HALF_WORDS; i++)        /* the jack: MASTER, as eng_render applied it before */
        o[i] = ((o[i] >> 5) * m) >> 8;
    us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
    {
        uint32_t q = (us * 256u) / budget;
        last_q8 = q;
        if (q > LOAD_BUSY_ON) eng_busy = 1; /* over 85 %: the averaging steps aside ... */
        else if (q < LOAD_BUSY_OFF) eng_busy = 0;   /* ... until under 75 % */
        dbg.cpu_q8 = (dbg.cpu_q8 * 15u + q) / 16u;
        audio_sum_q8 += q;
        audio_n++;
        if (q > audio_peak_q8) audio_peak_q8 = q;
    }
    audio_cpu_pct = (dbg.cpu_q8 * 100u) >> 8;
    dbg.halves++;
    dbg.last_us = us;
    if (us > dbg.max_us)
        dbg.max_us = us;
    dbg.in_audio = 0;
}

void fm1_alnk0_irq(void)                    /* via isr_alnk0 (hal/fm1_isr.S) */
{
    uint8_t p = fm1_audio_pending();
    fm1_audio_ack_aux(p);
    if (p & FM1_AUDIO_HALF) {
        uint32_t half = fm1_audio_free_half();
        render_block(&abuf[half * HALF_WORDS]);
        fm1_audio_ack_half();
        if (fm1_audio_free_half() != half) {
            dbg.late++;                     /* the DMA moved on while we rendered */
            if (eng_playing)                /* a dropout only while a song sounds: stopped, the render */
                audio_xruns++;              /* is silence and so is the half it was late for (a run of
                                             * flash erases, the song faded out first, can do this) */
        }
    }
}
extern void isr_alnk0(void);

static void audio_init(void)
{
    uint32_t i;
    for (i = 0; i < 2u * HALF_WORDS; i++)
        abuf[i] = 0;
    fm1_audio_init(abuf, HALF_WORDS, isr_alnk0, 3);
}

static void audio_silence(void)             /* IRQs off (flash erase): the DMA would loop stale audio */
{
    uint32_t i;
    for (i = 0; i < sizeof abuf / sizeof abuf[0]; i++)
        abuf[i] = 0;
}

/* ------------------------------------------------------------ timer --- */
static uint8_t usb_due;
void fm1_timer5_irq(void)
{
    static uint32_t sub;
    fm1_timer5_ack();
    dbg.timer_irqs++;
    fm1_input_tick();
    /* 2 kHz: all USB SIE traffic lives here. This ISR nests into the audio render (so the key
     * matrix keeps its rhythm); USB shares the MIDI queues with the main loop, and a poll that
     * falls inside the render waits for the first tick outside it. */
    if (sub % 5u == 0u)
        usb_due = 1;
    if (usb_due && !dbg.in_audio) {
        usb_due = 0;
        usb_poll();
#if SIDJ_TRS
        uart_midi_poll();                   /* the TRS jack: same context, same queue */
#endif
    }
#if FELUCCA_UAC
    else if (dbg.in_audio && sub % 5u == 0u)
        uac_service();                      /* USB audio: a packet each 1 ms frame, renders or not (a
                                             * render takes 3 to 5 ms); only EP4, and usb_poll never
                                             * runs meanwhile, so INDEX is not shared */
#endif
    if (++sub == 10u)
        sub = 0;
    {
        static uint32_t last, acc;
        uint32_t now = fm1_ticks();
        acc += now - last;
        last = now;
        while (acc >= 1000u * FM1_TICKS_PER_US) {
            acc -= 1000u * FM1_TICKS_PER_US;
            fm1_ms++;
        }
    }
}
extern void isr_timer5(void);

/* ------------------------------------------------------------- boot --- */
#define BOOTGUARD_MAGIC 0x42475244u
struct bootguard_s bootguard __attribute__((section(".noinit")));   /* declared in sidj.c (ota_commit) */

static void hexs(char *b, uint32_t v)
{
    uint32_t i;
    for (i = 0; i < 8u; i++)
        b[i] = "0123456789ABCDEF"[(v >> (28u - 4u * i)) & 15u];
    b[8] = 0;
}

static void fm1_fault(const fm1_crash_t *c)
{
    char b[12];
    uint32_t t0;
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, RGB(160, 0, 0));
    draw_text_box(0, 8, 240, &FONT_S, "SiDJ CRASH", C_WHITE, 1);
    hexs(b, c->vec);
    draw_text_box(10, 40, 220, &FONT_S, b, C_WHITE, 0);
    hexs(b, c->pc);
    draw_text_box(10, 60, 220, &FONT_S, b, C_WHITE, 0);
    hexs(b, c->emu);
    draw_text_box(10, 84, 220, &FONT_S, b, C_WHITE, 0);
    hexs(b, c->dbg);
    draw_text_box(10, 102, 220, &FONT_S, b, C_WHITE, 0);
    hexs(b, c->rets);
    draw_text_box(10, 120, 220, &FONT_S, b, C_WHITE, 0);
    t0 = fm1_ticks();
    while ((uint32_t)(fm1_ticks() - t0) < 4000u * 1000u * FM1_TICKS_PER_US)
        ;
    fm1_reboot();
}

static void enter_uboot(const char *why)
{
    fm1_audio_stop();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 110, 240, &FONT_S, why, C_WHITE, 1);
    fm1_delay_ms(20);
    usb_detach();
    fm1_delay_ms(30);
    bootguard.pending = 0;                  /* intentional reset: not a failed boot */
    fm1_enter_uboot();
}

/* SAFE MODE: the firmware crashed (or hung) twice while starting. Nothing that makes sound
 * runs; USB is on, so an installer can reach it. PLAY tries again. The chip's own update mode
 * stays the last resort (fm1_cstart). */
static void safe_main(void)
{
    char b[24];
    uint32_t t0, play;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 18, 240, &FONT_B, "SiDJ SAFE MODE", C_HI, 1);
    draw_text_box(0, 52, 240, &FONT_S, "IT CRASHED TWICE", C_WHITE, 1);
    draw_text_box(0, 72, 240, &FONT_S, "WHILE STARTING.", C_WHITE, 1);
    draw_text_box(0, 102, 240, &FONT_S, "NO SOUND. USB IS ON:", C_WHITE, 1);
    draw_text_box(0, 122, 240, &FONT_S, "REINSTALL FROM THE", C_WHITE, 1);
    draw_text_box(0, 142, 240, &FONT_S, "INSTALLER.", C_WHITE, 1);
    draw_text_box(0, 172, 240, &FONT_S, "PLAY: TRY AGAIN", C_HI, 1);
    if (fm1_crash.magic == FM1_CRASH_MAGIC) {
        b[0] = 'P';
        b[1] = 'C';
        b[2] = ' ';
        hexs(b + 3, fm1_crash.pc);
        draw_text_box(0, 208, 240, &FONT_S, b, C_GRAY, 1);
    }
    fm1_input_init();
    panel_init();
    usb_start();
    fm1_timer5_start(isr_timer5, 1);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    play = 1u << panel.btn[B_PLAY];
    t0 = fm1_ms;
    for (;;) {
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        if (bootguard.pending && fm1_ms - t0 > 10000u)
            bootguard.pending = 0;          /* safe mode itself is up: not another failed boot */
        if ((fm1_in.buttons & play) && fm1_ms - t0 > 500u) {
            bootguard.failed = 0;           /* try again, from scratch */
            bootguard.pending = 0;
            draw_text_box(0, 172, 240, &FONT_S, "STARTING...", C_HI, 1);
            usb_detach();
            fm1_delay_ms(30);
            fm1_reboot();
        }
        ota_service();
        if (usb.ota_req) {
            usb.ota_req = 0;
            if (flash_ok)
                ota_session();              /* an install commits and resets; else back here */
        }
        if (usb.uboot_req)
            enter_uboot("UBOOT (USB)");
    }
}

static void fm1_main(void)
{
    int32_t knob = 512 * 16;
    uint32_t last_frame = 0;
    {
        uint32_t f = irq_save();
        fm1_vdc14_raise(7);                 /* M-VAVE's own supplies before anything loads them: */
        fm1_sysvdd_raise(15);               /* 1.60 V, then 1.38 V (fm1_sys.h, from X0X) */
        supply_vdc14 = (uint8_t)fm1_vdc14();
        supply_sysvdd = (uint8_t)fm1_sysvdd();
        flash_ok = FL_FAR(fl_jedec_ram)() == 0x856014u;   /* the expected 1 MiB part, else RAM only */
        irq_restore(f);
        if (flash_ok) {
            fl_plain_window_init();         /* the song slots read as plaintext through XIP */
            ota_boot_cleanup();
        }
    }
    lcd_init();
    palette_set(0);
    lcd_fill(0, 0, 240, 240, C_BLACK);      /* black until the UI's intro starts */
    if (dbg.magic != DBG_MAGIC) {
        memset(&dbg, 0, sizeof dbg);
        dbg.magic = DBG_MAGIC;
    }
    if (safe_mode)
        safe_main();                        /* never returns */
    if (fm1_crash.magic == FM1_CRASH_MAGIC && fm1_crash.count != dbg.crash_seen) {
        char b[16];                         /* a crash since the last boot: say so, a moment */
        dbg.crash_seen = fm1_crash.count;
        b[0] = 'P';
        b[1] = 'C';
        b[2] = ' ';
        hexs(b + 3, fm1_crash.pc);
        draw_text_box(0, 176, 240, &FONT_S, "RESTARTED AFTER A CRASH", RGB(255, 80, 60), 1);
        draw_text_box(0, 196, 240, &FONT_S, b, C_GRAY, 1);
        fm1_wdt_feed();
        fm1_delay_ms(2500);
        fm1_wdt_feed();
    }
    dbg.boots++;
    dbg.max_us = 0;
    fm1_input_init();
    fm1_adc_init();
    panel_init();
    led_pos_init();
    eng_init();
    settings_load();                        /* defaults if nothing is saved */
    eng_usb4 = set.usb4;                    /* the speed cap and ... */
#if FELUCCA_UAC_STEMS
    usb_audio_mode(set.usb4);               /* ... USB audio's format, before usb_start */
#endif
    if (flash_ok)
        songs_scan();                       /* the index of the song pool (songs.c) */
    audio_init();
    usb_start();
#if SIDJ_TRS
    uart_midi_init();                       /* before TIMER5: it read-modify-writes port H too */
#endif
    /* above ALNK0 (3): the key / encoder / LED matrix is scanned one column per tick and must
     * keep its rhythm through a long render */
    fm1_timer5_start(isr_timer5, 4);
    fm1_guard_lock_top();
    fm1_irq_enable_all();
    fm1_delay_ms(30);
    if ((fm1_in.buttons & 3u) == 3u)
        panel_setup();                      /* OCT- + OCT+ held at power-on */
    fm1_delay_ms(300);
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui_init();

    for (;;) {
        fm1_wdt_feed();
        usb_retry(fm1_ms);
        if (fm1_ms > 30000u && bootguard.pending) {   /* a crash or hang in the first 30 s counts */
            bootguard.pending = 0;
            bootguard.failed = 0;
        }
        {
            int32_t a = fm1_adc_read(FM1_ADC_MASTER);
            if (a >= 0) {
                uint32_t k10;
                int32_t dev = a * 16 - knob;
                if ((uint32_t)a < adc_min) adc_min = (uint32_t)a;   /* (plat_master_stats) */
                if ((uint32_t)a > adc_max) adc_max = (uint32_t)a;
                if (dev > 64 * 16 || dev < -64 * 16) adc_jumps++;
                adc_reads++;
                knob += (a * 16 - knob) / 8;
                k10 = (uint32_t)(knob / 16);
                master_q12 = (k10 * k10) >> 8;         /* 0 .. ~4096 */
            }
        }
        {   /* OCT- + OCT+ held 5 s: update mode (Felucca's, unchanged) */
            static uint32_t t0, shown;
            uint32_t both = (1u << panel.btn[B_OCTDN]) | (1u << panel.btn[B_OCTUP]);
            if ((fm1_in.buttons & both) != both) {
                if (shown)
                    ui_say("UPDATE MODE CANCELLED", 0);
                shown = 0;
                t0 = fm1_ms;
            } else if (fm1_ms - t0 > 2000u && fm1_ms - t0 <= 5000u) {
                uint32_t left = (5000u - (fm1_ms - t0) + 999u) / 1000u;
                if (left != shown) {
                    char d[4] = {(char)('0' + left), '.', '.', 0};
                    ui_say("UPDATE MODE IN ", d);
                    shown = left;
                }
            } else if (fm1_ms - t0 > 5000u) {
                enter_uboot("UBOOT");
            }
        }
        eng_poll();                         /* a song load, outside the audio interrupt */
        songs_service();                    /* .sid files over SysEx (tools/fm1_sid.py) */
        if (flash_resume && fm1_ms - flash_quiet_ms > 100u) {   /* paused for a settings save: on again */
            flash_resume = 0;
            eng_post(EV_PLAY, 0, 0, 0);
        }
        ota_service();                      /* M-UPGRADE handshake */
        if (usb.ota_req) {
            usb.ota_req = 0;
            flash_quiet(0);                 /* faded out before the staging's erases */
            if (flash_ok)
                ota_session();              /* returns only if nothing was committed */
            lcd_fill(0, 0, 240, 240, C_BLACK);
            ui_init();
        }
        if (usb.uboot_req)
            enter_uboot("UBOOT (USB)");
        if (fm1_ms - last_frame >= 16u) {   /* ~60 frames/s at most; input in between */
            last_frame = fm1_ms;
            dbg.ui_frames++;
            ui_frame();
        } else {
            ui_input_only();
        }
    }
}

void fm1_cstart(void)
{
    uint32_t *s, *d, p3, src, wdt;
    fm1_time_init();
    fm1_reset_reason();
    p3 = fm1_boot.p3_rst;
    src = fm1_boot.rst_src;
    wdt = fm1_boot.wdt_con;
    fm1_wdt_arm(0x0D);
    if (bootguard.magic != BOOTGUARD_MAGIC) {
        bootguard.magic = BOOTGUARD_MAGIC;
        bootguard.failed = 0;
        bootguard.pending = 0;
    }
    if (bootguard.pending)
        bootguard.failed++;
    bootguard.pending = 1;
    if (bootguard.failed >= 4u) {           /* safe mode failed too: the chip's own update mode */
        bootguard.failed = 0;
        bootguard.pending = 0;
        fm1_enter_uboot();
    }
    p3 |= (bootguard.failed >= 2u) << 8;    /* two failed boots: safe mode (kept past the .bss clear) */
    fm1_irq_init();
    for (d = _bss_start; d < _bss_end; d++)
        *d = 0;
    for (d = _pool_start; d < _pool_end; d++)
        *d = 0;
    for (s = _data_load, d = _data_start; d < _data_end; s++, d++)
        *d = *s;
    for (s = _rt_load, d = _rt_start; d < _rt_end; s++, d++)
        *d = *s;
    fm1_mailbox_clear();
    fm1_guard_enable(FM1_GUARD_STACK | FM1_GUARD_WRITE | FM1_GUARD_BUS | FM1_GUARD_PC);
    safe_mode = (uint8_t)(p3 >> 8);
    fm1_boot.p3_rst = (uint8_t)p3;
    fm1_boot.rst_src = src;
    fm1_boot.wdt_con = (uint8_t)wdt;
    fm1_main();
    for (;;)
        ;
}
