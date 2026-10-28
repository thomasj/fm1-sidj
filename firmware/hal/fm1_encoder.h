/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments */
/* One FM-1 encoder: a quadrature decoder with detent counting, fed one sampled state per scan
 * frame (~1.1 ms). Pure: no hardware here, so tests/enc_test.c runs it on the host.
 *
 * An FM-1 detent is one full quadrature cycle (4 transitions) and the knob rests in one state:
 * the one seen at power-on. A step is emitted on arriving back at that state, the net
 * transitions since leaving it rounded to whole cycles (>= 2 counts as one: a lost transition
 * or two is forgiven, 6 to 9 count two, and so on, so a fast turn that crossed the rest state
 * between two samples still gives every click). Bounce and back-and-forth cancel out.
 *
 * The rest state is relearned only after ~1 s still in another state (a knob held off its
 * detent at power-on), never from the pauses of a slow turn: Felucca's first decoder learned
 * every state the knob stayed in for 25 ms, so a careful one-click turn, whose mid states last
 * longer than that, taught a mid state as the detent and the click at the real detent was lost
 * (Felucca #23). No two-sample filter either (X0X): a state that lasts one scan counts, a scan
 * is longer than a contact bounce, and a glitch cannot reach two net transitions.
 *
 * Two states in one sample (both lines changed) is a fast turn that skipped a state: it counts
 * two more in the direction of travel; when the direction is not known yet (the first move
 * away from the detent), it is held and counted with the next valid transition. */
#pragma once
#include <stdint.h>

#define FM1_ENC_REST_FRAMES 900u    /* ~1 s still off the rest state: that is the detent */

typedef struct {
    uint8_t prev, last;             /* the last decoded state, the last sampled state (0xFF: none yet) */
    uint8_t rest;                   /* the detent state, 0..3 */
    uint8_t skip;                   /* skipped-state jumps seen before the direction was known */
    uint16_t still;                 /* frames since the sampled state last changed */
    int8_t sub;                     /* net transitions since the rest state */
    int16_t steps;                  /* detent steps turned, + = clockwise, taken by the reader */
} fm1_enc_t;

static inline void fm1_enc_init(volatile fm1_enc_t *q)
{
    q->prev = q->last = 0xFF;
    q->rest = 0;
    q->skip = 0;
    q->still = 0;
    q->sub = 0;
    q->steps = 0;
}

/* one sampled state (A << 1 | B) per scan frame */
static inline void fm1_enc_feed(volatile fm1_enc_t *q, uint32_t cur)
{
    uint32_t idx;
    int sub;
    if (cur != q->last) {
        q->last = (uint8_t)cur;
        q->still = 0;
    }
    if (q->prev == 0xFF) {                          /* first frame: the knob rests here */
        q->prev = (uint8_t)cur;
        q->rest = (uint8_t)cur;
    }
    if (q->still < 0xFFFFu && ++q->still == FM1_ENC_REST_FRAMES && cur != q->rest) {
        q->rest = (uint8_t)cur;                     /* parked a long time off the detent: that is the detent */
        q->sub = 0;
        q->skip = 0;
    }
    if (cur == q->prev)
        return;
    idx = (uint32_t)q->prev << 2 | cur;
    sub = q->sub;
    if ((0x4182u >> idx) & 1u) {
        sub += 1 + 2 * q->skip;
        q->skip = 0;
    } else if ((0x2814u >> idx) & 1u) {
        sub -= 1 + 2 * q->skip;
        q->skip = 0;
    } else if (sub > 0) {                           /* a state skipped: two more the way it was going */
        sub += 2;
    } else if (sub < 0) {
        sub -= 2;
    } else if (q->skip < 8) {
        q->skip++;                                  /* direction not known yet: counted with the next move */
    }
    if (sub > 100 || sub < -100)
        sub = 0;                                    /* (never off the detent that long) */
    q->prev = (uint8_t)cur;
    if (cur == q->rest) {                           /* back on the detent: whole cycles, a lost transition or two forgiven */
        int n = sub < 0 ? -sub : sub;
        n = n >= 2 ? (n + 2) / 4 : 0;
        q->steps = (int16_t)(q->steps + (sub < 0 ? -n : n));
        sub = 0;
        q->skip = 0;
    }
    q->sub = (int8_t)sub;
}
