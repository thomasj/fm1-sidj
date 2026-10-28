/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* USB full-speed device on USB0: a class-compliant USB-MIDI interface.
 * All SIE traffic happens in usb_poll(), called from the TIMER5 ISR at 2 kHz,
 * so INDEX is never shared and no USB IRQ is needed. MIDI in goes to a ring
 * the audio ISR drains; MIDI out comes from a ring the audio ISR fills.
 * SysEx F0 22 24 35 7D F7 (soft key) asks the main loop to enter UBOOT.
 * VID 0x1209 / PID 0x0001 is the pid.codes test id.
 * FELUCCA_CDC=1 adds a CDC-ACM serial function (IAD composite: EP2 notify,
 * EP3 bulk data) for the console in console.c.
 * FELUCCA_UAC=1 adds a USB audio input (UAC1, class compliant; ported from Felucca 1.0): the master
 * output as 16-bit stereo at 44.1 kHz on an isochronous asynchronous IN endpoint (EP4), so a
 * computer can record the FM-1 over the cable. The audio ISR fills a ring (uac_render_start,
 * uac_tap); uac_service sends one packet per USB frame from the TIMER5 ISR, nested in the render
 * too (main_fm1.c). The update loader leaves it off: MIDI only.
 * FELUCCA_UAC_STEMS=1 (SiDJ): usb_audio_mode(1) before usb_start makes the stream five channels,
 * the mix as L and R then the app's three stems (uac_stems, uac_tap's stems); 0 keeps the stereo
 * mix. One format either way: the descriptors are patched in RAM, and the device version differs
 * (hosts cache descriptors per version). The SiDJ mix is mono: L = R. */
#include "../hal/fm1_usb.h"   /* registers; relative, so the loader and the host tests find it too */
#ifndef FELUCCA_CDC
#define FELUCCA_CDC 0
#endif
#ifndef FELUCCA_UAC
#define FELUCCA_UAC 0
#endif
#ifndef FELUCCA_UAC_STEMS
#define FELUCCA_UAC_STEMS 0
#endif
#if !FELUCCA_UAC
#undef FELUCCA_UAC_STEMS
#define FELUCCA_UAC_STEMS 0
#endif
enum { S_FADDR = 0, S_POWER = 1, S_INTRTX1 = 2, S_INTRTX2 = 3, S_INTRRX1 = 4, S_INTRRX2 = 5, S_INTRUSB = 6,
       S_INTRTX1E = 7, S_INTRTX2E = 8, S_INTRRX1E = 9, S_INTRRX2E = 10, S_INTRUSBE = 11, S_FRAME1 = 12,
       S_FRAME2 = 13, S_INDEX = 14,
       S_TXMAXP = 16, S_CSR0 = 17, S_TXCSR1 = 17, S_TXCSR2 = 18, S_RXMAXP = 19, S_RXCSR1 = 20, S_RXCSR2 = 21,
       S_COUNT0 = 22, S_RXCOUNT1 = 22, S_RXCOUNT2 = 23 };

static uint8_t ep0buf[64 + 4] __attribute__((aligned(4)));
static uint8_t ep1tx[64] __attribute__((aligned(4)));
static uint8_t ep1rx[64 + 4] __attribute__((aligned(4)));
#if FELUCCA_CDC
static uint8_t ep2tx[8] __attribute__((aligned(4)));
static uint8_t ep3tx[64] __attribute__((aligned(4)));
static uint8_t ep3rx[64 + 4] __attribute__((aligned(4)));
/* serial rings: in = host -> console (ISR writes), out = console -> host (ISR reads) */
#define CI_N 256u
#define CO_N 2048u
static uint8_t cdc_in[CI_N], cdc_out[CO_N];
static volatile uint32_t ci_w, ci_r, co_w, co_r;
static struct {
    uint8_t line[7];             /* line coding: just stored and echoed back */
    volatile uint8_t dtr;        /* host has the port open */
    uint8_t e0_rx;               /* SET_LINE_CODING data stage pending */
    uint8_t rx_pend;             /* EP3 OUT packet seen, not yet taken (the ring was full) */
    uint32_t rx_pkts, tx_pkts;
} cdc = {{0x00, 0xC2, 0x01, 0x00, 0, 0, 8}};   /* 115200 8N1 */
#endif

#if FELUCCA_UAC
/* USB audio input. The FM-1's I2S clock is its own (~44,145 Hz on X0X, measured), not the host's
 * 44,100. Felucca sends what its ring holds, 44 or 45 frames a 1 ms packet and a frame more or less
 * to hold the ring's fill, so the host sees a device running at 44,145: a recorder is happy, but an
 * app passing the input straight to an output (Hosting AU) has two clocks and no drift correction,
 * and drops or repeats a little every few seconds. X0X resamples instead (cubic, in uac_tap) and
 * sends exactly the 44.1 pattern: to the host the FM-1 runs at its own USB clock.
 * The fill is taken just before each 256-frame block goes in (the ring's low point: a half adds two
 * blocks back to back, every 11.6 ms), the lowest over UA_WIN blocks (~190 ms) is steered to UA_MID
 * by the resampler's step, proportional plus a learned trim (the clocks' drift). */
#define UA_N 2048u               /* ring, frames (16-bit: mix | V1 << 16, V2 | V3 << 16); a power of two */
#define UA_HALF 256u             /* frames one engine_render adds (main_fm1.c REND_FRAMES; two per half) */
#define UA_PRIME 460u            /* silence ahead of the first render: UA_MID and the most it can take */
#define UA_MID 200               /* the window's lowest fill, steered here: a busy bar's renders swing the
                                  * low point by ~200 frames (measured), and latency is cheap for a recorder */
#define UA_WIN 32u               /* renders per window (~185 packets) */
#define UA_MAXF 46u              /* frames in the largest packet */
#define UAC_MAXP (UA_MAXF * 4u)                 /* alt 1: 2 channels */
#define UAC_MAXP5 (UA_MAXF * 10u)               /* the mix and the voices: 5 channels */
#define UAC_RATE 44100u
_Static_assert(UA_PRIME + 4u * UA_HALF + UA_HALF + UA_MAXF < UA_N, "the ring holds a start, two renders and a packet");
static uint32_t ua_ring[UA_N][2];
static volatile uint32_t ua_w, ua_r;
static uint32_t ep4tx[2][(UA_MAXF * 10u + 3u) / 4u] __attribute__((aligned(4)));   /* alternate: the DMA may still read the last */
static uint8_t uac_wide;         /* usb_audio_mode: 1 five channels (the mix L R, the voices), 0 the mix L R */
static uint8_t ep4_k;
static struct {
    volatile uint8_t alt;        /* streaming interface setting: 1 = the host records (2 = four channels) */
    uint8_t nch;                 /* this render's channels in the ring: 2 (alt 1) or 4 (alt 2) */
    volatile uint8_t go;         /* the ring is primed: the consumer reads it (0: silence packets) */
    volatile uint8_t flowing;    /* the host takes our packets (it may set alt 1 well before it reads) */
    uint8_t queued;              /* a packet was queued since the stream (re)started */
    uint8_t feed;                /* this render feeds the ring (sampled when it starts) */
    uint8_t wait;                /* polls the queued packet has waited */
    uint8_t e0_rx;               /* SET_CUR sampling frequency: data stage pending */
    volatile uint32_t fill_min;  /* the lowest fill over the last window (just before a block went in) */
    uint32_t win_min, win_n;     /* the window so far */
    float rs_step, rs_trim;      /* the resampler: input frames per output frame; its learned drift */
    int32_t rs_pos, rs_stepq;    /* the next output's place after the second-newest input frame, and
                                  * the step, Q24 (integer: float is slow in the audio ISR here) */
    int32_t rs_h[16];            /* the last four input frames, four channels each, oldest first */
    uint32_t acc;                /* 44.1 frames per ms: 0.1 frame steps */
    uint32_t last[2];            /* the last frame sent, repeated on an underrun */
    uint32_t starts, pkts, frames;               /* frames in packets sent from the ring */
    uint32_t underruns, overruns, missed;        /* glitches: ring empty (repeat), ring full (drop), no packet ready */
    uint32_t stalls;                             /* the host stopped reading for > 20 ms (alt still 1) */

