/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * SiDJ SID engine: a fixed-point C port of reSID 0.16 by Dag Lem.
 * Copyright (C) 2004 Dag Lem <resid@nimrod.no>
 * Port Copyright (C) 2026 Thomas Jansson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * The structure follows reSID 0.16 file by file (wave, envelope, voice,
 * filter, extfilt, sid) so that it can be compared with the original, and the
 * host test tests/sid_ref_test.cc checks that the two produce identical
 * samples. Only the delta-cycle ("SAMPLE_FAST") paths are ported.
 */
#include "sid.h"
#include "sid_tables.h"

#define SID_INLINE static inline

/* ------------------------------------------------------------------------ */
/* Waveform generator (reSID wave.h / wave.cc)                              */
/* ------------------------------------------------------------------------ */

static uint32_t comb_rec[257];              /* 65536 / n, for the average's divide */
static uint16_t comb_pre[4][4097];          /* running sums of _ST, P_T, PS_, PST of comb_model, mod 2^16:
                                             * pre[t][i] is table t's entries 0 .. i-1 added up */
static int comb_model = -1;

static void comb_load(sid_model_t m)
{
    const uint8_t *t[4];
    int i, j;
    if (comb_model == (int)m) return;
    if (m == SID_MOS6581) { t[0] = sid_wave6581__ST; t[1] = sid_wave6581_P_T; t[2] = sid_wave6581_PS_; t[3] = sid_wave6581_PST; }
    else { t[0] = sid_wave8580__ST; t[1] = sid_wave8580_P_T; t[2] = sid_wave8580_PS_; t[3] = sid_wave8580_PST; }
    for (i = 0; i < 4; i++) {
        uint16_t acc = 0;
        comb_pre[i][0] = 0;
        for (j = 0; j < 4096; j++) { acc = (uint16_t)(acc + t[i][j]); comb_pre[i][j + 1] = acc; }
    }
    comb_model = (int)m;
    for (i = 1; i <= 256; i++) comb_rec[i] = (65536u + (uint32_t)i / 2) / (uint32_t)i;
}

static void wave_set_model(sid_wave_t *w, sid_model_t model)
{
    if (model == SID_MOS6581) {
        w->wave__ST = sid_wave6581__ST; w->wave_P_T = sid_wave6581_P_T;
        w->wave_PS_ = sid_wave6581_PS_; w->wave_PST = sid_wave6581_PST;
    } else {
        w->wave__ST = sid_wave8580__ST; w->wave_P_T = sid_wave8580_P_T;
        w->wave_PS_ = sid_wave8580_PS_; w->wave_PST = sid_wave8580_PST;
    }
    comb_load(model);                       /* the averaging's RAM copy follows the last chip set */
    w->comb_model = (int)model;
    w->held_ttl0 = model == SID_MOS6581 ? 54000 : 800000;
    w->held_fade = model == SID_MOS6581 ? 1400 : 50000;
}

static void wave_reset(sid_wave_t *w)
{
    w->accumulator = 0;
    w->shift_register = 0x7ffff8;
    w->freq = 0;
    w->pw = 0;
    w->test = 0;
    w->ring_mod = 0;
    w->sync = 0;
    w->msb_rising = 0;
    w->pulse_avg = 0xfff;
    w->saw_avg = 0;
    w->avg_dt = 0;
    w->held = 0;
}

/* The pulse averaged over the delta_t cycles just clocked: how many of the accumulator values
 * prev + k*freq (k = 1..delta_t, modulo 2^24, at most one wrap since delta*freq < 2^24) are at
 * or above pw << 12. Each half of the wrapped range is one division for its first k and one for
 * its last. */
SID_INLINE uint32_t wave_pulse_average(uint32_t f, uint32_t pw, uint32_t prev, int delta_t)
{
    uint32_t P = pw << 12, count = 0, half;
    if (!f || delta_t <= 0)
        return ((prev >> 12) >= pw) ? 0xfff : 0x000;
    {                                       /* SiDJ: most deltas cross no edge (P, the wrap, P past
                                             * the wrap): all high or all low, without the divides */
        uint32_t first = prev + f, last = prev + (uint32_t)delta_t * f;
        if (!((first <= P && P <= last) || (first <= 0x1000000u && 0x1000000u <= last) ||
              (first <= P + 0x1000000u && P + 0x1000000u <= last)))
            return (first & 0xffffff) >= P ? 0xfff : 0x000;
    }
    for (half = 0; half < 2; half++) {
        uint32_t A = P + (half << 24), B = (half + 1) << 24;   /* k with A <= prev + k*f < B */
        uint32_t klo = A > prev ? (A - prev + f - 1) / f : 1;
        uint32_t khi = (B - 1) > prev ? (B - 1 - prev) / f : 0;
        if (klo < 1) klo = 1;
        if (khi > (uint32_t)delta_t) khi = (uint32_t)delta_t;
        if (khi >= klo) count += khi - klo + 1;
    }
    return (count * 0xfffu + (uint32_t)delta_t / 2) / (uint32_t)delta_t;
}

SID_INLINE uint32_t wave_saw_average(uint32_t f, uint32_t prev, int delta_t);

static uint32_t wave_output_now(const sid_wave_t *w);

