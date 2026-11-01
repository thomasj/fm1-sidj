/* SPDX-License-Identifier: GPL-3.0-only */
/* SiDJ's beat (firmware/src/app/beat.c): grids found, folded and refused on made-up note starts.
 *   cc -o beat_test tests/beat_test.c firmware/src/app/beat.c && ./beat_test */
#include <stdio.h>
#include "../firmware/src/app/beat.h"

#define PAL_CALLS_MIN 3007u                 /* a frame-timed PAL tune: 50.125 player calls a second */
static int fails;
static void check(const char *what, int ok) { printf("  %s  %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }

/* note starts on rows of `row` calls from `start`, following a pattern of row steps (0 ends) */
static void feed(beat_t *b, uint32_t start, const uint8_t *steps, uint32_t row_a, uint32_t row_b, int rows)
{
    uint32_t c = start;
    int i, k = 0;
    for (i = 0; i < rows; i++) {
        uint32_t s = steps[k], j;
        if (!steps[++k]) k = 0;
        for (j = 0; j < s; j++) c += (i + j) % 2 ? row_b : row_a;   /* (each row its length: a, b, a, b ...) */
        beat_note(b, c);
        i += (int)s - 1;
    }
}

int main(void)
{
    static beat_t b;
    beat_grid_t g;
    static const uint8_t busy[] = {1, 1, 2, 1, 1, 1, 2, 3, 1, 1, 0};     /* mostly 16ths, some longer notes */
    static const uint8_t sparse[] = {11, 0};

    beat_reset(&b);
    beat_note(&b, 6);
    check("too few note starts: no grid", !beat_find(&b, PAL_CALLS_MIN, &g));

    beat_reset(&b);                                         /* Commando-like: rows of 6 frames */
    beat_note(&b, 0);
    feed(&b, 0, busy, 6, 6, 200);
    check("rows of 6 calls: found", beat_find(&b, PAL_CALLS_MIN, &g) && g.a == 6 && g.b == 6);
    check("rows of 6 calls: 125.3 BPM, a row a 16th", g.rpb2 == 8 && beat_bpm_x10(&g, PAL_CALLS_MIN) / 2 == 626);
    check("rows of 6 calls: the bar starts on the first row", g.anchor == 0);

    beat_reset(&b);                                         /* Monty-like: rows of 4 frames, folded to the half */
    feed(&b, 2, busy, 4, 4, 200);
    check("rows of 4 calls: 94.0 BPM (folded from 188)", beat_find(&b, PAL_CALLS_MIN, &g) && g.a == 4 && g.rpb2 == 16 &&
          beat_bpm_x10(&g, PAL_CALLS_MIN) == 939);

    beat_reset(&b);                                         /* swing: rows of 6 then 7 frames */
    feed(&b, 13, busy, 6, 7, 240);
    check("swing 6 / 7: found as a pair", beat_find(&b, PAL_CALLS_MIN, &g) && g.a + g.b == 13 && g.a != g.b);
    check("swing 6 / 7: 115.7 BPM", beat_bpm_x10(&g, PAL_CALLS_MIN) == 1156 || beat_bpm_x10(&g, PAL_CALLS_MIN) == 1157);

    beat_reset(&b);                                         /* the 4x of a CIA tune: 200 calls a second, rows of 24 */
    feed(&b, 0, busy, 24, 24, 200);
    check("4x speed, rows of 24 calls: 125 BPM", beat_find(&b, PAL_CALLS_MIN * 4, &g) && g.a == 24 && beat_bpm_x10(&g, PAL_CALLS_MIN * 4) / 2 == 626);

    beat_reset(&b);                                         /* The Last Ninja-like: a note every 11 rows of 2 */
    feed(&b, 4, sparse, 2, 2, 200);
    check("sparse note starts (a gap of 11 rows): no grid", !beat_find(&b, PAL_CALLS_MIN, &g));

    beat_reset(&b);                                         /* free timing: no grid */
    {
        uint32_t c = 0, x = 12345;
        int i;
        for (i = 0; i < 200; i++) { x = x * 1103515245u + 12345u; c += 3 + (x >> 16) % 9; beat_note(&b, c); }
    }
    check("note starts off any grid: no grid", !beat_find(&b, PAL_CALLS_MIN, &g));

    beat_reset(&b);                                         /* an intro off the grid, then rows of 6 */
    beat_note(&b, 1); beat_note(&b, 4);
    feed(&b, 30, busy, 6, 6, 200);
    check("an intro off the grid: found, the bar on the first row on it",
          beat_find(&b, PAL_CALLS_MIN, &g) && g.a == 6 && (g.anchor % 6) == 0 && g.anchor >= 30);

    beat_reset(&b);                                         /* a pickup, then notes on every other beat's half: */
    {                                                       /* beat 1 where most notes are, not on the pickup */
        uint32_t k;
        beat_note(&b, 13);
        for (k = 2; k < 160; k++) if (k % 2 == 0 || k % 4 == 1) beat_note(&b, 13 * k);
    }
    check("beat 1 on the half of the beat most notes are on (not a pickup's)",
          beat_find(&b, PAL_CALLS_MIN, &g) && (uint32_t)g.rpb2 * (g.a + g.b) / 4u == 26u && g.anchor % 26 == 0);

    beat_reset(&b);                                         /* swing, notes on both rows of each pair: read long then short */
    {
        uint32_t k;
        for (k = 1; k < 120; k++) { beat_note(&b, 13 * k); beat_note(&b, 13 * k + 7); }
    }
    check("swing with notes on both rows: 7 then 6 (the beat before the long row)",
          beat_find(&b, PAL_CALLS_MIN, &g) && g.a == 7 && g.b == 6 && g.anchor % 13 == 0);

    beat_reset(&b);                                         /* a triplet shuffle (TwinCross): rows of 5, notes on the first */
    {                                                       /* and third of each three: 10 then 5, 100 BPM, not straight 150 */
        uint32_t k;
        for (k = 0; k < 60; k++) { beat_note(&b, 4 + 15 * k); if (k % 4 != 3) beat_note(&b, 14 + 15 * k); }
    }
    check("triplet shuffle 10 / 5: found as a pair, 100.2 BPM",
          beat_find(&b, PAL_CALLS_MIN, &g) && g.a == 10 && g.b == 5 && beat_bpm_x10(&g, PAL_CALLS_MIN) == 1002 && g.anchor % 15 == 4);

    beat_reset(&b);                                         /* in threes (12/8): rows of 6, a figure every 6 rows */
    {                                                       /* (rows 0, 2, 4, 5): a beat of six rows, 83.5 BPM */
        uint32_t k;
        for (k = 0; k < 40; k++) { beat_note(&b, 36 * k); beat_note(&b, 36 * k + 12); beat_note(&b, 36 * k + 24); beat_note(&b, 36 * k + 30); }
    }
    check("in threes: six rows a beat, 83.5 BPM",
          beat_find(&b, PAL_CALLS_MIN, &g) && g.a == 6 && g.rpb2 == 12 && beat_bpm_x10(&g, PAL_CALLS_MIN) == 835);

    printf("%s\n", fails ? "beat_test FAIL" : "beat_test PASS");
    return fails != 0;
}
