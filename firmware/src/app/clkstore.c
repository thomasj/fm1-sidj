/* SPDX-License-Identifier: GPL-3.0-only */
/* SiDJ's MIDI clock, kept per song: the tempo a song (and subsong) was found to have, and once it is
 * put right on the GLO page (KNOB 4 its tempo's ratio, KNOB 3 its offset) both settings too (with
 * no tempo found, KNOB 4 sets one by hand: kept until it is turned down to none), and the song's
 * next load in SID+CLK takes them instead of looking for its tempo again. Both turned back to their
 * own (x1, 0 ms) forgets the song: its next load looks again. Kept in OBJ_SOUND (unused by SiDJ otherwise), by the
 * file's MD5 and the subsong, so they stay through Tidy up and a setlist loaded again; written at
 * the next quiet moment (a flash write stops the audio). Included by the unity build and the host
 * after songs.c, before ui.c. */
#define CS_MAGIC 0x5453434Bu                /* "KCST" */

typedef struct {
    uint8_t md5[6], sub, ratio;             /* the song: its file's MD5 (the start), the subsong; the ratio */
    int16_t offset_ms;
    uint16_t tick, call, off, bpm_x10;      /* the tempo found (eng_clk_grid_t) */
    uint32_t base;
} cs_rec_t;
#define CS_RECS ((PLAT_STORE_MAX - 8u) / sizeof(cs_rec_t))

static struct { uint32_t magic, count; cs_rec_t r[CS_RECS]; } cs_tab SIDJ_POOL;
static uint8_t cs_dirty;

static void cs_init(void)
{
    if (plat_store_load(OBJ_SOUND, &cs_tab, sizeof cs_tab) < 8 || cs_tab.magic != CS_MAGIC || cs_tab.count > CS_RECS) {
        cs_tab.magic = CS_MAGIC;
        cs_tab.count = 0;
    }
}

static int cs_find(int k, int sub)          /* the record of song entry k, subsong sub, or -1 */
{
    const uint8_t *md5;
    uint32_t i;
    if (k < 0 || k >= song_count) return -1;
    md5 = ((const song_hdr_t *)song_ptr(song_index[k]))->md5;
    for (i = 0; i < cs_tab.count; i++) {
        const cs_rec_t *r = &cs_tab.r[i];
        if (r->sub == (uint8_t)sub && r->md5[0] == md5[0] && r->md5[1] == md5[1] && r->md5[2] == md5[2] &&
            r->md5[3] == md5[3] && r->md5[4] == md5[4] && r->md5[5] == md5[5]) return (int)i;
    }
    return -1;
}

static int cs_preset(int k, int sub)        /* the song's kept clock for its next load: 1 = there is one */
{
    int i = cs_find(k, sub);
    const cs_rec_t *r;
    if (i < 0) return 0;
    r = &cs_tab.r[i];
    eng_clk_preset.ratio = r->ratio; eng_clk_preset.offset_ms = r->offset_ms;
    eng_clk_preset.tick = r->tick; eng_clk_preset.call = r->call; eng_clk_preset.off = r->off;
    eng_clk_preset.bpm_x10 = r->bpm_x10; eng_clk_preset.base = r->base;
    eng_clk_preset.valid = 1;
    return 1;
}

static void cs_remember(int k, int sub)     /* the clock as it is now, for the song (both at their own: forgotten) */
{
    eng_clk_grid_t g;
    cs_rec_t r;
    int i = cs_find(k, sub);
    uint32_t j;
    const uint8_t *md5;
    if (i >= 0) {
        for (j = (uint32_t)i; j + 1 < cs_tab.count; j++) cs_tab.r[j] = cs_tab.r[j + 1];
        cs_tab.count--;
        cs_dirty = 1;
    }
    if (k < 0 || k >= song_count) return;
    if (eng_clk_manual) {                   /* a tempo set by hand (tick 0; the engine may not have it yet) */
        g.ratio = ENG_CLK_X1; g.offset_ms = eng_clk_offset_ms;
        g.tick = g.call = g.off = 0; g.bpm_x10 = eng_clk_bpm_x10; g.base = 0;
    } else if (!eng_clk_get(&g) || !g.tick || (g.ratio == ENG_CLK_X1 && g.offset_ms == 0)) return;
    if (cs_tab.count == CS_RECS) {          /* full: the oldest goes */
        for (j = 0; j + 1 < cs_tab.count; j++) cs_tab.r[j] = cs_tab.r[j + 1];
        cs_tab.count--;
    }
    md5 = ((const song_hdr_t *)song_ptr(song_index[k]))->md5;
    for (j = 0; j < 6; j++) r.md5[j] = md5[j];
    r.sub = (uint8_t)sub; r.ratio = g.ratio; r.offset_ms = g.offset_ms;
    r.tick = g.tick; r.call = g.call; r.off = g.off; r.bpm_x10 = g.bpm_x10; r.base = g.base;
    cs_tab.r[cs_tab.count++] = r;
    cs_dirty = 1;
}

static void cs_found(int k, int sub)        /* a load found the song's tempo: kept (as it is, if it already was) */
{
    eng_clk_grid_t g;
    cs_rec_t r;
    uint32_t j;
    const uint8_t *md5;
    if (k < 0 || k >= song_count || cs_find(k, sub) >= 0 || !eng_clk_get(&g) || !g.tick) return;
    if (cs_tab.count == CS_RECS) {          /* full: the oldest goes */
        for (j = 0; j + 1 < cs_tab.count; j++) cs_tab.r[j] = cs_tab.r[j + 1];
        cs_tab.count--;
    }
    md5 = ((const song_hdr_t *)song_ptr(song_index[k]))->md5;
    for (j = 0; j < 6; j++) r.md5[j] = md5[j];
    r.sub = (uint8_t)sub; r.ratio = g.ratio; r.offset_ms = g.offset_ms;
    r.tick = g.tick; r.call = g.call; r.off = g.off; r.bpm_x10 = g.bpm_x10; r.base = g.base;
    cs_tab.r[cs_tab.count++] = r;
    cs_dirty = 1;
}

static void cs_service(void)                /* quiet: what changed goes to flash */
{
    if (cs_dirty && !eng_playing && !eng_file.pending &&
        plat_store_save(OBJ_SOUND, &cs_tab, 8u + cs_tab.count * (uint32_t)sizeof(cs_rec_t)) == 0)
        cs_dirty = 0;
}