static void wave_write_control(sid_wave_t *w, uint32_t control)
{
    uint32_t was = w->waveform;
    if (w->float_on && was && !((control >> 4) & 0x0f)) {   /* to no waveform: the DAC keeps the level */
        w->held = wave_output_now(w);
        w->held_ttl = w->held_ttl0;
    }
    w->waveform = (control >> 4) & 0x0f;
    if (w->avg_on && w->waveform != was) {  /* now showing an average the last delta did not take */
        if (w->waveform == 0x4) w->pulse_avg = w->avg_dt ? wave_pulse_average(w->avg_freq, w->avg_pw, w->avg_prev, w->avg_dt) : 0xfff;
        else if (w->waveform == 0x2) w->saw_avg = w->avg_dt ? wave_saw_average(w->avg_freq, w->avg_prev, w->avg_dt) : 0;
    }
    w->ring_mod = control & 0x04;
    w->sync = control & 0x02;
    uint32_t test_next = control & 0x08;
    if (test_next) {
        /* Test bit set: accumulator and shift register are cleared. */
        w->accumulator = 0;
        w->shift_register = 0;
    } else if (w->test) {
        /* Test bit cleared: the shift register restarts from 0x7ffff8. */
        w->shift_register = 0x7ffff8;
    }
    w->test = test_next;
}

/* The sawtooth (accumulator >> 12) averaged over the delta_t cycles just clocked: the sum of
 * prev + k*freq for k = 1..delta_t, less 2^24 for every k past the wrap, over 4096*delta_t.
 * Exact in 32 bits while delta_t*2^24 fits, so up to 255 cycles (a sample is ~22). */
SID_INLINE uint32_t wave_saw_average(uint32_t f, uint32_t prev, int delta_t)
{
    uint32_t d = (uint32_t)delta_t, sum, nwrap = 0;
    if (!f || delta_t <= 0 || delta_t > 255)
        return (prev + f * (uint32_t)delta_t) >> 12 & 0xfff;
    sum = d * prev + f * (d * (d + 1) / 2);
    if (prev + d * f >= 0x1000000u) {                 /* k from the wrap on: prev + k*f >= 2^24 */
        uint32_t kfirst = (0x1000000u - prev + f - 1) / f;
        if (kfirst < 1) kfirst = 1;
        if (kfirst <= d) nwrap = d - kfirst + 1;
    }
    sum -= nwrap << 24;
    return (sum / d + 2048u) >> 12;
}

/* SiDJ: a combined waveform (sawtooth or triangle indexing a table, with the pulse) averaged
 * over the sawtooth positions (12-bit) the delta_t cycles just clocked went through. Its tables
 * are spiky (the 6581's most): taken once a sample, as reSID 0.16's fast sampling does, the
 * spikes alias into pops. A run of positions adds up from the running sums (comb_pre) in two
 * look-ups, whatever its length: a look-up every other cycle cost the FM-1 about 5 points of
 * its audio load per voice. Mod 2^16, a run's sum is exact up to 256 positions (a note up to
 * ~2.8 kHz at 44.1 kHz); a longer one, or a chip of the other model than comb_model, is taken
 * at its end, as before. Ring modulation takes the source's accumulator as it stands. */
static uint32_t comb_run(const uint16_t *p, uint32_t lo, uint32_t hi)   /* entries lo .. hi */
{
    return (uint16_t)(p[hi + 1] - p[lo]);
}

static uint32_t comb_sum(const sid_wave_t *w, uint32_t lo, uint32_t hi, uint32_t src_msb)
{
    uint32_t s = 0;
    if (w->waveform != 0x3) {                /* the pulse gates: positions below the width are 0 */
        if (hi < w->pw) return 0;
        if (lo < w->pw) lo = w->pw;
    }
    if (w->waveform != 0x5)                  /* the sawtooth indexes _ST, PS_, PST directly */
        return comb_run(comb_pre[w->waveform == 0x3 ? 0 : w->waveform == 0x6 ? 2 : 3], lo, hi);
    /* P_T: the triangle, 11 bits, rises over the first half and falls over the second (each
     * swapped while ring modulation's source has its top bit set) */
    if (lo <= 2047) {
        uint32_t h = hi < 2047 ? hi : 2047;
        s += src_msb ? comb_run(comb_pre[1], 2047 - h, 2047 - lo) : comb_run(comb_pre[1], lo, h);
    }
    if (hi >= 2048) {
        uint32_t l = lo > 2048 ? lo : 2048;
        s += src_msb ? comb_run(comb_pre[1], l - 2048, hi - 2048) : comb_run(comb_pre[1], 4095 - hi, 4095 - l);
    }
    return s;
}

/* 0xffffffff: not averaged (take the waveform at the step's end) */
static uint32_t wave_comb_average(const sid_wave_t *w, uint32_t prev, int delta_t)
{
    uint32_t f = w->freq, first, last, i0, i1, n, sum;
    uint32_t msb = w->ring_mod ? (w->sync_source->accumulator >> 23) & 1 : 0;
    if ((uint32_t)delta_t * f >= 0x1000000u - 0x1000u) return 0xffffffffu;   /* (a wrap and more) */
    first = prev + f;                        /* the positions after the first cycle and the last */
    last = prev + (uint32_t)delta_t * f;
    i0 = (first & 0xffffff) >> 12;
    i1 = (last & 0xffffff) >> 12;
    if ((first >> 24) == (last >> 24)) {
        n = i1 - i0 + 1;                     /* no wrap */
        if (n > 256) return 0xffffffffu;
        sum = comb_sum(w, i0, i1, msb);
    } else {                                 /* through the wrap: i0 .. 4095, then 0 .. i1 */
        n = 4096 - i0 + i1 + 1;
        if (n > 256) return 0xffffffffu;
        sum = comb_sum(w, i0, 4095, msb) + comb_sum(w, 0, i1, msb);
    }
    return (sum * comb_rec[n] + 0x800u) >> 12;   /* sum / n << 4; sum <= 255 n: fits */
}

