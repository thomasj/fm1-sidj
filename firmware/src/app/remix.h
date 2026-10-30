/* SPDX-License-Identifier: GPL-3.0-only */
/* The remix: a layer between the tune's SID writes and the chip. It keeps the register image
 * as the tune wrote it (`tune`) and as it reached the chip (`out`), and the overrides and
 * offsets the panel sets: a held key replacing a voice's frequency or shifting it by
 * semitones, pitch offsets in octaves and cents (fine), a frozen waveform / sync / ring field, a
 * mute, a pulse width, the four envelope nibbles, a voice's filter routing bit, the filter's
 * LP / BP / HP field, a cutoff scale (x0 to x16) or offset (the panel picks one) and a
 * resonance offset, and an LFO per voice on its pulse width, its pitch or the cutoff. Noise is left alone,
 * as on TherapSID: a voice playing pure noise keeps the tune's pitch and its waveform; a chosen
 * waveform replaces the tune's only while the tune plays a tonal one (pulse, saw, triangle).
 * Sync and ring are frozen and toggled on their own. A tune write is transformed on its way through; an
 * override change re-derives the registers it touches and emits what changed. Pure: no
 * hardware, no audio; the engine runs it in the audio context and the display reads its
 * state. First SID only. */
#pragma once
#include <stdint.h>
#include "../c64/sidreg.h"

#define RX_NV SID_NVOICE

#define RX_WAVE_TONAL (SID_CTRL_PULSE | SID_CTRL_SAW | SID_CTRL_TRI)   /* the waveforms a chosen one replaces */
#define RX_WAVE_MASK (SID_CTRL_WAVES | SID_CTRL_RING | SID_CTRL_SYNC)   /* what the panel shows and toggles */
/* the cutoff scale in Q7: 128 is the tune's cutoff, 0 .. 2048 is x0 .. x16 */
#define RX_CUTOFF_ONE 128
#define RX_CUTOFF_MAX 2048
enum { RX_CUT_SCALE, RX_CUT_OFFSET };       /* cutoff_mode */
enum { RX_ADSR_A = 1, RX_ADSR_D = 2, RX_ADSR_S = 4, RX_ADSR_R = 8 };

#define RX_OCT_MAX 3                        /* the octave offset: +/- octaves */
#define RX_FINE_MAX 50                      /* the fine offset: +/- cents (TherapSID's remix: about 53) */
/* the LFOs: one per voice, its rate one of 64 steps from 0.05 to 20 Hz, its depth 0 .. 100 %:
 * the pulse width +/-2048 at 100 %, the pitch +/-1200 cents (depth squared: 10 % is 12 cents,
 * 50 % 300), the cutoff +/-2047 (on top of the scale or offset). Ticked from the audio, so
 * the tempo leaves them alone. */
enum { RX_LFO_OFF, RX_LFO_PW, RX_LFO_PITCH, RX_LFO_CUTOFF, RX_LFO_NTARGET };
enum { RX_LFO_SIN, RX_LFO_TRI, RX_LFO_SAW, RX_LFO_SQR, RX_LFO_RND, RX_LFO_NSHAPE };
#define RX_LFO_NRATE 64
#define RX_LFO_DEPTH_MAX 100                /* the depth in % */
#define RX_LFO_RATE_HZ 44100u               /* the samples remix_lfo_tick counts */
extern const uint16_t RX_LFO_CHZ[RX_LFO_NRATE];   /* the rates in 0.01 Hz, for the panel */

typedef struct {
    uint8_t wave_ovr, wave_val;             /* the waveform replaced by wave_val (one of pulse, saw, triangle, noise) */
    uint8_t sr_ovr, sr_val;                 /* the sync / ring bits in sr_ovr frozen at sr_val's (the panel
                                             * freezes both, a MIDI CC one) */
    uint8_t mute;
    uint8_t pw_ovr; uint16_t pw_val;        /* 12-bit pulse width */
    uint8_t adsr_ovr;                       /* RX_ADSR_* bits: which nibbles are overridden */
    uint8_t a, d, s, r;                     /* 0 .. 15 */
    int8_t oct, fine;                       /* pitch offset: +/- RX_OCT_MAX octaves, +/- RX_FINE_MAX cents */
    uint8_t lfo_target, lfo_shape, lfo_rate, lfo_depth;   /* RX_LFO_*, 0 .. 63, 0 .. 100 */
    uint32_t lfo_phase;
    int16_t lfo_val;                        /* the LFO now, -32767 .. 32767 (a random step held) */
    int16_t lfo_pw, lfo_cents;              /* its offsets now on this voice */
    uint8_t key_on; uint16_t key_freq;      /* a held key: the voice's frequency */
    uint8_t shift_on; int8_t shift_semi;    /* a held key in transpose mode: semitones on top of oct / semi */
    uint8_t filt_ovr, filt_val;             /* the voice's filter routing bit, frozen at filt_val (0 / 1) */
} rx_voice_t;

typedef struct {
    rx_voice_t v[RX_NV];
    uint8_t cutoff_mode;                    /* RX_CUT_SCALE: cutoff_scale applies; RX_CUT_OFFSET: cutoff_off */
    uint16_t cutoff_scale;                  /* Q7 factor on the tune's cutoff, 0 .. RX_CUTOFF_MAX */
    int16_t cutoff_off;                     /* -2047 .. 2047 */
    int8_t res_off;                         /* -15 .. 15 */
    uint8_t fmode_ovr, fmode_val;           /* the LP / BP / HP bits in fmode_ovr frozen at fmode_val's (the
                                             * panel freezes all three, a MIDI CC one) */
    uint8_t tune[SID_NREG];                 /* the image as the tune wrote it */
    uint8_t out[SID_NREG];                  /* the image as it reached the chip */
    uint8_t out_valid[SID_NREG];            /* out[] has been written at least once */
    int16_t lfo_cut;                        /* the LFOs' cutoff offset now (the sum) */
    uint32_t lfo_seed;                      /* the random shape's generator */
    uint8_t neutral;                        /* nothing changes the tune's writes: they pass as they are */
} remix_t;

typedef struct { uint8_t reg, val; } rx_out_t;

void remix_init(remix_t *rx);               /* images cleared, no overrides, the scale at x1 */
void remix_reset(remix_t *rx);              /* overrides and offsets off; the images and the cutoff mode stay */
/* a tune write: 0 .. 3 output writes in o[]; returns the count */
int remix_write(remix_t *rx, uint8_t reg, uint8_t val, rx_out_t *o);
/* after an override changed: re-derive voice v's registers (-1: all voices and the filter);
 * the output writes that differ from out[] go to o[] (max entries); returns the count */
int remix_refresh(remix_t *rx, int v, rx_out_t *o, int max);
/* the LFOs run on by n samples (at RX_LFO_RATE_HZ): the registers they moved go to o[] (max
 * entries, 12 is enough); returns the count */
int remix_lfo_tick(remix_t *rx, uint32_t n, rx_out_t *o, int max);
/* the transformed value of a register from the current images and overrides */
uint8_t remix_derive(const remix_t *rx, uint8_t reg);
/* helpers for the panel: a frequency from a MIDI note on a PAL / NTSC clock; the tune's
 * current values (for a knob that starts from them) */
uint16_t remix_note_freq(int midi_note, int ntsc);
uint16_t remix_tune_freq(const remix_t *rx, int v);
uint16_t remix_tune_pw(const remix_t *rx, int v);
uint16_t remix_tune_cutoff(const remix_t *rx);
int remix_voice_wave(const remix_t *rx, int v);     /* the waveform field reaching the chip */
