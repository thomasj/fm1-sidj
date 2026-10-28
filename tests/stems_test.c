/* SPDX-License-Identifier: GPL-3.0-only */
/* The SID engine's stems (sid_set_stems): the output is the same with them on as off, bit for
 * bit; the three stems add up to the output (to the rounding); and each stem is its voice:
 * against the same chip playing that voice alone, within -50 dB of the mix above 20 Hz (but for the
 * volume steps and voice 3's switching, the mixer's own sound), through a
 * resonant filter sweep with two voices routed, voice 3 off, and volume-register steps (the
 * samples' path), on both chip models. */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "sid.h"

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int solo_voice = -1;                 /* the script for one voice alone: the others never sound */

static void w(sid_t *s, int reg, int val)
{
    if (solo_voice >= 0 && reg < 0x15 && reg / 7 != solo_voice && reg % 7 >= 4) val = 0;
    sid_write(s, (uint8_t)reg, (uint8_t)val);
}

static void script(sid_t *s, int t)                 /* t: the 20 ms frame */
{
    if (t == 0) {
        static const uint8_t init[] = {0x00, 0x30, 0x00, 0x08, 0x41, 0x22, 0xa8,   /* v1 pulse */
                                       0x00, 0x12, 0, 0, 0x21, 0x11, 0xf9,         /* v2 saw */
                                       0x00, 0x60, 0, 0, 0x11, 0x00, 0xf0,         /* v3 triangle */
                                       0x00, 0x40, 0xf3, 0x1f};                    /* v1+v2 filtered, LP, vol 15 */
        for (int r = 0; r < 25; r++) w(s, r, init[r]);
    }
    w(s, 0x16, 0x10 + (t * 5) % 0xe0);                         /* the sweep */
    if (t == 40) w(s, 0x18, 0x9f);                                         /* voice 3 off, LP, vol 15 */
    if (t >= 60 && t < 80) w(s, 0x18, 0x10 | (t & 15));         /* volume steps */
    if (t == 80) { w(s, 0x18, 0x5f); w(s, 0x17, 0xc3); }          /* BP + LP */
    if (t % 8 == 0) { w(s, 0x04, 0x40); w(s, 0x0b, 0x20); }        /* gates off ... */
    if (t % 8 == 1) { w(s, 0x04, 0x41); w(s, 0x0b, 0x21); }        /* ... and on */
}

static void run(sid_model_t m, const char *name)
{
    static sid_t a, b, solo[4];
    static int16_t oa[882], ob[882], st[882 * 3], os[4][882];
    double se2[3] = {0}, ss2[3] = {0}, lp[3] = {0}; int maxerr = 0, differ = 0;
    sid_init(&a); sid_init(&b); sid_set_model(&a, m); sid_set_model(&b, m);
    b.stem = st; sid_set_stems(&b, 1);
    for (int v = 0; v < 4; v++) { sid_init(&solo[v]); sid_set_model(&solo[v], m); }
    for (int t = 0; t < 100; t++) {
        script(&a, t); script(&b, t);
        for (int v = 0; v < 4; v++) { solo_voice = v; script(&solo[v], t); solo_voice = -1; }   /* 3: all muted */
        sid_render(&a, oa, 882); sid_render(&b, ob, 882);
        for (int v = 0; v < 4; v++) sid_render(&solo[v], os[v], 882);
        for (int i = 0; i < 882; i++) {
            int e = abs(st[3 * i] + st[3 * i + 1] + st[3 * i + 2] - ob[i]);
            if (oa[i] != ob[i]) differ++;
            if (e > maxerr) maxerr = e;
            for (int v = 0; v < 3; v++) {       /* each stem against its voice alone (less the muted chip,
                                                 * whose DC is the 6581's mixer), above 20 Hz, away from the
                                                 * volume steps and voice 3 going off and on: there the 6581
                                                 * mixer thumps, the chip's own sound in the mix, no voice's */
                double ref = os[v][i] - os[3][i], d = st[3 * i + v] - ref;
                lp[v] += (d - lp[v]) * (2 * 3.14159265 * 20 / 44100);
                if (t >= 10 && (t < 40 || t >= 50) && (t < 58 || t >= 90)) { se2[v] += (d - lp[v]) * (d - lp[v]); ss2[v] += (double)ob[i] * ob[i]; }
            }
        }
    }
    printf("  %s: output with stems %s; stems vs output worst %d; each stem vs its voice alone: %.1f %.1f %.1f dB of the mix\n", name,
           differ ? "DIFFERS" : "unchanged", maxerr,
           10 * log10(se2[0] / ss2[0]), 10 * log10(se2[1] / ss2[1]), 10 * log10(se2[2] / ss2[2]));
    CHECK(!differ);
    CHECK(maxerr <= 3);                              /* the three roundings to 16 bits */
    for (int v = 0; v < 3; v++) CHECK(10 * log10(se2[v] / ss2[v]) < -50.0);
}

int main(void)
{
    run(SID_MOS6581, "6581");
    run(SID_MOS8580, "8580");
    printf(fails ? "stems_test FAIL\n" : "stems_test PASS\n");
    return fails != 0;
}