    uint32_t fill_lo, fill_hi;                   /* range of fill_min since the stream started */
    uint32_t windows;

} uac;
#endif

static struct {
    uint8_t up, config, pend_addr, has_pend_addr, e0_tx, e0_zlp;
    const uint8_t *e0_src;
    uint16_t e0_left;
    uint32_t resets, setups, rx_pkts, tx_pkts, sof_seen, timeouts, no_sof;
    uint32_t suspends, max_gap, retries, frame_stalls;
    uint16_t frame, frame_same;  /* last SOF frame number and how long it has not moved (x 10 ms) */
    uint8_t detached;            /* usb_detach() was called: no retry */
    volatile uint8_t suspended;  /* bus idle > 3 ms (unplugged or host asleep): the UI hides "USB" */
    uint8_t last_setup[8];
    uint8_t sysex[16];
    uint8_t sx_len, sx_on;
    volatile uint8_t uboot_req;
    volatile uint8_t ota_req;    /* F0 22 24 35 7F F7: M-UPGRADE upgrade command (FELUCCA_OTA) */
} usb;

#if FELUCCA_OTA
/* M-UPGRADE SysEx (ota.c): one received frame at a time (7-bit bytes between
 * F0 and F7; frames arriving while one is pending are dropped, the host
 * retries), and a TX ring of SysEx event packets sent before any MIDI. */
static uint8_t sx_frame[640];
static uint32_t sx_pos;
static volatile uint32_t sx_frame_len;
static volatile uint8_t sx_ready, sx_collect, sx_busy;
#define SXQ 64u
static uint32_t sx_out_q[SXQ];
static volatile uint32_t so_w, so_r;
#endif

/* MIDI rings: 4-byte USB-MIDI event packets */
#define MQ 256u                                     /* ASID: up to 14 packets a chip and frame */
static uint32_t midi_in_q[MQ], midi_out_q[MQ];
static volatile uint32_t mi_w, mi_r, mo_w, mo_r;

static void midi_out_event(uint32_t pkt)            /* from the audio ISR */
{
    if (usb.config && mo_w - mo_r < MQ) {
        midi_out_q[mo_w % MQ] = pkt;
        RING_PUBLISH();
        mo_w++;
    }
}

/* ------------------------------------------------------- descriptors --- */
/* interfaces: 0 audio control, 1 MIDI streaming, 2 audio streaming (FELUCCA_UAC), then CDC's two.
 * bcdDevice: 3.00 MIDI, +0.01 CDC, +0.10 the audio input, +0.08 the audio streaming interface first
 * in the AC header's collection (hosts cache descriptors per version).
 * The collection lists audio streaming before MIDI (Felucca 1.5.1, its #67): macOS up to 15
 * (AppleUSBAudio) drops a MIDI interface from the list while it walks it by bInCollection, so with
 * MIDI first it runs past the end, never parses the audio streaming interface and shows no audio
 * device (MIDI still works). The class spec gives the order no meaning. */
#define UAC_BCD (0x10 * FELUCCA_UAC + 0x08 * FELUCCA_UAC)
#define CDC_IF (2 + FELUCCA_UAC)
#define UAC_AS_IF 2u
#define CFG_LEN (101 + 74 * FELUCCA_CDC + 74 * FELUCCA_UAC)
#define USB_NIF (2 + FELUCCA_UAC + 2 * FELUCCA_CDC)
#if FELUCCA_CDC
static const uint8_t DEV_DESC[18] = {18, 1, 0x00, 0x02, 0xEF, 0x02, 0x01, 64, 0x09, 0x12, 0x01, 0x00,
                                     0x01 + UAC_BCD, 0x03, 1, 2, 0, 1};   /* misc/IAD */
