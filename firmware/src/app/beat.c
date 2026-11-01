/* SPDX-License-Identifier: GPL-3.0-only */
/* SiDJ's beat: see beat.h. */
#include "beat.h"

#define FIT_PCT 95u                         /* the share of note starts on the grid */
#define R_MAX 32u                           /* the longest row, in player calls */
#define SW_MAX 16u                          /* swing: each of the pair at most this */
#define TRIPLE_PCT 5u                       /* threes over fours: by this share of the notes */
#define ODD_PCT 30u                         /* odd rows nearly empty: fewer repeats three rows on */

void beat_reset(beat_t *b)
{
    uint32_t i;
    for (i = 0; i < BEAT_N; i++) b->on[i] = 0;
    b->first = 0;
    b->n = 0;
}

void beat_note(beat_t *b, uint32_t call)
{
    uint32_t n = b->n;
    if (n && b->on[(n - 1) % BEAT_N] == call) return;
    if (!n) b->first = call;
    b->on[n % BEAT_N] = call;
    b->n = n + 1;
}

static int fits(uint32_t on_grid, uint32_t m) { return on_grid * 100u >= FIT_PCT * m; }

static uint32_t repeats(const uint32_t *w, uint32_t m, uint32_t lag)   /* note starts with another one lag calls on */
{
    uint32_t i, j = 0, hits = 0;
    for (i = 0; i < m; i++) {
        while (j < m && w[j] < w[i] + lag) j++;
        if (j < m && w[j] == w[i] + lag) hits++;
    }
    return hits;
}