SID_INLINE void wave_clock(sid_wave_t *w, int delta_t)
{
    if (!w->waveform && w->held) {          /* the floating DAC: held, then a bit at a time away */
        w->held_ttl -= delta_t;
        while (w->held_ttl <= 0 && w->held) { w->held &= w->held >> 1; w->held_ttl += w->held_fade; }
    }
    if (w->test) { w->pulse_avg = 0xfff; w->saw_avg = 0; w->comb_avg = 0; w->comb_avg_on = 0; w->avg_dt = 0; return; }

    uint32_t accumulator_prev = w->accumulator;
    if (w->avg_on) {
        w->avg_prev = accumulator_prev; w->avg_freq = w->freq; w->avg_pw = w->pw; w->avg_dt = delta_t;
        if (w->waveform == 0x4) w->pulse_avg = wave_pulse_average(w->freq, w->pw, accumulator_prev, delta_t);
        else if (w->waveform == 0x2) w->saw_avg = wave_saw_average(w->freq, accumulator_prev, delta_t);
        w->comb_avg_on = (w->waveform == 0x3 || (w->waveform >= 0x5 && w->waveform <= 0x7)) && delta_t > 0 && delta_t <= 64
                         && w->comb_model == comb_model && !w->comb_off;   /* (the average itself once the accumulator has moved) */
    }
    uint32_t delta_accumulator = (uint32_t)delta_t * w->freq;
    w->accumulator = (w->accumulator + delta_accumulator) & 0xffffff;
    if (w->comb_avg_on) {
        w->comb_avg = wave_comb_average(w, accumulator_prev, delta_t);
        if (w->comb_avg == 0xffffffffu) w->comb_avg_on = 0;
    }
    w->msb_rising = !(accumulator_prev & 0x800000) && (w->accumulator & 0x800000);

    /* Shift the noise register once for each time accumulator bit 19 goes high. */
    uint32_t shift_period = 0x100000;
    while (delta_accumulator) {
        if (delta_accumulator < shift_period) {
            shift_period = delta_accumulator;
            if (shift_period <= 0x080000) {
                /* Flip from 0 to 1? */
                if (((w->accumulator - shift_period) & 0x080000) || !(w->accumulator & 0x080000))
                    break;
            } else {
                /* Flip from 0 (to 1 or via 1 to 0) or from 1 via 0 to 1? */
                if (((w->accumulator - shift_period) & 0x080000) && !(w->accumulator & 0x080000))
                    break;
            }
        }
        uint32_t bit0 = ((w->shift_register >> 22) ^ (w->shift_register >> 17)) & 0x1;
        w->shift_register = ((w->shift_register << 1) & 0x7fffff) | bit0;
        delta_accumulator -= shift_period;
    }
}

SID_INLINE void wave_synchronize(sid_wave_t *w)
{
    if (w->msb_rising && w->sync_dest->sync && !(w->sync && w->sync_source->msb_rising))
        w->sync_dest->accumulator = 0;
}

SID_INLINE uint32_t wave_out_T(const sid_wave_t *w)
{
    uint32_t msb = (w->ring_mod ? w->accumulator ^ w->sync_source->accumulator : w->accumulator) & 0x800000;
    return ((msb ? ~w->accumulator : w->accumulator) >> 11) & 0xfff;
}
SID_INLINE uint32_t wave_out_S(const sid_wave_t *w) { return w->accumulator >> 12; }
SID_INLINE uint32_t wave_out_P(const sid_wave_t *w)
{
    return (w->test || (w->accumulator >> 12) >= w->pw) ? 0xfff : 0x000;
}
SID_INLINE uint32_t wave_out_N(const sid_wave_t *w)
{
    uint32_t r = w->shift_register;
    return ((r & 0x400000) >> 11) | ((r & 0x100000) >> 10) | ((r & (1u << 16)) >> 7) |
           ((r & 0x002000) >> 5) | ((r & 0x000800) >> 4) | ((r & 0x000080) >> 1) |
           ((r & 0x000010) << 1) | ((r & 0x000004) << 2);
}

SID_INLINE uint32_t wave_output(const sid_wave_t *w)
{
    switch (w->waveform) {
    default:
    case 0x0: return w->held;               /* (0 unless the floating DAC is on) */
    case 0x1: return wave_out_T(w);
    case 0x2: return w->avg_on ? w->saw_avg : wave_out_S(w);
    case 0x3: if (w->comb_avg_on) return w->comb_avg;
              return (uint32_t)w->wave__ST[wave_out_S(w)] << 4;
    case 0x4: return w->avg_on ? w->pulse_avg : wave_out_P(w);
    case 0x5: case 0x6: case 0x7:
        if (w->comb_avg_on) return w->comb_avg;
        if (w->waveform == 0x5) return ((uint32_t)w->wave_P_T[wave_out_T(w) >> 1] << 4) & wave_out_P(w);
        if (w->waveform == 0x6) return ((uint32_t)w->wave_PS_[wave_out_S(w)] << 4) & wave_out_P(w);
        return ((uint32_t)w->wave_PST[wave_out_S(w)] << 4) & wave_out_P(w);
    case 0x8: return wave_out_N(w);
    /* Noise combined with other waveforms is not modeled in reSID 0.16. */
    case 0x9: case 0xa: case 0xb: case 0xc: case 0xd: case 0xe: case 0xf: return 0;
    }
}

/* the waveform's output at this cycle, not averaged (what the DAC holds when it is switched off) */
static uint32_t wave_output_now(const sid_wave_t *w)
{
    switch (w->waveform) {
    case 0x2: return wave_out_S(w);
    case 0x4: return wave_out_P(w);
    case 0x3: return (uint32_t)w->wave__ST[wave_out_S(w)] << 4;
    case 0x5: return ((uint32_t)w->wave_P_T[wave_out_T(w) >> 1] << 4) & wave_out_P(w);
    case 0x6: return ((uint32_t)w->wave_PS_[wave_out_S(w)] << 4) & wave_out_P(w);
    case 0x7: return ((uint32_t)w->wave_PST[wave_out_S(w)] << 4) & wave_out_P(w);
    default: return wave_output(w);
    }
}

/* ------------------------------------------------------------------------ */
/* Envelope generator (reSID envelope.h / envelope.cc)                      */
/* ------------------------------------------------------------------------ */

