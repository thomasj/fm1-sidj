/* SPDX-License-Identifier: GPL-3.0-only */
/* The SID file parser against headers built here: PSID v1 / v2 / v3, RSID, the load address in
 * the data, the speed bits (v1 style and v2NG style), clock and model flags, the second SID. */
#include "../firmware/src/c64/sidfile.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static void be16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static void be32(uint8_t *p, unsigned v) { be16(p, v >> 16); be16(p + 2, v & 0xffff); }

/* a header: version, load / init / play, songs, start, speed, flags, data with or without a load address */
static unsigned build(uint8_t *b, const char *magic, int version, unsigned load, unsigned init, unsigned play,
                      unsigned songs, unsigned start, unsigned speed, unsigned flags, unsigned sid2, int prefix, unsigned prefix_addr)
{
    unsigned off = version == 1 ? 0x76 : 0x7c, i;
    memset(b, 0, 0x7c);
    memcpy(b, magic, 4);
    be16(b + 4, (unsigned)version); be16(b + 6, off); be16(b + 8, load); be16(b + 0x0a, init); be16(b + 0x0c, play);
    be16(b + 0x0e, songs); be16(b + 0x10, start); be32(b + 0x12, speed);
    memcpy(b + 0x16, "A name", 6); memcpy(b + 0x36, "An author", 9); memcpy(b + 0x56, "2026 Nobody", 11);
    if (version >= 2) { be16(b + 0x76, flags); b[0x78] = 0x10; b[0x79] = 0x04; b[0x7a] = (uint8_t)sid2; }
    i = off;
    if (prefix) { b[i++] = (uint8_t)prefix_addr; b[i++] = (uint8_t)(prefix_addr >> 8); }
    b[i++] = 0x60;                                      /* RTS */
    b[i++] = 0xea;
    return i;
}

int main(void)
{
    uint8_t b[256];
    sidfile_t f;
    unsigned n;
    /* PSID v2, load address in the data */
    n = build(b, "PSID", 2, 0, 0x1000, 0x1003, 3, 2, 0x5, 0x04 | 0x20, 0, 1, 0x1000);
    CHECK(sidfile_parse(&f, b, n) == 0);
    CHECK(!f.rsid && f.version == 2 && f.load == 0x1000 && f.init == 0x1000 && f.play == 0x1003);
    CHECK(f.songs == 3 && f.start_song == 2 && f.data_len == 2 && f.data[0] == 0x60);
    CHECK(!strcmp(f.name, "A name") && !strcmp(f.author, "An author") && !strcmp(f.released, "2026 Nobody"));
    CHECK(sidfile_cia(&f, 1) == 1 && sidfile_cia(&f, 2) == 0 && sidfile_cia(&f, 3) == 1);
    CHECK(sidfile_clock(&f) == SID_CLOCK_PAL && sidfile_model(&f, 0) == SID_MODEL_8580 && sidfile_model(&f, 1) == SID_MODEL_8580);
    CHECK(sidfile_nsids(&f) == 1 && f.start_page == 0x10 && f.page_len == 4);
    /* v1: no flags, 0x76 data offset, load address given in the header, no prefix */
    n = build(b, "PSID", 1, 0x2000, 0x2000, 0x2003, 40, 1, 0xffffffffu, 0, 0, 0, 0);
    CHECK(sidfile_parse(&f, b, n) == 0);
    CHECK(f.load == 0x2000 && f.data_len == 2 && sidfile_clock(&f) == SID_CLOCK_UNKNOWN && sidfile_model(&f, 0) == SID_MODEL_UNKNOWN);
    CHECK(sidfile_cia(&f, 33) == 1);                    /* v1: song 33 uses bit 0 */
    /* v2NG, more than 32 songs: songs above 32 use bit 31 */
    n = build(b, "PSID", 2, 0x2000, 0x2000, 0x2003, 40, 1, 0x80000000u, 0x04, 0, 0, 0);
    CHECK(sidfile_parse(&f, b, n) == 0);
    CHECK(sidfile_cia(&f, 1) == 0 && sidfile_cia(&f, 32) == 1 && sidfile_cia(&f, 40) == 1);
    /* v2NG with the PlaySID flag: v1 semantics for the speed bits */
    n = build(b, "PSID", 2, 0x2000, 0x2000, 0x2003, 40, 1, 0x1, 0x02, 0, 0, 0);
    CHECK(sidfile_parse(&f, b, n) == 0 && f.playsid && sidfile_cia(&f, 33) == 1 && sidfile_cia(&f, 2) == 0);
    /* v3 with a second SID at $D420 */
    n = build(b, "PSID", 3, 0, 0x1000, 0x1003, 1, 1, 0, 0x04, 0x42, 1, 0x1000);
    CHECK(sidfile_parse(&f, b, n) == 0 && sidfile_nsids(&f) == 2 && f.sid2 == 0xd420);
    /* RSID: play 0, load above $07E8, CIA timed by definition, the BASIC flag */
    n = build(b, "RSID", 2, 0, 0x1000, 0, 1, 1, 0, 0x02, 0, 1, 0x1000);
    CHECK(sidfile_parse(&f, b, n) == 0 && f.rsid && f.basic && sidfile_cia(&f, 1) == 1);
    n = build(b, "RSID", 2, 0, 0x1000, 0x1003, 1, 1, 0, 0, 0, 1, 0x1000);
    CHECK(sidfile_parse(&f, b, n) == -4);               /* RSID with a play address */
    /* errors */
    CHECK(sidfile_parse(&f, b, 10) == -1);
    memcpy(b, "XSID", 4);
    CHECK(sidfile_parse(&f, b, n) == -2);
    n = build(b, "PSID", 5, 0, 0x1000, 0x1003, 1, 1, 0, 0, 0, 1, 0x1000);
    CHECK(sidfile_parse(&f, b, n) == -3);
    n = build(b, "PSID", 2, 0xffff, 0xffff, 0, 1, 1, 0, 0, 0, 0, 0);
    CHECK(sidfile_parse(&f, b, n) == -4);               /* runs past the end of memory */
    printf(fails ? "sidfile_test FAIL\n" : "sidfile_test PASS\n");
    return fails != 0;
}
