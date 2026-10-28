/* SPDX-License-Identifier: GPL-3.0-only */
/* The SID's registers: three voices of seven registers, the filter, the mode and volume, then
 * four read-only ones; a chip repeats every 32 bytes from its base ($D400 for the first).
 *   SID_ADDR_*   a register, as an offset from the chip's base (an index into a register image)
 *   SID_CTRL_*   the bits of a voice's control register (SID_ADDR_CTRL)
 *   SID_FILT_*   the bits of the resonance and routing register (SID_ADDR_RES_FILT)
 *   SID_MODE_*   the bits of the filter mode and volume register (SID_ADDR_MODE_VOL)
 * and the counts and ranges, with the 16-bit fields read from a register image. */
#pragma once
#include <stdint.h>

#define SID_NVOICE 3
enum {                                      /* a voice's registers, from SID_ADDR_VOICE(v) */
    SID_ADDR_FREQ_LO, SID_ADDR_FREQ_HI,     /* the frequency, 16 bits */
    SID_ADDR_PW_LO, SID_ADDR_PW_HI,         /* the pulse width, 12 bits */
    SID_ADDR_CTRL,                          /* the waveforms, test, ring, sync, gate */
    SID_ADDR_AD, SID_ADDR_SR,               /* attack | decay, sustain | release: a nibble each */
    SID_VOICE_REGS                          /* 7 */
};
#define SID_ADDR_VOICE(v) ((v) * SID_VOICE_REGS)   /* voice v's first register, v = 0 .. 2 */

enum {
    SID_ADDR_FC_LO = 0x15,                  /* the cutoff's low 3 bits */
    SID_ADDR_FC_HI = 0x16,                  /* its high 8 */
    SID_ADDR_RES_FILT = 0x17,               /* the resonance (high nibble), the filter routing */
    SID_ADDR_MODE_VOL = 0x18,               /* voice 3 off, HP, BP, LP, the volume (low nibble) */
    SID_ADDR_POTX = 0x19, SID_ADDR_POTY = 0x1a,   /* read only: the paddles, */
    SID_ADDR_OSC3 = 0x1b, SID_ADDR_ENV3 = 0x1c    /* voice 3's oscillator and envelope */
};
#define SID_NWRITE 0x19                     /* the registers a tune writes: 0x00 .. 0x18 */
#define SID_NREG 0x20                       /* the window a chip answers in */

enum {                                      /* SID_ADDR_CTRL */
    SID_CTRL_GATE = 0x01, SID_CTRL_SYNC = 0x02, SID_CTRL_RING = 0x04, SID_CTRL_TEST = 0x08,
    SID_CTRL_TRI = 0x10, SID_CTRL_SAW = 0x20, SID_CTRL_PULSE = 0x40, SID_CTRL_NOISE = 0x80,
    SID_CTRL_WAVES = 0xf0                   /* the waveform field */
};
enum {                                      /* SID_ADDR_RES_FILT */
    SID_FILT_VOICES = 0x07,                 /* bit v: voice v through the filter */
    SID_FILT_EXT = 0x08,                    /* the external input through the filter */
    SID_FILT_RES_SHIFT = 4                  /* the resonance, 0 .. 15, in the high nibble */
};
enum {                                      /* SID_ADDR_MODE_VOL */
    SID_MODE_VOL = 0x0f,                    /* the volume */
    SID_MODE_LP = 0x10, SID_MODE_BP = 0x20, SID_MODE_HP = 0x40,
    SID_MODE_FILTERS = 0x70,                /* the filter type field */
    SID_MODE_3OFF = 0x80                    /* voice 3 silent (still running) */
};

#define SID_PW_MAX 0xfff
#define SID_FC_MAX 0x7ff                    /* 11 bits: FC_LO's low 3, FC_HI's 8 */
#define SID_FC_LO_BITS 3
#define SID_NIBBLE_MAX 15                   /* an envelope rate or level, the resonance, the volume */

/* the 16-bit fields from a register image */
static inline uint16_t sid_freq(const uint8_t *r, int v)
{
    return (uint16_t)(r[SID_ADDR_VOICE(v) + SID_ADDR_FREQ_LO] | r[SID_ADDR_VOICE(v) + SID_ADDR_FREQ_HI] << 8);
}
static inline uint16_t sid_pw(const uint8_t *r, int v)
{
    return (uint16_t)((r[SID_ADDR_VOICE(v) + SID_ADDR_PW_LO] | r[SID_ADDR_VOICE(v) + SID_ADDR_PW_HI] << 8) & SID_PW_MAX);
}
static inline uint16_t sid_fc(const uint8_t *r)
{
    return (uint16_t)((r[SID_ADDR_FC_LO] & ((1 << SID_FC_LO_BITS) - 1)) | r[SID_ADDR_FC_HI] << SID_FC_LO_BITS);
}
