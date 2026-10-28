/* SPDX-License-Identifier: GPL-3.0-only
 * After X0X's x0x_web.c (charlesvestal/fm1-x0x, GPL-3.0-only). */
/* SiDJ in the browser: the host simulator (host/sidj_host.c: the whole app against a
 * simulated FM-1) compiled to WebAssembly and run in an AudioWorklet. The worklet asks for
 * audio; producing it advances the device's clock one millisecond at a time, which runs the
 * engine every 256 samples (the I2S half buffer) and the UI every 16 ms, as on the FM-1. The
 * page sends input and .sid files, and reads the screen, the lights, the saved objects and the
 * MIDI out stream (the ASID SysEx, for Web MIDI) back through these exports.
 *
 *   web/emu/build.sh   ->  build/emu/sidj.wasm (+ the page) */
#include <stdint.h>

#include "../../host/sidj_host.c"

#undef __attribute__
#define EXPORT __attribute__((used, visibility("default")))

/* output ring, stereo float, filled by run_ms, drained by web_render */
#define RING 4096u
static float ring[RING * 2];
static uint32_t ring_w, ring_r;
static void web_audio(const int32_t *blk, uint32_t n)
{
    uint32_t k;
    for (k = 0; k < n; k++) {
        uint32_t i = (ring_w++ % RING) * 2u;
        ring[i] = (float)blk[2 * k] / 8388608.0f;
        ring[i + 1] = (float)blk[2 * k + 1] / 8388608.0f;
    }
}

static float out_l[1024], out_r[1024];
EXPORT float *web_out_l(void) { return out_l; }
EXPORT float *web_out_r(void) { return out_r; }

/* n frames (<= 1024) into out_l / out_r: the device runs until they exist */
EXPORT void web_render(uint32_t n)
{
    uint32_t k;
    if (n > 1024u)
        n = 1024u;
    while (ring_w - ring_r < n)
        run_ms(1);
    for (k = 0; k < n; k++) {
        uint32_t i = (ring_r++ % RING) * 2u;
        out_l[k] = ring[i];
        out_r[k] = ring[i + 1];
    }
}

/* MIDI out: whole SysEx messages, as the FM-1 sends them over USB; the worklet takes them
 * every render quantum and the page passes them to the chosen Web MIDI output. A message that
 * does not fit is dropped whole (the page fell behind). */
static uint8_t mo_buf[8192];
static uint32_t mo_n, mo_dropped;
static void web_sysex(const uint8_t *m, int n)
{
    if (n <= 0) return;
    if (mo_n + (uint32_t)n > sizeof mo_buf) { mo_dropped++; return; }
    memcpy(mo_buf + mo_n, m, (size_t)n);
    mo_n += (uint32_t)n;
}
EXPORT uint8_t *web_midi_out(void) { return mo_buf; }
EXPORT uint32_t web_midi_out_len(void) { return mo_n; }
EXPORT void web_midi_out_clear(void) { mo_n = 0; }
EXPORT uint32_t web_midi_out_dropped(void) { return mo_dropped; }

EXPORT void web_boot(void)
{
    memset(pool, 0xff, sizeof pool);        /* blank flash; the page puts its songs back after */
    boot();
}
EXPORT void web_buttons(uint32_t m) { held_btn = m; }
EXPORT void web_keys(uint32_t m) { held_keys = m; }
EXPORT void web_enc(int role, int32_t n)
{
    if (role >= 0 && role < NE)
        enc_acc[role] += n;
}
EXPORT void web_master(uint32_t v) { master = v > 4096u ? 4096u : v; }
EXPORT uint32_t web_lit_buttons(void) { return lit_btn; }
EXPORT uint32_t web_lit_keys(void) { return lit_keys; }
EXPORT uint16_t *web_fb(void) { return fb; }
EXPORT uint32_t web_blits(void) { return blits; }
EXPORT uint32_t web_now(void) { return now_ms; }
EXPORT uint32_t web_cpu(void) { return cpu_pct; }