static const uint8_t CFG_DESC[] = {
    9, 2, CFG_LEN & 0xFF, CFG_LEN >> 8, USB_NIF, 1, 0, 0x80, 50,
    8, 0x0B, 0, 2 + FELUCCA_UAC, 1, 1, 0, 0,            /* IAD: audio + MIDI (IF 0-1, 0-2) */
#else
#ifndef FELUCCA_USB_PID
#define FELUCCA_USB_PID 0x0001   /* the update loader is 0x0002 */
#endif
static const uint8_t DEV_DESC[18] = {18, 1, 0x10, 0x01, 0, 0, 0, 64, 0x09, 0x12, FELUCCA_USB_PID & 0xFF,
                                     FELUCCA_USB_PID >> 8, 0x00 + UAC_BCD, 0x03,
                                     1, 2, 0, 1};
static const uint8_t CFG_DESC[] = {
    9, 2, CFG_LEN & 0xFF, CFG_LEN >> 8, USB_NIF, 1, 0, 0x80, 50,
#endif
    9, 4, 0, 0, 0, 1, 1, 0, 0,
#if FELUCCA_UAC
    10, 0x24, 1, 0x00, 0x01, 31, 0, 2, UAC_AS_IF, 1,    /* AC header 1.00: audio + MIDI streaming */
    12, 0x24, 2, 1, 0x03, 0x06, 0, 2, 0x03, 0x00, 0, 0, /* input terminal 1: line, 2 ch (L R) */
    9, 0x24, 3, 2, 0x01, 0x01, 0, 1, 0,                 /* output terminal 2: USB streaming, from 1 */
#else
    9, 0x24, 1, 0x00, 0x01, 9, 0, 1, 1,
#endif
    9, 4, 1, 0, 2, 1, 3, 0, 0,
    7, 0x24, 1, 0x00, 0x01, 0x41, 0x00,
    6, 0x24, 2, 1, 1, 0,
    6, 0x24, 2, 2, 2, 0,
    9, 0x24, 3, 1, 3, 1, 2, 1, 0,
    9, 0x24, 3, 2, 4, 1, 1, 1, 0,
    9, 5, 0x01, 2, 64, 0, 0, 0, 0,
    5, 0x25, 1, 1, 1,
    9, 5, 0x81, 2, 64, 0, 0, 0, 0,
    5, 0x25, 1, 1, 3,
#if FELUCCA_UAC
    9, 4, UAC_AS_IF, 0, 0, 1, 2, 0, 0,                  /* IF2 audio streaming, alt 0: no bandwidth */
    9, 4, UAC_AS_IF, 1, 1, 1, 2, 0, 0,                  /* alt 1: the stream */
    7, 0x24, 1, 2, 1, 0x01, 0x00,                       /* AS general: terminal 2, delay 1, PCM */
    11, 0x24, 2, 1, 2, 2, 16, 1, UAC_RATE & 0xFF, (UAC_RATE >> 8) & 0xFF, UAC_RATE >> 16,
                                                        /* type I: 2 ch, 16 bit, one rate */
    9, 5, 0x84, 0x05, UAC_MAXP & 0xFF, UAC_MAXP >> 8, 1, 0, 0,   /* EP4 IN isochronous async, 1 ms */
    7, 0x25, 1, 0x01, 0, 0, 0,                          /* CS endpoint: sampling frequency control */
#endif
#if FELUCCA_CDC
    8, 0x0B, CDC_IF, 2, 2, 2, 1, 0,                     /* IAD: CDC ACM (IF 2-3, 3-4) */
    9, 4, CDC_IF, 0, 1, 2, 2, 1, 0,                     /* communication */
    5, 0x24, 0x00, 0x10, 0x01,                          /* header 1.10 */
    5, 0x24, 0x01, 0x00, CDC_IF + 1,                    /* call management: the data IF */
    4, 0x24, 0x02, 0x02,                                /* ACM: line coding + state */
    5, 0x24, 0x06, CDC_IF, CDC_IF + 1,                  /* union */
    7, 5, 0x82, 3, 8, 0, 16,                            /* EP2 IN interrupt (never sent) */
    9, 4, CDC_IF + 1, 0, 2, 0x0A, 0, 0, 0,              /* data */
    7, 5, 0x03, 2, 64, 0, 0,                            /* EP3 OUT bulk */
    7, 5, 0x83, 2, 64, 0, 0,                            /* EP3 IN bulk */
#endif
};
typedef char cfg_len_ok[sizeof CFG_DESC == CFG_LEN ? 1 : -1];

#if FELUCCA_UAC_STEMS
/* the descriptors as sent: in five-channel mode the input terminal, the format and the endpoint
 * say five channels, and the device version is one more (a host's cache must not mix them) */
static uint8_t dev_desc[sizeof DEV_DESC], cfg_desc[sizeof CFG_DESC];
static void usb_desc_build(void)
{
    uint32_t i;
    for (i = 0; i < sizeof DEV_DESC; i++) dev_desc[i] = DEV_DESC[i];
    for (i = 0; i < sizeof CFG_DESC; i++) cfg_desc[i] = CFG_DESC[i];
    if (!uac_wide) return;
    dev_desc[12] = (uint8_t)(dev_desc[12] + 0x40u);   /* (0x40: the names changed twice; an older cache is stale) */
    for (i = 0; i + 1 < sizeof cfg_desc; i += cfg_desc[i]) {
        uint8_t *d = cfg_desc + i;
        if (!d[0]) break;
        if (d[0] == 12 && d[1] == 0x24 && d[2] == 2) { d[7] = 5; d[10] = 4; }  /* input terminal: 5 ch, the mix as
                                                             * front left and right (the standard stereo pair) and
                                                             * three named by strings 4 .. 6 (Voice 1 .. 3) */
        if (d[0] == 11 && d[1] == 0x24 && d[2] == 2) d[4] = 5;                 /* the format */
        if (d[0] == 9 && d[1] == 5 && d[2] == 0x84) { d[4] = UAC_MAXP5 & 0xFF; d[5] = UAC_MAXP5 >> 8; }
    }
}

static void usb_audio_mode(int wide) { uac_wide = wide ? 1u : 0u; usb_desc_build(); }   /* before usb_start */
#else
#define dev_desc DEV_DESC                   /* (the update loader: as built) */
#define cfg_desc CFG_DESC
#endif
static const uint8_t STR0[4] = {4, 3, 0x09, 0x04};
#ifdef FELUCCA_LOADER
static const uint8_t STR1[] = {42, 3, 'H', 0, 0xFC, 0, 'g', 0, 'e', 0, 'l', 0, 't', 0, 'o', 0, 'n', 0, ' ', 0, 'I', 0,
                               'n', 0, 's', 0, 't', 0, 'r', 0, 'u', 0, 'm', 0, 'e', 0, 'n', 0, 't', 0, 's', 0};
#else
static const uint8_t STR1[] = {10, 3, 'S', 0, 'i', 0, 'D', 0, 'J', 0};   /* SiDJ: manufacturer */
#endif
#ifdef FELUCCA_LOADER
static const uint8_t STR2[] = {30, 3, 'F', 0, 'e', 0, 'l', 0, 'u', 0, 'c', 0, 'c', 0, 'a', 0, ' ', 0, 'U', 0, 'p', 0,
                               'd', 0, 'a', 0, 't', 0, 'e', 0};
#else
static const uint8_t STR2[] = {20, 3, 'S', 0, 'i', 0, 'D', 0, 'J', 0, ' ', 0, 'F', 0, 'M', 0, '-', 0, '1', 0};   /* SiDJ: product */
#endif

#if FELUCCA_UAC_STEMS                       /* the five-channel stream's channels (iChannelNames) */
static const uint8_t STR_V[3][16] = {{16, 3, 'V', 0, 'o', 0, 'i', 0, 'c', 0, 'e', 0, ' ', 0, '1', 0},
                                     {16, 3, 'V', 0, 'o', 0, 'i', 0, 'c', 0, 'e', 0, ' ', 0, '2', 0},
                                     {16, 3, 'V', 0, 'o', 0, 'i', 0, 'c', 0, 'e', 0, ' ', 0, '3', 0}};
#endif

static int get_desc(uint32_t wvalue, const uint8_t **d, uint16_t *l)
{
    switch (wvalue >> 8) {
    case 1:
        *d = dev_desc;
        *l = sizeof dev_desc;
        return 1;
    case 2:
        *d = cfg_desc;
        *l = sizeof cfg_desc;
        return 1;
    case 3:
        switch (wvalue & 0xFFu) {
        case 0:
            *d = STR0;
            *l = sizeof STR0;
            return 1;
        case 1:
            *d = STR1;
            *l = sizeof STR1;
            return 1;
        case 2:
            *d = STR2;
            *l = sizeof STR2;
            return 1;
#if FELUCCA_UAC_STEMS
        case 4: case 5: case 6:
            *d = STR_V[(wvalue & 0xFFu) - 4u];
            *l = sizeof STR_V[0];
            return 1;
#endif
        default:
            return 0;
        }
    default:
        return 0;
    }
}

/* ---------------------------------------------------------- the SIE --- */
static void sie_wr(uint32_t r, uint32_t v)
{
    uint32_t n = 20000;
    if (!fm1_usb_sie_on())
        return;
    fm1_usb_sie_wr_start(r, v);
    while (!fm1_usb_sie_done() && --n)
        ;
    if (n)
        usb.timeouts = 0;                               /* consecutive failures only */
    else if (++usb.timeouts > 50u)
        usb.up = 0;                                     /* SIE dead (no clock?): stop polling it */
}

static uint32_t sie_rd(uint32_t r)
{
    uint32_t n = 20000;
    if (!fm1_usb_sie_on())
        return 0;
    fm1_usb_sie_rd_start(r);
    while (!fm1_usb_sie_done())
        if (!--n) {
            if (++usb.timeouts > 50u)
                usb.up = 0;
            return 0;
        }
    usb.timeouts = 0;
    return fm1_usb_sie_data();
}

#if FELUCCA_UAC
static void uac_ep4_reset(void)                         /* drop a queued packet; isochronous mode */
{
    sie_wr(S_INDEX, 4);
    sie_wr(S_TXCSR1, 0x48);                             /* ClrDataTog + FlushFIFO (SDK usb_g_ep_config) */
    sie_wr(S_TXCSR2, 0);
    sie_wr(S_TXCSR1, 0);
    sie_wr(S_TXCSR2, 0x40);                             /* ISO */
}

static void uac_stream(uint32_t alt)                    /* SET_INTERFACE, configuration, bus reset */
{
    uac.go = 0;                                         /* the next render primes the ring again */
    uac.flowing = 0;                                    /* ... once the host has taken a packet */
    uac.queued = 0;
    uac.wait = 0;
    RING_PUBLISH();
    uac.alt = (uint8_t)alt;
    uac.acc = 0;
    uac.last[0] = uac.last[1] = 0;
    if (alt) {                                          /* (the fill range of the last stream stays readable) */
        uac.starts++;
        uac.fill_lo = 0xFFFFFFFFu;
        uac.fill_hi = 0;
    }
}
#endif

static void ep1_config(void)
{
    fm1_usb_ep_txbuf(1, ep1tx);
    fm1_usb_ep_rxbuf(1, ep1rx);
    sie_wr(S_INDEX, 1);
    sie_wr(S_TXMAXP, 0xFF);
    sie_wr(S_TXCSR1, 0x48);
    sie_wr(S_TXCSR2, 0);
    sie_wr(S_RXMAXP, 0xFF);
    sie_wr(S_RXCSR1, 0x90);
    sie_wr(S_RXCSR2, 0);
    sie_wr(S_INTRRX1E, 0x02);
    fm1_usb_ep_enable(1u << 1);
#if FELUCCA_CDC
    fm1_usb_ep_txbuf(2, ep2tx);
    sie_wr(S_INDEX, 2);
    sie_wr(S_TXMAXP, 0xFF);
    sie_wr(S_TXCSR1, 0x48);
    sie_wr(S_TXCSR2, 0);
    fm1_usb_ep_txbuf(3, ep3tx);
    fm1_usb_ep_rxbuf(3, ep3rx);
    sie_wr(S_INDEX, 3);
    sie_wr(S_TXMAXP, 0xFF);
    sie_wr(S_TXCSR1, 0x48);
    sie_wr(S_TXCSR2, 0);
    sie_wr(S_RXMAXP, 0xFF);
    sie_wr(S_RXCSR1, 0x90);
    sie_wr(S_RXCSR2, 0);
    sie_wr(S_INTRRX1E, 0x0A);
    fm1_usb_ep_enable((1u << 2) | (1u << 3));
    cdc.rx_pend = 1;                                    /* look once: a packet may already wait */
#endif
#if FELUCCA_UAC
    fm1_usb_ep4_txbuf(ep4tx[0]);
    sie_wr(S_INDEX, 4);
    sie_wr(S_TXMAXP, 0xFF);
    uac_ep4_reset();
    fm1_usb_ep_enable(1u << 4);
    uac_stream(0);
#endif
}

static void e0_chunk(void)
{
    uint32_t n = usb.e0_left > 64u ? 64u : usb.e0_left, i;
    int last;
    for (i = 0; i < n; i++)
        ep0buf[i] = usb.e0_src[i];
    fm1_usb_ep0_send(ep0buf, n);
    usb.e0_src += n;
    usb.e0_left = (uint16_t)(usb.e0_left - n);
    last = usb.e0_left == 0 && !(n == 64u && usb.e0_zlp);
    if (usb.e0_left == 0 && n == 64u)
        usb.e0_zlp = 0;
    sie_wr(S_INDEX, 0);
    sie_wr(S_CSR0, last ? 0x0A : 0x02);
    if (last)
        usb.e0_tx = 0;
}

static void e0_send(const uint8_t *p, uint16_t len, uint16_t wlen)
{
    if (len > wlen)
        len = wlen;
    usb.e0_src = p;
    usb.e0_left = len;
    usb.e0_zlp = (len < wlen) && (len % 64u == 0);
    usb.e0_tx = 1;
    sie_wr(S_INDEX, 0);
    sie_wr(S_CSR0, 0x40);
    e0_chunk();
}

static void ep0_service(void)
{
    static const uint8_t zero2[2];
    uint32_t csr, i;
    uint16_t wvalue, wlength;
    uint8_t *s = usb.last_setup;
    if (usb.has_pend_addr) {
        sie_wr(S_FADDR, usb.pend_addr);
        usb.has_pend_addr = 0;
    }
    sie_wr(S_INDEX, 0);
    csr = sie_rd(S_CSR0);
    if (csr & 0x04u) {                                  /* SentStall */
        sie_wr(S_CSR0, 0);
        usb.e0_tx = 0;
#if FELUCCA_CDC
        cdc.e0_rx = 0;
#endif
#if FELUCCA_UAC
        uac.e0_rx = 0;
#endif
        return;
    }
    if (csr & 0x10u) {                                  /* SetupEnd: the host abandoned the transfer */
        sie_wr(S_CSR0, 0x80);
        usb.e0_tx = 0;
#if FELUCCA_CDC
        cdc.e0_rx = 0;                                  /* else the next SETUP is taken as line coding */
#endif
#if FELUCCA_UAC
        uac.e0_rx = 0;
#endif
    }
    if (usb.e0_tx) {
        if (!(csr & 0x02u))
            e0_chunk();
        return;
    }
    if (!(csr & 0x01u))
        return;
    fm1_usb_rx_sync();
#if FELUCCA_CDC
    if (cdc.e0_rx) {                                    /* SET_LINE_CODING data stage */
        uint32_t n = sie_rd(S_COUNT0);
        for (i = 0; i < 7u && i < n; i++)
            cdc.line[i] = ep0buf[i];
        cdc.e0_rx = 0;
        goto ack;
    }
#endif
#if FELUCCA_UAC
    if (uac.e0_rx) {                                    /* SET_CUR sampling frequency: 44100 is all we */
        uac.e0_rx = 0;                                  /* have, whatever the host asks for */
        goto ack;
    }
#endif
    for (i = 0; i < 8u; i++)
        s[i] = ep0buf[i];
    usb.setups++;
    wvalue = (uint16_t)(s[2] | s[3] << 8);
    wlength = (uint16_t)(s[6] | s[7] << 8);
    switch ((uint32_t)s[0] << 8 | s[1]) {
    case 0x0005:                                        /* SET_ADDRESS */
        usb.pend_addr = s[2] & 0x7Fu;
        usb.has_pend_addr = 1;
        goto ack;
    case 0x8006: {                                      /* GET_DESCRIPTOR */
        const uint8_t *d;
        uint16_t l;
        if (get_desc(wvalue, &d, &l)) {
            if (!wlength)
                goto ack;
            e0_send(d, l, wlength);
            return;
        }
        goto stall;
    }
    case 0x0009:                                        /* SET_CONFIGURATION: 0 or 1 only */
        if (s[2] > 1u)
            goto stall;
        usb.config = s[2];
        if (usb.config == 1u)
            ep1_config();
        else
            sie_wr(S_INTRRX1E, 0);
#if FELUCCA_UAC
        if (usb.config != 1u)
            uac_stream(0);
#endif
        goto ack;
    case 0x8008:
        e0_send(&usb.config, 1, wlength);
        return;
    case 0x8000:
    case 0x8100:
    case 0x8200:
        e0_send(zero2, 2, wlength);
        return;
    case 0x010B:                                        /* SET_INTERFACE: alt 0 (alt 1 for the audio stream) */
#if FELUCCA_UAC
        if (s[4] == UAC_AS_IF && wvalue <= 1u && usb.config) {
            uac_ep4_reset();
            uac_stream(wvalue);
            goto ack;
        }
#endif
        if (wvalue == 0)
            goto ack;
        goto stall;
    case 0x810A:
#if FELUCCA_UAC
        if (s[4] == UAC_AS_IF) {
            static uint8_t alt;
            alt = uac.alt;
            e0_send(&alt, 1, wlength);
            return;
        }
#endif
        e0_send(zero2, 1, wlength);
        return;
    case 0x0201: {                                      /* CLEAR_FEATURE(ENDPOINT_HALT): data toggle reset */
#if FELUCCA_CDC
        uint32_t ep = s[4] & 0x0Fu, last = 3u;
#else
        uint32_t ep = s[4] & 0x0Fu, last = 1u;
#endif
#if FELUCCA_UAC
        if (wvalue == 0 && s[4] == 0x84u)
            goto ack;                                   /* isochronous: no halt, no toggle */
#endif
        if (wvalue != 0 || ep > last)
            goto stall;                                 /* not an endpoint we have */
        if (ep) {                                       /* (EP0 has no halt to clear) */
            sie_wr(S_INDEX, ep);
            if (s[4] & 0x80u)
                sie_wr(S_TXCSR1, 0x40);
            else
                sie_wr(S_RXCSR1, 0x80);
        }
        goto ack;
    }
#if FELUCCA_CDC
    case 0x2120:                                        /* SET_LINE_CODING: 7 bytes follow */
        if (wlength) {
            cdc.e0_rx = 1;
            sie_wr(S_INDEX, 0);
            sie_wr(S_CSR0, 0x40);                       /* ServicedRxPktRdy, no DataEnd yet */
            return;
        }
        goto ack;
    case 0xA121:                                        /* GET_LINE_CODING */
        e0_send(cdc.line, 7, wlength);
        return;
    case 0x2122:                                        /* SET_CONTROL_LINE_STATE */
        cdc.dtr = s[2] & 1u;
        goto ack;
    case 0x2123:                                        /* SEND_BREAK */
        goto ack;
#endif
#if FELUCCA_UAC
    case 0x2201:                                        /* SET_CUR, endpoint: sampling frequency */
        if (s[4] != 0x84u || s[3] != 1u)
            goto stall;
        if (wlength) {
            uac.e0_rx = 1;
            sie_wr(S_INDEX, 0);
            sie_wr(S_CSR0, 0x40);                       /* the 3 bytes follow */
            return;
        }
        goto ack;
    case 0xA281:                                        /* GET_CUR / MIN / MAX: the one rate */
    case 0xA282:
    case 0xA283: {
        static const uint8_t RATE[3] = {UAC_RATE & 0xFF, (UAC_RATE >> 8) & 0xFF, UAC_RATE >> 16};
        if (s[4] != 0x84u || s[3] != 1u)
            goto stall;
        e0_send(RATE, 3, wlength);
        return;
    }
#endif
    default:
        goto stall;
    }
ack:
    sie_wr(S_INDEX, 0);
    sie_wr(S_CSR0, 0x48);
    return;
stall:
    sie_wr(S_INDEX, 0);
    sie_wr(S_CSR0, 0x60);
}

/* SysEx assembly: only short commands matter here */
static void sysex_byte(uint8_t b)
{
    static const uint8_t UBOOT_KEY[6] = {0xF0, 0x22, 0x24, 0x35, 0x7D, 0xF7};
    if (b == 0xF0) {
        usb.sx_on = 1;
        usb.sx_len = 0;
#if FELUCCA_OTA
        sx_collect = !sx_ready;
        sx_pos = 0;
#endif
    }
    if (!usb.sx_on)
        return;
#if FELUCCA_OTA
    if (sx_collect && b != 0xF0 && b != 0xF7) {
        if (sx_pos < sizeof sx_frame)
            sx_frame[sx_pos++] = b;
        else
            sx_collect = 0;                             /* too long: not ours */
    }
#endif
    if (usb.sx_len < sizeof usb.sysex)
        usb.sysex[usb.sx_len++] = b;
    if (b == 0xF7) {
        uint32_t i, ok = usb.sx_len == 6u;
        for (i = 0; ok && i < 6u; i++)
            ok = usb.sysex[i] == UBOOT_KEY[i];
        if (ok)
            usb.uboot_req = 1;
#if FELUCCA_OTA
        else if (usb.sx_len == 6u && usb.sysex[1] == 0x22 && usb.sysex[2] == 0x24 && usb.sysex[3] == 0x35 &&
                 usb.sysex[4] == 0x7F)
            usb.ota_req = 1;
        else if (sx_collect && sx_pos) {
            sx_frame_len = sx_pos;
            RING_PUBLISH();
            sx_ready = 1;                               /* main loop: ota_take() / ed_service() */
        }
        sx_collect = 0;
#endif
        usb.sx_on = 0;
    }
}

static void ep1_rx(void)
{
    uint32_t csr, n, i;
    sie_wr(S_INDEX, 1);
    csr = sie_rd(S_RXCSR1) | (sie_rd(S_RXCSR2) << 8);
    if (!(csr & 1u))
        return;
    n = sie_rd(S_RXCOUNT1) | (sie_rd(S_RXCOUNT2) << 8);
    if (n > 64u)
        n = 64u;
    fm1_usb_rx_sync();
    for (i = 0; i + 3u < n; i += 4u) {
        uint32_t cin = ep1rx[i] & 15u, pkt = (uint32_t)ep1rx[i] | (uint32_t)ep1rx[i + 1] << 8 |
                                            (uint32_t)ep1rx[i + 2] << 16 | (uint32_t)ep1rx[i + 3] << 24;
        if (cin >= 4u && cin <= 7u) {                   /* SysEx */
            uint32_t k, nb = cin == 4u || cin == 7u ? 3u : cin == 6u ? 2u : 1u;
            for (k = 0; k < nb; k++)
                sysex_byte(ep1rx[i + 1 + k]);
        } else if (((cin >= 8u && cin <= 0xEu) || (cin == 0xFu && ep1rx[i + 1] >= 0xF8u)) && mi_w - mi_r < MQ) {
            /* CIN 0xF: a single byte; the real-time ones (clock, start, continue, stop) matter */
            midi_in_q[mi_w % MQ] = pkt;
            RING_PUBLISH();
            mi_w++;
        }
    }
    usb.rx_pkts++;
    csr = (csr & ~0x164u) | 0x10u;
    sie_wr(S_RXCSR1, csr & 0xFFu);
    sie_wr(S_RXCSR2, csr >> 8);
}

#if FELUCCA_OTA
/* ---- SysEx frames for the main loop (ota.c / editor.c hooks; felucca.c and the
 * update loader supply ota_now_ms / ota_idle) ---- */
static uint32_t ota_now_ms(void);
static void ota_idle(void);

static int ota_wire_send(const uint8_t *p, uint32_t n)   /* F0..F7 -> USB-MIDI SysEx packets */
{
    uint32_t i = 0, t0 = ota_now_ms();
    sx_busy = 1;
    while (i < n) {
        uint32_t k = n - i >= 3u ? 3u : n - i, pkt;
        uint32_t cin = k == 3u && i + 3u < n ? 4u : k == 3u ? 7u : 4u + k;   /* 4 continues; 5/6/7 end */
        pkt = cin | (uint32_t)p[i] << 8 | (k > 1u ? (uint32_t)p[i + 1] << 16 : 0u) |
              (k > 2u ? (uint32_t)p[i + 2] << 24 : 0u);
        while (so_w - so_r >= SXQ) {
            if (!*(volatile uint8_t *)&usb.config || ota_now_ms() - t0 > 200u) {
                sx_busy = 0;
                return -1;
            }
            ota_idle();
        }
        sx_out_q[so_w % SXQ] = pkt;
        RING_PUBLISH();
        so_w++;
        i += k;
    }
    sx_busy = 0;
    return 0;
}
static int ota_frame_get(const uint8_t **p, uint32_t *n)
{
    if (!sx_ready)
        return 0;
    RING_PUBLISH();                                     /* read the frame only after the flag */
    *p = sx_frame;
    *n = sx_frame_len;
    return 1;
}
static void ota_frame_done(void)
{
    RING_PUBLISH();                                     /* done with the frame before the ISR may refill it */
    sx_ready = 0;
}
#endif

static void ep1_tx(void)
{
    uint32_t csr, n = 0;
#if FELUCCA_OTA
    if (mo_w == mo_r && so_w == so_r)
        return;
#else
    if (mo_w == mo_r)
        return;
#endif
    sie_wr(S_INDEX, 1);
    csr = sie_rd(S_TXCSR1);
    if (csr & 0x01u)
        return;                                         /* previous packet still pending */
    if (csr & 0x80u)
        sie_wr(S_TXCSR1, csr & ~0x80u);
#if FELUCCA_OTA
    while (so_r != so_w && n < 64u) {                   /* SysEx first, never split by notes */
        uint32_t pkt = sx_out_q[so_r % SXQ];
        ep1tx[n] = (uint8_t)pkt;
        ep1tx[n + 1] = (uint8_t)(pkt >> 8);
        ep1tx[n + 2] = (uint8_t)(pkt >> 16);
        ep1tx[n + 3] = (uint8_t)(pkt >> 24);
        n += 4u;
        so_r++;
    }
    while (!sx_busy && so_r == so_w && mo_r != mo_w && n < 64u) {
#else
    while (mo_r != mo_w && n < 64u) {
#endif
        uint32_t pkt;
        RING_PUBLISH();                                 /* the audio ISR (producer) can preempt us: */
        pkt = midi_out_q[mo_r % MQ];                    /* slot read strictly between the index checks */
        ep1tx[n] = (uint8_t)pkt;
        ep1tx[n + 1] = (uint8_t)(pkt >> 8);
        ep1tx[n + 2] = (uint8_t)(pkt >> 16);
        ep1tx[n + 3] = (uint8_t)(pkt >> 24);
        n += 4u;
        RING_PUBLISH();
        mo_r++;
    }
    if (!n)
        return;
    fm1_usb_ep_send(1, ep1tx, n);
    sie_wr(S_TXCSR1, sie_rd(S_TXCSR1) | 0x01u);
    usb.tx_pkts++;
}

#if FELUCCA_CDC
static void ep3_rx(void)                                /* leaves the packet (NAK) while the ring is full */
{
    uint32_t csr, n, i;
    sie_wr(S_INDEX, 3);
    csr = sie_rd(S_RXCSR1) | (sie_rd(S_RXCSR2) << 8);
    if (!(csr & 1u)) {
        cdc.rx_pend = 0;
        return;
    }
    n = sie_rd(S_RXCOUNT1) | (sie_rd(S_RXCOUNT2) << 8);
    if (n > 64u)
        n = 64u;
    if (CI_N - (ci_w - ci_r) < n)
        return;                                         /* rx_pend stays: retried next poll */
    cdc.rx_pend = 0;
    fm1_usb_rx_sync();
    for (i = 0; i < n; i++)
        cdc_in[(ci_w + i) % CI_N] = ep3rx[i];
    RING_PUBLISH();
    ci_w += n;
    cdc.rx_pkts++;
    csr = (csr & ~0x164u) | 0x10u;
    sie_wr(S_RXCSR1, csr & 0xFFu);
    sie_wr(S_RXCSR2, csr >> 8);
}

static void ep3_tx(void)                                /* <= 63 bytes per packet: never needs a ZLP */
{
    uint32_t csr, n = 0;
    if (co_w == co_r)
        return;
    sie_wr(S_INDEX, 3);
    csr = sie_rd(S_TXCSR1);
    if (csr & 0x01u)
        return;
    if (csr & 0x80u)
        sie_wr(S_TXCSR1, csr & ~0x80u);
    while (co_r != co_w && n < 63u)
        ep3tx[n++] = cdc_out[co_r++ % CO_N];
    fm1_usb_ep_send(3, ep3tx, n);
    sie_wr(S_TXCSR1, sie_rd(S_TXCSR1) | 0x01u);
    cdc.tx_pkts++;
}
#endif

#if FELUCCA_UAC
/* ---- the audio input: audio.c produces (render start, then each block), TIMER5 consumes ---- */
static __attribute__((noinline)) void uac_render_start(void)   /* audio ISR, before a half buffer renders */
{
    uac.feed = uac.alt && usb.config && uac.flowing;
    uac.nch = uac_wide ? 4u : 1u;           /* in the ring: the mix, and the voices */
    if (!uac.feed) {
        uac.go = 0;                                     /* not read (yet / any more): prime when it is */
        return;
    }
    if (!uac.go) {                                      /* (re)start: silence ahead, then this render */
        uint32_t r = ua_r, i;
        for (i = 0; i < UA_PRIME; i++)
            ua_ring[(r + i) & (UA_N - 1u)][0] = ua_ring[(r + i) & (UA_N - 1u)][1] = 0;
        RING_PUBLISH();
        ua_w = r + UA_PRIME;
        uac.win_min = 0xFFFFFFFFu;
        uac.win_n = 0;
        uac.rs_trim = 1.0e-3f;                          /* X0X's I2S: ~44,145 Hz (measured) */
        uac.rs_step = 1.0f + uac.rs_trim;
        uac.rs_stepq = (int32_t)(uac.rs_step * 16777216.0f);
        uac.rs_pos = 0;
        RING_PUBLISH();
        uac.go = 1;
    }
}

/* the fill just before a block goes in: the ring's low point. Each UA_WIN, the window's lowest
 * decides whether the packets owe a few frames more (fill too high) or fewer (too low). */
static void uac_window(uint32_t fill)
{
    if (fill < uac.win_min)
        uac.win_min = fill;
    if (++uac.win_n < UA_WIN)
        return;
    uac.fill_min = uac.win_min;
    if (uac.win_min < uac.fill_lo)
        uac.fill_lo = uac.win_min;
    if (uac.win_min > uac.fill_hi)
        uac.fill_hi = uac.win_min;
    {   /* fill too high: take input faster (fewer frames out per frame in), and the other way */
        float err = (float)(int32_t)((uac.win_min > 900u ? 900u : uac.win_min) - UA_MID);
        /* a step change d moves the fill ~8,400 d frames a window: proportional 1e-5 settles in a
         * few seconds, an error of 50 frames is 0.05 % (under a cent); the trim learns the drift */
        uac.rs_trim += err * 1.0e-6f;                   /* integral: the clocks' drift */
        uac.rs_trim = uac.rs_trim > 8.0e-3f ? 8.0e-3f : uac.rs_trim < -8.0e-3f ? -8.0e-3f : uac.rs_trim;
        uac.rs_step = 1.0f + uac.rs_trim + err * 1.0e-5f;
        uac.rs_stepq = (int32_t)(uac.rs_step * 16777216.0f);
    }
    uac.windows++;
    uac.win_min = 0xFFFFFFFFu;
    uac.win_n = 0;
}

static inline int32_t uac_s16(int32_t v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

/* audio ISR: one block in, 24-bit in int32 (X0X): out the mix, L R interleaved; stems three
 * int16 per frame (the voices) or null. Alt 1 rings L R; alt 2 the stems and the mix (L) */
static __attribute__((noinline)) void uac_tap(const int32_t *out, const int16_t *stems, uint32_t n)
{
    uint32_t w = ua_w, i, nch = uac.nch, c;
    if (!uac.feed)
        return;
    uac_window(w - ua_r);
    for (i = 0; i < n; i++) {                            /* each frame in: as many out as the step gives */
        int32_t *h = uac.rs_h, in[4], q[4] = {0, 0, 0, 0};
        in[0] = uac_s16(out[2u * i] >> 8);                /* the mix (mono: L = R) */
        in[1] = stems ? stems[3u * i] : 0;                /* the voices, or silence */
        in[2] = stems ? stems[3u * i + 1u] : 0;
        in[3] = stems ? stems[3u * i + 2u] : 0;
        for (c = 0; c < nch; c++) {
            h[c] = h[4u + c]; h[4u + c] = h[8u + c]; h[8u + c] = h[12u + c]; h[12u + c] = in[c];
        }
        while (uac.rs_pos < (1 << 24)) {                /* between frames 1 and 2 of the four: cubic */
            int32_t t = uac.rs_pos >> 13;               /* Hermite (Catmull-Rom), doubled to stay integer, t Q11 */
            for (c = 0; c < nch; c++) {
                int32_t xm = h[c], x0 = h[4u + c], x1 = h[8u + c], x2 = h[12u + c];
                int32_t c1, c2, c3, y;
                if (!(xm | x0 | x1 | x2)) { q[c] = 0; continue; }   /* silent (stems off): nothing to do */
                if (c && xm == h[c - 1u] && x0 == h[3u + c] && x1 == h[7u + c] && x2 == h[11u + c]) {
                    q[c] = q[c - 1u];                   /* as the channel before (the mono mix's L R) */
                    continue;
                }
                c1 = x1 - xm; c2 = 2 * xm - 5 * x0 + 4 * x1 - x2; c3 = (x2 - xm) + 3 * (x0 - x1);
                y = ((((((c3 * t) >> 11) + c2) * t >> 11) + c1) * t >> 11);
                q[c] = uac_s16(x0 + ((y + 1) >> 1));
            }
            if (w - ua_r >= UA_N) {                     /* nobody reads: drop the frame */
                uac.overruns++;
                uac.rs_pos += uac.rs_stepq;
                continue;
            }
            ua_ring[w & (UA_N - 1u)][0] = (uint32_t)(uint16_t)q[0] | (uint32_t)(uint16_t)q[1] << 16;
            ua_ring[w & (UA_N - 1u)][1] = (uint32_t)(uint16_t)q[2] | (uint32_t)(uint16_t)q[3] << 16;
            w++;
            uac.rs_pos += uac.rs_stepq;
        }
        uac.rs_pos -= 1 << 24;
    }
    RING_PUBLISH();
    ua_w = w;
}

#if FELUCCA_UAC_STEMS
static int uac_stems(void) { return uac.feed && uac_wide; }   /* this render: the app's stems wanted */
#endif

/* the next packet into d: 44 / 45 frames, the 44.1 pattern, from the ring or, until it is
 * primed, silence; each frame the mix as L and R, then (five-channel mode) the voices. An empty
 * ring repeats the last frame. Returns the frame count. */
static uint32_t uac_packet(uint16_t *d)
{
    uint32_t n = 44u, r, fill, k, i, w0, w1;
    uac.acc += UAC_RATE % 1000u;
    if (uac.acc >= 1000u) {
        uac.acc -= 1000u;
        n++;
    }
    r = ua_r;
    fill = uac.go ? ua_w - r : 0;
    RING_PUBLISH();                                     /* the slots after the index */
    k = fill < n ? fill : n;
    if (uac.go && k < n) uac.underruns++;
    for (i = 0; i < n; i++) {
        if (!uac.go) w0 = w1 = 0;                       /* not primed: silence */
        else if (i < k) { w0 = ua_ring[(r + i) & (UA_N - 1u)][0]; w1 = ua_ring[(r + i) & (UA_N - 1u)][1]; }
        else { w0 = uac.last[0]; w1 = uac.last[1]; }    /* empty: the last frame again */
        uac.last[0] = w0; uac.last[1] = w1;
        *d++ = (uint16_t)w0;                            /* L */
        *d++ = (uint16_t)w0;                            /* R */
        if (uac_wide) { *d++ = (uint16_t)(w0 >> 16); *d++ = (uint16_t)w1; *d++ = (uint16_t)(w1 >> 16); }
    }
    if (!uac.go) return n;
    RING_PUBLISH();
    ua_r = r + k;
    uac.pkts++;
    uac.frames += n;
    return n;
}

/* TIMER5, 2 kHz, also nested in the render: the host takes one packet per 1 ms frame; queue the
 * next one as soon as the last has gone (TxPktRdy clear). Not paced on SOF-pending (see usb_poll).
 * The ring is fed only while the host reads: it often selects alt 1 long before its first IN token,
 * and a recorder may stop reading without going back to alt 0 (a packet waiting > 20 ms). */
static void uac_service(void)
{
    uint32_t csr, n;
    if (!usb.up || !usb.config || !uac.alt)
        return;
    sie_wr(S_INDEX, 4);
    csr = sie_rd(S_TXCSR1);
    if (csr & 0x01u) {                                  /* still queued */
        if (uac.wait < 255u && ++uac.wait == 40u && uac.flowing) {
            uac.flowing = 0;
            uac.stalls++;
        }
        return;
    }
    if (uac.queued) {
        if (csr & 0x04u)
            uac.missed++;                               /* UnderRun: an IN token found nothing queued */
        uac.flowing = 1;
    }
    uac.queued = 1;
    uac.wait = 0;
    ep4_k ^= 1u;
    n = uac_packet((uint16_t *)ep4tx[ep4_k]);
    fm1_usb_ep4_send(ep4tx[ep4_k], n * (uac_wide ? 10u : 4u));
    sie_wr(S_TXCSR1, 0x01);                             /* TxPktRdy (and UnderRun cleared) */
}
#endif

static void usb_poll(void)                              /* TIMER5 ISR, 2 kHz */
{
    uint32_t iu, it, ir;
    if (!usb.up)
        return;
    if (fm1_usb_sof_take()) {                           /* SOF pending: not a reliable "host is there", */
                                                        /* it keeps firing with the cable out */
        usb.sof_seen++;
        usb.no_sof = 0;
    } else if (++usb.no_sof > usb.max_gap) {
        usb.max_gap = usb.no_sof;
    }
    if (usb.config) {                                   /* the frame number only advances with a real host */
        static uint32_t tick;
        if (++tick >= 20u) {                            /* every 20 polls = 10 ms */
            uint16_t f;
            tick = 0;
            f = (uint16_t)(sie_rd(S_FRAME1) | (sie_rd(S_FRAME2) & 7u) << 8);
            if (f != usb.frame) {
                usb.frame = f;
                usb.frame_same = 0;
                usb.suspended = 0;
            } else if (++usb.frame_same >= 10u && !usb.suspended) {   /* 100 ms frozen: unplugged / host asleep */
                usb.suspended = 1;
                usb.frame_stalls++;
            }
        }
    }
    iu = sie_rd(S_INTRUSB);
#ifdef FELUCCA_LOADER
    it = sie_rd(S_INTRTX1) | sie_rd(S_INTRTX2) << 8;    /* the update loader reads both */
    ir = sie_rd(S_INTRRX1) | sie_rd(S_INTRRX2) << 8;
#else
    it = sie_rd(S_INTRTX1);                             /* EP0..7 only: skipping INTR*2 saves */
    ir = sie_rd(S_INTRRX1);                             /* 2 SIE round trips per poll */
#endif
    if (iu & 0x01u) {                                   /* suspend: the bus went idle */
        usb.suspends++;
        usb.suspended = 1;
    }
    if (iu & 0x02u)                                     /* resume */
        usb.suspended = 0;
    if (iu & 0x04u) {                                   /* bus reset */
        usb.resets++;
        sie_wr(S_FADDR, 0);
        usb.config = 0;
        usb.suspended = 0;
        mo_r = mo_w;                                    /* nothing stale for the next host */
        usb.sx_on = 0;
#if FELUCCA_OTA
        so_r = so_w;
        sx_collect = 0;
#endif
        usb.e0_tx = 0;
        usb.has_pend_addr = 0;
#if FELUCCA_CDC
        cdc.dtr = 0;
        cdc.e0_rx = 0;
        cdc.rx_pend = 0;
#endif
#if FELUCCA_UAC
        uac.e0_rx = 0;
        uac_stream(0);
#endif
        fm1_usb_ep0_buf(ep0buf);
        sie_wr(S_INTRUSBE, 0x07);
        sie_wr(S_INTRTX1E, 0x01);
        sie_wr(S_INTRRX1E, 0);
    }
    if (it & 0x01u)
        ep0_service();
    if (ir & 0x02u)
        ep1_rx();
    if (usb.config)
        ep1_tx();
#if FELUCCA_CDC
    if (usb.config) {
        if (ir & 0x08u)
            cdc.rx_pend = 1;
        if (cdc.rx_pend)                                /* not every poll: 3 SIE round trips each */
            ep3_rx();
        if (cdc.dtr)
            ep3_tx();
    }
#endif
#if FELUCCA_UAC
    uac_service();
#endif
}

static void usb_start(void)                             /* boot, or main-loop retry while usb.up == 0 */
{
#if FELUCCA_UAC_STEMS
    if (!dev_desc[0]) usb_desc_build();
#endif
    usb.timeouts = 0;
    fm1_usb_reset();                                    /* reset whatever the ROM left */
    fm1_delay_ms(25);
    fm1_usb_attach(ep0buf);
    sie_wr(S_POWER, 0x60);
    sie_wr(S_INTRUSBE, 0x07);                           /* suspend, resume, reset (polled) */
    sie_wr(S_INTRTX1E, 0x01);
    sie_wr(S_INTRTX2E, 0);
    sie_wr(S_INTRRX1E, 0);
    sie_wr(S_INTRRX2E, 0);
    fm1_usb_ep_enable(0x1Fu);
    usb.up = usb.timeouts < 3u;                         /* a dead SIE leaves USB off */
}

/* Powered on without a cable, the SIE does not answer and usb_start leaves
 * USB off: retry once a second from the main loop so a cable plugged in
 * later still enumerates. */
static void usb_retry(uint32_t now_ms)
{
    static uint32_t t;
    if (usb.up || usb.detached || now_ms - t < 1000u)
        return;
    t = now_ms;
    usb.retries++;
    usb_start();
}

static void usb_detach(void)
{
    usb.detached = 1;
    usb.up = 0;
    fm1_usb_off();
}