static const uint16_t env_rate_counter_period[16] = {
    9, 32, 63, 95, 149, 220, 267, 313, 392, 977, 1954, 3126, 3907, 11720, 19532, 31251
};
static const uint8_t env_sustain_level[16] = {
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
};

static void env_reset(sid_env_t *e)
{
    e->envelope_counter = 0;
    e->attack = e->decay = e->sustain = e->release = 0;
    e->gate = 0;
    e->rate_counter = 0;
    e->exponential_counter = 0;
    e->exponential_counter_period = 1;
    e->state = SID_ENV_RELEASE;
    e->rate_period = env_rate_counter_period[e->release];
    e->hold_zero = 1;
}

static void env_write_control(sid_env_t *e, uint32_t control)
{
    uint32_t gate_next = control & 0x01;
    if (!e->gate && gate_next) {
        e->state = SID_ENV_ATTACK;
        e->rate_period = env_rate_counter_period[e->attack];
        e->hold_zero = 0;
    } else if (e->gate && !gate_next) {
        e->state = SID_ENV_RELEASE;
        e->rate_period = env_rate_counter_period[e->release];
    }
    e->gate = gate_next;
}

static void env_write_attack_decay(sid_env_t *e, uint32_t v)
{
    e->attack = (v >> 4) & 0x0f;
    e->decay = v & 0x0f;
    if (e->state == SID_ENV_ATTACK) e->rate_period = env_rate_counter_period[e->attack];
    else if (e->state == SID_ENV_DECAY_SUSTAIN) e->rate_period = env_rate_counter_period[e->decay];
}

static void env_write_sustain_release(sid_env_t *e, uint32_t v)
{
    e->sustain = (v >> 4) & 0x0f;
    e->release = v & 0x0f;
    if (e->state == SID_ENV_RELEASE) e->rate_period = env_rate_counter_period[e->release];
}

SID_INLINE void env_clock(sid_env_t *e, int delta_t)
{
    /* ADSR delay bug: a rate period set below the counter makes it wrap at 0x8000. */
    int rate_step = (int)e->rate_period - (int)e->rate_counter;
    if (rate_step <= 0) rate_step += 0x7fff;

    while (delta_t) {
        if (delta_t < rate_step) {
            e->rate_counter += (uint32_t)delta_t;
            if (e->rate_counter & 0x8000) e->rate_counter = (e->rate_counter + 1) & 0x7fff;
            return;
        }
        e->rate_counter = 0;
        delta_t -= rate_step;

        if (e->state == SID_ENV_ATTACK || ++e->exponential_counter == e->exponential_counter_period) {
            e->exponential_counter = 0;
            if (e->hold_zero) {
                rate_step = (int)e->rate_period;
                continue;
            }
            switch (e->state) {
            case SID_ENV_ATTACK:
                e->envelope_counter = (e->envelope_counter + 1) & 0xff;
                if (e->envelope_counter == 0xff) {
                    e->state = SID_ENV_DECAY_SUSTAIN;
                    e->rate_period = env_rate_counter_period[e->decay];
                }
                break;
            case SID_ENV_DECAY_SUSTAIN:
                if (e->envelope_counter != env_sustain_level[e->sustain]) --e->envelope_counter;
                break;
            case SID_ENV_RELEASE:
                e->envelope_counter = (e->envelope_counter - 1) & 0xff;
                break;
            }
            switch (e->envelope_counter) {
            case 0xff: e->exponential_counter_period = 1; break;
            case 0x5d: e->exponential_counter_period = 2; break;
            case 0x36: e->exponential_counter_period = 4; break;
            case 0x1a: e->exponential_counter_period = 8; break;
            case 0x0e: e->exponential_counter_period = 16; break;
            case 0x06: e->exponential_counter_period = 30; break;
            case 0x00: e->exponential_counter_period = 1; e->hold_zero = 1; break;
            }
        }
        rate_step = (int)e->rate_period;
    }
}

/* ------------------------------------------------------------------------ */
/* Voice (reSID voice.h / voice.cc)                                         */
/* ------------------------------------------------------------------------ */

static void voice_set_model(sid_voice_t *v, sid_model_t model)
{
    wave_set_model(&v->wave, model);
    if (model == SID_MOS6581) {
        v->wave_zero = 0x380;       /* measured waveform D/A "zero" level */
        v->voice_DC = 0x800 * 0xff; /* envelope D/A DC offset */
    } else {
        v->wave_zero = 0x800;
        v->voice_DC = 0;
    }
}

/* Amplitude modulated waveform output, range [-2048*255, 2047*255]. */
SID_INLINE int32_t voice_output(const sid_voice_t *v)
{
    return ((int32_t)wave_output(&v->wave) - v->wave_zero) * (int32_t)v->envelope.envelope_counter + v->voice_DC;
}

/* ------------------------------------------------------------------------ */
/* Filter (reSID filter.h / filter.cc)                                      */
/* ------------------------------------------------------------------------ */

static void filter_set_w0(sid_filter_t *f)
{
    /* w0 = 2*pi*f0*1.048576, from the generated table. The ceilings keep the
     * one-cycle (16 kHz) and delta-cycle (4 kHz) integrators stable. */
    enum { W0_MAX_1 = 105414, W0_MAX_DT = 26353 }; /* (int)(2*pi*16000*1.048576), (int)(2*pi*4000*1.048576) */
    f->w0 = (int32_t)f->w0_table[f->fc];
    f->w0_ceil_1 = f->w0 <= W0_MAX_1 ? f->w0 : W0_MAX_1;
    f->w0_ceil_dt = f->w0 <= W0_MAX_DT ? f->w0 : W0_MAX_DT;
}

static void filter_set_Q(sid_filter_t *f)
{
    f->_1024_div_Q = sid_1024_div_q[f->res];
}

