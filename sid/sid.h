/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * SiDJ SID engine: a fixed-point C port of reSID 0.16, the MOS6581 / MOS8580
 * emulator by Dag Lem (Copyright (C) 2004 Dag Lem <resid@nimrod.no>).
 * Port Copyright (C) 2026 Thomas Jansson, for FM-1 firmware.
 *
 * Integer only (the target CPU has no FPU). The chip is clocked in deltas of
 * roughly one output sample (reSID's SAMPLE_FAST method), which is what made
 * reSID run in real time on 1990s machines and what fits a small MCU today.
 */
#ifndef SIDJ_SID_H
#define SIDJ_SID_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { SID_MOS6581 = 0, SID_MOS8580 = 1 } sid_model_t;

enum { SID_ENV_ATTACK = 0, SID_ENV_DECAY_SUSTAIN = 1, SID_ENV_RELEASE = 2 };

typedef struct sid_wave {
    const struct sid_wave *sync_source;
    struct sid_wave *sync_dest;
    int msb_rising;
    uint32_t accumulator;       /* 24 bits */
    uint32_t shift_register;    /* 23 bits */
    uint32_t freq;              /* 16 bits */
    uint32_t pw;                /* 12 bits */
    uint32_t waveform;          /* control >> 4 */
    uint32_t test, ring_mod, sync;
    uint32_t pulse_avg;         /* the pulse output averaged over the last delta (12 bits) */
    uint32_t saw_avg;           /* the sawtooth output averaged over the last delta (12 bits) */
    uint32_t avg_on;
    /* SiDJ: the last delta, for the average the waveform did not need then (only the one it
     * shows is taken: each costs divides); 0 cycles: a test-bit step */
    uint32_t avg_prev, avg_freq, avg_pw;
    int avg_dt;
    /* SiDJ: the floating DAC (sid_set_floating_dac): with no waveform selected the output keeps
     * the last one's level, then fades a bit at a time */
    uint32_t held, float_on;
    int32_t held_ttl, held_ttl0, held_fade;
    uint32_t comb_avg, comb_avg_on;     /* SiDJ: a combined waveform averaged likewise (12 bits), when it is one */
    int comb_model;                     /* the model of its tables (the averaging's running sums must match) */
    int comb_off;                       /* the averaging held off (sid_set_comb_averaging: a busy moment) */
    const uint8_t *wave__ST, *wave_P_T, *wave_PS_, *wave_PST;
} sid_wave_t;

typedef struct sid_env {
    uint32_t rate_counter;      /* 15 bits */
    uint32_t rate_period;
    uint32_t exponential_counter;
    uint32_t exponential_counter_period;
    uint32_t envelope_counter;  /* 8 bits */
    int hold_zero;
    uint32_t attack, decay, sustain, release;
    uint32_t gate;
    int state;
} sid_env_t;

typedef struct sid_voice {
    sid_wave_t wave;
    sid_env_t envelope;
    int32_t wave_zero;          /* waveform D/A zero level */
    int32_t voice_DC;           /* multiplying D/A DC offset */
} sid_voice_t;

typedef struct sid_filter {
    int enabled;
    uint32_t fc;                /* 11 bits */
    uint32_t res, filt, voice3off, hp_bp_lp, vol;
    int32_t mixer_DC;
    int32_t Vhp, Vbp, Vlp, Vnf;
    int32_t w0, w0_ceil_1, w0_ceil_dt;
    int32_t _1024_div_Q;
    const uint32_t *w0_table;
} sid_filter_t;

typedef struct sid_extfilt {
    int enabled;
    int32_t mixer_DC;
    int32_t Vlp, Vhp, Vo;
    int32_t w0lp, w0hp;
} sid_extfilt_t;

typedef struct sid {
    sid_voice_t voice[3];
    sid_filter_t filter;
    sid_extfilt_t extfilt;
    uint32_t bus_value;
    int32_t bus_value_ttl;
    int32_t ext_in;
    sid_model_t model;
    /* Sampling: cycles per output sample in 16.16 fixed point, and the running offset. */
    int32_t cycles_per_sample;
    int32_t sample_offset;
    /* SiDJ: when set, sid_clock_render() also writes each sample's three voice outputs
     * (20-bit, scaled to 16) to tap[3 * i + v]; the caller sizes it for n samples */
    int16_t *tap;
    /* SiDJ: stems. When set (sid_set_stems), each voice's part of the filter is kept (linear, so a
     * voice alone is what it adds to the mix; one voice's part is the filter less the others',
     * so a single routed voice costs nothing extra), voices 1 and 2 go through the external
     * filter (a DC blocker for its high-pass), voice 3 is the output less the two, and sid_clock_render() writes the three, 16-bit like the
     * output, to stem[3 * i + v]: they add up to the output. The mixer's DC, which carries
     * volume-register samples, is shared out among them. The output itself is untouched
     * (bit-exact with reSID 0.16). */
    int16_t *stem;
    int stems;
    int32_t st_hp[3], st_bp[3], st_lp[3];   /* each voice's part of the filter's state */
    int st_act, st_imp;                     /* the voices in the filter (bits); the implicit one, -1 */
    int32_t st_in[3], st_lp2[3];            /* their external low-pass: its output, its state */
    int32_t st_ehp[3];                      /* their DC blocker (Q4) */
    int32_t st_k;                           /* the DC blocker's step a sample, Q16 */
    /* SiDJ: a voice muted at the output (0 by default): it adds nothing to the filter or the mix,
     * not even the DC an envelope makes with no waveform (a register mute leaves that) */
    uint8_t vmute[3];
} sid_t;

/* Set up a chip: reset, MOS6581, PAL clock (985248 Hz), 44100 Hz output. */
void sid_init(sid_t *s);
void sid_reset(sid_t *s);
void sid_set_model(sid_t *s, sid_model_t model);
void sid_set_sampling(sid_t *s, uint32_t clock_hz, uint32_t sample_hz);
void sid_enable_filter(sid_t *s, int enable);
void sid_enable_extfilter(sid_t *s, int enable);
/* Waveform averaging (off by default, which is reSID 0.16's exact behaviour): with delta
 * clocking the waveform is read once per output sample, so a pulse narrower than a sample is
 * often missed entirely (narrow pulse widths flutter) and every edge is quantized to the sample
 * grid (aliasing). On, the plain pulse and sawtooth waveforms output their mean over the
 * delta's cycles, computed exactly from the accumulator's linear advance: a one-sample box
 * filter, so edges land between samples as fractional values. */
void sid_set_averaging(sid_t *s, int enable);

/* Register access, offsets $00-$1c as on the chip. */
void sid_write(sid_t *s, uint8_t reg, uint8_t value);
uint8_t sid_read(sid_t *s, uint8_t reg);

/* 16-bit EXT IN sample. */
void sid_input(sid_t *s, int sample);

/* Advance the chip by delta_t cycles without producing output. */
void sid_clock(sid_t *s, int delta_t);

/* The current 16-bit audio output. */
int sid_output(const sid_t *s);

/* reSID's clock(delta_t&, buf, n, interleave): advance up to *delta_t cycles,
 * writing at most n samples; *delta_t is reduced by the cycles consumed.
 * Returns the number of samples written. */
int sid_clock_render(sid_t *s, int *delta_t, int16_t *buf, int n, int interleave);

/* Render exactly n samples (clocking the cycles they take). */
void sid_render(sid_t *s, int16_t *buf, int n);
/* SiDJ: the combined waveforms' averaging (with sid_set_averaging) on, or held off: taken once
 * a sample again, cheaper, for a moment the CPU is short */
void sid_set_comb_averaging(sid_t *s, int on);
/* stems on (their filters start from rest) or off */
void sid_set_stems(sid_t *s, int on);
/* SiDJ: the floating waveform DAC on or off (off by default: reSID 0.16, where no waveform outputs
 * 0). On, a voice switched to no waveform keeps the level it had (sample players set the test bit
 * with no waveform to freeze a triangle at a height: the 8580's sample trick), for about 54000
 * cycles on the 6581 and 800000 on the 8580, then fades one bit every 1400 / 50000 cycles (the
 * times reSIDfp measures). */
void sid_set_floating_dac(sid_t *s, int on);

#ifdef __cplusplus
}
#endif
#endif
