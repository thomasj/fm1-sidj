/* SPDX-License-Identifier: GPL-3.0-only */
/* SiDJ's beat: the tracker rows of a playing tune, found from when its voices start to sound,
 * for a MIDI clock in time with the music. A tracker starts notes only on rows, every few player
 * calls, and a row is nearly always a 16th: so the note starts, counted in player calls, sit on a
 * grid. The grid is the longest row length (or a swing pair of row lengths, a then b) that 95 % of
 * the note starts land on, when they are dense enough to show it (the middle gap at most 3 rows).
 * Measured over HVSC: about two thirds of PSID tunes have one, mostly found within 8 seconds.
 * Included by engine.c (the note starts come from the audio interrupt); beat_find runs in the main
 * loop. Pure: no hardware, the host tests use it as it is. */
#pragma once
#include <stdint.h>

#define BEAT_N 128                          /* note starts kept: the latest are the window */
#define BEAT_MIN 24                         /* fewer than this: no grid yet */

typedef struct {
    uint32_t on[BEAT_N];                    /* note starts, in player calls from the song's start (a ring) */
    volatile uint32_t n;                    /* note starts so far */
    uint32_t first;                         /* the first one: the tune's first row */
} beat_t;

typedef struct {
    uint8_t a, b;                           /* player calls a row: a == b, or a swing pair, a then b */
    uint8_t rpb2;                           /* rows a beat, doubled (8: four rows, a row is a 16th; 12: six,
                                             * a tune in threes; folded so the beat is 70 to 180 BPM) */
    uint8_t rsv;
    uint32_t anchor;                        /* a bar line: the call of the row a bar starts on (the tune's
                                             * first row, or the first row of the window on the grid) */
} beat_grid_t;

void beat_reset(beat_t *b);
void beat_note(beat_t *b, uint32_t call);   /* a voice starts to sound (several at one call count once) */
/* the grid of the note starts so far; calls_min: player calls a minute (for the fold). 1 = found */
int beat_find(const beat_t *b, uint32_t calls_min, beat_grid_t *g);

/* what follows from a grid */
static inline uint32_t beat_bar_calls(const beat_grid_t *g) { return (uint32_t)g->rpb2 * (g->a + g->b); }   /* 4 beats */
static inline uint32_t beat_bpm_x10(const beat_grid_t *g, uint32_t calls_min)
{
    return calls_min * 40u / ((uint32_t)g->rpb2 * (g->a + g->b));   /* a beat: rpb2 * (a + b) / 4 calls */
}
