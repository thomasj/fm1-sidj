/* SPDX-License-Identifier: GPL-3.0-only */
/* SiDJ song store: .sid files packed one after another in Felucca's user data area
 * (0xA0000 to 0xDBFFF, 240 KB), each entry 256-byte aligned: a 32-byte header, the HVSC song
 * lengths, the file. Deleting clears the header's live flag in place (a bit can go from 1 to 0
 * without an erase); new entries go after the last one, into blank flash; when the pool is
 * full the transfer tool wipes it and sends the live entries again. The index of live entries
 * is built at boot by walking the pool and read in place through the XIP window.
 *
 * And the SysEx transfer that fills it from a computer (tools/fm1_sid.py): manufacturer id 7D
 * (educational), 'S', a command byte and a 7-bit packed payload (7 data bytes -> 8 MIDI bytes).
 *
 * Shared with the host simulator: the flash primitives below (song_ptr, song_blank,
 * song_erase_sector, song_prog) are defined by the unity build over the flash driver and by the
 * simulator over RAM. Needs: ENG_FILE_MAX (engine.h), eng_post, ota_frame_get / ota_frame_done /
 * ota_wire_send (device) or their host stand-ins, songs_changed(), songs_play_request(). */
#define SONG_POOL_BASE 0xA0000u
#define SONG_POOL_END 0xDC000u
#define SONG_POOL_SIZE (SONG_POOL_END - SONG_POOL_BASE)
#define SONG_ALIGN 256u
#define SONG_SECTOR 4096u
#define SONG_MAGIC 0x50444953u              /* "SIDP" */
#define SONG_HDR 32u
#define SONG_MAX_LEN 64                     /* song lengths kept per file */
#define SONG_MAX_ENTRIES 400

typedef struct {
    uint32_t magic;
    uint32_t size;                          /* file bytes */
    uint8_t live;                           /* 0xFF live, 0x00 deleted */
    uint8_t nlen;                           /* song lengths that follow the header (uint16 deciseconds each) */
    uint8_t rsv[2];
    uint8_t md5[16];
    uint16_t crc;                           /* of the file, CCITT 0x1021, init 0 */
    uint16_t hcrc;                          /* of the header's first 30 bytes, live counted as 0xFF */
} song_hdr_t;

static const uint8_t *song_ptr(uint32_t off);                 /* read in place */
static int song_blank(uint32_t off, uint32_t n);              /* 1 = all 0xFF */
static int song_erase_sector(uint32_t off);                   /* 0 = ok */
static int song_prog(uint32_t off, const uint8_t *src, uint32_t n);   /* 0 = ok, verified */

static uint32_t song_index[SONG_MAX_ENTRIES];                 /* flash offsets of the live entries */
static int song_count;
static uint32_t song_end;                                     /* where the next entry goes */

static uint16_t song_crc16(const uint8_t *p, uint32_t n)
{
    uint32_t c = 0, i, k;
    for (i = 0; i < n; i++) {
        c ^= (uint32_t)p[i] << 8;
        for (k = 0; k < 8; k++)
            c = (c & 0x8000u) ? ((c << 1) ^ 0x1021u) : (c << 1);
        c &= 0xffffu;
    }
    return (uint16_t)c;
}

static uint32_t song_entry_bytes(const song_hdr_t *h)        /* header + lengths + file, aligned */
{
    uint32_t n = SONG_HDR + ((uint32_t)h->nlen * 2u + 3u) / 4u * 4u + h->size;
    return (n + SONG_ALIGN - 1u) / SONG_ALIGN * SONG_ALIGN;
}

/* the header's CRC leaves the live flag out (counted as 0xFF): a delete clears the flag in
 * place, and the entry must still read as a valid one so the scan steps over it */
static uint16_t song_hdr_crc(const song_hdr_t *h)
{
    song_hdr_t c = *h;
    c.live = 0xff;
    return song_crc16((const uint8_t *)&c, SONG_HDR - 2u);
}

static int song_hdr_ok(const song_hdr_t *h)
{
    return h->magic == SONG_MAGIC && h->size && h->size <= ENG_FILE_MAX && h->nlen <= SONG_MAX_LEN &&
           h->hcrc == song_hdr_crc(h);
}

static void songs_scan(void)
{
    uint32_t off = SONG_POOL_BASE;
    song_count = 0;
    while (off + SONG_HDR <= SONG_POOL_END) {
        const song_hdr_t *h = (const song_hdr_t *)song_ptr(off);
        uint32_t n;
        if (!song_hdr_ok(h)) break;
        n = song_entry_bytes(h);
        if (off + n > SONG_POOL_END) break;
        if (h->live == 0xff && song_count < SONG_MAX_ENTRIES) song_index[song_count++] = off;
        off += n;
    }
    song_end = off;
}

