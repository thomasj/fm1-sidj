/* SPDX-License-Identifier: GPL-3.0-only */
/* SiDJ engine: what the audio interrupt renders. The C64 (c64/c64.h) runs the tune in
 * cycle-counted chunks; its SID writes pass through the remix (remix.h) and land on the
 * emulated SIDs at their exact cycles, which render the audio. The main loop (buttons, keys,
 * knobs, the UI) never touches the C64, the SIDs or the remix directly: it posts events into a
 * ring that eng_render() drains before each block. The UI reads the state for display only.
 * The output is the emulated SIDs, ASID over USB-MIDI (asid.h: one SysEx per chip and frame,
 * sent from here through plat_midi_out), or both. */
#pragma once
#include <stdint.h>
#include "../c64/c64.h"
#include "../../../sid/sid.h"
#include "remix.h"
#include "asid.h"

#ifdef SIDJ_HOST
#define ENG_POOL
#else
#define ENG_POOL __attribute__((section(".pool")))
#endif

#define ENG_FILE_MAX 61440u                 /* the largest .sid file the player takes */
#define ENG_SCOPE_N 4096                    /* samples of each voice kept for the scopes: two periods of a
                                             * low note (~45 Hz) and one more to find the trigger in */
#define ENG_RATE 44100u

enum { EV_LOAD, EV_PLAY, EV_STOP, EV_MODEL, EV_RX, EV_RX_RESET, EV_VOLUME, EV_OUTPUT, EV_USB4, EV_RX_SET };
/* EV_USB4: a = 1 USB audio offers four channels (the tempo and fader then share a 100 % cap), 0 the mix */
/* EV_OUTPUT: a = 0 the emulated SID, 1 ASID over USB-MIDI (the emulation runs for the screen,
 * silent), 2 both */
/* EV_RX: a = parameter, b = voice, pkt = value (signed deltas as int32) */
/* EV_RX_SET: the same, pkt the value itself (MIDI CCs). -1 is the tune's own for pulse width, A, D,
 * S, R, sync, ring, the filter routing and the filter mode; otherwise 0 off, 1 on. The waveform:
 * a SID_CTRL_* waveform bit, 0 the tune's. RX_P_FMODE_TOGGLE: b the LP / BP / HP bit. The cutoff:
 * the scale or the offset, whichever mode is on. RX_P_SOLO: 1 the voice alone, 0 all back */
enum { RX_P_KEY, RX_P_WAVE_TOGGLE, RX_P_MUTE_TOGGLE, RX_P_PW, RX_P_OCT, RX_P_FINE,
       RX_P_A, RX_P_D, RX_P_S, RX_P_R, RX_P_CUTOFF, RX_P_RES,
       RX_P_SOLO,                           /* the voice alone; again: all voices back */
       RX_P_FILT_TOGGLE,                    /* the voice's filter routing bit: frozen and toggled */
       RX_P_FMODE_TOGGLE,                   /* pkt: an LP / BP / HP bit of the filter mode field */
       RX_P_KEY_SHIFT,                      /* pkt: 0x100 | (semitones & 0xff) held, 0 released */
       RX_P_CUTOFF_MODE,                    /* pkt: RX_CUT_SCALE / RX_CUT_OFFSET (kept across songs) */
       RX_P_LFO_RATE, RX_P_LFO_DEPTH, RX_P_LFO_TARGET, RX_P_LFO_SHAPE,   /* voice b's LFO, pkt a delta */
       RX_P_TEMPO,                          /* pkt: a delta in %, 50 .. 200 */
       RX_P_VINYL,                          /* pkt: a delta in 0.1 %, -80 .. 80 */
       RX_P_SYNC, RX_P_RING };              /* EV_RX_SET only */
#define ENG_TEMPO_MIN 25
#define ENG_TEMPO_MAX 150                  /* 200 % overloaded heavy tunes on the FM-1 (measured) */
#define ENG_VINYL_MAX 80

