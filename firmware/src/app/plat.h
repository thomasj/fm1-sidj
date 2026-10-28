/* SPDX-License-Identifier: GPL-3.0-only
 * After X0X's plat.h (charlesvestal/fm1-x0x, GPL-3.0-only), a Felucca fork. */
/* SiDJ platform interface: everything the app needs from the hardware.
 *
 * The device implementation (plat_fm1.c) wraps Felucca's HAL and panel map; the host
 * implementation (host/sidj_host.c) is a simulator fed by a script, which is what lets the
 * whole app run and be tested on a Mac. Drawing goes through lcd_fill / lcd_blit (lcd.c on
 * the device, a framebuffer on the host) and gfx.c on top. */
#pragma once
#include <stdint.h>

#ifdef SIDJ_HOST
#define SIDJ_POOL
#else
#define SIDJ_POOL __attribute__((section(".pool")))   /* big zeroed buffers: the .pool RAM region */
#endif

/* panel controls, by printed label (panel.c maps these to matrix ids) */
enum { B_FX, B_SEL, B_ENV, B_LFO, B_EDIT, B_GLO, B_HOME, B_SAVE, B_ARP, B_SEQ, B_PLAY, B_REC,
       B_OCTDN, B_OCTUP, NB };
enum { EN_SELECT, EN_ALGO, EN_PRESET, EN_K1, EN_K2, EN_K3, EN_K4, NE };

#define NKEYS 27                            /* F3 .. G5 */

uint32_t plat_ms(void);
uint32_t plat_buttons(void);                /* held: bit B_* */
uint32_t plat_keys(void);                   /* held: bit k = key k (0 = F3) */
int32_t plat_enc(int role);                 /* detents since the last call, + = clockwise; EN_* */
uint32_t plat_master(void);                 /* MASTER pot, 0..4096 gain Q12 */
void plat_leds(uint32_t buttons, uint32_t keys);   /* lit: bit B_*, bit key */

/* USB / TRS MIDI in, as 4-byte USB-MIDI event packets (cable 0); out to USB */
int plat_midi_in(uint32_t *pkt);            /* 1 = got one */
void plat_midi_out(uint32_t pkt);           /* from the audio context too */
int plat_midi_out_free(void);               /* packets the out ring takes now (0 without a host) */

/* persistent storage (Felucca's storage.c objects: A/B sectors, CRC, torn-write safe) */
int plat_store_load(uint32_t obj, void *dst, uint32_t max);    /* bytes loaded, < 0 = none / bad */
int plat_store_save(uint32_t obj, const void *src, uint32_t len);   /* 0 = ok */
#define PLAT_STORE_MAX 3840u
enum { OBJ_SET, OBJ_SOUND, OBJ_PAT0, OBJ_PAT1, OBJ_PANEL, OBJ_SONG, OBJ_MOTION, OBJ_NOBJ };
/* flash (storage.c): OBJ_SET 0xFC000; SOUND / PAT0 / PAT1 / SONG 0x97000.. (2 sectors each);
 * OBJ_PANEL 0xDC000, OBJ_MOTION 0xDE000. SiDJ uses SET (settings) and
 * PANEL (the calibration); SOUND, PAT0, PAT1, SONG and MOTION are unused here. */

/* audio load in % of the render budget, and dropped half buffers since boot */
uint32_t plat_cpu_pct(void);
uint32_t plat_xruns(void);
/* USB audio: s[0] streaming (the computer reads), s[1] packets, s[2] underruns (ring empty),
 * s[3] overruns (ring full), s[4] missed (no packet ready), s[5] stalls; zeros without it */
void plat_usb_audio(uint32_t s[6]);
/* the MASTER pot: s[0] lowest and s[1] highest raw reading (0..1023) since the last call, s[2]
 * readings 64 or more away from the smoothed value, s[3] readings, s[4] the gain (Q12) */
void plat_master_stats(uint32_t s[5]);
/* USB audio's format: 1 the mix and the three voices, 0 the mix; the FM-1 reconnects to the
 * computer (a moment without MIDI) so it sees the new one */
void plat_usb_audio_mode(int wide);
/* the audio load since the last call, in 0.1 % of the render budget: the mean and the worst
 * half buffer; the call starts the next window */
void plat_cpu_stats(uint32_t *mean_x10, uint32_t *peak_x10);
/* stop the song for a flash change: it fades out, and returns once it has (the erase then stops
 * no audio mid-sound) */
void plat_stop_for_flash(void);
/* the supply levels set at boot: s[0] VDC14 (1.25 V + 50 mV a level), s[1] SYSVDD (0.93 V + 30 mV) */
void plat_supplies(uint8_t s[2]);