static void filter_set_model(sid_filter_t *f, sid_model_t model)
{
    if (model == SID_MOS6581) {
        f->mixer_DC = -0xfff * 0xff / 18 >> 7;
        f->w0_table = sid_w0_6581;
    } else {
        f->mixer_DC = 0;
        f->w0_table = sid_w0_8580;
    }
    filter_set_w0(f);
    filter_set_Q(f);
}

static void filter_reset(sid_filter_t *f)
{
    f->fc = 0; f->res = 0; f->filt = 0; f->voice3off = 0; f->hp_bp_lp = 0; f->vol = 0;
    f->Vhp = f->Vbp = f->Vlp = f->Vnf = 0;
    filter_set_w0(f);
    filter_set_Q(f);
}

SID_INLINE void filter_clock(sid_filter_t *f, int delta_t, int32_t voice1, int32_t voice2, int32_t voice3, int32_t ext_in)
{
    /* Scale each voice down from 20 to 13 bits. */
    voice1 >>= 7;
    voice2 >>= 7;
    /* Voice 3 is not silenced by voice3off if it is routed through the filter. */
    if (f->voice3off && !(f->filt & 0x04)) voice3 = 0;
    else voice3 >>= 7;
    ext_in >>= 7;

    if (!f->enabled) {
        f->Vnf = voice1 + voice2 + voice3 + ext_in;
        f->Vhp = f->Vbp = f->Vlp = 0;
        return;
    }

    /* Route voices into or around the filter. */
    int32_t Vi = 0;
    f->Vnf = 0;
    if (f->filt & 0x1) Vi += voice1; else f->Vnf += voice1;
    if (f->filt & 0x2) Vi += voice2; else f->Vnf += voice2;
    if (f->filt & 0x4) Vi += voice3; else f->Vnf += voice3;
    if (f->filt & 0x8) Vi += ext_in; else f->Vnf += ext_in;

    /* Maximum delta cycles for the filter to work satisfactorily under current
     * cutoff frequency and resonance constraints is approximately 8. */
    int delta_t_flt = 8;
    while (delta_t) {
        if (delta_t < delta_t_flt) delta_t_flt = delta_t;
        /* Vhp = Vbp/Q - Vlp - Vi; dVbp = -w0*Vhp*dt; dVlp = -w0*Vbp*dt */
        int32_t w0_delta_t = f->w0_ceil_dt * delta_t_flt >> 6;
        int32_t dVbp = w0_delta_t * f->Vhp >> 14;
        int32_t dVlp = w0_delta_t * f->Vbp >> 14;
        f->Vbp -= dVbp;
        f->Vlp -= dVlp;
        f->Vhp = (f->Vbp * f->_1024_div_Q >> 10) - f->Vlp - Vi;
        delta_t -= delta_t_flt;
    }
}

SID_INLINE int32_t filter_output(const sid_filter_t *f)
{
    if (!f->enabled) return (f->Vnf + f->mixer_DC) * (int32_t)f->vol;
    int32_t Vf = 0;
    if (f->hp_bp_lp & 0x1) Vf += f->Vlp;
    if (f->hp_bp_lp & 0x2) Vf += f->Vbp;
    if (f->hp_bp_lp & 0x4) Vf += f->Vhp;
    return (f->Vnf + Vf + f->mixer_DC) * (int32_t)f->vol;
}

/* ------------------------------------------------------------------------ */
/* External filter (reSID extfilt.h / extfilt.cc)                           */
/* ------------------------------------------------------------------------ */

static void extfilt_set_model(sid_extfilt_t *x, sid_model_t model)
{
    if (model == SID_MOS6581)
        x->mixer_DC = ((((0x800 - 0x380) + 0x800) * 0xff * 3 - 0xfff * 0xff / 18) >> 7) * 0x0f;
    else
        x->mixer_DC = 0;
}

static void extfilt_reset(sid_extfilt_t *x)
{
    x->Vlp = x->Vhp = x->Vo = 0;
}

SID_INLINE void extfilt_clock(sid_extfilt_t *x, int delta_t, int32_t Vi)
{
    if (!x->enabled) {
        x->Vlp = x->Vhp = 0;
        x->Vo = Vi - x->mixer_DC;
        return;
    }
    int delta_t_flt = 8;
    while (delta_t) {
        if (delta_t < delta_t_flt) delta_t_flt = delta_t;
        /* Vo = Vlp - Vhp; Vlp += w0lp*(Vi - Vlp)*dt; Vhp += w0hp*(Vlp - Vhp)*dt */
        int32_t dVlp = (x->w0lp * delta_t_flt >> 8) * (Vi - x->Vlp) >> 12;
        int32_t dVhp = x->w0hp * delta_t_flt * (x->Vlp - x->Vhp) >> 20;
        x->Vo = x->Vlp - x->Vhp;
        x->Vlp += dVlp;
        x->Vhp += dVhp;
        delta_t -= delta_t_flt;
    }
}

/* ------------------------------------------------------------------------ */
/* SID (reSID sid.h / sid.cc)                                               */
/* ------------------------------------------------------------------------ */

void sid_set_model(sid_t *s, sid_model_t model)
{
    s->model = model;
    for (int i = 0; i < 3; i++) voice_set_model(&s->voice[i], model);
    filter_set_model(&s->filter, model);
    extfilt_set_model(&s->extfilt, model);
}

void sid_set_sampling(sid_t *s, uint32_t clock_hz, uint32_t sample_hz)
{
    /* cycles_per_sample = clock/sample in 16.16, rounded to nearest; 32-bit safe. */
    uint32_t q = clock_hz / sample_hz, r = clock_hz % sample_hz;
    uint32_t frac = ((r << 16) + sample_hz / 2) / sample_hz;
    s->cycles_per_sample = (int32_t)((q << 16) + frac);
    s->sample_offset = 0;
}