extern c64_t c64;                           /* read-only outside eng_render */
extern uint8_t c64_mem[65536];
extern sid_t eng_sid[C64_NSID];
extern remix_t rx;
extern asid_t eng_asid;                     /* the ASID sink (read-only outside eng_render) */
extern volatile uint8_t eng_output;         /* 0 emulated, 1 ASID, 2 both */
extern uint8_t eng_regs[C64_NSID][SID_NREG];      /* what reached each chip (chip 0: rx.out) */
extern volatile uint8_t eng_playing;        /* the C64 is running */
extern volatile uint8_t eng_loaded;         /* a tune is in the C64 */
extern volatile uint8_t eng_ntsc;
/* the deck: the tempo (the C64 runs faster or slower than the chips: the play calls, not the
 * pitch) and the vinyl fader (both run faster or slower: pitch and tempo, a turntable's fader).
 * A new song or subsong sets them back, as OCT- and OCT+ together do */
extern volatile uint8_t eng_tempo;          /* % */
extern volatile int8_t eng_vinyl;           /* 0.1 % */
/* an RSID tune (its own interrupts, often digis) plays at 100 %, no fader and no LFOs: the deck
 * knobs and the LFO page's do nothing */
extern volatile uint8_t eng_deck_off;
/* the speed cap, %: tempo x (1 + a raised fader) never goes past it, so the FM-1 never runs out
 * of time (measured): 150, or 100 while USB audio offers four channels (eng_usb4) */
extern volatile uint8_t eng_usb4;
int eng_speed_cap(void);
/* stems: set before eng_render (in the same context) to get each voice of chip 0 there too, three
 * int16 per frame (V1 V2 V3), at the chip's level with the fades applied: zeros where the mix is
 * silent. Null: none, and nothing spent on them */
extern int16_t *eng_stem_out;
/* set before eng_render while the audio budget runs short: the combined waveforms' averaging (a
 * quality, not the sound) steps aside until it eases */
extern volatile uint8_t eng_busy;
extern volatile uint32_t eng_log_lost;
extern int16_t eng_scope[3][ENG_SCOPE_N];   /* chip 0's voices, a ring; eng_scope_w the next slot */
extern volatile uint32_t eng_scope_w;
extern volatile uint32_t eng_scope_n;       /* samples written, ever (eng_scope_w is it modulo ENG_SCOPE_N) */
/* what the scopes hold: 1 each voice as it sounds (its stem: the filter, the volume and the
 * envelope in it, at the mix's level, with the mixer's DC), whenever the audio has time
 * (eng_busy) and the tune is not an RSID; 0 the voice before the filter (16-bit) */
extern volatile uint8_t eng_scope_stems;
extern sidfile_t eng_file_info;             /* the loaded file's header (after EV_LOAD) */
extern volatile int eng_load_rc;            /* sidfile_parse's result of the last EV_LOAD */

/* EV_LOAD: copy the file here, set len and pending, then post with a = song (1-based, 0 = the
 * file's start song), b = bit0 NTSC override, bit1 PAL override, bits 2-3 model (0 file, 1 6581, 2 8580),
 * ENG_LOAD_PAUSED: loaded and ready, waiting for EV_PLAY */
#define ENG_LOAD_PAUSED 0x10
typedef struct { volatile uint8_t pending; uint32_t len; uint8_t data[ENG_FILE_MAX]; } eng_file_t;
extern eng_file_t eng_file;

void eng_init(void);
int eng_post(uint8_t type, uint8_t a, uint8_t b, uint32_t pkt);   /* 0 = queued, -1 = full */
/* audio ISR (or the host): drain the events, render n stereo frames of 24-bit audio. Play, pause
 * and a new song fade in and out over 10 ms (no clicks); while paused the chips are not clocked */
void eng_render(int32_t *lr, int n, uint32_t master_q12);
/* the main loop, often: the heavy part of an EV_LOAD (parsing the file, setting the C64 up) runs
 * here, outside the audio interrupt, while it renders silence */
void eng_poll(void);
/* the tune's elapsed time in deciseconds */
uint32_t eng_elapsed_ds(void);
