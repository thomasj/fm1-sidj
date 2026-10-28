/* SPDX-License-Identifier: GPL-3.0-only */
/* SiDJ settings: what SAVE keeps in OBJ_SET. Included by the unity build (and the host)
 * after plat.h. */
#define SET_MAGIC 0x59415031u               /* "1PAY" */
#define SET_FORMAT 1u

typedef struct {
    uint32_t magic, format;
    uint8_t theme_unused;                   /* was the theme (now always DEBUT) */
    uint8_t model;                          /* chip override: 0 the file's, 1 6581, 2 8580 */
    uint8_t clock_unused;                   /* was the clock override (now always the file's) */
    uint8_t output;                         /* 0 emulated, 1 ASID over USB-MIDI, 2 both */
    uint16_t song;                          /* the entry to select at boot */
    uint8_t usb4;                           /* USB audio: 0 the mix (L R), 1 the mix and the three voices
                                             * (five channels; the tempo then caps at 100 %) */
    uint8_t rsv[5];
} settings_t;

static settings_t set;

static void settings_defaults(void)
{
    uint32_t i;
    for (i = 0; i < sizeof set; i++) ((uint8_t *)&set)[i] = 0;
    set.magic = SET_MAGIC;
    set.format = SET_FORMAT;
}

static int settings_load(void)             /* 0 = loaded; else the defaults are in place */
{
    settings_t s;
    settings_defaults();
    if (plat_store_load(OBJ_SET, &s, sizeof s) != (int)sizeof s || s.magic != SET_MAGIC || s.format != SET_FORMAT)
        return -1;
    set = s;
    if (set.model > 2) set.model = 0;
    if (set.output > 2) set.output = 0;
    if (set.usb4 > 1) set.usb4 = 0;
    return 0;
}

static int settings_save(void) { return plat_store_save(OBJ_SET, &set, sizeof set); }
