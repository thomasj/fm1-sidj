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
#include "beat.h"

#ifdef SIDJ_HOST
#define ENG_POOL
#else
#define ENG_POOL __attribute__((section(".pool")))
#endif

#define ENG_FILE_MAX 61440u                 /* the largest .sid file the player takes */
#define ENG_SCOPE_N 4096                    /* samples of each voice kept for the scopes: two periods of a
                                             * low note (~45 Hz) and one more to find the trigger in */
#define ENG_RATE 44100u

enum { EV_LOAD, EV_PLAY, EV_STOP, EV_MODEL, EV_RX, EV_RX_RESET, EV_VOLUME, EV_OUTPUT, EV_USB4, EV_RX_SET, EV_CLOCK_RATIO, EV_CLOCK_OFFSET, EV_CLOCK_MANUAL };
/* EV_CLOCK_MANUAL: pkt = BPM x 10: SID+CLK, no tempo found: the MIDI clock at that tempo, set by
 * hand (from now, or from the tune's first write on its next load again); 0: no clock */
#define ENG_CLK_MANUAL_MIN 400
#define ENG_CLK_MANUAL_MAX 2500
/* EV_CLOCK_OFFSET: pkt = ms (signed): the MIDI clock that much later than the tempo found puts it
 * (earlier when negative), from now on; the tune is untouched; a load: 0 (or the song's saved) */
#define ENG_CLK_OFFSET_MAX 250
/* EV_CLOCK_RATIO: a = ENG_CLK_RATIO index: the MIDI clock at that ratio of the tempo found (a wrong
 * detection put right: half, double ...), from its next beat; the tune is untouched; a load: x1 */
#define ENG_CLK_RATIOS 7
#define ENG_CLK_X1 3
static const uint8_t ENG_CLK_RATIO[ENG_CLK_RATIOS][2] = {{1, 2}, {2, 3}, {3, 4}, {1, 1}, {4, 3}, {3, 2}, {2, 1}};   /* num, den */
/* EV_USB4: a = 1 USB audio offers four channels (the tempo and fader then share a 100 % cap), 0 the mix */
/* EV_OUTPUT: a = 0 the emulated SID, 1 ASID over USB-MIDI (the emulation runs for the screen,
 * silent), 2 both, 3 the emulated SID and a MIDI clock over USB (ENG_OUT_CLOCK: a load first finds
 * the tune's beat, beat.h, then the clock starts on its first row: Start, 24 pulses a beat, in the
 * tune's time, so the tempo and the fader carry it; a pause sends Stop, the play after it Continue) */
#define ENG_OUT_CLOCK 3
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
#define ENG_LOAD_REMIX 0x40                 /* the same song again (any subsong): its remix, tempo and fader stay */
#define ENG_LOAD_BEAT1 0x80                 /* a MIDI Start (SID+CLK): the tune from its first beat, as the clock's */
#define ENG_LOAD_AGAIN 0x20                 /* the song just played, from its start again (REC + PLAY): SID+CLK keeps
                                             * the tempo it found (and the clock's ratio), no new look */
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

/* the MIDI clock (ENG_OUT_CLOCK): running; the tempo the last load found, BPM x 10 (0: none) */
extern volatile uint8_t eng_clk_on;
extern volatile uint16_t eng_clk_bpm_x10;
extern volatile uint32_t eng_clk_pulses;      /* pulses since its Start (the PLAY LED flashes the beat) */
extern volatile uint8_t eng_clk_ratio;        /* the ratio asked for (ENG_CLK_RATIO; it applies from the next beat) */
extern volatile uint32_t eng_clk_first_cyc;   /* the first pulse (beat 1), in the tune's C64 cycles from its start */
extern volatile int16_t eng_clk_offset_ms;    /* the offset asked for (EV_CLOCK_OFFSET) */
extern volatile uint8_t eng_clk_manual;       /* 1: the clock's tempo was set by hand (EV_CLOCK_MANUAL, or kept so) */
extern volatile uint32_t eng_calls_min_x10;   /* the tune's player calls a minute, x10 (a beat is whole calls) */
extern volatile uint8_t eng_clk_click;        /* 1: a click on each of the clock's beats in the output (louder on a bar's first) */
/* following a MIDI clock that comes in (SID+CLK with a beat found): the tune's speed keeps its
 * beats on the clock's, and SiDJ sends no clock of its own meanwhile; the clock's BPM x 10; the last
 * difference between the tune's pulse and the clock's (µs, + the tune late) */
extern volatile uint8_t eng_follow;
extern volatile uint16_t eng_follow_bpm_x10;
extern volatile int32_t eng_follow_err_us;
extern volatile uint16_t eng_follow_speed;     /* the tune's speed while following, Q12 (4096: as written) */
/* a tempo found before (the song's saved one): set it, then EV_LOAD a playing load with SID+CLK
 * skips the look and starts the clock from it, at its ratio and offset (the load clears valid).
 * eng_clk_get: the tempo the last look found (or was given), 0 if none */
typedef struct { uint8_t valid, ratio; int16_t offset_ms; uint16_t tick, call, off, bpm_x10; uint32_t base; } eng_clk_grid_t;   /* tick 0: set by hand */
extern eng_clk_grid_t eng_clk_preset;
int eng_clk_get(eng_clk_grid_t *g);
extern volatile uint8_t eng_clk_tried;        /* the last load looked for the tempo (a paused load does not:
                                             * browsing stays quick; its first PLAY loads it again to look) */
