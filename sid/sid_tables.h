/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Constant tables of the SiDJ SID engine, generated into sid_tables.c from reSID 0.16. */
#ifndef SIDJ_SID_TABLES_H
#define SIDJ_SID_TABLES_H
#include <stdint.h>

/* Combined waveforms, indexed by the 12-bit sawtooth (or triangle >> 1) output; 8-bit samples. */
extern const uint8_t sid_wave6581__ST[4096];
extern const uint8_t sid_wave6581_P_T[4096];
extern const uint8_t sid_wave6581_PS_[4096];
extern const uint8_t sid_wave6581_PST[4096];
extern const uint8_t sid_wave8580__ST[4096];
extern const uint8_t sid_wave8580_P_T[4096];
extern const uint8_t sid_wave8580_PS_[4096];
extern const uint8_t sid_wave8580_PST[4096];

/* Filter cutoff by the 11-bit FC register, as w0 = 2*pi*f0*1.048576 (reSID's fixed-point scale). */
extern const uint32_t sid_w0_6581[2048];
extern const uint32_t sid_w0_8580[2048];

/* 1024/Q for the 4-bit resonance register. */
extern const uint16_t sid_1024_div_q[16];

#endif
