/* SPDX-License-Identifier: GPL-3.0-only */
/* The SID file format, see sidfile.h. Header fields are big-endian. */
#include "sidfile.h"

static uint32_t be16(const uint8_t *p) { return (uint32_t)p[0] << 8 | p[1]; }
static uint32_t be32(const uint8_t *p) { return be16(p) << 16 | be16(p + 2); }

static void str32(char *dst, const uint8_t *src)
{
    int i;
    for (i = 0; i < PSID_STR_LEN && src[i]; i++) dst[i] = (char)src[i];
    dst[i] = 0;
}

/* a second or third SID's base from its header byte ($Dxx0): an even value from $42 up, not in
 * $80 .. $DF ($D800 .. $DDFF: colour RAM, the CIAs); 0 = none */
static uint16_t extra_sid(uint8_t b)
{
    if (b < 0x42 || b > 0xfe || (b & 1) || (b >= 0x80 && b <= 0xdf)) return 0;
    return (uint16_t)(0xd000 | (uint32_t)b << 4);
}

int sidfile_parse(sidfile_t *f, const uint8_t *buf, uint32_t len)
{
    uint32_t off, i;
    for (i = 0; i < sizeof *f; i++) ((uint8_t *)f)[i] = 0;
    if (len < PSID_HDR_V1_SIZE) return -1;
    if (buf[0] == 'P' && buf[1] == 'S' && buf[2] == 'I' && buf[3] == 'D') f->rsid = 0;
    else if (buf[0] == 'R' && buf[1] == 'S' && buf[2] == 'I' && buf[3] == 'D') f->rsid = 1;
    else return -2;
    f->version = (uint16_t)be16(buf + PSID_HDR_VERSION);
    off = be16(buf + PSID_HDR_DATA);
    if (f->version < 1 || f->version > 4) return -3;
    if ((f->version == 1 && off != PSID_HDR_V1_SIZE) || (f->version >= 2 && off != PSID_HDR_V2_SIZE) || off > len) return -3;
    f->load = (uint16_t)be16(buf + PSID_HDR_LOAD);
    f->init = (uint16_t)be16(buf + PSID_HDR_INIT);
    f->play = (uint16_t)be16(buf + PSID_HDR_PLAY);
    f->songs = (uint16_t)be16(buf + PSID_HDR_SONGS);
    f->start_song = (uint16_t)be16(buf + PSID_HDR_START_SONG);
    f->speed = be32(buf + PSID_HDR_SPEED);
    str32(f->name, buf + PSID_HDR_NAME);
    str32(f->author, buf + PSID_HDR_AUTHOR);
    str32(f->released, buf + PSID_HDR_RELEASED);
    if (f->version >= 2) {
        f->flags = (uint16_t)be16(buf + PSID_HDR_FLAGS);
        f->start_page = buf[PSID_HDR_START_PAGE];
        f->page_len = buf[PSID_HDR_PAGE_LEN];
        if (f->version >= 3) f->sid2 = extra_sid(buf[PSID_HDR_SID2]);
        if (f->version >= 4) f->sid3 = extra_sid(buf[PSID_HDR_SID3]);
        if (f->rsid) f->basic = (f->flags & PSID_FLAGS_PLAYSID_BASIC) != 0;
        else f->playsid = (f->flags & PSID_FLAGS_PLAYSID_BASIC) != 0;
    }
    if (f->songs == 0) f->songs = 1;
    if (f->songs > 256) f->songs = 256;
    if (f->start_song == 0 || f->start_song > f->songs) f->start_song = 1;
    f->data = buf + off;
    f->data_len = len - off;
    if (f->load == 0) {
        if (f->data_len < 2) return -4;
        f->load = (uint16_t)(f->data[0] | f->data[1] << 8);
        f->data += 2;
        f->data_len -= 2;
    }
    if (f->data_len == 0 || (uint32_t)f->load + f->data_len > 65536u) return -4;
    if (f->rsid && (f->play != 0 || f->load < RSID_LOAD_MIN)) return -4;
    if (f->init == 0) f->init = f->load;
    return 0;
}

int sidfile_cia(const sidfile_t *f, int song)
{
    int bit;
    if (f->rsid) return 1;
    if (song < 1) song = 1;
    bit = f->version >= 2 && !f->playsid ? (song > 32 ? 31 : song - 1) : (song - 1) % 32;
    return (int)((f->speed >> bit) & 1u);
}

int sidfile_clock(const sidfile_t *f) { return f->version >= 2 ? (f->flags >> PSID_FLAGS_CLOCK_SHIFT) & 3 : SID_CLOCK_UNKNOWN; }

int sidfile_model(const sidfile_t *f, int chip)
{
    int m1, m;
    if (f->version < 2) return SID_MODEL_UNKNOWN;
    m1 = (f->flags >> PSID_FLAGS_MODEL_SHIFT) & 3;
    if (chip == 0) return m1;
    m = (f->flags >> (chip == 1 ? PSID_FLAGS_MODEL2_SHIFT : PSID_FLAGS_MODEL3_SHIFT)) & 3;
    return m ? m : m1;                      /* 0: as the first SID */
}

int sidfile_nsids(const sidfile_t *f) { return f->sid2 ? (f->sid3 ? 3 : 2) : 1; }