int beat_find(const beat_t *b, uint32_t calls_min, beat_grid_t *g)
{
    uint32_t w[BEAT_N], gap[BEAT_N], cnt[2 * SW_MAX + 2];
    uint32_t n = b->n, m = n < BEAT_N ? n : BEAT_N, i, j, r, ga = 0, gb = 0, gph = 0, rowlen2 = 0, beat;
    if (m < BEAT_MIN) return 0;
    for (i = 0; i < m; i++) w[i] = b->on[(n - m + i) % BEAT_N];   /* (the oldest first) */
    for (i = 1; i < m; i++) if (w[i] < w[i - 1]) return 0;        /* overwritten while copied: next time */
    /* even rows: the longest R that one phase of it holds nearly all the note starts */
    for (r = 2; r <= R_MAX; r++) {
        uint32_t best = 0, ph = 0;
        for (i = 0; i < r; i++) cnt[i] = 0;
        for (i = 0; i < m; i++) cnt[w[i] % r]++;
        for (i = 0; i < r; i++) if (cnt[i] > best) { best = cnt[i]; ph = i; }
        if (fits(best, m)) { ga = gb = r; gph = ph; rowlen2 = 2 * r; }
    }
    /* swing: rows a then b (b = a + 1 or a + 2, or twice a: a triplet shuffle), the pair repeating;
     * longer than the even row wins. Of
     * the readings that fit, the one with the most note starts on the pair's first row (the beat goes
     * there), then long before short (swing's beat is on the note before the long gap) */
    for (r = 2; r <= SW_MAX; r++) {
        uint32_t b2, t;
        for (t = 0; t < 3; t++) {
            uint32_t p, k, first, score = 0;
            b2 = t < 2 ? r + 1 + t : 2 * r;
            if (b2 > SW_MAX || (t == 2 && b2 <= r + 2)) continue;   /* (2 x 2 is a + 2) */
            p = r + b2;
            if (p <= rowlen2) continue;
            for (i = 0; i < p; i++) cnt[i] = 0;
            for (i = 0; i < m; i++) cnt[w[i] % p]++;
            for (k = 0; k < 2; k++) {
                first = k ? r : b2;
                for (i = 0; i < p; i++)
                    if (fits(cnt[i] + cnt[(i + first) % p], m) && cnt[i] * 2u + (k ? 0u : 1u) + 1u > score) {
                        score = cnt[i] * 2u + (k ? 0u : 1u) + 1u;
                        ga = first; gb = p - first; gph = i;
                    }
            }
            if (score) rowlen2 = p;
        }
    }
    if (!rowlen2) return 0;
    /* dense enough to show the grid: the middle gap between note starts at most 3 rows */
    for (i = 1; i < m; i++) {
        uint32_t d = w[i] - w[i - 1], k = i - 1;
        while (k > 0 && gap[k - 1] > d) { gap[k] = gap[k - 1]; k--; }
        gap[k] = d;
    }
    if (gap[(m - 1) / 2] * 2u > 3u * rowlen2) return 0;
    g->a = (uint8_t)ga; g->b = (uint8_t)gb; g->rsv = 0;
    /* in threes or fours: the notes' pattern repeats more after 6, 12 or 24 rows than after 4, 8, 16
     * or 32 (by TRIPLE_PCT of the notes): a beat of six rows (12/8), else four. Not when the odd rows
     * stay nearly empty (few repeats three rows on): the tune's real row is two of these, and in
     * those its six-row repeats are three-row ones, a 3 + 3 + 2 rhythm in four */
    {
        static const uint8_t rq[4] = {4, 8, 16, 32}, rt[3] = {6, 12, 24};
        uint32_t q = 0, t = 0, h;
        for (i = 0; i < 4; i++) { h = repeats(w, m, rq[i] * rowlen2 / 2u); if (h > q) q = h; }
        for (i = 0; i < 3; i++) { h = repeats(w, m, rt[i] * rowlen2 / 2u); if (h > t) t = h; }
        g->rpb2 = t * 100u >= q * 100u + TRIPLE_PCT * m ? 12 : 8;
        if (ga == gb && repeats(w, m, 3u * ga) * 100u < ODD_PCT * m) g->rpb2 = 8;
    }
    /* folded to 70..180 BPM (a swing pair kept whole within a beat, six rows at least three) */
    for (j = 0; j < 6; j++) {
        beat = calls_min * 40u / ((uint32_t)g->rpb2 * rowlen2);   /* BPM x 10 */
        if (beat > 1800u && g->rpb2 < 128) g->rpb2 *= 2;
        else if (beat < 700u && g->rpb2 > (ga == gb ? (g->rpb2 % 3 ? 1 : 6) : (g->rpb2 % 3 ? 4 : 12))) g->rpb2 /= 2;
        else break;
    }
    /* a bar line: the tune's first row when it is on the grid, else the window's first note on it */
    {
        uint32_t c = b->first, ok;
        for (i = 0;; i++) {
            uint32_t x = (c + rowlen2 * 64u - gph) % (ga == gb ? ga : rowlen2);
            ok = ga == gb ? x == 0 : x == 0 || x == ga;
            if (ok || i == m) break;
            c = w[i];
        }
        if (!ok) return 0;
        if (ga != gb && (c + rowlen2 * 64u - gph) % rowlen2 == ga) c = c >= ga ? c - ga : c + gb;   /* (a pair's start) */
        g->anchor = c;
    }
    /* which row of the beat is beat 1: the one the most note starts land on (a tune may open with a
     * pickup); rows a beat: rpb2 / 2, swing pairs: rpb2 / 4 (the beat stays on a pair's start) */
    {
        uint32_t step = ga == gb ? ga : rowlen2, n = ga == gb ? g->rpb2 / 2u : g->rpb2 / 4u;
        uint32_t beat = n * step, best = 0, at = g->anchor, j;
        if (n > 1) {
            for (j = 0; j < n; j++) {
                uint32_t c = g->anchor + j * step, hits = 0;
                for (i = 0; i < m; i++) if (w[i] >= c % beat && (w[i] - c % beat) % beat == 0) hits++;
                if (hits > best) { best = hits; at = c; }
            }
            g->anchor = at;
        }
    }
    return 1;
}