void sid_enable_filter(sid_t *s, int enable) { s->filter.enabled = enable; }
void sid_set_averaging(sid_t *s, int enable)
{
    for (int i = 0; i < 3; i++) s->voice[i].wave.avg_on = enable ? 1u : 0u;
}
void sid_enable_extfilter(sid_t *s, int enable) { s->extfilt.enabled = enable; }

void sid_reset(sid_t *s)
{
    for (int i = 0; i < 3; i++) {
        wave_reset(&s->voice[i].wave);
        env_reset(&s->voice[i].envelope);
    }
    filter_reset(&s->filter);
    extfilt_reset(&s->extfilt);
    s->bus_value = 0;
    s->bus_value_ttl = 0;
}

void sid_init(sid_t *s)
{
    for (int i = 0; i < 3; i++) {
        s->voice[i].wave.sync_source = &s->voice[i].wave;
        s->voice[i].wave.sync_dest = &s->voice[i].wave;
    }
    /* Voice 1 syncs to 3, 2 to 1, 3 to 2. */
    s->voice[0].wave.sync_source = &s->voice[2].wave; s->voice[2].wave.sync_dest = &s->voice[0].wave;
    s->voice[1].wave.sync_source = &s->voice[0].wave; s->voice[0].wave.sync_dest = &s->voice[1].wave;
    s->voice[2].wave.sync_source = &s->voice[1].wave; s->voice[1].wave.sync_dest = &s->voice[2].wave;

    for (int i = 0; i < 3; i++) s->voice[i].wave.avg_on = s->voice[i].wave.comb_avg_on = s->voice[i].wave.float_on = 0;
    s->filter.enabled = 1;
    s->extfilt.enabled = 1;
    /* Low-pass: R = 10kOhm, C = 1000pF; high-pass: R = 1kOhm, C = 10uF; scaled by 1.048576. */
    s->extfilt.w0lp = 104858;
    s->extfilt.w0hp = 105;
    s->ext_in = 0;
    s->stems = 0;                           /* (stem and tap stay: the caller's buffers) */
    s->vmute[0] = s->vmute[1] = s->vmute[2] = 0;
    s->filter.w0_table = sid_w0_6581;
    s->filter.fc = 0;
    s->filter.res = 0;
    sid_reset(s);
    sid_set_model(s, SID_MOS6581);
    sid_set_sampling(s, 985248, 44100);
}

void sid_input(sid_t *s, int sample)
{
    /* Voice outputs are 20 bits; scale up to match three voices. */
    s->ext_in = (sample << 4) * 3;
}

int sid_output(const sid_t *s)
{
    /* 20-bit mixer output down to 16 bits, as reSID's output(16). */
    enum { RANGE = 1 << 16, HALF = RANGE >> 1, DIV = (4095 * 255 >> 7) * 3 * 15 * 2 / RANGE };
    int sample = s->extfilt.Vo / DIV;
    if (sample >= HALF) return HALF - 1;
    if (sample < -HALF) return -HALF;
    return sample;
}

uint8_t sid_read(sid_t *s, uint8_t reg)
{
    switch (reg) {
    case 0x19: case 0x1a: return 0xff;                         /* POT X/Y: not modeled */
    case 0x1b: return (uint8_t)(wave_output(&s->voice[2].wave) >> 4);
    case 0x1c: return (uint8_t)s->voice[2].envelope.envelope_counter;
    default: return (uint8_t)s->bus_value;
    }
}

void sid_write(sid_t *s, uint8_t reg, uint8_t value)
{
    s->bus_value = value;
    s->bus_value_ttl = 0x2000;
    if (reg < 0x15) {
        sid_voice_t *v = &s->voice[reg / 7];
        switch (reg % 7) {
        case 0: v->wave.freq = (v->wave.freq & 0xff00) | value; break;
        case 1: v->wave.freq = ((uint32_t)value << 8) | (v->wave.freq & 0x00ff); break;
        case 2: v->wave.pw = (v->wave.pw & 0xf00) | value; break;
        case 3: v->wave.pw = (((uint32_t)value << 8) & 0xf00) | (v->wave.pw & 0x0ff); break;
        case 4: wave_write_control(&v->wave, value); env_write_control(&v->envelope, value); break;
        case 5: env_write_attack_decay(&v->envelope, value); break;
        case 6: env_write_sustain_release(&v->envelope, value); break;
        }
        return;
    }
    sid_filter_t *f = &s->filter;
    switch (reg) {
    case 0x15: f->fc = (f->fc & 0x7f8) | (value & 0x007); filter_set_w0(f); break;
    case 0x16: f->fc = (((uint32_t)value << 3) & 0x7f8) | (f->fc & 0x007); filter_set_w0(f); break;
    case 0x17: f->res = (value >> 4) & 0x0f; filter_set_Q(f); f->filt = value & 0x0f; break;
    case 0x18: f->voice3off = value & 0x80; f->hp_bp_lp = (value >> 4) & 0x07; f->vol = value & 0x0f; break;
    default: break;
    }
}

/* the external low-pass over d cycles (in steps of 8, as extfilt_clock), with the input held: the
 * part of the gap to the input left after the steps, and before the last, in Q12 (w0lp fixed) */
#define ST_LPN 64
static int32_t st_lp_left[ST_LPN], st_lp_pre[ST_LPN];
static void st_lp_tables(void)
{
    int d;
    if (st_lp_left[0]) return;
    for (d = 0; d < ST_LPN; d++) {
        int32_t left = 4096, pre = 4096, dt = d, step = 8;
        while (dt) {
            if (dt < step) step = dt;
            pre = left;
            left -= left * (104858 * step >> 8) >> 12;
            dt -= step;
        }
        st_lp_left[d] = left; st_lp_pre[d] = pre;
    }
}