/* the song store: the page hands over a .sid file (web_song_buf: the bytes, web_song_lens: up
 * to SONG_MAX_LEN lengths in deciseconds, web_song_md5: 16 bytes) and puts it at the end of
 * the pool, as tools/fm1_sid.py does on the FM-1 */
static uint8_t web_song_bytes[ENG_FILE_MAX];
static uint16_t web_song_len[SONG_MAX_LEN];
static uint8_t web_song_md5_b[16];
EXPORT uint8_t *web_song_buf(void) { return web_song_bytes; }
EXPORT uint16_t *web_song_lens(void) { return web_song_len; }
EXPORT uint8_t *web_song_md5(void) { return web_song_md5_b; }
EXPORT uint32_t web_song_max(void) { return ENG_FILE_MAX; }
EXPORT uint32_t web_song_max_lens(void) { return SONG_MAX_LEN; }
/* 0 = kept; -1 not a .sid file, too big, or a 2SID / 3SID tune; else the pool is full */
EXPORT int web_song_put(uint32_t n, int nlen, int quiet)
{
    sidfile_t sf;
    int rc;
    if (!n || n > ENG_FILE_MAX || sidfile_parse(&sf, web_song_bytes, n) || sidfile_nsids(&sf) > 1) return -1;
    if (nlen < 0) nlen = 0;
    if (nlen > SONG_MAX_LEN) nlen = SONG_MAX_LEN;
    rc = song_put(web_song_bytes, n, web_song_md5_b, web_song_len, nlen);
    if (!rc && !quiet) songs_changed();
    return rc;
}
EXPORT void web_songs_changed(void) { songs_changed(); }
EXPORT int web_song_count(void) { return song_count; }
EXPORT int web_song_delete(int k)
{
    int rc = song_delete(k);
    songs_scan();
    songs_changed();
    return rc;
}
EXPORT void web_songs_clear(void)
{
    memset(pool, 0xff, sizeof pool);
    songs_scan();
    songs_changed();
}
EXPORT uint32_t web_pool_used(void) { return (uint32_t)(song_end - SONG_POOL_BASE); }
EXPORT uint32_t web_pool_size(void) { return SONG_POOL_SIZE; }

/* the song transfer: a SysEx frame in (the bytes between F0 and F7), the answers out (whole
 * messages); for the site's song module tested against the simulator (tests/site_test.mjs) */
EXPORT uint8_t *web_sx_in_buf(void) { return sxi_buf; }
EXPORT int web_sx_in(uint32_t n)
{
    if (sxi_ready || n > sizeof sxi_buf) return -1;
    sxi_n = n;
    sxi_ready = 1;
    return 0;
}
EXPORT uint8_t *web_sx_out(void) { return sxo_buf; }
EXPORT uint32_t web_sx_out_len(void) { return sxo_n; }
EXPORT void web_sx_out_clear(void) { sxo_n = 0; }
EXPORT uint32_t web_state(void) { return (uint32_t)eng_playing | (uint32_t)eng_loaded << 1 | (uint32_t)(ui.cur + 1) << 8; }

/* saved objects (the settings): the page keeps them (localStorage) and gives them back before boot */
EXPORT uint8_t *web_store(uint32_t obj) { return obj < OBJ_NOBJ ? store[obj] : 0; }
EXPORT int web_store_len(uint32_t obj) { return obj < OBJ_NOBJ ? store_len[obj] : -1; }
EXPORT void web_store_set_len(uint32_t obj, int n)
{
    if (obj < OBJ_NOBJ)
        store_len[obj] = n < 0 ? -1 : n > (int)PLAT_STORE_MAX ? (int)PLAT_STORE_MAX : n;
}
EXPORT uint32_t web_store_writes(void) { return store_writes; }
EXPORT uint32_t web_nobj(void) { return OBJ_NOBJ; }
EXPORT uint32_t web_store_max(void) { return PLAT_STORE_MAX; }