/* the live entry k: the file, its size, the lengths (deciseconds, nlen of them) */
static const uint8_t *song_file(int k, uint32_t *size, const uint16_t **len, int *nlen)
{
    const song_hdr_t *h;
    uint32_t lo;
    if (k < 0 || k >= song_count) return 0;
    h = (const song_hdr_t *)song_ptr(song_index[k]);
    lo = SONG_HDR + ((uint32_t)h->nlen * 2u + 3u) / 4u * 4u;
    if (size) *size = h->size;
    if (len) *len = (const uint16_t *)((const uint8_t *)h + SONG_HDR);
    if (nlen) *nlen = h->nlen;
    return (const uint8_t *)h + lo;
}

static int song_file_ok(int k)              /* the file as it was stored: its CRC matches the header's */
{
    uint32_t size;
    const uint8_t *f = song_file(k, &size, 0, 0);
    return f && song_crc16(f, size) == ((const song_hdr_t *)song_ptr(song_index[k]))->crc;
}

static const uint8_t *song_hvsc_name(int k)                   /* the file's name field (32 bytes, may lack a NUL) */
{
    uint32_t size;
    const uint8_t *f = song_file(k, &size, 0, 0);
    return f && size >= PSID_HDR_V1_SIZE ? f + PSID_HDR_NAME : 0;
}

static int song_delete(int k)                                 /* clear the live flag: a page rewritten with the bit off */
{
    uint8_t page[SONG_ALIGN];
    uint32_t off, i;
    if (k < 0 || k >= song_count) return -1;
    off = song_index[k];
    for (i = 0; i < SONG_ALIGN; i++) page[i] = song_ptr(off)[i];
    ((song_hdr_t *)page)->live = 0;
    if (song_prog(off, page, SONG_ALIGN)) return -2;
    songs_scan();
    return 0;
}

/* append a file: 0 = ok, -1 too big, -2 no room, -3 flash error */
static int song_put(const uint8_t *data, uint32_t size, const uint8_t *md5, const uint16_t *len, int nlen)
{
    song_hdr_t h;
    uint32_t off = song_end, need, i, lo;
    uint8_t lens[SONG_MAX_LEN * 2 + 4];
    if (!size || size > ENG_FILE_MAX || nlen > SONG_MAX_LEN) return -1;
    for (i = 0; i < sizeof h; i++) ((uint8_t *)&h)[i] = 0;
    h.magic = SONG_MAGIC; h.size = size; h.live = 0xff; h.nlen = (uint8_t)nlen;
    for (i = 0; i < 16; i++) h.md5[i] = md5 ? md5[i] : 0;
    h.crc = song_crc16(data, size);
    h.hcrc = song_hdr_crc(&h);
    need = song_entry_bytes(&h);
    /* the space must be blank: the rest of the last sector may hold stale bytes (an older
     * firmware's data), in which case the entry starts at the next sector, and the sectors it
     * covers are erased when they are not blank (nothing live is beyond song_end) */
    if (!song_blank(off, (off + need > SONG_POOL_END ? SONG_POOL_END : off + need) - off)) {
        uint32_t s = (off + SONG_SECTOR - 1u) / SONG_SECTOR * SONG_SECTOR;
        if (s > off && !song_blank(off, s - off)) off = s;
        for (s = off / SONG_SECTOR * SONG_SECTOR; s < off + need && s < SONG_POOL_END; s += SONG_SECTOR)
            if (s >= off && !song_blank(s, SONG_SECTOR) && song_erase_sector(s)) return -3;
    }
    if (off + need > SONG_POOL_END) return -2;
    lo = SONG_HDR + ((uint32_t)nlen * 2u + 3u) / 4u * 4u;
    for (i = 0; i < sizeof lens; i++) lens[i] = 0xff;
    for (i = 0; i < (uint32_t)nlen; i++) { lens[2 * i] = (uint8_t)len[i]; lens[2 * i + 1] = (uint8_t)(len[i] >> 8); }
    if (song_prog(off + lo, data, size)) return -3;
    if (nlen && song_prog(off + SONG_HDR, lens, lo - SONG_HDR)) return -3;
    if (song_prog(off, (const uint8_t *)&h, SONG_HDR)) return -3;   /* the header last: the commit */
    songs_scan();
    return 0;
}

