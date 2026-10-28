/* SPDX-License-Identifier: GPL-3.0-only
 * After X0X's x0x.c (charlesvestal/fm1-x0x, GPL-3.0-only), a fork of
 * Felucca by Leo Kuroshita. */
/* SiDJ firmware for the M-VAVE FM-1: one compilation unit for the platform and the app
 * (Felucca's layout: the HAL is header-only, so everything that touches it is here). The SID
 * engine (sid/), the C64 (c64/) and the engine (app/engine.c, app/remix.c) are separate units
 * compiled at -O2 by tools/build.py; this one is -Os. Order matters. */
#include <stdint.h>
#include "fm1_time.h"
#include "fm1_sys.h"
#include "fm1_irq.h"
#include "fm1_guard.h"
#include "fm1_input.h"
#include "fm1_timer.h"
#include "fm1_audio.h"
#include "fm1_adc.h"
#include "fm1_lcd_hw.h"
#include "fm1_flash.h"
#include "fm1_xip.h"

#include "libc.c"
#include "lcd.c"
#include "gfx.c"

static volatile uint32_t fm1_ms;            /* milliseconds since boot (TIMER4-based, TIMER5 ISR) */
#define RING_PUBLISH() __asm__ volatile("" ::: "memory")

#include "app/plat.h"
#include "app/engine.h"

#define FELUCCA_OTA 1                       /* M-UPGRADE update entry: keeps the installers working */
#ifndef SIDJ_CDC
#define SIDJ_CDC 0                        /* USB serial console off: one plain MIDI interface */
#endif
#define FELUCCA_CDC SIDJ_CDC
#ifndef SIDJ_UAC
#define SIDJ_UAC 1                          /* USB audio: the mix as a stereo input on the computer */
#endif
#define FELUCCA_UAC SIDJ_UAC
#define FELUCCA_UAC_STEMS SIDJ_UAC              /* and its four-channel format: the voices and the mix */
#ifndef FELUCCA_ID
#define FELUCCA_ID "FM-1_64000000"          /* package identity (tools/build.py) */
#endif
#include "usb.c"
#ifndef SIDJ_TRS
#define SIDJ_TRS 1                          /* MIDI IN on the TRS jack: a keyboard plays the voices */
#endif
#if SIDJ_TRS
#include "midi_uart.c"
#endif

static uint8_t flash_ok;                    /* JEDEC id matched at boot */
static void audio_silence(void);

/* Before a flash erase. The chip runs no code from flash while it erases, so the audio stops for
 * it (audio_silence: the DMA loops silence). A playing song fades out first and pauses, so the
 * stop is no click and no late render; resume 1: it plays on once the writing is done (the main
 * loop, flash_resume, 100 ms after the last erase), 0: it stays stopped (a song store change, an
 * update). The fade takes ~10 ms after the next half buffer; at most 200 ms is waited. */
static uint8_t flash_resume;
static uint32_t flash_quiet_ms;
static void flash_quiet(int resume)
{
    uint32_t t0 = fm1_ms;
    flash_quiet_ms = fm1_ms;
    if (!resume) flash_resume = 0;
    if (!eng_playing) return;
    eng_post(EV_STOP, 0, 0, 0);
    if (resume) flash_resume = 1;
    while (eng_playing && fm1_ms - t0 < 200u)
        fm1_wdt_feed();
}
static int st_read(uint32_t off, void *dst, uint32_t n)   /* 256-byte IRQ-off windows: audio keeps up */
{
    uint8_t *d = dst;
    while (n) {
        uint32_t k = n > 256u ? 256u : n, f = irq_save();
        int rc = FL_FAR(fl_read_ram)(off, d, k);
        irq_restore(f);
        if (rc)
            return rc;
        off += k;
        d += k;
        n -= k;
    }
    return 0;
}
static int st_erase(uint32_t off)
{
    uint32_t took;
    if (!FL_STORE_OK(off, 0x1000u))
        return -8;
    flash_quiet(1);
    audio_silence();
    return fl_erase4k(off, &took);
}
static int st_prog(uint32_t off, const void *src, uint32_t n)
{
    if (!FL_STORE_OK(off, n))
        return -8;
    return fl_write(off, src, n);
}
#include "storage.c"

struct bootguard_s { uint32_t magic, failed, pending; };
extern struct bootguard_s bootguard;
#include "ota.c"
static uint32_t ota_now_ms(void) { return fm1_ms; }
static void ota_idle(void) { fm1_wdt_feed(); }
static int ota_in_area(uint32_t off, uint32_t n) { return FL_IN(off, n, OTA_AREA, OTA_AREA + OTA_AREA_LEN); }
static int ota_erase(uint32_t off)
{
    uint32_t took;
    if (!ota_in_area(off, 0x1000u) || (off & 0xFFFu))
        return -8;
    flash_quiet(0);
    audio_silence();
    return fl_erase4k(off, &took);
}
static int ota_prog(uint32_t off, const void *p, uint32_t n)
{
    if (!ota_in_area(off, n))
        return -8;
    return fl_write(off, p, n);
}
static int ota_fread(uint32_t off, void *p, uint32_t n) { return st_read(off, p, n); }
static void ota_show(uint32_t step, int32_t code)
{
    static const char *const STEP[] = {"", "PACKAGE", "CHECK HEAD", "LOADER", "CONFIRM", "RESTART"};
    lcd_fill(0, 0, 240, 240, C_BLACK);
    draw_text_box(0, 92, 240, &FONT_S, "UPDATE", C_WHITE, 1);
    if (step < 9u) {
        draw_text_box(0, 124, 240, &FONT_S, STEP[step < 6u ? step : 0], C_HI, 1);
        return;
    }
    draw_text_box(0, 124, 240, &FONT_S, code == 1 ? "DRY RUN OK" : "FAILED", C_HI, 1);
    fm1_delay_ms(1500);
}
static void ota_commit(const uint8_t *parm)
{
    bootguard.pending = 0;                  /* intentional reset */
    bootguard.failed = 0;                   /* a new install starts with a clean slate */
    usb_detach();
    fm1_delay_ms(30);
    fm1_enter_update(parm);
}

/* the song pool over the flash driver, read in place through the plain XIP window (songs.c) */
static const uint8_t *song_ptr(uint32_t off) { return fm1_xip_ptr(off); }
static int song_blank(uint32_t off, uint32_t n)
{
    const uint8_t *p = fm1_xip_ptr(off);
    uint32_t i;
    if (!flash_ok) return 0;
    fl_inval(off, n);
    for (i = 0; i < n; i++)
        if (p[i] != 0xFFu) return 0;
    return 1;
}
static int song_erase_sector(uint32_t off)
{
    uint32_t took;
    if (!flash_ok || !FL_STORE_OK(off, 0x1000u)) return -1;
    flash_quiet(1);
    audio_silence();
    if (fl_erase4k(off, &took)) return -1;
    fl_inval(off, 0x1000u);
    return 0;
}
static int song_prog(uint32_t off, const uint8_t *src, uint32_t n)
{
    if (!flash_ok || !FL_STORE_OK(off, n)) return -1;
    if (fl_write(off, src, n)) return -1;
    fl_inval(off, n);
    return fl_verify_xip(off, src, n) ? -2 : 0;
}
static void songs_changed(void);            /* the song store changed over USB (songs.c -> the UI) */
static int songs_play_request(int k, int subsong);
#include "app/settings.c"
#include "app/songs.c"
#include "app/panel.c"
#include "app/plat_fm1.c"
#include "app/ui.c"
#include "app/main_fm1.c"
