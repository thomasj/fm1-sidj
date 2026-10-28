/* SPDX-License-Identifier: GPL-3.0-only */
/* The SID file format (PSID / RSID, header versions 1 to 4) as HVSC documents it in
 * DOCUMENTS/SID_file_format.txt: the header fields, the data, and the per-song and per-chip
 * attributes derived from the flags. A parse keeps pointers into the caller's buffer. */
#pragma once
#include <stdint.h>

enum { SID_CLOCK_UNKNOWN = 0, SID_CLOCK_PAL = 1, SID_CLOCK_NTSC = 2, SID_CLOCK_ANY = 3 };
enum { SID_MODEL_UNKNOWN = 0, SID_MODEL_6581 = 1, SID_MODEL_8580 = 2, SID_MODEL_ANY = 3 };

enum {                                      /* the header's fields, as offsets into the file (big-endian) */
    PSID_HDR_VERSION = 0x04, PSID_HDR_DATA = 0x06,   /* (after the magic "PSID" / "RSID") */
    PSID_HDR_LOAD = 0x08, PSID_HDR_INIT = 0x0a, PSID_HDR_PLAY = 0x0c,
    PSID_HDR_SONGS = 0x0e, PSID_HDR_START_SONG = 0x10, PSID_HDR_SPEED = 0x12,
    PSID_HDR_NAME = 0x16, PSID_HDR_AUTHOR = 0x36, PSID_HDR_RELEASED = 0x56,   /* 32 bytes each */
    PSID_HDR_FLAGS = 0x76,                  /* version 2 on */
    PSID_HDR_START_PAGE = 0x78, PSID_HDR_PAGE_LEN = 0x79,
    PSID_HDR_SID2 = 0x7a, PSID_HDR_SID3 = 0x7b,   /* versions 3, 4: the middle byte of $Dxx0 */
    PSID_HDR_V1_SIZE = 0x76, PSID_HDR_V2_SIZE = 0x7c
};
#define PSID_STR_LEN 32
#define PSID_INFO_LEN (3 * PSID_STR_LEN)    /* name, author, released, from PSID_HDR_NAME */
enum {                                      /* PSID_HDR_FLAGS */
    PSID_FLAGS_PLAYSID_BASIC = 0x02,        /* PSID: PlaySID samples; RSID: a BASIC program */
    PSID_FLAGS_CLOCK_SHIFT = 2,             /* SID_CLOCK_*, 2 bits */
    PSID_FLAGS_MODEL_SHIFT = 4,             /* SID_MODEL_* of the first, second, third SID, 2 bits each */
    PSID_FLAGS_MODEL2_SHIFT = 6,
    PSID_FLAGS_MODEL3_SHIFT = 8
};
#define RSID_LOAD_MIN 0x07e8                /* an RSID loads above BASIC's start */

typedef struct {
    uint8_t rsid;                           /* RSID (a real C64 environment, no play routine) */
    uint8_t basic;                          /* RSID: a BASIC program (not supported) */
    uint8_t playsid;                        /* PSID: PlaySID-specific samples (the speed bits are per song, v1 style) */
    uint16_t version;                       /* 1 .. 4 */
    uint16_t load, init, play;              /* C64 addresses; load from the data when the header says 0 */
    uint16_t songs, start_song;             /* songs 1 .. 256, start_song 1-based */
    uint32_t speed;                         /* bit n: song n+1 is CIA-timed (1) or frame-timed (0) */
    uint16_t flags;
    uint8_t start_page, page_len;           /* free memory the player may use: start page, pages */
    uint16_t sid2, sid3;                    /* the second and third SID's base address, 0 = none */
    char name[33], author[33], released[33];
    const uint8_t *data;                    /* the C64 image, data_len bytes, loads at `load` */
    uint32_t data_len;
} sidfile_t;

/* 0 = ok; -1 too short, -2 not a SID file, -3 bad version / offsets, -4 bad addresses */
int sidfile_parse(sidfile_t *f, const uint8_t *buf, uint32_t len);
int sidfile_cia(const sidfile_t *f, int song);     /* song 1-based: 1 = CIA timer, 0 = frame (VBI) */
int sidfile_clock(const sidfile_t *f);             /* SID_CLOCK_* */
int sidfile_model(const sidfile_t *f, int chip);   /* SID_MODEL_* for chip 0 .. 2 */
int sidfile_nsids(const sidfile_t *f);             /* 1 .. 3 */