/* SiDJ: the stems. The filter is linear, so its state is the sum of each voice's own (what
 * that voice alone would have put in it, its tail after it is unrouted included). The voices
 * in it (st_act) all have their own state but one, the implicit one (st_imp), whose state is
 * the filter's less the others': one voice routed (most tunes) costs no second filter at all,
 * two cost one, three two. Voices 1 and 2 then go to the mixer, filtered or not, with their
 * share of its DC, times the volume, and through the external filter's low-pass: st_in, held
 * for the sample. Voice 3 is taken at the output (stems_sample). */
static void stems_clock(sid_t *s, int delta_t, const int32_t *vo)
{
    const sid_filter_t *f = &s->filter;
    int v, routed = f->enabled ? (int)(f->filt & 7) : 0;
    int act = s->st_act, imp = s->st_imp;
    if (!routed && !act) goto mixer;        /* nothing in the filter: straight to the mixer */
    for (v = 0; v < 3; v++)                 /* in: newly routed voices (from rest, as the filter
                                             * has none of them yet) */
        if ((routed >> v & 1) && !(act >> v & 1)) {
            act |= 1 << v;
            s->st_hp[v] = s->st_bp[v] = s->st_lp[v] = 0;
            if (v == 2 && imp >= 0 && imp != 2) {   /* voice 3 takes the implicit place: the old */
                int o;                              /* implicit voice's state made explicit */
                s->st_hp[imp] = f->Vhp; s->st_bp[imp] = f->Vbp; s->st_lp[imp] = f->Vlp;
                for (o = 0; o < 3; o++)
                    if (o != imp && o != 2 && (act >> o & 1)) {
                        s->st_hp[imp] -= s->st_hp[o]; s->st_bp[imp] -= s->st_bp[o]; s->st_lp[imp] -= s->st_lp[o];
                    }
                imp = 2;
            }
            if (imp < 0) imp = v;
        }
    for (v = 0; v < 3; v++) {               /* the explicit voices, filter_clock's loop with this one in */
        int32_t in;
        int dt = delta_t, step = 8;
        if (!(act >> v & 1) || v == imp) continue;
        in = (routed >> v & 1) ? vo[v] >> 7 : 0;
        if (v == 2 && f->voice3off && !(f->filt & 0x04)) in = 0;
        while (dt) {
            if (dt < step) step = dt;
            int32_t w0_delta_t = f->w0_ceil_dt * step >> 6;
            int32_t dVbp = w0_delta_t * s->st_hp[v] >> 14;
            int32_t dVlp = w0_delta_t * s->st_bp[v] >> 14;
            s->st_bp[v] -= dVbp;
            s->st_lp[v] -= dVlp;
            s->st_hp[v] = (s->st_bp[v] * f->_1024_div_Q >> 10) - s->st_lp[v] - in;
            dt -= step;
        }
    }
    if (imp >= 0) {                         /* the implicit voice: the filter less the others */
        s->st_hp[imp] = f->Vhp; s->st_bp[imp] = f->Vbp; s->st_lp[imp] = f->Vlp;
        for (v = 0; v < 3; v++)
            if (v != imp && (act >> v & 1)) {
                s->st_hp[imp] -= s->st_hp[v]; s->st_bp[imp] -= s->st_bp[v]; s->st_lp[imp] -= s->st_lp[v];
            }
    }
    for (v = 0; v < 3; v++)                 /* out: unrouted voices whose tail has died away */
        if ((act >> v & 1) && !(routed >> v & 1)) {
            int32_t m = s->st_hp[v] < 0 ? -s->st_hp[v] : s->st_hp[v];
            m += s->st_bp[v] < 0 ? -s->st_bp[v] : s->st_bp[v];
            m += s->st_lp[v] < 0 ? -s->st_lp[v] : s->st_lp[v];
            if (m < 16) {
                act &= ~(1 << v);
                if (imp == v) {             /* another voice in the filter becomes the implicit one */
                    int o;
                    imp = -1;
                    for (o = 2; o >= 0; o--) if (act >> o & 1) { imp = o; break; }
                }
            }
        }
    s->st_act = act;
    s->st_imp = imp;
mixer:
    for (v = 0; v < 2; v++) {               /* voices 1 and 2 to the mixer and the low-pass */
        int32_t out = (routed >> v & 1) ? 0 : vo[v] >> 7, e;
        if (act >> v & 1) {
            if (f->hp_bp_lp & 0x1) out += s->st_lp[v];
            if (f->hp_bp_lp & 0x2) out += s->st_bp[v];
            if (f->hp_bp_lp & 0x4) out += s->st_hp[v];
        }
        out = (out + f->mixer_DC / 3) * (int32_t)f->vol;
        e = out - s->st_lp2[v];             /* extfilt_clock's low-pass over delta_t, closed form: */
        if (delta_t < ST_LPN) {             /* what is left of the gap after the steps, and before */
            s->st_in[v] = out - ((e >> 6) * st_lp_pre[delta_t] >> 6);   /* the last (its output lags a step) */
            s->st_lp2[v] = out - ((e >> 6) * st_lp_left[delta_t] >> 6);
        } else {
            int dt = delta_t, step = 8;
            while (dt) {
                if (dt < step) step = dt;
                s->st_in[v] = s->st_lp2[v];
                s->st_lp2[v] += (s->extfilt.w0lp * step >> 8) * (out - s->st_lp2[v]) >> 12;
                dt -= step;
            }
        }
    }
}

/* at the output point, the 16-bit output o in hand: voices 1 and 2 through a DC blocker (the
 * external filter's 16 Hz high-pass, once a sample, in Q4) to 16 bits (x / 11 as x * 745 >> 13,
 * 0.03 % off: the output's own divide is exact); voice 3 is the output less the two, so the
 * three add up to the output exactly */