static int song_wipe(void)                                    /* every sector of the pool */
{
    uint32_t s;
    for (s = SONG_POOL_BASE; s < SONG_POOL_END; s += SONG_SECTOR)
        if (!song_blank(s, SONG_SECTOR) && song_erase_sector(s)) return -1;
    songs_scan();
    return 0;
}

/* ------------------------------------------------- the SysEx transfer --- */
enum { SX_INFO = 1, SX_ENTRY, SX_PUT_BEGIN, SX_PUT_DATA, SX_PUT_END, SX_GET, SX_DELETE, SX_WIPE, SX_PLAY, SX_STATS, SX_OUTPUT, SX_RX, SX_USB };
static uint32_t sx_usb_at;                  /* SX_USB: when to reconnect (after the reply has gone), 0 none */

static uint8_t song_rx[ENG_FILE_MAX] SIDJ_POOL;             /* the file being received */
static uint32_t song_rx_size, song_rx_got;
static int song_rx_open, song_rx_nlen;
static uint8_t song_rx_md5[16];
static uint16_t song_rx_len[SONG_MAX_LEN];
static uint8_t sx_dec[400], sx_msg[520];

static uint32_t sx_pack7(const uint8_t *src, uint32_t n, uint8_t *dst)
{
    uint32_t i, o = 0;
    for (i = 0; i < n; i += 7) {
        uint32_t k = n - i < 7 ? n - i : 7, j, hi = 0;
        for (j = 0; j < k; j++) hi |= (uint32_t)(src[i + j] >> 7) << j;
        dst[o++] = (uint8_t)hi;
        for (j = 0; j < k; j++) dst[o++] = src[i + j] & 0x7f;
    }
    return o;
}
static uint32_t sx_unpack7(const uint8_t *src, uint32_t n, uint8_t *dst, uint32_t max)
{
    uint32_t i, o = 0;
    for (i = 0; i < n;) {
        uint32_t hi = src[i++], j;
        for (j = 0; j < 7 && i < n && o < max; j++) dst[o++] = (uint8_t)(src[i++] | ((hi >> j) & 1u) << 7);
    }
    return o;
}

static int song_multisid(const uint8_t *f, uint32_t n)        /* a 2SID / 3SID file: not taken */
{
    sidfile_t sf;
    return !sidfile_parse(&sf, f, n) && sidfile_nsids(&sf) > 1;
}

