/* SPDX-License-Identifier: GPL-3.0-only */
/* The remix layer: writes pass through unchanged with nothing set; a held key replaces the
 * voice's frequency and the tune's writes to it; octave, semitone (transpose) and fine offsets scale the
 * frequency; the waveform field freezes at the first toggle; a mute clears the waveform bits
 * and keeps the gate; the envelope nibbles override one at a time; the cutoff scale moves
 * both filter registers and never goes below zero, up to x16; the cutoff offset mode adds and
 * clips; the routing bits and the LP / BP / HP field override; a transposing key shifts the
 * tune's pitch on top of the offsets and leaves noise alone; noise keeps its pitch and is
 * never taken away by a frozen waveform; a reset returns to the tune's values. */
#include "../firmware/src/app/remix.h"
#include <stdio.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int find(const rx_out_t *o, int n, int reg)   /* the value emitted for reg, or -1 */
{
    int i;
    for (i = 0; i < n; i++) if (o[i].reg == reg) return o[i].val;
    return -1;
}

int main(void)
{
    remix_t rx;
    rx_out_t o[32];
    int n;
    remix_init(&rx);
    /* pass-through */
    n = remix_write(&rx, 0x00, 0x34, o); CHECK(n == 2 && o[0].reg == 0 && o[0].val == 0x34 && o[1].reg == 1 && o[1].val == 0);
    n = remix_write(&rx, 0x01, 0x12, o); CHECK(n == 1 && o[0].reg == 1 && o[0].val == 0x12);
    n = remix_write(&rx, 0x04, 0x41, o); CHECK(n == 1 && o[0].val == 0x41);
    n = remix_write(&rx, 0x04, 0x41, o); CHECK(n == 1);               /* a control write always passes */
    n = remix_write(&rx, 0x18, 0x0f, o); CHECK(n == 1 && o[0].val == 0x0f);
    CHECK(remix_tune_freq(&rx, 0) == 0x1234);
    /* a key: 440 Hz on PAL is 7493 in the frequency register (within a few units of rounding) */
    CHECK(remix_note_freq(69, 0) >= 7489 && remix_note_freq(69, 0) <= 7497);
    CHECK(remix_note_freq(69, 1) < remix_note_freq(69, 0));             /* NTSC: a lower register value */
    CHECK(remix_note_freq(81, 0) == 2 * remix_note_freq(69, 0) || remix_note_freq(81, 0) == 2 * remix_note_freq(69, 0) + 1);
    rx.v[0].key_on = 1; rx.v[0].key_freq = 0x1cd6;
    n = remix_refresh(&rx, 0, o, 32); CHECK(find(o, n, 0) == 0xd6 && find(o, n, 1) == 0x1c);   /* (never-written registers go out too, as 0) */
    n = remix_write(&rx, 0x00, 0x00, o); CHECK(n == 1 && o[0].reg == 0 && o[0].val == 0xd6);   /* the tune's write is replaced (forced for the written register) */
    CHECK(remix_tune_freq(&rx, 0) == 0x1200);                           /* but remembered */
    rx.v[0].key_on = 0;
    n = remix_refresh(&rx, 0, o, 32); CHECK(n == 2 && find(o, n, 0) == 0x00 && find(o, n, 1) == 0x12);   /* released: the tune's value returns */
    /* an octave up doubles, a semitone up is 2^(1/12) */
    rx.v[0].oct = 1;
    CHECK(remix_derive(&rx, 1) == 0x24 && remix_derive(&rx, 0) == 0x00);
    rx.v[0].oct = 0; rx.v[0].shift_on = 1; rx.v[0].shift_semi = 12;
    CHECK(remix_derive(&rx, 1) == 0x24);
    rx.v[0].shift_semi = -12;
    CHECK(remix_derive(&rx, 1) == 0x09);
    rx.v[0].shift_semi = 1;
    CHECK((remix_derive(&rx, 1) << 8 | remix_derive(&rx, 0)) == (0x1200 * 4340) / 4096);
    rx.v[0].shift_on = 0; rx.v[0].shift_semi = 0;
    /* fine: +/-50 cents is 2^(+/-50/1200) (4608 Hz-units: 4743.0 and 4476.8), on top of an octave */
    rx.v[0].fine = 50;
    CHECK((remix_derive(&rx, 1) << 8 | remix_derive(&rx, 0)) == 4743);
    rx.v[0].fine = -50;
    CHECK((remix_derive(&rx, 1) << 8 | remix_derive(&rx, 0)) == 4476);
    rx.v[0].fine = 1;
    CHECK((remix_derive(&rx, 1) << 8 | remix_derive(&rx, 0)) == 4610);    /* 4610.66 */
    rx.v[0].oct = 1;
    CHECK((remix_derive(&rx, 1) << 8 | remix_derive(&rx, 0)) == 9220);
    rx.v[0].oct = 0; rx.v[0].fine = 0;
    /* a chosen waveform replaces the tune's: the tune writes a sawtooth, the panel picks noise:
     * noise alone (not saw + noise), through later writes; the gate stays the tune's */
    remix_write(&rx, 0x04, 0x21, o);
    rx.v[0].wave_ovr = 1; rx.v[0].wave_val = SID_CTRL_NOISE;
    n = remix_refresh(&rx, 0, o, 32); CHECK(n == 1 && o[0].reg == 4 && o[0].val == 0x81);
    n = remix_write(&rx, 0x04, 0x40, o); CHECK(n == 1 && o[0].val == 0x80);   /* gate off from the tune, waveform from the override */
    /* sync and ring toggle on their own, frozen: ring on over the tune's sync-less pulse; the waveform still replaced */
    rx.v[0].sr_ovr = 1; rx.v[0].sr_val = SID_CTRL_RING;
    n = remix_write(&rx, 0x04, 0x43, o); CHECK(n == 1 && o[0].val == 0x85);   /* the tune's sync gone, ring on, noise, gate */
    rx.v[0].wave_ovr = 0;
    n = remix_write(&rx, 0x04, 0x41, o); CHECK(n == 1 && o[0].val == 0x45);   /* the waveform the tune's again, ring still frozen */
    rx.v[0].sr_ovr = 0;
    /* mute: waveform bits cleared, gate and sync kept */
    rx.v[0].mute = 1;
    n = remix_write(&rx, 0x04, 0x43, o); CHECK(n == 1 && o[0].val == 0x03);
    rx.v[0].mute = 0;
    /* envelope: attack alone */
    remix_write(&rx, 0x05, 0x29, o);
    rx.v[0].adsr_ovr = RX_ADSR_A; rx.v[0].a = 0xf;
    CHECK(remix_derive(&rx, 5) == 0xf9);
    n = remix_write(&rx, 0x05, 0x3a, o); CHECK(n == 1 && o[0].val == 0xfa);
    /* pulse width override, 12 bits across two registers */
    rx.v[1].pw_ovr = 1; rx.v[1].pw_val = 0x800;
    n = remix_refresh(&rx, 1, o, 32); CHECK(n >= 1);
    CHECK(remix_derive(&rx, 9) == 0x00 && remix_derive(&rx, 10) == 0x08);
    n = remix_write(&rx, 10, 0x01, o); CHECK(n == 1 && o[0].reg == 10 && o[0].val == 0x08);
    /* noise keeps its pitch: voice 3 plays pure noise at 0x1000; key, octave and semitone leave it */
    remix_write(&rx, 14, 0x00, o); remix_write(&rx, 15, 0x10, o); remix_write(&rx, 18, 0x81, o);
    rx.v[2].oct = 1; rx.v[2].shift_on = 1; rx.v[2].shift_semi = 3; rx.v[2].fine = 20;
    CHECK(remix_derive(&rx, 15) == 0x10 && remix_derive(&rx, 14) == 0x00);
    rx.v[2].fine = 0;
    rx.v[2].key_on = 1; rx.v[2].key_freq = 0x1cd6;
    CHECK(remix_derive(&rx, 15) == 0x10);
    /* a pulse: the key takes the pitch, and the frequency goes out with the control write, before it */
    n = remix_write(&rx, 18, 0x41, o); CHECK(n == 3 && o[0].reg == 14 && o[0].val == 0xd6 && o[1].reg == 15 && o[1].val == 0x1c && o[2].reg == 18 && o[2].val == 0x41);
    CHECK(remix_derive(&rx, 15) == 0x1c && remix_derive(&rx, 14) == 0xd6);
    n = remix_write(&rx, 18, 0x81, o); CHECK(n == 3 && o[0].val == 0x00 && o[1].val == 0x10 && o[2].val == 0x81);   /* back to noise: the tune's pitch returns */
    n = remix_write(&rx, 18, 0x41, o);
    rx.v[2].key_on = 0;
    CHECK(remix_derive(&rx, 15) == 0x26);                                /* an octave and three semitones up */
    rx.v[2].oct = 0; rx.v[2].shift_on = 0; rx.v[2].shift_semi = 0;
    /* pure noise is never taken away: a chosen sawtooth replaces a pulse (and pulse + noise),
     * leaves pure noise alone, and a voice with no waveform stays silent */
    rx.v[2].wave_ovr = 1; rx.v[2].wave_val = SID_CTRL_SAW;
    CHECK(remix_derive(&rx, 18) == 0x21);
    n = remix_write(&rx, 18, 0x81, o); CHECK(find(o, n, 18) == 0x81);   /* (the tune's pitch goes out with it: the key is gone) */
    n = remix_write(&rx, 18, 0xc1, o); CHECK(n == 1 && o[0].val == 0x21);
    n = remix_write(&rx, 18, 0x00, o); CHECK(n == 1 && o[0].val == 0x00);   /* no waveform at all: not touched */
    rx.v[2].wave_ovr = 0;
    /* the filter: cutoff 1000 x 1.25, resonance 5 - 2, the routing bits untouched */
    remix_write(&rx, 0x15, 1000 & 7, o); remix_write(&rx, 0x16, 1000 >> 3, o); remix_write(&rx, 0x17, 0x57, o);
    CHECK(rx.cutoff_scale == RX_CUTOFF_ONE);
    rx.cutoff_scale = 160; rx.res_off = -2;
    CHECK(((remix_derive(&rx, 0x16) << 3) | (remix_derive(&rx, 0x15) & 7)) == 1250);
    CHECK(remix_derive(&rx, 0x17) == 0x37);
    rx.cutoff_scale = 256;
    CHECK(((remix_derive(&rx, 0x16) << 3) | (remix_derive(&rx, 0x15) & 7)) == 2000);
    CHECK(RX_CUTOFF_MAX == 16 * RX_CUTOFF_ONE);                         /* up to x16 */
    remix_write(&rx, 0x16, 100 >> 3, o); remix_write(&rx, 0x15, 100 & 7, o);
    rx.cutoff_scale = RX_CUTOFF_MAX;
    CHECK(((remix_derive(&rx, 0x16) << 3) | (remix_derive(&rx, 0x15) & 7)) == 1600);
    /* the offset mode: added, clipped at both ends, the scale ignored */
    rx.cutoff_mode = RX_CUT_OFFSET; rx.cutoff_off = -300;
    CHECK(((remix_derive(&rx, 0x16) << 3) | (remix_derive(&rx, 0x15) & 7)) == 0);
    rx.cutoff_off = 250;
    CHECK(((remix_derive(&rx, 0x16) << 3) | (remix_derive(&rx, 0x15) & 7)) == 350);
    rx.cutoff_off = 2047;
    CHECK(((remix_derive(&rx, 0x16) << 3) | (remix_derive(&rx, 0x15) & 7)) == 2047);
    rx.cutoff_mode = RX_CUT_SCALE; rx.cutoff_off = 0;
    remix_write(&rx, 0x16, 1000 >> 3, o); remix_write(&rx, 0x15, 1000 & 7, o);
    rx.cutoff_scale = 256;
    CHECK(((remix_derive(&rx, 0x16) << 3) | (remix_derive(&rx, 0x15) & 7)) == 2000);
    remix_write(&rx, 0x16, 1500 >> 3, o); remix_write(&rx, 0x15, 1500 & 7, o);
    CHECK(((remix_derive(&rx, 0x16) << 3) | (remix_derive(&rx, 0x15) & 7)) == 2047);   /* clipped at the top */
    rx.cutoff_scale = 0;
    CHECK(((remix_derive(&rx, 0x16) << 3) | (remix_derive(&rx, 0x15) & 7)) == 0);      /* and never below zero */
    /* routing: the tune routes voices 1 and 3; voice 2 forced on, voice 3 forced off; the
     * resonance and the EXT bit untouched */
    remix_write(&rx, 0x17, 0x5d, o);                                    /* res 5, EXT, voices 1 and 3 */
    rx.res_off = 0;
    rx.v[1].filt_ovr = 1; rx.v[1].filt_val = 1;
    rx.v[2].filt_ovr = 1; rx.v[2].filt_val = 0;
    CHECK(remix_derive(&rx, 0x17) == 0x5b);
    n = remix_write(&rx, 0x17, 0x50, o); CHECK(n == 1 && o[0].val == 0x52);   /* the tune's later writes keep the overrides */
    rx.v[1].filt_ovr = 0; rx.v[2].filt_ovr = 0;
    /* filter type: LP + HP frozen over the tune's BP; volume and voice 3 off follow the tune */
    remix_write(&rx, 0x18, 0xaf, o);                                    /* 3OFF, BP, volume 15 */
    rx.fmode_ovr = 1; rx.fmode_val = 0x50;
    CHECK(remix_derive(&rx, 0x18) == 0xdf);
    n = remix_write(&rx, 0x18, 0x24, o); CHECK(n == 1 && o[0].val == 0x54);
    rx.fmode_ovr = 0;
    /* transpose: voice 1 at 0x1200 is shifted +12 (and +12 more with an octave offset); a key
     * in set mode wins over it; pure noise is left alone */
    remix_write(&rx, 0x00, 0x00, o); remix_write(&rx, 0x01, 0x12, o); remix_write(&rx, 0x04, 0x41, o);
    rx.v[0].shift_on = 1; rx.v[0].shift_semi = 12;
    CHECK(remix_derive(&rx, 1) == 0x24 && remix_derive(&rx, 0) == 0x00);
    rx.v[0].oct = 1;
    CHECK(remix_derive(&rx, 1) == 0x48);
    rx.v[0].oct = 0; rx.v[0].shift_semi = -13;
    CHECK((remix_derive(&rx, 1) << 8 | remix_derive(&rx, 0)) == ((0x1200u * 7732u) >> 12) >> 2);
    rx.v[0].shift_semi = 0;
    CHECK(remix_derive(&rx, 1) == 0x12 && remix_derive(&rx, 0) == 0x00);
    rx.v[0].shift_semi = 5; rx.v[0].key_on = 1; rx.v[0].key_freq = 0x1cd6;
    CHECK(remix_derive(&rx, 1) == 0x1c);
    rx.v[0].key_on = 0;
    remix_write(&rx, 0x04, 0x81, o);
    CHECK(remix_derive(&rx, 1) == 0x12);
    remix_write(&rx, 0x04, 0x41, o);
    rx.v[0].shift_on = 0;
    CHECK(remix_derive(&rx, 1) == 0x12);
    /* reset: everything back to the tune's values */
    remix_reset(&rx);
    n = remix_refresh(&rx, -1, o, 32);
    CHECK(rx.cutoff_scale == RX_CUTOFF_ONE && rx.cutoff_off == 0 && !rx.fmode_ovr && !rx.v[1].filt_ovr && !rx.v[0].shift_on);
    CHECK(remix_derive(&rx, 5) == 0x3a && remix_derive(&rx, 10) == 0x01 && remix_derive(&rx, 0x17) == 0x50);
    CHECK(((remix_derive(&rx, 0x16) << 3) | (remix_derive(&rx, 0x15) & 7)) == 1500);
    printf(fails ? "remix_test FAIL\n" : "remix_test PASS\n");
    return fails != 0;
}