static void stems_sample(sid_t *s, int16_t o, int16_t *st)
{
    int v;
    int32_t r = o;
    for (v = 0; v < 2; v++) {
        int32_t x = s->st_in[v] * 16, y;   /* (a multiply: a negative value shifted left is undefined C) */
        s->st_ehp[v] += (x - s->st_ehp[v]) * s->st_k >> 16;
        y = ((x - s->st_ehp[v]) >> 4) * 745 >> 13;
        st[v] = (int16_t)(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
        r -= st[v];
    }
    st[2] = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
}

void sid_set_floating_dac(sid_t *s, int on)
{
    for (int i = 0; i < 3; i++) {
        s->voice[i].wave.float_on = on ? 1u : 0u;
        if (!on) s->voice[i].wave.held = 0;
    }
}

void sid_set_comb_averaging(sid_t *s, int on)
{
    for (int i = 0; i < 3; i++) s->voice[i].wave.comb_off = !on;
}

void sid_set_stems(sid_t *s, int on)
{
    int v;
    if (on && !s->stems)
    {
        for (v = 0; v < 3; v++) s->st_hp[v] = s->st_bp[v] = s->st_lp[v] = s->st_in[v] = s->st_lp2[v] = s->st_ehp[v] = 0;
        s->st_act = 0;                      /* the filter's present state goes to the implicit voice */
        s->st_imp = -1;
        for (v = 0; v < 3; v++) if (s->filter.filt >> v & 1) { s->st_act |= 1 << v; s->st_imp = v; }
        if (s->st_act && (s->st_act & (s->st_act - 1))) {   /* more than one: the filter's state is theirs */
            s->st_act = 1 << s->st_imp;     /* together; given to the implicit one, the others join from rest */
        }
    }
    /* the high-pass's step a sample: w0hp (100 rad/s, scaled by 1.048576) times the cycles of
     * a sample, in Q16 (about 149 at 44.1 kHz) */
    s->st_k = 105 * (s->cycles_per_sample >> 8) >> 12;
    st_lp_tables();
    s->stems = on;
}

void sid_clock(sid_t *s, int delta_t)
{
    if (delta_t <= 0) return;

    /* Age the bus value. */
    s->bus_value_ttl -= delta_t;
    if (s->bus_value_ttl <= 0) {
        s->bus_value = 0;
        s->bus_value_ttl = 0;
    }

    for (int i = 0; i < 3; i++) env_clock(&s->voice[i].envelope, delta_t);

    /* Clock the oscillators in steps that stop at every accumulator MSB toggle of
     * a sync source, so hard sync lands on the right cycle. */
    int delta_t_osc = delta_t;
    while (delta_t_osc) {
        int delta_t_min = delta_t_osc;
        for (int i = 0; i < 3; i++) {
            sid_wave_t *w = &s->voice[i].wave;
            if (!(w->sync_dest->sync && w->freq)) continue;
            uint32_t delta_accumulator = (w->accumulator & 0x800000 ? 0x1000000u : 0x800000u) - w->accumulator;
            int delta_t_next = (int)(delta_accumulator / w->freq);
            if (delta_accumulator % w->freq) ++delta_t_next;
            if (delta_t_next < delta_t_min) delta_t_min = delta_t_next;
        }
        for (int i = 0; i < 3; i++) wave_clock(&s->voice[i].wave, delta_t_min);
        for (int i = 0; i < 3; i++) wave_synchronize(&s->voice[i].wave);
        delta_t_osc -= delta_t_min;
    }

    int32_t vo[3] = {voice_output(&s->voice[0]), voice_output(&s->voice[1]), voice_output(&s->voice[2])};
    if (s->vmute[0]) vo[0] = 0;   /* SiDJ: muted at the output */
    if (s->vmute[1]) vo[1] = 0;
    if (s->vmute[2]) vo[2] = 0;
    filter_clock(&s->filter, delta_t, vo[0], vo[1], vo[2], s->ext_in);
    extfilt_clock(&s->extfilt, delta_t, filter_output(&s->filter));
    if (s->stems) stems_clock(s, delta_t, vo);
}

int sid_clock_render(sid_t *s, int *delta_t, int16_t *buf, int n, int interleave)
{
    enum { FIXP_SHIFT = 16, FIXP_MASK = 0xffff };
    int count = 0;
    for (;;) {
        int32_t next_sample_offset = s->sample_offset + s->cycles_per_sample + (1 << (FIXP_SHIFT - 1));
        int delta_t_sample = next_sample_offset >> FIXP_SHIFT;
        if (delta_t_sample > *delta_t) break;
        if (count >= n) return count;
        sid_clock(s, delta_t_sample);
        *delta_t -= delta_t_sample;
        s->sample_offset = (next_sample_offset & FIXP_MASK) - (1 << (FIXP_SHIFT - 1));
        if (s->tap) {                       /* the three voices, before the filter, for the scopes */
            s->tap[count * 3 + 0] = (int16_t)(voice_output(&s->voice[0]) >> 4);
            s->tap[count * 3 + 1] = (int16_t)(voice_output(&s->voice[1]) >> 4);
            s->tap[count * 3 + 2] = (int16_t)(voice_output(&s->voice[2]) >> 4);
        }
        {
            int16_t o = (int16_t)sid_output(s);
            if (s->stems && s->stem) stems_sample(s, o, s->stem + count * 3);
            buf[count++ * interleave] = o;
        }
    }
    sid_clock(s, *delta_t);
    s->sample_offset -= *delta_t << FIXP_SHIFT;
    *delta_t = 0;
    return count;
}

void sid_render(sid_t *s, int16_t *buf, int n)
{
    /* Enough cycles for n samples; sid_clock_render stops after n and leaves the rest. */
    int delta_t = 0x3fffffff;
    sid_clock_render(s, &delta_t, buf, n, 1);
}