static void sx_reply(uint8_t cmd, const uint8_t *payload, uint32_t n)
{
    uint32_t len = 0;
    sx_msg[len++] = 0xF0; sx_msg[len++] = 0x7D; sx_msg[len++] = 'S'; sx_msg[len++] = cmd;
    len += sx_pack7(payload, n, sx_msg + len);
    sx_msg[len++] = 0xF7;
    ota_wire_send(sx_msg, len);
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

/* main loop, before ota_service: a frame of ours is handled and released, others are left */
static void songs_service(void)
{
    const uint8_t *p;
    uint32_t n, d, i;
    uint8_t cmd, out[300];
    if (sx_usb_at && (int32_t)(plat_ms() - sx_usb_at) >= 0) { sx_usb_at = 0; plat_usb_audio_mode(set.usb4); }
    if (!ota_frame_get(&p, &n)) return;
    if (n < 3 || p[0] != 0x7D || p[1] != 'S') return;      /* not ours: ota.c's turn */
    cmd = p[2];
    d = sx_unpack7(p + 3, n - 3, sx_dec, sizeof sx_dec);
    ota_frame_done();
    switch (cmd) {
    case SX_INFO:                                           /* -> pool size, used, entries (2), max file, version */
        wr32(out, SONG_POOL_SIZE); wr32(out + 4, song_end - SONG_POOL_BASE);
        out[8] = (uint8_t)song_count; out[9] = (uint8_t)(song_count >> 8);
        wr32(out + 10, ENG_FILE_MAX); out[14] = 1;
        sx_reply(0x40 | SX_INFO, out, 15);
        break;
    case SX_ENTRY: {                                        /* idx (2) -> status, size (4), nlen, lengths (2 each), name / author / released (32 each) */
        int k = d >= 2 ? sx_dec[0] | sx_dec[1] << 8 : -1, nlen = 0;
        uint32_t size = 0;
        const uint16_t *len = 0;
        const uint8_t *f = song_file(k, &size, &len, &nlen);
        out[0] = f ? 0 : 1;
        wr32(out + 1, size);
        out[5] = (uint8_t)nlen;
        n = 6;
        for (i = 0; i < (uint32_t)nlen; i++) { out[n++] = (uint8_t)len[i]; out[n++] = (uint8_t)(len[i] >> 8); }
        for (i = 0; i < PSID_INFO_LEN; i++) out[n++] = (f && size >= PSID_HDR_V1_SIZE) ? f[PSID_HDR_NAME + i] : 0;
        sx_reply(0x40 | SX_ENTRY, out, n);
        break;
    }
    case SX_PUT_BEGIN:                                      /* size (4), nlen, md5 (16), lengths (2 each) -> status */
        if (d < 21) { out[0] = 1; sx_reply(0x40 | SX_PUT_BEGIN, out, 1); break; }
        song_rx_size = rd32(sx_dec);
        song_rx_nlen = sx_dec[4] > SONG_MAX_LEN ? SONG_MAX_LEN : sx_dec[4];
        for (i = 0; i < 16; i++) song_rx_md5[i] = sx_dec[5 + i];
        for (i = 0; i < (uint32_t)song_rx_nlen && 21 + 2 * i + 1 < d; i++) song_rx_len[i] = (uint16_t)(sx_dec[21 + 2 * i] | sx_dec[22 + 2 * i] << 8);
        song_rx_got = 0;
        song_rx_open = song_rx_size && song_rx_size <= ENG_FILE_MAX;
        out[0] = song_rx_open ? 0 : 1;
        sx_reply(0x40 | SX_PUT_BEGIN, out, 1);
        break;
    case SX_PUT_DATA:                                       /* offset (3), bytes -> status, offset (3) */
        if (d < 3 || !song_rx_open) { out[0] = 1; sx_reply(0x40 | SX_PUT_DATA, out, 1); break; }
        {
            uint32_t off = sx_dec[0] | (uint32_t)sx_dec[1] << 8 | (uint32_t)sx_dec[2] << 16, len = d - 3;
            if (off + len > song_rx_size) { out[0] = 2; sx_reply(0x40 | SX_PUT_DATA, out, 1); break; }
            memcpy(song_rx + off, sx_dec + 3, len);
            if (off + len > song_rx_got) song_rx_got = off + len;
            out[0] = 0; out[1] = sx_dec[0]; out[2] = sx_dec[1]; out[3] = sx_dec[2];
            sx_reply(0x40 | SX_PUT_DATA, out, 4);
        }
        break;
    case SX_PUT_END:                                        /* crc (2) -> status: 0 ok, 1 no transfer, 2 bytes missing, 3 crc, 4 too big, 5 full, 6 flash,
                                                             * 7 two or three SIDs (the FM-1 plays one) */
        if (d < 2 || !song_rx_open) out[0] = 1;
        else if (song_rx_got != song_rx_size) out[0] = 2;
        else if (song_crc16(song_rx, song_rx_size) != (sx_dec[0] | (uint32_t)sx_dec[1] << 8)) out[0] = 3;
        else if (song_multisid(song_rx, song_rx_size)) out[0] = 7;
        else {
            int rc;
            plat_stop_for_flash();      /* fades out before the erases */
            rc = song_put(song_rx, song_rx_size, song_rx_md5, song_rx_len, song_rx_nlen);
            out[0] = rc == 0 ? 0 : rc == -1 ? 4 : rc == -2 ? 5 : 6;
        }
        song_rx_open = 0;
        sx_reply(0x40 | SX_PUT_END, out, 1);
        songs_changed();
        break;
    case SX_GET: {                                          /* idx (2), offset (3), len (2) -> offset (3), bytes */
        int k = d >= 7 ? sx_dec[0] | sx_dec[1] << 8 : -1;
        uint32_t size = 0, off, len;
        const uint8_t *f = song_file(k, &size, 0, 0);
        off = sx_dec[2] | (uint32_t)sx_dec[3] << 8 | (uint32_t)sx_dec[4] << 16;
        len = sx_dec[5] | (uint32_t)sx_dec[6] << 8;
        if (!f || off > size) { sx_reply(0x40 | SX_GET, out, 0); break; }
        if (len > 256) len = 256;
        if (off + len > size) len = size - off;
        out[0] = (uint8_t)off; out[1] = (uint8_t)(off >> 8); out[2] = (uint8_t)(off >> 16);
        memcpy(out + 3, f + off, len);
        sx_reply(0x40 | SX_GET, out, 3 + len);
        break;
    }
    case SX_DELETE:
        plat_stop_for_flash();      /* fades out before the erases */
        out[0] = (d >= 2) ? (song_delete(sx_dec[0] | sx_dec[1] << 8) ? 1 : 0) : 1;
        sx_reply(0x40 | SX_DELETE, out, 1);
        songs_changed();
        break;
    case SX_WIPE:                                           /* "WIPE" -> status */
        if (d >= 4 && sx_dec[0] == 'W' && sx_dec[1] == 'I' && sx_dec[2] == 'P' && sx_dec[3] == 'E') {
            plat_stop_for_flash();      /* fades out before the erases */
            out[0] = song_wipe() ? 1 : 0;
        } else out[0] = 2;
        sx_reply(0x40 | SX_WIPE, out, 1);
        songs_changed();
        break;
    case SX_STATS: {                                        /* -> load mean (2), peak (2) in 0.1 %, xruns (4), output, playing,
                                                             * loaded, SIDs, frames (4); USB audio: streaming (1), packets,
                                                             * underruns, overruns, missed, stalls (4 each); MASTER: lowest, highest
                                                             * reading (2 each), jumps, reads (4 each), gain (2); the C64's clock
                                                             * (1 NTSC), 0, tempo, vinyl (signed); the windows restart */
        uint32_t mean, peak, ua[6], ms[5], i;
        plat_cpu_stats(&mean, &peak);
        out[0] = (uint8_t)mean; out[1] = (uint8_t)(mean >> 8); out[2] = (uint8_t)peak; out[3] = (uint8_t)(peak >> 8);
        wr32(out + 4, plat_xruns());
        out[8] = set.output; out[9] = eng_playing; out[10] = eng_loaded; out[11] = (uint8_t)c64.nsid;
        wr32(out + 12, c64.frames);
        plat_usb_audio(ua);
        out[16] = (uint8_t)ua[0];
        for (i = 1; i < 6; i++) wr32(out + 13 + 4 * i, ua[i]);
        plat_master_stats(ms);
        out[37] = (uint8_t)ms[0]; out[38] = (uint8_t)(ms[0] >> 8); out[39] = (uint8_t)ms[1]; out[40] = (uint8_t)(ms[1] >> 8);
        wr32(out + 41, ms[2]); wr32(out + 45, ms[3]);
        out[49] = (uint8_t)ms[4]; out[50] = (uint8_t)(ms[4] >> 8);
        out[51] = eng_ntsc; out[52] = 0; out[53] = eng_tempo; out[54] = (uint8_t)eng_vinyl;
        plat_supplies(out + 55);            /* VDC14, SYSVDD levels */
        sx_reply(0x40 | SX_STATS, out, 57);
        break;
    }
    case SX_OUTPUT:                                         /* mode (0 SID, 1 ASID, 2 both) -> status; not saved */
        if (d >= 1 && sx_dec[0] <= 2) { set.output = sx_dec[0]; eng_post(EV_OUTPUT, sx_dec[0], 0, 0); out[0] = 0; }
        else out[0] = 1;
        sx_reply(0x40 | SX_OUTPUT, out, 1);
        break;
    case SX_USB:                                            /* USB audio: 0 the mix, 1 the mix and the voices, as SONGS'
                                                             * KNOB 3 (not saved) -> status; the FM-1 reconnects 200 ms later */
        if (d >= 1 && sx_dec[0] <= 1) {
            out[0] = 0;
            if (sx_dec[0] != set.usb4) { set.usb4 = sx_dec[0]; eng_post(EV_USB4, set.usb4, 0, 0); sx_usb_at = plat_ms() + 200u; }
        } else out[0] = 1;
        sx_reply(0x40 | SX_USB, out, 1);
        break;
    case SX_RX:                                             /* a remix event, as the panel posts it (for tests): param (RX_P_*,
                                                             * 255 the reset), voice, value (4, signed) -> status */
        if (d >= 6) {
            int32_t v = (int32_t)(sx_dec[2] | (uint32_t)sx_dec[3] << 8 | (uint32_t)sx_dec[4] << 16 | (uint32_t)sx_dec[5] << 24);
            out[0] = (uint8_t)(sx_dec[0] == 255 ? eng_post(EV_RX_RESET, 0, 0, 0) : eng_post(EV_RX, sx_dec[0], sx_dec[1], (uint32_t)v)) ? 1 : 0;
        } else out[0] = 2;
        sx_reply(0x40 | SX_RX, out, 1);
        break;
    case SX_PLAY:                                           /* idx (2), song -> status */
        out[0] = (d >= 3 && songs_play_request(sx_dec[0] | sx_dec[1] << 8, sx_dec[2])) ? 0 : 1;
        sx_reply(0x40 | SX_PLAY, out, 1);
        break;
    default:
        break;
    }
}
