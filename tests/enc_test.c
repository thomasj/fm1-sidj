/* SPDX-License-Identifier: GPL-3.0-only */
/* The encoder decoder (firmware/hal/fm1_encoder.h) fed sampled states as the scan would see
 * them: one click = one step whether turned slowly (mid states lasting far longer than any
 * learning time), quickly (states skipped, the rest state crossed between samples) or with
 * bounce; back-and-forth gives nothing; a knob held off its detent for a second relearns. */
#include "../firmware/hal/fm1_encoder.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* the + direction per the decoder's table: 00 -> 01 -> 11 -> 10 -> 00 */
static const uint8_t CW[4] = { 0, 1, 3, 2 };

static fm1_enc_t q;
static void feed(uint32_t st, int frames) { while (frames-- > 0) fm1_enc_feed(&q, st); }
static int take(void) { int s = q.steps; q.steps = 0; return s; }
static void fresh(uint32_t rest) { fm1_enc_init(&q); feed(rest, 10); take(); }
/* a click: the three mid states then the rest state, `dwell` frames each (0 = one sample) */
static void click(int dir, int dwell)
{
    int i, at = 0;
    for (i = 0; i < 4; i++) if (CW[i] == q.rest) at = i;
    for (i = 1; i <= 4; i++) feed(CW[(at + dir * i + 8) % 4], dwell > 0 ? dwell : 1);
}

int main(void)
{
    int i;
    fresh(0);
    click(1, 1); CHECK(take() == 1);                          /* a quick click */
    click(-1, 1); CHECK(take() == -1);
    for (i = 0; i < 10; i++) click(1, 1);
    CHECK(take() == 10);
    /* a slow, careful click: every mid state held 300 frames (330 ms): still one step, and
     * the next quick click still one step (the old decoder learned the mid states as detents) */
    click(1, 300); CHECK(take() == 1);
    click(1, 1); CHECK(take() == 1);
    click(-1, 200); CHECK(take() == -1);
    click(-1, 1); CHECK(take() == -1);
    /* bounce on the detent, and a half click back and forth: nothing */
    feed(1, 1); feed(0, 1); feed(1, 1); feed(0, 5); CHECK(take() == 0);
    feed(1, 1); feed(3, 1); feed(1, 1); feed(0, 5); CHECK(take() == 0);
    feed(1, 1); feed(3, 1); feed(2, 1); feed(3, 1); feed(1, 1); feed(0, 5); CHECK(take() == 0);
    /* fast: a mid state skipped (both lines changed) */
    feed(1, 1); feed(2, 1); feed(0, 1); CHECK(take() == 1);   /* 00 01 [11] 10 00 */
    feed(3, 1); feed(2, 1); feed(0, 1); CHECK(take() == 1);   /* 00 [01] 11 10 00: the first move skipped */
    feed(2, 1); feed(1, 1); feed(0, 1); CHECK(take() == -1);  /* 00 10 [11] 01 00: the other way */
    feed(3, 1); feed(1, 1); feed(0, 1); CHECK(take() == -1);  /* 00 [10] 11 01 00 */
    /* fast: two clicks with the rest state crossed between samples */
    feed(1, 1); feed(3, 1); feed(2, 1); feed(1, 1); feed(3, 1); feed(2, 1); feed(0, 1); CHECK(take() == 2);
    feed(1, 1); feed(3, 1); feed(2, 1); feed(1, 1); feed(3, 1); feed(2, 1); feed(1, 1); feed(3, 1); feed(2, 1); feed(0, 1);
    CHECK(take() == 3);
    /* a lost transition on the way: 3 of 4 seen, still one click */
    feed(1, 1); feed(2, 1); feed(0, 1); CHECK(take() == 1);
    /* a glitch that jumps across and back cannot make a step */
    feed(3, 1); feed(0, 1); CHECK(take() == 0);
    feed(3, 1); feed(0, 1); feed(3, 1); feed(0, 1); CHECK(take() == 0);
    /* the rest state learned at power-on wherever the knob sits */
    fresh(3);
    click(1, 1); CHECK(take() == 1);
    click(-1, 1); CHECK(take() == -1);
    /* held off the detent for a second (power-on between detents): relearned, then clicks count from there */
    fresh(0);
    feed(1, 1); feed(3, 1);
    feed(3, FM1_ENC_REST_FRAMES + 10);
    CHECK(q.rest == 3 && take() == 0);
    click(1, 1); CHECK(take() == 1);
    click(1, 50); CHECK(take() == 1);
    /* but a mid state held just under that is not learned */
    fresh(0);
    feed(1, FM1_ENC_REST_FRAMES - 1); feed(3, 1); feed(2, 1); feed(0, 1);
    CHECK(q.rest == 0 && take() == 1);
    printf(fails ? "enc_test FAILED (%d)\n" : "enc_test PASS\n", fails);
    return fails != 0;
}
