/* SPDX-License-Identifier: GPL-3.0-only */
/* SiDJ UI: two modes, SEQ switches between them.
 *
 * SONGS: the list of files in the store. SELECT picks, PLAY plays (and goes to the player),
 * PRESETS picks the subsong, OCT-/OCT+ jump eight rows, KNOB 1, 2, 3 set the chip, the output and
 * USB audio (the mix, or the mix and the voices: the tempo then caps at 100 %), SAVE
 * keeps the settings.
 *
 * PLAYER: the tune on screen: the three voices (a scope each, note, waveform, envelope, pulse
 * width) and the filter. PLAY plays / pauses (REC held: restarts), SELECT changes the song,
 * PRESETS the subsong (both wait for PLAY), ALGORITHM the output (the emulated SID, ASID over USB-MIDI, or both; in
 * SONGS it is KNOB 2, and SAVE keeps it). A song with a known length stops at its end and
 * waits: SELECT picks the next one, PLAY plays it again.
 *
 * The remix: HOME, SAVE, ARP select voice 1, 2, 3; the keys hold the voice's pitch, or in
 * transpose mode shift it (F#4 the middle, no change; OCT-/OCT+ shift the keys); REC mutes the
 * selected voice (on release, so REC + PLAY still restarts); with a voice button held, FX SEL
 * ENV LFO EDIT GLO show and toggle its pulse / triangle / sawtooth / noise / sync / ring bits
 * and REC solos it; with SEL held, the voice buttons toggle the voices' filter routing and LFO
 * EDIT GLO the LP / BP / HP bits; with FX held, OCT- sets the keys to transpose and OCT+ to set
 * the pitch; with SEL held, OCT- sets the cutoff knob to an offset and OCT+ to a scale
 * (x0 to x16). Otherwise FX, ENV and SEL pick the knob page: pulse
 * width, octave, fine (cents) / A D S R / cutoff, resonance; LFO the selected voice's LFO
 * (rate, depth, target: pulse width, pitch or cutoff, shape); GLO the deck (tempo, the vinyl
 * fader). OCT- and OCT+ together clear every override, the tempo and the fader.
 * Included by the unity build (and the host) after gfx.c, plat.h, engine.h, settings.c and
 * songs.c. */

enum { UI_SONGS, UI_PLAYER };
#define SIDJ_SITE "thomasj.github.io/fm1-sidj"   /* where songs go on (the web page's step 2), for an empty list */
#define BRAND_G RGB(130, 235, 130)          /* the brand: the intro's logo, the song list's groups */
#define BRAND_R RGB(240, 54, 60)
#define BRAND_B RGB(70, 90, 255)
enum { PG_FX, PG_ENV, PG_SEL, PG_LFO, PG_GLO, NPG };
static const char *const PG_NAME[NPG] = {"OSC", "ENV", "FLT", "LFO", "GLO"};   /* what the page controls (buttons FX, ENV, SEL, LFO, GLO) */
static const int8_t PG_BTN[NPG] = {B_FX, B_ENV, B_SEL, B_LFO, B_GLO};
static const char *const LFO_TGT_NAME[RX_LFO_NTARGET] = {"OFF", "PW", "PITCH", "CUT"};
static const char *const LFO_SHAPE_NAME[RX_LFO_NSHAPE] = {"SIN", "TRI", "SAW", "SQR", "RND"};
static const int8_t VOICE_BTN[3] = {B_HOME, B_SAVE, B_ARP};
/* the six waveform buttons, in the panel's order FX SEL ENV LFO EDIT GLO */
static const int8_t WAVE_BTN[6] = {B_FX, B_SEL, B_ENV, B_LFO, B_EDIT, B_GLO};
static const uint8_t WAVE_BIT[6] = {SID_CTRL_PULSE, SID_CTRL_TRI, SID_CTRL_SAW, SID_CTRL_NOISE, SID_CTRL_SYNC, SID_CTRL_RING};
static const char *const NOTE_NAME[12] = {"C-", "C#", "D-", "D#", "E-", "F-", "F#", "G-", "G#", "A-", "A#", "B-"};
static const char *const OUT_NAME[3] = {"EMULATED SID", "ASID OVER USB-MIDI", "SID + ASID"};
static const char *const OUT_TAG[3] = {"", "ASID", "BOTH"};
#define KEY_BASE 53                         /* MIDI note of the lowest key (F3) at octave 0 */
#define KEY_OCT_MAX 3                       /* the keys' octaves either way: F0 .. G8, the SID's notes (C0 .. E0, under 21 Hz,
                                             * left out; above B7 the frequency register is at its top) */
#define KEY_MID 13                          /* F#4, the middle key: no shift in transpose mode */
enum { KM_SET, KM_SHIFT };                  /* the keys: set the pitch, transpose it */
#define LIST_ROWS 8                         /* 18-pixel rows in the 144-row canvas */
#define ROW_H 18

static struct {
    uint8_t mode;
    int sel;                                /* the entry under the cursor */
    int cur;                                /* the loaded entry, -1 */
    int subsong, nsongs;
    uint8_t voice;                          /* the voice the remix edits */
    uint8_t page;
    int8_t octave;                          /* the keys' octave, -KEY_OCT_MAX .. KEY_OCT_MAX */
    int voice_held;                         /* a voice button is down: which voice, else -1 */
    uint8_t oct_pending;                    /* bit 0 OCT- pressed and not yet released, bit 1 OCT+ */
    uint8_t both_reset;                     /* both were down: the releases do nothing */
    uint8_t key_mode;                       /* KM_SET / KM_SHIFT */
    uint8_t cut_mode;                       /* RX_CUT_SCALE / RX_CUT_OFFSET */
    uint8_t rec_pending;                    /* REC down alone so far: mute on release */
    uint8_t intro;                          /* the boot intro is on screen */
    uint8_t activity;                       /* a button, key or knob moved since the last frame */
    uint32_t intro_t0, intro_at;
    uint32_t btn_prev, keys_prev;
    uint32_t enc_t[NE];
    char msg[48];
    uint32_t msg_until;
    uint8_t dirty_top, dirty_list, dirty_voices, dirty_filter, dirty_status;
    uint32_t top_at, voices_at, filter_at, status_at;
    uint32_t top_sig;                       /* the remix state the top bar shows (redrawn when it moves) */
    uint8_t load_wait;                      /* an EV_LOAD is out; report its result */
    uint8_t ended;                          /* the song ran past its length */
    uint32_t lit_keys;
} ui;
#define NLANES 17                           /* the keys' sources: MIDI channels 1 .. 16, the FM-1's keys */
#define KB_LANE 16
static struct { uint32_t on[4], age; uint8_t sound; } lane[NLANES];   /* held notes; sound: note + 1, 0 */
static uint32_t lane_clock;

static void ui_say(const char *a, const char *b)
{
    uint32_t i = 0;
    for (; *a && i < sizeof ui.msg - 1; a++) ui.msg[i++] = *a;
    if (b) for (; *b && i < sizeof ui.msg - 1; b++) ui.msg[i++] = *b;
    ui.msg[i] = 0;
    ui.msg_until = plat_ms() + 1500u;
    ui.dirty_status = 1;
}

static void ui_num(char *b, int v)         /* decimal into b (which has room), signed */
{
    char t[12];
    int i = 0, n = 0;
    if (v < 0) { *b++ = '-'; v = -v; }
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) b[i++] = t[--n];
    b[i] = 0;
}
static int ui_strlen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void ui_cat(char *b, const char *s) { b += ui_strlen(b); while (*s) *b++ = *s++; *b = 0; }
static void ui_time(char *b, uint32_t ds)  /* "m:ss" */
{
    uint32_t s = ds / 10u;
    ui_num(b, (int)(s / 60u));
    b += ui_strlen(b);
    *b++ = ':';
    *b++ = (char)('0' + (s / 10u) % 6u);
    *b++ = (char)('0' + s % 10u);
    *b = 0;
}
static void ui_scale(char *b, uint32_t q7)  /* a Q7 factor as "x1.25" */
{
    uint32_t cents = (q7 & 127) * 100u / 128u;
    *b++ = 'x';
    ui_num(b, (int)(q7 >> 7));
    b += ui_strlen(b);
    *b++ = '.';
    *b++ = (char)('0' + cents / 10u);
    *b++ = (char)('0' + cents % 10u);
    *b = 0;
}
static void ui_hvsc_str(char *dst, const uint8_t *src, int max)   /* a 32-byte field, maybe without NUL */
{
    int i;
    for (i = 0; i < 32 && i < max && src && src[i]; i++) dst[i] = (char)src[i];
    dst[i] = 0;
}

static void ui_say_output(void)             /* the output turned; an RSID never goes out as ASID */
{
    if (set.output && eng_loaded && eng_deck_off) ui_say("RSID: NO ASID, EMULATED SID ONLY", 0);
    else ui_say("OUTPUT: ", OUT_NAME[set.output]);
}

/* ------------------------------------------------------------ loading --- */
static int ui_load(int k, int subsong, int play)   /* play 0: loaded, ready for PLAY */
{
    uint32_t size = 0, i;
    const uint8_t *f = song_file(k, &size, 0, 0);
    sidfile_t sf;
    uint8_t opts;
    if (!f || eng_file.pending || size > ENG_FILE_MAX) return -1;
    if (!song_file_ok(k)) { ui_say("SONG DAMAGED IN FLASH: ADD IT AGAIN", 0); return -1; }   /* (not played: it could crash) */
    if (sidfile_parse(&sf, f, size)) { ui_say("NOT A SID FILE", 0); return -1; }
    if (sf.basic) { ui_say("BASIC TUNE: NOT SUPPORTED", 0); return -1; }
    if (sidfile_nsids(&sf) > 1) { ui_say(sidfile_nsids(&sf) == 2 ? "2SID TUNE: NOT SUPPORTED" : "3SID TUNE: NOT SUPPORTED", 0); return -1; }
    for (i = 0; i < size; i++) eng_file.data[i] = f[i];
    eng_file.len = size;
    __asm__ volatile("" ::: "memory");      /* the file is complete before the flag says so */
    ui.nsongs = sf.songs;
    if (subsong < 1 || subsong > sf.songs) subsong = sf.start_song;
    ui.subsong = subsong;
    ui.cur = k;
    memset(lane, 0, sizeof lane);           /* a new song starts clean: the held keys are let go */
    ui.ended = 0;
    opts = (uint8_t)(set.model << 2 | (play ? 0 : ENG_LOAD_PAUSED));   /* the clock: always the file's */
    eng_file.pending = 1;
    eng_post(EV_LOAD, (uint8_t)subsong, opts, 0);
    ui.load_wait = 1;
    ui.dirty_top = ui.dirty_list = ui.dirty_voices = ui.dirty_filter = ui.dirty_status = 1;
    return 0;
}

static int songs_play_request(int k, int subsong)   /* from the transfer tool */
{
    if (ui_load(k, subsong, 1)) return 0;
    ui.sel = k;
    if (ui.mode != UI_PLAYER) { ui.mode = UI_PLAYER; lcd_fill(0, 0, 240, 240, C_BLACK); }
    return 1;
}

/* the list's order: grouped by composer (each group under its name), within one as stored */
static int16_t song_ord[SONG_MAX_ENTRIES];
static int song_ord_n = -1;                 /* -1: to be built (songs_changed) */

#define HVSC_AUTHOR (PSID_HDR_AUTHOR - PSID_HDR_NAME)   /* the author field, from song_hvsc_name's */

static int composer_cmp(int a, int b)       /* the files' author fields (maybe no NUL) */
{
    const uint8_t *x = song_hvsc_name(a), *y = song_hvsc_name(b);
    int i;
    if (!x || !y) return x ? 1 : y ? -1 : 0;
    for (i = HVSC_AUTHOR; i < HVSC_AUTHOR + PSID_STR_LEN; i++) {
        uint8_t cx = x[i], cy = y[i];
        if (cx >= 'a' && cx <= 'z') cx = (uint8_t)(cx - 'a' + 'A');
        if (cy >= 'a' && cy <= 'z') cy = (uint8_t)(cy - 'a' + 'A');
        if (cx != cy) return cx < cy ? -1 : 1;
        if (!cx) return 0;
    }
    return 0;
}

static void song_order(void)                /* insertion sort: stable, so a group keeps store order */
{
    int i, j;
    for (i = 0; i < song_count && i < SONG_MAX_ENTRIES; i++) {
        int16_t k = (int16_t)i;
        for (j = i; j > 0 && composer_cmp(song_ord[j - 1], k) > 0; j--) song_ord[j] = song_ord[j - 1];
        song_ord[j] = k;
    }
    song_ord_n = i;
}

static int song_pos(int k)                  /* where song k sits in the list */
{
    int i;
    if (song_ord_n != song_count) song_order();
    for (i = 0; i < song_ord_n; i++) if (song_ord[i] == k) return i;
    return 0;
}

static int song_at(int pos)                 /* the song at a list position (clamped) */
{
    if (song_ord_n != song_count) song_order();
    if (pos < 0) pos = 0;
    if (pos >= song_ord_n) pos = song_ord_n - 1;
    return pos < 0 ? 0 : song_ord[pos];
}

static void ui_next_song(int d)             /* the player's SELECT: the next or previous in the list, ready for PLAY */
{
    if (!song_count) return;
    ui.sel = song_at(((song_pos(ui.sel) + d) % song_count + song_count) % song_count);
    ui_load(ui.sel, 0, 0);
}

static void ui_init(void)
{
    memset(&ui, 0, sizeof ui);
    ui.cur = -1;
    ui.voice_held = -1;
    memset(lane, 0, sizeof lane);
    ui.sel = set.song < song_count ? set.song : 0;
    palette_set(0);                         /* DEBUT */
    eng_post(EV_OUTPUT, set.output, 0, 0);
    ui.dirty_top = ui.dirty_list = ui.dirty_voices = ui.dirty_filter = ui.dirty_status = 1;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.intro = 1;
    ui.intro_t0 = plat_ms();
    ui.intro_at = ui.intro_t0 - 1000u;
}

static int32_t ui_enc(int role)             /* an encoder's detents; any turn is activity */
{
    int32_t d = plat_enc(role);
    if (d) ui.activity = 1;
    return d;
}

/* --------------------------------------------------------------- keys --- */
static void ui_key_off(int v)              /* whatever the keys hold on voice v goes */
{
    eng_post(EV_RX, RX_P_KEY, (uint8_t)v, 0);
    eng_post(EV_RX, RX_P_KEY_SHIFT, (uint8_t)v, 0);
}

/* The keys: the FM-1's own and a MIDI keyboard's (USB or the MIDI IN jack), one logic. Each
 * source holds notes: MIDI channels 1, 2, 3 on voices 1, 2, 3; the FM-1's keys and the other
 * channels on the selected voice, moving with the selection. In a source the newest press
 * sounds; let go, the lowest note still held takes over. On a voice, the source pressed last
 * sounds. The octave buttons and the keys' mode (set the pitch, or transpose: F#4 = 0) apply to
 * all, and the keys play only on the player. */
static int lane_voice(int l) { return l < 3 ? l : ui.voice; }

static int voice_lane(int v)               /* the source sounding on voice v, -1 */
{
    int l, best = -1;
    for (l = 0; l < NLANES; l++)
        if (lane[l].sound && lane_voice(l) == v && (best < 0 || lane[l].age > lane[best].age)) best = l;
    return best;
}

static void key_send(int v)                /* voice v's sounding note (or none) to the engine */
{
    int l = voice_lane(v), n;
    if (l < 0) { eng_post(EV_RX, ui.key_mode == KM_SHIFT ? RX_P_KEY_SHIFT : RX_P_KEY, (uint8_t)v, 0); return; }
    n = lane[l].sound - 1 + 12 * ui.octave;
    if (ui.key_mode == KM_SHIFT)
        eng_post(EV_RX, RX_P_KEY_SHIFT, (uint8_t)v, 0x100u | (uint32_t)((n - KEY_BASE - KEY_MID) & 0xff));
    else
        eng_post(EV_RX, RX_P_KEY, (uint8_t)v, (uint32_t)(n < 1 ? 1 : n));
}

static void keys_send(void) { int v; for (v = 0; v < 3; v++) key_send(v); }

static void note_on(int l, int n)
{
    if (ui.mode != UI_PLAYER) return;
    lane[l].on[n >> 5] |= 1u << (n & 31);
    lane[l].sound = (uint8_t)(n + 1);
    lane[l].age = ++lane_clock;
    key_send(lane_voice(l));
    ui.activity = 1;
}

static void note_off(int l, int n)         /* n < 0: all the source's notes */
{
    int i;
    if (n < 0) memset(lane[l].on, 0, sizeof lane[l].on);
    else lane[l].on[n >> 5] &= ~(1u << (n & 31));
    if (!lane[l].sound || (n >= 0 && lane[l].sound != n + 1)) return;
    lane[l].sound = 0;
    for (i = 0; i < 128; i++)               /* another note still down takes over: the lowest */
        if (lane[l].on[i >> 5] >> (i & 31) & 1u) { lane[l].sound = (uint8_t)(i + 1); break; }
    key_send(lane_voice(l));
}

enum { MIDI_NOTE_OFF = 0x80, MIDI_NOTE_ON = 0x90, MIDI_CC = 0xb0,   /* status, the channel in the low nibble */
       MIDI_CC_ALL_SOUND_OFF = 120, MIDI_CC_ALL_NOTES_OFF = 123 };

static void ui_rx_reset(void);
static void ui_set_cut_mode(int m);

/* The remix over MIDI CCs, on the player as the keys. A voice's CC on channel 1, 2 or 3 sets that
 * voice, on another channel the selected one; the global ones take any channel. 71 .. 75 are the
 * standard sound controllers; 91 .. 95 (reverb, chorus: sent by many keyboards) stay clear. Where
 * the tune has a value of its own, an end or a zone of the range gives it back. */
enum { CC_PW, CC_A, CC_D, CC_S, CC_R, CC_OCT, CC_FINE, CC_WAVE, CC_SYNC, CC_RING, CC_MUTE, CC_SOLO,
       CC_FILT, CC_LFO_RATE, CC_LFO_DEPTH, CC_LFO_TARGET, CC_LFO_SHAPE, CC_NVOICE,   /* (the voice's) */
       CC_CUT_MODE = CC_NVOICE, CC_CUTOFF, CC_RES, CC_LP, CC_BP, CC_HP, CC_TEMPO, CC_VINYL, CC_RESET, NCC };
static const uint8_t CC_NUM[NCC] = { 70, 73, 75, 76, 72, 77, 78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88,
                                     102, 74, 71, 103, 104, 105, 106, 107, 108 };   /* (the mode before the cutoff) */
static const uint16_t CUT_EXP[16] = { 256, 267, 279, 292, 304, 318, 332, 347, 362, 378, 395, 412, 431, 450, 470, 490 };   /* 2^(i/16), Q8 */
static uint8_t cc_pend[NCC][3];            /* the latest value + 1 of each not yet with the engine, 0 none */

static int cc_zone(int v, int n) { return v * n / 128; }
static int cc_tune_or(int v, int max) { return v ? ((v - 1) * max + 63) / 126 : -1; }   /* 0 the tune's, 1 .. 127: 0 .. max */
static int cc_mid(int v, int max) { return v < 64 ? (v - 64) * max / 64 : (v - 64) * max / 63; }   /* 64 is 0 */

static int cc_send(int i, int voice, int v)    /* one CC's value to the engine: -1 when its queue is full */
{
    static const uint8_t P[NCC] = { RX_P_PW, RX_P_A, RX_P_D, RX_P_S, RX_P_R, RX_P_OCT, RX_P_FINE, RX_P_WAVE_TOGGLE,
        RX_P_SYNC, RX_P_RING, RX_P_MUTE_TOGGLE, RX_P_SOLO, RX_P_FILT_TOGGLE, RX_P_LFO_RATE, RX_P_LFO_DEPTH,
        RX_P_LFO_TARGET, RX_P_LFO_SHAPE, RX_P_CUTOFF_MODE, RX_P_CUTOFF, RX_P_RES, RX_P_FMODE_TOGGLE,
        RX_P_FMODE_TOGGLE, RX_P_FMODE_TOGGLE, RX_P_TEMPO, RX_P_VINYL, 0 };
    static const uint8_t WAVE[4] = { SID_CTRL_PULSE, SID_CTRL_TRI, SID_CTRL_SAW, SID_CTRL_NOISE };   /* (the panel's order) */
    int32_t x;
    uint8_t b = (uint8_t)voice;
    switch (i) {
    case CC_PW: x = cc_tune_or(v, SID_PW_MAX); break;
    case CC_A: case CC_D: case CC_S: case CC_R: x = cc_tune_or(v, SID_NIBBLE_MAX); break;
    case CC_OCT: x = cc_zone(v, 2 * RX_OCT_MAX + 1) - RX_OCT_MAX; break;
    case CC_FINE: x = cc_mid(v, RX_FINE_MAX); break;
    case CC_WAVE: x = cc_zone(v, 5); x = x ? WAVE[x - 1] : 0; break;
    case CC_SYNC: case CC_RING: case CC_FILT: x = cc_zone(v, 3) - 1; break;   /* the tune's, off, on */
    case CC_MUTE: case CC_SOLO: x = v >= 64; break;
    case CC_LFO_RATE: x = v * RX_LFO_NRATE / 128; break;
    case CC_LFO_DEPTH: x = (v * RX_LFO_DEPTH_MAX + 63) / 127; break;
    case CC_LFO_TARGET: x = cc_zone(v, RX_LFO_NTARGET); break;
    case CC_LFO_SHAPE: x = cc_zone(v, RX_LFO_NSHAPE); break;
    case CC_CUTOFF:                         /* scale: 64 the tune's, x2 every 16; offset: 64 is 0 */
        if (ui.cut_mode == RX_CUT_OFFSET) x = cc_mid(v, SID_FC_MAX);
        else { int o = v / 16 - 4; x = RX_CUTOFF_ONE * CUT_EXP[v % 16] >> 8; x = o >= 0 ? x << o : x >> -o; }
        break;
    case CC_RES: x = cc_mid(v, SID_NIBBLE_MAX); break;
    case CC_CUT_MODE:
        if ((v >= 64) != (ui.cut_mode == RX_CUT_OFFSET)) ui_set_cut_mode(v >= 64 ? RX_CUT_OFFSET : RX_CUT_SCALE);
        return 0;
    case CC_LP: case CC_BP: case CC_HP:
        b = i == CC_LP ? SID_MODE_LP : i == CC_BP ? SID_MODE_BP : SID_MODE_HP;
        x = cc_zone(v, 3) - 1;
        break;
    case CC_TEMPO: x = v <= 64 ? ENG_TEMPO_MIN + v * (100 - ENG_TEMPO_MIN) / 64 : 100 + (v - 64) * (ENG_TEMPO_MAX - 100) / 63; break;
    case CC_VINYL: x = cc_mid(v, ENG_VINYL_MAX); break;
    case CC_RESET: if (v >= 64) ui_rx_reset(); return 0;
    default: return 0;
    }
    return eng_post(EV_RX_SET, P[i], b, (uint32_t)x);
}

static void cc_in(int ch, int cc, int v)
{
    int i;
    if (ui.mode != UI_PLAYER) return;
    for (i = 0; i < NCC && CC_NUM[i] != cc; i++) {}
    if (i == NCC) return;
    cc_pend[i][i < CC_NVOICE ? (ch < 3 ? ch : ui.voice) : 0] = (uint8_t)(v + 1);
}

static void cc_flush(void)                 /* what came in goes on, the latest value of each */
{
    int i, v, any = 0;
    for (i = 0; i < NCC; i++)
        for (v = 0; v < 3; v++)
            if (cc_pend[i][v] && cc_send(i, v, cc_pend[i][v] - 1) == 0) { cc_pend[i][v] = 0; any = 1; }
    if (any) { ui.dirty_status = ui.dirty_voices = ui.dirty_filter = 1; ui.activity = 1; }
}

static void ui_midi(void)                  /* the MIDI in queue (USB-MIDI packets: CIN, status, data 1, data 2) */
{
    uint32_t pkt;
    while (plat_midi_in(&pkt)) {
        int st = (int)(pkt >> 8 & 0xf0), ch = (int)(pkt >> 8 & 0x0f), d1 = (int)(pkt >> 16 & 0x7f), d2 = (int)(pkt >> 24 & 0x7f);
        if (st == MIDI_NOTE_ON && d2) note_on(ch, d1);
        else if (st == MIDI_NOTE_OFF || st == MIDI_NOTE_ON) note_off(ch, d1);   /* velocity 0: off */
        else if (st == MIDI_CC && (d1 == MIDI_CC_ALL_SOUND_OFF || d1 == MIDI_CC_ALL_NOTES_OFF)) note_off(ch, -1);
        else if (st == MIDI_CC) cc_in(ch, d1, d2);
    }
    cc_flush();
}

static void ui_set_key_mode(int m)
{
    if (ui.key_mode != m) {
        int v;
        for (v = 0; v < 3; v++) ui_key_off(v);
        ui.key_mode = (uint8_t)m;
        keys_send();
    }
    ui_say("KEYS: ", m == KM_SHIFT ? "TRANSPOSE (F#4 = 0)" : "SET THE PITCH");
    ui.dirty_voices = 1;
}

static void ui_set_cut_mode(int m)
{
    ui.cut_mode = (uint8_t)m;
    eng_post(EV_RX, RX_P_CUTOFF_MODE, 0, (uint32_t)m);
    ui_say("CUTOFF: ", m == RX_CUT_OFFSET ? "OFFSET" : "SCALE x0 .. x16");
    ui.dirty_filter = 1;
}

static void ui_keys(uint32_t keys)         /* the FM-1's keys: notes F3 .. G5 */
{
    uint32_t down = keys & ~ui.keys_prev, up = ui.keys_prev & ~keys;
    int i;
    ui.keys_prev = keys;
    for (i = 0; i < NKEYS; i++) if (up & (1u << i)) note_off(KB_LANE, KEY_BASE + i);
    for (i = 0; i < NKEYS; i++) if (down & (1u << i)) note_on(KB_LANE, KEY_BASE + i);
}

/* ------------------------------------------------------------ buttons --- */
static void ui_set_page(int pg) { ui.page = (uint8_t)pg; ui.dirty_status = 1; }

static void ui_select_voice(int v)
{
    int old = ui.voice;
    ui.voice = (uint8_t)v;
    if (old != v) { key_send(old); key_send(v); }   /* the keys move with the selection */
    ui.dirty_voices = ui.dirty_status = 1;
}

static void ui_say_keys(void)              /* the keys' range at this octave, F3 .. G5 at 0 */
{
    char t[] = "F0 .. G0";
    t[1] = (char)('3' + ui.octave);
    t[7] = (char)('5' + ui.octave);
    ui_say("KEYS: ", t);
}

static void ui_rx_reset(void)              /* everything back to the tune: OCT- and OCT+ together, or CC 108 */
{
    eng_post(EV_RX_RESET, 0, 0, 0);
    memset(lane, 0, sizeof lane);            /* the held keys are let go */
    ui_say("REMIX RESET", 0);
    ui.dirty_voices = ui.dirty_filter = 1;
}

static void ui_octave_btn(uint32_t pressed, uint32_t released, uint32_t held)
{
    uint32_t both = (1u << B_OCTDN) | (1u << B_OCTUP);
    if (pressed & (1u << B_OCTDN)) ui.oct_pending |= 1;
    if (pressed & (1u << B_OCTUP)) ui.oct_pending |= 2;
    if ((held & both) == both && !ui.both_reset) {
        ui.both_reset = 1;
        ui.oct_pending = 0;
        ui_rx_reset();
        return;
    }
    if (ui.mode == UI_PLAYER) {
        if ((released & (1u << B_OCTDN)) && (ui.oct_pending & 1) && !ui.both_reset && ui.octave > -KEY_OCT_MAX) { ui.octave--; keys_send(); ui_say_keys(); }
        if ((released & (1u << B_OCTUP)) && (ui.oct_pending & 2) && !ui.both_reset && ui.octave < KEY_OCT_MAX) { ui.octave++; keys_send(); ui_say_keys(); }
    } else {
        if ((released & (1u << B_OCTDN)) && (ui.oct_pending & 1) && !ui.both_reset && song_count) { ui.sel = song_at(song_pos(ui.sel) - 8); ui.dirty_list = 1; }
        if ((released & (1u << B_OCTUP)) && (ui.oct_pending & 2) && !ui.both_reset && song_count) { ui.sel = song_at(song_pos(ui.sel) + 8); ui.dirty_list = 1; }
    }
    if (released & (1u << B_OCTDN)) ui.oct_pending &= (uint8_t)~1u;
    if (released & (1u << B_OCTUP)) ui.oct_pending &= (uint8_t)~2u;
    if (!(held & both)) ui.both_reset = 0;
}

static void ui_play_btn(uint32_t held)
{
    if (ui.mode == UI_SONGS) {
        if (!song_count) { ui_say("NO SONGS YET: SEE ABOVE", 0); return; }
        if (ui.sel == ui.cur && eng_loaded && !eng_playing && !ui.ended) eng_post(EV_PLAY, 0, 0, 0);   /* loaded and waiting (its subsong kept) */
        else if (ui_load(ui.sel, 0, 1)) return;
        ui.mode = UI_PLAYER; lcd_fill(0, 0, 240, 240, C_BLACK); ui.dirty_top = ui.dirty_voices = ui.dirty_filter = ui.dirty_status = 1;
        return;
    }
    if ((held & (1u << B_REC)) || ui.ended) {   /* REC + PLAY, or PLAY after the end: from the start */
        if (ui.cur >= 0) ui_load(ui.cur, ui.subsong, 1);
        return;
    }
    if (!eng_loaded) { if (song_count) ui_load(ui.sel, 0, 1); return; }
    if (eng_playing) eng_post(EV_STOP, 0, 0, 0);
    else eng_post(EV_PLAY, 0, 0, 0);
    ui.dirty_top = 1;
}

static void ui_rx_knob(int k, int32_t d, int enc)
{
    uint32_t now = plat_ms();
    int fast = (now - ui.enc_t[enc]) < 40u;
    ui.enc_t[enc] = now;
    switch (ui.page) {
    case PG_FX:
        if (k == 0) eng_post(EV_RX, RX_P_PW, ui.voice, (uint32_t)(d * (fast ? 128 : 32)));
        else if (k == 1) eng_post(EV_RX, RX_P_OCT, ui.voice, (uint32_t)d);
        else if (k == 2) eng_post(EV_RX, RX_P_FINE, ui.voice, (uint32_t)(d * (fast ? 4 : 1)));   /* cents */
        break;
    case PG_ENV:
        eng_post(EV_RX, (uint8_t)(RX_P_A + k), ui.voice, (uint32_t)d);
        break;
    case PG_SEL:
        if (k == 0) eng_post(EV_RX, RX_P_CUTOFF, 0, (uint32_t)(d * (fast ? 4 : 1)));   /* clicks: the engine steps */
        else if (k == 1) eng_post(EV_RX, RX_P_RES, 0, (uint32_t)d);
        break;
    case PG_LFO:                            /* the selected voice's LFO: rate, depth, target, shape */
        if (eng_deck_off) { ui_say("RSID: NO LFO", 0); break; }
        if (k == 0) eng_post(EV_RX, RX_P_LFO_RATE, ui.voice, (uint32_t)(d * (fast ? 4 : 1)));
        else if (k == 1) eng_post(EV_RX, RX_P_LFO_DEPTH, ui.voice, (uint32_t)(d * (fast ? 4 : 1)));
        else eng_post(EV_RX, k == 2 ? RX_P_LFO_TARGET : RX_P_LFO_SHAPE, ui.voice, (uint32_t)(d > 0 ? 1 : -1));
        break;
    case PG_GLO:                            /* the deck: tempo (%) and the vinyl fader (0.1 %) */
        if (eng_deck_off && k < 2) { ui_say("RSID: NO TEMPO OR PITCH", 0); break; }
        if (k < 2 && d > 0 && (k == 1 ? eng_vinyl >= 0 : 1)   /* up at the cap: say why it stops */
            && (uint32_t)eng_tempo * (uint32_t)(1000 + (eng_vinyl > 0 ? eng_vinyl : 0)) >= (uint32_t)eng_speed_cap() * 1000u - 1000u) {
            ui_say(eng_usb4 ? "LIMIT 100 %: USB MIX + VOICES" : "LIMIT 150 %: TEMPO X PITCH", 0);
            break;
        }
        if (k == 0) eng_post(EV_RX, RX_P_TEMPO, 0, (uint32_t)(d * (fast ? 5 : 1)));
        else if (k == 1) eng_post(EV_RX, RX_P_VINYL, 0, (uint32_t)(d * (fast ? 5 : 1)));
        break;
    default: break;
    }
    ui.dirty_status = ui.dirty_voices = ui.dirty_filter = 1;
}

static void draw_status(void);

static void ui_songs_knob(int k, int32_t d)
{
    if (k == 0) {
        set.model = (uint8_t)(((int)set.model + (d > 0 ? 1 : -1) + 3) % 3);
        eng_post(EV_MODEL, set.model, 0, 0);
        ui_say("CHIP: ", set.model == 0 ? "THE FILE'S" : set.model == 1 ? "6581" : "8580");
    } else if (k == 1) {                    /* the output: the emulated SID, ASID, both */
        set.output = (uint8_t)(((int)set.output + (d > 0 ? 1 : -1) + 3) % 3);
        eng_post(EV_OUTPUT, set.output, 0, 0);
        ui_say_output();
    } else if (k == 2) {                    /* USB audio: the mix, or the mix and the voices (tempo <= 100 %) */
        uint8_t w = d > 0 ? 1 : 0;
        if (w != set.usb4) {
            set.usb4 = w;
            eng_post(EV_USB4, w, 0, 0);
            ui_say("USB AUDIO: ", w ? "MIX+VOICES" : "MIX");
            draw_status();                  /* (the reconnect below takes a moment) */
            plat_usb_audio_mode(w);         /* the computer sees it again in a moment */
        }
    }
    ui.dirty_status = 1;
}

static void ui_input(void)
{
    uint32_t btn = plat_buttons(), keys = plat_keys(), pressed = btn & ~ui.btn_prev, released = ui.btn_prev & ~btn;
    int i;
    int32_t d;
    if (pressed || (keys & ~ui.keys_prev)) ui.activity = 1;
    ui.btn_prev = btn;
    ui_keys(keys);
    ui_midi();
    if (pressed & (1u << B_SEQ)) {
        ui.mode = ui.mode == UI_SONGS ? UI_PLAYER : UI_SONGS;
        lcd_fill(0, 0, 240, 240, C_BLACK);
        ui.dirty_top = ui.dirty_list = ui.dirty_voices = ui.dirty_filter = ui.dirty_status = 1;
        ui.voice_held = -1;
    }
    ui_octave_btn(pressed, released, btn);
    if (pressed & (1u << B_PLAY)) ui_play_btn(btn);
    if (ui.mode == UI_PLAYER) {
        /* held before this press: a modifier */
        uint32_t mod = btn & ~pressed, both = (1u << B_OCTDN) | (1u << B_OCTUP);
        int sel_mod = (mod & (1u << B_SEL)) && ui.voice_held < 0;
        if ((pressed & both) && (mod & ((1u << B_FX) | (1u << B_SEL))) && ui.voice_held < 0) {
            /* FX (the OSC page) held + OCT- / OCT+: the keys transpose / set the pitch; SEL (FLT) held +
             * OCT- / OCT+: the cutoff knob offsets / scales. That octave press leaves the keys' octave */
            int dn = (pressed & (1u << B_OCTDN)) != 0;
            if (mod & (1u << B_FX)) ui_set_key_mode(dn ? KM_SHIFT : KM_SET);
            else ui_set_cut_mode(dn ? RX_CUT_OFFSET : RX_CUT_SCALE);
            ui.oct_pending &= (uint8_t)~((pressed & (1u << B_OCTDN) ? 1u : 0u) | (pressed & (1u << B_OCTUP) ? 2u : 0u));
            pressed &= ~both;
        }
        for (i = 0; i < 3; i++)
            if (pressed & (1u << VOICE_BTN[i])) {
                if (sel_mod) { eng_post(EV_RX, RX_P_FILT_TOGGLE, (uint8_t)i, 0); ui.dirty_filter = 1; }
                else { ui_select_voice(i); ui.voice_held = i; }
            }
        if (ui.voice_held >= 0 && !(btn & (1u << VOICE_BTN[ui.voice_held]))) ui.voice_held = -1;
        if (pressed & (1u << B_PLAY)) ui.rec_pending = 0;  /* REC + PLAY: the restart, no mute */
        if (ui.voice_held >= 0) {
            for (i = 0; i < 6; i++)
                if (pressed & (1u << WAVE_BTN[i])) { eng_post(EV_RX, RX_P_WAVE_TOGGLE, (uint8_t)ui.voice_held, WAVE_BIT[i]); ui.dirty_voices = 1; }
            if (pressed & (1u << B_REC)) { eng_post(EV_RX, RX_P_SOLO, (uint8_t)ui.voice_held, 0); ui.dirty_voices = 1; }
        } else {
            if (sel_mod) {
                if (pressed & (1u << B_LFO)) eng_post(EV_RX, RX_P_FMODE_TOGGLE, 0, SID_MODE_LP);
                if (pressed & (1u << B_EDIT)) eng_post(EV_RX, RX_P_FMODE_TOGGLE, 0, SID_MODE_BP);
                if (pressed & (1u << B_GLO)) eng_post(EV_RX, RX_P_FMODE_TOGGLE, 0, SID_MODE_HP);
                ui.dirty_filter = 1;
            }
            for (i = 0; i < NPG; i++)             /* (LFO and GLO with SEL held: the filter type) */
                if ((pressed & (1u << PG_BTN[i])) && !(sel_mod && (PG_BTN[i] == B_LFO || PG_BTN[i] == B_GLO))) ui_set_page(i);
            if (pressed & (1u << B_REC)) ui.rec_pending = 1;
        }
        if ((released & (1u << B_REC)) && ui.rec_pending) {
            ui.rec_pending = 0;
            eng_post(EV_RX, RX_P_MUTE_TOGGLE, ui.voice, 0);
            ui.dirty_voices = 1;
        }
        if (!(btn & (1u << B_REC))) ui.rec_pending = 0;
        if ((d = ui_enc(EN_SELECT)) != 0) ui_next_song(d > 0 ? 1 : -1);
        for (i = 0; i < 4; i++)
            if ((d = ui_enc(EN_K1 + i)) != 0) ui_rx_knob(i, d, EN_K1 + i);
    } else {
        if (pressed & (1u << B_SAVE)) { set.song = (uint16_t)ui.sel; ui_say(settings_save() == 0 ? "SETTINGS SAVED" : "SAVE FAILED", 0); }
        if ((d = ui_enc(EN_SELECT)) != 0 && song_count) {
            ui.sel = song_at(song_pos(ui.sel) + (int)d);
            ui.dirty_list = 1;
        }
        for (i = 0; i < 4; i++)
            if ((d = ui_enc(EN_K1 + i)) != 0) ui_songs_knob(i, d);
    }
    if ((d = ui_enc(EN_PRESET)) != 0 && ui.cur >= 0 && ui.nsongs > 1) {
        int s = ui.subsong + (d > 0 ? 1 : -1);
        if (s < 1) s = ui.nsongs;
        if (s > ui.nsongs) s = 1;
        ui_load(ui.cur, s, 0);              /* ready for PLAY */
    }
    if ((d = ui_enc(EN_ALGO)) != 0 && ui.mode == UI_PLAYER) {   /* (SONGS: KNOB 2) */
        set.output = (uint8_t)(((int)set.output + (d > 0 ? 1 : -1) + 3) % 3);
        eng_post(EV_OUTPUT, set.output, 0, 0);
        ui_say_output();
        ui.dirty_status = 1;
    }
}

/* ------------------------------------------------------------ drawing --- */
static int note_of_freq(uint32_t f)        /* the nearest MIDI note of a SID frequency, 0 = none */
{
    int n, best = 0;
    uint32_t bd = 0xffffffffu;
    if (!f) return 0;
    for (n = 12; n < 108; n++) {
        uint32_t t = remix_note_freq(n, eng_ntsc), d = t > f ? t - f : f - t;
        if (d < bd) { bd = d; best = n; }
        if (t > f) break;
    }
    return best;
}

static void note_text(char *b, int note)
{
    if (!note) { b[0] = b[1] = b[2] = '-'; b[3] = 0; return; }
    b[0] = NOTE_NAME[note % 12][0];
    b[1] = NOTE_NAME[note % 12][1];
    b[2] = (char)('0' + (note / 12 - 1 < 0 ? 0 : note / 12 - 1));
    b[3] = 0;
}

static void badge_c(int x, int y, const char *s, int on, int w, uint16_t col)
{
    if (on) { cv_rect(x, y, w, 13, col); cv_text(x + 2, y, &FONT_XS, s, C_BLACK); }
    else { cv_rect(x, y, w, 13, C_LINE); cv_rect(x + 1, y + 1, w - 2, 11, C_BLACK); cv_text(x + 2, y, &FONT_XS, s, C_DIM); }
}
static void badge(int x, int y, const char *s, int on, int w) { badge_c(x, y, s, on, w, C_HI); }

/* the transport, a tape deck's symbols, 9 pixels high at (x, y): 0 play, 1 pause, 2 stop */
#define TRANSPORT_W 9
static void draw_transport(int32_t x, int32_t y, int what, uint16_t c)
{
    int i;
    if (what == 0)                          /* a triangle pointing right */
        for (i = 0; i < 9; i++) cv_rect(x, y + i, 1 + 2 * (i < 4 ? i : 8 - i), 1, c);
    else if (what == 1) {                   /* two bars */
        cv_rect(x, y, 3, 9, c);
        cv_rect(x + 6, y, 3, 9, c);
    } else
        cv_rect(x, y, 9, 9, c);             /* a square */
}

/* the remix, against the tune as it is (OCT- and OCT+ together put it all back) */
static int ui_cut_changed(void);
static int voice_remixed(int v)             /* anything on voice v overridden */
{
    const rx_voice_t *x = &rx.v[v];
    return x->wave_ovr || x->sr_ovr || x->mute || x->pw_ovr || x->adsr_ovr || x->oct || x->fine ||
           x->key_on || x->shift_on || (x->lfo_target != RX_LFO_OFF && x->lfo_depth);
}
static int filter_remixed(void)             /* the cutoff, resonance, type or a voice's routing */
{
    return ui_cut_changed() || rx.res_off || rx.fmode_ovr || rx.v[0].filt_ovr || rx.v[1].filt_ovr || rx.v[2].filt_ovr;
}
static int deck_on(void) { return !eng_deck_off && (eng_tempo != 100 || eng_vinyl); }   /* (an RSID ignores it) */
static int remixed(void) { return voice_remixed(0) || voice_remixed(1) || voice_remixed(2) || filter_remixed() || deck_on(); }
static uint32_t remix_sig(void)             /* what the top bar shows of it */
{
    return (uint32_t)remixed() | (uint32_t)deck_on() << 1 | (uint32_t)eng_tempo << 2 | (uint32_t)(uint8_t)eng_vinyl << 10;
}

static void draw_logo_small(int x, int y, int cell);   /* (with the intro's logo, below) */
static uint16_t mix565(uint16_t a, uint16_t b, uint32_t q8);

static void draw_top(void)                  /* rows 0 .. 35 */
{
    char b[48], t[16];
    const uint8_t *nm = ui.cur >= 0 ? song_hvsc_name(ui.cur) : 0;
    cv_begin(240, 36, C_BLACK);
    if (ui.mode == UI_SONGS) {
        int used = (int)((song_end - SONG_POOL_BASE) * 100u / SONG_POOL_SIZE);
        int32_t mx;
        draw_logo_small(6, 3, 2);           /* the intro's logo, small: S i D J green red blue white */
        ui_num(b, used); ui_cat(b, "%");    /* right: the songs, the store's meter, how full */
        mx = 240 - 6 - text_w(&FONT_XS, b);
        cv_text(mx, 2, &FONT_XS, b, C_DIM);
        mx -= 6 + 34;
        cv_rect(mx, 7, 34, 5, C_LINE);
        cv_rect(mx, 7, 34 * (used > 100 ? 100 : used) / 100, 5, used >= 90 ? BRAND_R : C_DIM);
        ui_num(b, song_count); ui_cat(b, song_count == 1 ? " SONG" : " SONGS");
        cv_text(mx - 6 - text_w(&FONT_XS, b), 2, &FONT_XS, b, C_GRAY);
        {                                   /* the buttons: spread over the row */
            static const char *const HB[3] = {"SELECT", "PLAY", "SEQ"}, *const HA[3] = {"PICK", "PLAY", "PLAYER"};
            int32_t w[3], tw = 0, gap, x0 = 6;
            int i2;
            for (i2 = 0; i2 < 3; i2++) { w[i2] = text_w(&FONT_XS, HB[i2]) + 4 + text_w(&FONT_XS, HA[i2]); tw += w[i2]; }
            gap = (228 - tw) / 2;
            for (i2 = 0; i2 < 3; i2++) {
                int32_t x = cv_text(x0, 20, &FONT_XS, HB[i2], C_GRAY) + 4;
                cv_text(x, 20, &FONT_XS, HA[i2], C_DIM);
                x0 += w[i2] + gap;
            }
        }
    } else if (!nm) {
        cv_text(6, 2, &FONT_B, "SiDJ", C_HI);
        cv_text(6, 20, &FONT_XS, song_count ? "SELECT A SONG, PRESS PLAY" : "ADD SONGS AT " SIDJ_SITE, C_GRAY);
    } else {
        int32_t nx, tx;                     /* the song number's x, the transport's */
        ui_num(t, ui.subsong); ui_cat(t, "/"); ui_num(t + ui_strlen(t), ui.nsongs);
        nx = 240 - 6 - text_w(&FONT_S, t);
        tx = nx - 7 - TRANSPORT_W;
        ui.top_sig = remix_sig();
        {
            int32_t lim = tx - 12;
            if (ui.top_sig & 1u) {          /* remixed: REMIX, white, until OCT- + OCT+ put it back */
                int32_t w = text_w(&FONT_XS, "REMIX"), rx0 = tx - 8 - w;
                cv_rect(rx0 - 3, 3, w + 6, 13, C_RMX);   /* (the letters: rows 5 .. 13, as the symbol) */
                cv_text(rx0, 2, &FONT_XS, "REMIX", C_BLACK);
                lim = rx0 - 3 - 8;
            }
            ui_hvsc_str(b, nm, 30);         /* the title up to them */
            if (text_w(&FONT_B, b) > lim) { int n = 30; while (n > 0 && text_w(&FONT_B, b) > lim) b[--n] = 0; }
        }
        cv_text(6, 2, &FONT_B, b, C_HI);
        cv_text(nx, 2, &FONT_S, t, ui.nsongs > 1 ? C_WHITE : C_GRAY);
        if (eng_loaded)                     /* playing, paused, or ended (at the song's end or a crash) */
            draw_transport(tx, 5, eng_playing ? 0 : ui.ended ? 2 : 1, eng_playing ? C_HI : C_WHITE);
        {                                   /* the composer, then (right) the output and the time */
            const uint16_t *len = 0; int nlen = 0; uint32_t ds = eng_elapsed_ds();
            int32_t rx0;
            song_file(ui.cur, 0, &len, &nlen);
            ui_time(t, ds);
            if (len && ui.subsong <= nlen && len[ui.subsong - 1]) { ui_cat(t, "/"); ui_time(t + ui_strlen(t), len[ui.subsong - 1]); }
            rx0 = 240 - 6 - text_w(&FONT_XS, t);
            cv_text(rx0, 20, &FONT_XS, t, eng_playing ? C_WHITE : C_GRAY);
            if (set.output && !eng_deck_off) {   /* ASID or BOTH, left of the time (an RSID: neither) */
                const char *o = OUT_TAG[set.output < 3 ? set.output : 0];
                rx0 -= 6 + text_w(&FONT_XS, o);
                cv_text(rx0, 20, &FONT_XS, o, C_AMB);
            }
            if (deck_on()) {                /* the deck: T tempo, P the fader */
                char d[12];
                int v = eng_vinyl, a = v < 0 ? -v : v;
                if (v) {
                    d[0] = v < 0 ? '-' : '+'; d[1] = 0; ui_num(d + 1, a / 10); ui_cat(d, ".");
                    { char dd[2] = {(char)('0' + a % 10), 0}; ui_cat(d, dd); } ui_cat(d, "%");
                    rx0 -= 10 + text_w(&FONT_XS, d);
                    cv_text(rx0, 20, &FONT_XS, d, C_RMX);
                    rx0 -= 3 + text_w(&FONT_XS, "P");
                    cv_text(rx0, 20, &FONT_XS, "P", C_DIM);
                }
                if (eng_tempo != 100) {
                    ui_num(d, eng_tempo); ui_cat(d, "%");
                    rx0 -= 10 + text_w(&FONT_XS, d);
                    cv_text(rx0, 20, &FONT_XS, d, C_RMX);
                    rx0 -= 3 + text_w(&FONT_XS, "T");
                    cv_text(rx0, 20, &FONT_XS, "T", C_DIM);
                }
            }
            ui_hvsc_str(b, nm + HVSC_AUTHOR, 30);
            if (text_w(&FONT_XS, b) > rx0 - 8) { int n = 30; while (n > 0 && text_w(&FONT_XS, b) > rx0 - 8) b[--n] = 0; }
            cv_text(6, 20, &FONT_XS, b, C_AMB);
        }
    }
    cv_rect(0, 35, 240, 1, C_LINE);
    cv_blit(0, 0);
}

static void draw_list(void)                 /* rows 36 .. 215: the songs, under their composers */
{
    enum { HEAD_H = 15, SONG_H = 18, VIEW = 180 };
    int pos, y, total = 0, sel_y = 0, scroll, band, last = -1;
    const uint16_t gc = BRAND_G;            /* the composers, the selection, the loaded song: green */
    char b[40];
    if (song_ord_n != song_count) song_order();
    for (pos = 0; pos < song_ord_n; pos++) {    /* the layout: a heading where the composer changes */
        int k = song_ord[pos];
        if (last < 0 || composer_cmp(last, k)) total += HEAD_H;
        if (k == ui.sel) sel_y = total;
        total += SONG_H;
        last = k;
    }
    scroll = sel_y + SONG_H / 2 - VIEW / 2;     /* the selected song in the middle, within the list, */
    if (scroll > total - VIEW) scroll = total - VIEW;
    if (scroll < 0) scroll = 0;
    {                                           /* the top row whole: back to the start of a row; */
        int top = 0, at = 0, need = sel_y + SONG_H - VIEW, down = -1;   /* the selected one whole too: */
        last = -1;                                                      /* else on to the first row that */
        for (pos = 0; pos < song_ord_n; pos++) {                        /* shows it */
            int k = song_ord[pos];
            if (last < 0 || composer_cmp(last, k)) {
                if (at <= scroll) top = at;
                if (down < 0 && at >= need) down = at;
                at += HEAD_H;
            }
            if (at <= scroll) top = at;
            if (down < 0 && at >= need) down = at;
            at += SONG_H;
            last = k;
        }
        scroll = top < need && down >= 0 ? down : top;
    }
    for (band = 0; band < VIEW; band += 144) {  /* in two bands: the canvas holds 144 rows */
        int bh = VIEW - band < 144 ? VIEW - band : 144;
        cv_begin(240, (uint32_t)bh, C_BLACK);
        y = -scroll - band;
        last = -1;
        for (pos = 0; pos < song_ord_n && y < bh; pos++) {
            int k = song_ord[pos], selected = k == ui.sel;
            const uint8_t *nm = song_hvsc_name(k);
            if (last < 0 || composer_cmp(last, k)) {   /* the composer, once for the group */
                if (y > -HEAD_H) {
                    int w;
                    ui_hvsc_str(b, nm ? nm + HVSC_AUTHOR : 0, PSID_STR_LEN);
                    if (text_w(&FONT_XS, b) > 214) { int n = PSID_STR_LEN; while (n > 0 && text_w(&FONT_XS, b) > 214) b[--n] = 0; }
                    w = cv_text(8, y + 2, &FONT_XS, b, gc);
                    if (w + 6 < 236) cv_rect(w + 6, y + 9, 236 - w - 6, 1, mix565(C_BLACK, gc, 90));
                }
                y += HEAD_H;
            }
            if (y > -SONG_H) {                          /* the song, indented under it */
                uint32_t size;
                const uint16_t *len = 0;
                int nlen = 0, lw = 0;
                const uint8_t *f = song_file(k, &size, &len, &nlen);
                char t[12];
                if (selected) {                         /* a tint of the group's colour, a stripe */
                    cv_rect(4, y, 236, SONG_H, mix565(C_BLACK, gc, 70));
                    cv_rect(4, y, 3, SONG_H, gc);
                }
                t[0] = 0;                               /* the length of the start song, right */
                if (f && size > 0x11 && len) {
                    int st = f[0x11] ? f[0x11] : 1;
                    if (st <= nlen && len[st - 1]) ui_time(t, len[st - 1]);
                }
                if (t[0]) { lw = text_w(&FONT_XS, t); cv_text(240 - 6 - lw, y + 3, &FONT_XS, t, selected ? C_GRAY : C_DIM); }
                ui_hvsc_str(b, nm, PSID_STR_LEN);
                if (text_w(&FONT_S, b) > 214 - lw - 6) { int n = 32; while (n > 0 && text_w(&FONT_S, b) > 214 - lw - 6) b[--n] = 0; }
                cv_text(22, y + 1, &FONT_S, b, selected || k == ui.cur ? C_WHITE : C_GRAY);
                if (k == ui.cur) draw_transport(11, y + 4, eng_playing ? 0 : 1, gc);   /* the loaded song: playing, or waiting */
            }
            y += SONG_H;
            last = k;
        }
        if (total > VIEW) {                         /* the scrollbar: where the rows shown sit in the list */
            int th = VIEW * VIEW / total, ty;
            if (th < 10) th = 10;
            ty = (VIEW - th) * scroll / (total - VIEW) - band;
            cv_rect(0, 0, 3, bh, C_LINE);
            cv_rect(0, ty, 3, th, C_AMB);
        }
        if (!song_count && !band)
            cv_text(6, 4, &FONT_S, "NO SONGS YET. ADD THEM AT", C_GRAY), cv_text(6, 24, &FONT_S, SIDJ_SITE, C_AMB),
            cv_text(6, 46, &FONT_XS, "IN CHROME OR EDGE, THE FM-1 ON USB", C_GRAY);
        cv_blit(0, (uint32_t)(36 + band));
    }
}

/* The scopes' scale. With the stems (eng_scope_stems: each voice as it sounds, the filter, the
 * volume and the envelope in it) each voice is centred on its own average over the ring (the
 * mixer's DC is in the stems), smoothed from frame to frame, and scaled to fit: its largest swing
 * from that average at ~90 % of the box's half. A stem's level varies a lot from tune to tune and
 * with the resonance, so a fixed scale either clips or shows little. The swing is held: a larger
 * one takes over at once (no clipping), a smaller one only as the held one decays (1/64 a frame,
 * about a second), so an envelope or a drum hit does not make the picture breathe. A voice is
 * enlarged at most SCOPE_SPREAD times more than the loudest one's held swing, so a quiet voice
 * beside a loud one stays readable and still looks quieter (and the filter's effect on a voice's
 * level shows), x1 .. x48. Else (the load valve, an RSID) the voice before the filter, as it is. */
#define SCOPE_FIT 29500                     /* the swing fitted, of the half box's 32767 */
#define SCOPE_GAIN_MAX (48 * 256)
#define SCOPE_SPREAD 4
static int32_t scope_avg[3], scope_gain_q8[3] = {16 * 256, 16 * 256, 16 * 256}, scope_hold[3];
static void scope_scale(void)
{
    int v, i;
    int32_t loud = 0;
    for (v = 0; v < 3; v++) {
        int32_t sum = 0, lo = 32767, hi = -32768, avg, dev;
        for (i = 0; i < (int)ENG_SCOPE_N; i++) {
            int32_t s = eng_scope[v][i];
            sum += s;
            if (s < lo) lo = s;
            if (s > hi) hi = s;
        }
        avg = sum / (int32_t)ENG_SCOPE_N;
        scope_avg[v] += (avg - scope_avg[v]) / 4;
        dev = hi - scope_avg[v] > scope_avg[v] - lo ? hi - scope_avg[v] : scope_avg[v] - lo;
        if (dev > scope_hold[v]) scope_hold[v] = dev;
        else scope_hold[v] -= scope_hold[v] / 64;
        if (!rx.v[v].mute && scope_hold[v] > loud) loud = scope_hold[v];
    }
    if (!eng_scope_stems) return;           /* (the voice before the filter: as it is) */
    for (v = 0; v < 3; v++) {
        int32_t d = scope_hold[v], g;
        if (d <= 64) continue;              /* (silence keeps the gain it had) */
        if (d * SCOPE_SPREAD < loud) d = loud / SCOPE_SPREAD;   /* at most SCOPE_SPREAD x the loudest's gain */
        g = SCOPE_FIT * 256 / d;
        scope_gain_q8[v] = g < 256 ? 256 : g > SCOPE_GAIN_MAX ? SCOPE_GAIN_MAX : g;
    }
}

/* A voice's scope: a fixed 10 ms of it, so the pitch shows as how many cycles fit (as DeepSID's
 * fixed zoom does), but never under 1.5 periods of its note (a low note still shows its shape) nor
 * over 8 (a high one does not turn into a solid band); the period from the frequency register and
 * the chip's clock; at least a sample a column, at most half the ring; noise and a silent voice the
 * 10 ms. Each of the w columns shows the whole range of its samples (a pulse's edge, noise), joined
 * to the next.
 *
 * Held still: the window starts on a rising crossing of the voice's average, found to a fraction
 * of a sample, armed only once the wave has been clearly below it (hysteresis: a ripple is no
 * crossing). Of the crossings in the period before the window's end the one nearest where the last
 * frame's would be now (its position plus whole periods, eng_scope_n counting the samples) is
 * taken, so the same point of the wave stays under the same column; without one near enough (a new
 * note, a change of shape), the first after the lowest point of that period (a pulse's rising
 * edge, a resonant wave's main swing). With no crossing at all it runs free. Scale: scope_scale. */
#define SCOPE_WIN (ENG_RATE / 100)         /* 10 ms */
#define SCOPE_MIN_Q1 3                      /* at least 1.5 periods (in halves) */
#define SCOPE_MAX_PER 8                     /* at most 8 periods */
static uint32_t scope_trig[3];              /* the last trigger, in eng_scope_n's samples (Q8) */
static uint8_t scope_trig_ok[3];
#define SCOPE_COLS 96                       /* the scope's width: the shape kept for the correlation */
static int16_t scope_prof[3][SCOPE_COLS];   /* the last frame's columns (means, centred, / 32) */
static int32_t scope_prof_per[3];           /* the period they were drawn at, 0: none */
static int32_t scope_pre[ENG_SCOPE_N + 1] SIDJ_POOL;   /* prefix sums over the trigger's search and the window */
static void draw_scope(int x, int y, int w, int h, int v)
{
    const int32_t N = (int32_t)ENG_SCOPE_N, half = h / 2 - 1;
    uint32_t total = eng_scope_n, wp = total % ENG_SCOPE_N, f = sid_freq(rx.out, v);
    uint32_t clk = eng_ntsc ? C64_NTSC_HZ : C64_PAL_HZ, base = total - (uint32_t)N;   /* sample k is base + k */
    int32_t per_q8 = 0, per = 0, win, start_q8, i, lo, mid = y + h / 2, avg = scope_avg[v];
    int32_t g = eng_scope_stems ? scope_gain_q8[v] : 256, off = eng_scope_stems ? avg : 0, py = 0;
#define SC(k) ((int32_t)eng_scope[v][(wp + (uint32_t)(k)) % ENG_SCOPE_N])   /* k = 0 the oldest */
    if (f && (rx.out[SID_ADDR_VOICE(v) + SID_ADDR_CTRL] & SID_CTRL_WAVES) != SID_CTRL_NOISE) {
        uint32_t hz_q4 = (f * (clk >> 10)) >> 10;           /* f * clock / 2^24, in 1/16 Hz */
        if (hz_q4) { per_q8 = (int32_t)(ENG_RATE * 16u * 256u / hz_q4); per = per_q8 >> 8; }
    }
    win = SCOPE_WIN;
    if (per) {
        if (win < per * SCOPE_MIN_Q1 / 2) win = per * SCOPE_MIN_Q1 / 2;
        if (win > per * SCOPE_MAX_PER) win = per * SCOPE_MAX_PER;
    }
    if (win < w) win = w;
    if (win > N / 2) win = N / 2;           /* room before it for the trigger's search */
    start_q8 = (N - win) << 8;
    scope_trig_ok[v] = scope_trig_ok[v] && per;
    if (per && per < N - win) {             /* the trigger: in the period before the window's end */
        int32_t k, from = N - win - per, lowest = from, hys = scope_hold[v] / 8, armed = 0;
        int32_t best = -1, best_d = per_q8 / 3, first = -1;
        uint32_t pred = 0;
        if (hys < 32) hys = 32;
        if (scope_trig_ok[v]) {             /* where the last trigger's point of the wave is now */
            uint32_t at = (base + (uint32_t)(N - win - per / 2)) << 8;   /* the search's middle */
            uint32_t m = (at - scope_trig[v] + (uint32_t)per_q8 / 2) / (uint32_t)per_q8;
            pred = scope_trig[v] + m * (uint32_t)per_q8;
        }
        for (k = from; k < N - win; k++) if (SC(k) < SC(lowest)) lowest = k;
        for (k = from; k <= N - win; k++) {
            int32_t s0 = SC(k - 1), s1 = SC(k);
            if (s1 < avg - hys) armed = 1;
            if (armed && s0 < avg && s1 >= avg) {
                int32_t c = ((k - 1) << 8) + (s1 != s0 ? (avg - s0) * 256 / (s1 - s0) : 0);   /* Q8 */
                armed = 0;
                if (first < 0 && k > lowest) first = c;
                if (scope_trig_ok[v]) {
                    int32_t dd = (int32_t)(((base << 8) + (uint32_t)c) - pred);
                    if (dd < 0) dd = -dd;
                    if (dd < best_d) { best_d = dd; best = c; }
                }
            }
        }
        if (best < 0) best = first;
        if (best >= 0) {
            start_q8 = best;
            scope_trig[v] = (base << 8) + (uint32_t)best;
            scope_trig_ok[v] = 1;
        } else scope_trig_ok[v] = 0;
        /* the shape's correlation: with the last frame's columns at the same period (a note held, its
         * shape changing under a filter sweep or a pulse width), the start within the period where
         * this frame's columns match them best; an edge trigger cannot hold a shape whose edges
         * change, this does. The columns' means from prefix sums: each candidate costs w products. */
        if (w == SCOPE_COLS && scope_prof_per[v] && per_q8 / 8 > (scope_prof_per[v] > per_q8 ? scope_prof_per[v] - per_q8 :
                                                              per_q8 - scope_prof_per[v])) {
            /* (within 12 %: a vibrato or a slide; the columns follow the period, so the shape compares) */
            static int32_t score[257] SIDJ_POOL;
            int32_t n = win + per, c, step = (per + 255) / 256, cand, bestc = -1, nc = 0, j;   /* <= 256 candidates */
            int32_t bests = 0;
            if (n > (int32_t)ENG_SCOPE_N) n = (int32_t)ENG_SCOPE_N;   /* (from >= 0 holds it to N anyway) */
            scope_pre[0] = 0;
            for (k = 0; k < n; k++) scope_pre[k + 1] = scope_pre[k] + (SC(from + k) - avg);
            for (cand = 0; cand < per && nc < 257; cand += step) {   /* one period: each point of the wave once */
                int32_t sc = 0;   /* (|m|, |prof| < 2048, 96 columns: under 2^29) */
                for (c = 0; c < SCOPE_COLS; c++) {
                    int32_t a = cand + c * win / SCOPE_COLS, b = cand + (c + 1) * win / SCOPE_COLS, m;
                    if (b <= a) b = a + 1;
                    m = (scope_pre[b] - scope_pre[a]) / (b - a) / 32;
                    sc += m * scope_prof[v][c];
                }
                score[nc++] = sc;
                if (bestc < 0 || sc > bests) { bests = sc; bestc = cand; }
            }
            if (bestc >= 0 && bests > 0 && scope_trig_ok[v]) {   /* near-ties (a resonant wave matches
                                                                  * about as well half a period on): the
                                                                  * one nearest the prediction */
                int32_t bd = 0x7fffffff;
                for (j = 0; j < nc; j++)
                    if (score[j] >= bests - bests / 10) {
                        int32_t dd = (int32_t)(((base << 8) + (uint32_t)((from + j * step) << 8)) - pred);
                        if (dd < 0) dd = -dd;
                        if (dd < bd) { bd = dd; bestc = j * step; }
                    }
            }
            if (bestc >= 0 && bests > 0) {
                int32_t jb = bestc / step, frac = 0;
                start_q8 = (from + bestc) << 8;
                if (jb > 0 && jb < nc - 1) {    /* to a fraction of a step: the parabola through the
                                                 * best score and its neighbours (scaled: no overflow) */
                    int32_t a = score[jb - 1] >> 4, b = score[jb] >> 4, cc = score[jb + 1] >> 4, den = a - 2 * b + cc;
                    if (den <= -128) frac = (a - cc) / (den / 128);   /* the vertex, Q8 of a step: */
                    if (frac > 128) frac = 128;                      /* half a step either way */
                    if (frac < -128) frac = -128;
                    start_q8 += frac * step;
                }
                scope_trig[v] = (base << 8) + (uint32_t)start_q8;
                scope_trig_ok[v] = 1;
            }
        }
    }
    cv_rect(x, mid, w, 1, C_LINE);
    scope_prof_per[v] = 0;
    if (w == SCOPE_COLS && per) {           /* this frame's shape, for the next frame's correlation */
        for (i = 0; i < SCOPE_COLS; i++) {
            int32_t k0 = (start_q8 + i * win * 256 / w) >> 8, k1 = (start_q8 + (i + 1) * win * 256 / w) >> 8, k, sum = 0;
            if (k1 <= k0) k1 = k0 + 1;
            if (k1 > N) k1 = N;
            if (k0 >= k1) k0 = k1 - 1;
            for (k = k0; k < k1; k++) sum += SC(k) - avg;
            scope_prof[v][i] = (int16_t)(sum / (k1 - k0) / 32);
        }
        scope_prof_per[v] = per_q8;
    }
    if (rx.v[v].mute) return;               /* muted: the null line only (the voice's DC still moves) */
    for (i = 0; i < w; i++) {
        int32_t k0 = (start_q8 + i * win * 256 / w) >> 8, k1 = (start_q8 + (i + 1) * win * 256 / w) >> 8, k, hi, yh, yl, yf, yl2;
        if (k1 <= k0) k1 = k0 + 1;
        if (k1 > N) k1 = N;
        if (k0 >= k1) k0 = k1 - 1;
        lo = hi = SC(k0);
        for (k = k0 + 1; k < k1; k++) { int32_t s = SC(k); if (s < lo) lo = s; if (s > hi) hi = s; }
#define SY(s) (mid - (int)(((((s) - off) * g) >> 8 > 32767 ? 32767 : (((s) - off) * g) >> 8 < -32767 ? -32767 : (((s) - off) * g) >> 8) * half / 32768))
        yh = SY(hi); yl = SY(lo); yf = SY(SC(k0)); yl2 = SY(SC(k1 - 1));
        if (vc_glow) {                      /* the glow first: a fade hanging below the top */
            int j;
            for (j = 0; j < GLOW_N && yh + 1 + j < y + h; j++) cv_pset(x + i, yh + 1 + j, vc_grad[v][j]);
        }
        if (i) cv_line(x + i - 1, py, x + i, yf, vc_line[v]);   /* joined to the column before */
        cv_rect(x + i, yh, 1, yl - yh + 1, vc_line[v]);
        py = yl2;
#undef SY
    }
#undef SC
}

static void draw_voices(void)               /* rows 36 .. 179: three rows of 48 */
{
    int v;
    scope_scale();
    cv_begin(240, 144, C_BLACK);
    for (v = 0; v < 3; v++) {
        int y = v * 48, selected = v == ui.voice, held = v == ui.voice_held;
        const rx_voice_t *x = &rx.v[v];
        const uint8_t *r = rx.out + SID_ADDR_VOICE(v);
        uint8_t ctrl = r[SID_ADDR_CTRL], ad = r[SID_ADDR_AD], sr = r[SID_ADDR_SR];
        uint32_t freq = sid_freq(rx.out, v), pw = sid_pw(rx.out, v);
        int env = (int)eng_sid[0].voice[v].envelope.envelope_counter, gate = ctrl & SID_CTRL_GATE, i;
        char b[8];
        cv_rect(0, y + 47, 240, 1, C_LINE);
        b[0] = (char)('1' + v); b[1] = 0;
        {                                   /* the number: filled when selected; white when remixed */
            int rmx = voice_remixed(v);
            if (selected || held) {
                if (rmx) { cv_rect(0, y + 0, 18, 19, C_RMX); cv_rect(1, y + 1, 16, 17, C_BLACK); }
                cv_rect(2, y + 2, 14, 15, held ? C_WHITE : vc_line[v]); cv_text(5, y + 2, &FONT_XS, b, C_BLACK);
            } else {
                cv_rect(2, y + 2, 14, 15, rmx ? C_RMX : C_LINE); cv_rect(3, y + 3, 12, 13, C_BLACK);
                cv_text(5, y + 2, &FONT_XS, b, rmx ? C_RMX : C_GRAY);
            }
        }
        /* what is remixed on the voice, three letters under the number, clear of the scope */
        if (x->mute) cv_text(0, y + 20, &FONT_XS, "MUT", C_RMX);
        else if (x->key_on) cv_text(0, y + 20, &FONT_XS, "KEY", C_RMX);
        else if (x->shift_on) cv_text(0, y + 20, &FONT_XS, "TRN", C_RMX);
        else if (x->oct || x->fine) cv_text(0, y + 20, &FONT_XS, "PIT", C_RMX);
        else if (x->lfo_target != RX_LFO_OFF && x->lfo_depth) cv_text(0, y + 20, &FONT_XS, "LFO", C_RMX);
        draw_scope(24, y + 1, 96, 46, v);
        /* note, waveform */
        note_text(b, note_of_freq(freq));
        cv_text(124, y + 1, &FONT_S, b, gate ? (x->key_on ? C_RMX : vc_line[v]) : C_DIM);
        badge_c(160, y + 2, "P", ctrl & SID_CTRL_PULSE, 12, vc_line[v]);   /* the order of the buttons: FX SEL ENV LFO EDIT GLO */
        badge_c(173, y + 2, "T", ctrl & SID_CTRL_TRI, 12, vc_line[v]);
        badge_c(186, y + 2, "S", ctrl & SID_CTRL_SAW, 12, vc_line[v]);
        badge_c(199, y + 2, "N", ctrl & SID_CTRL_NOISE, 12, vc_line[v]);
        badge_c(213, y + 2, "SY", ctrl & SID_CTRL_SYNC, 13, vc_line[v]);
        badge_c(227, y + 2, "RI", ctrl & SID_CTRL_RING, 13, vc_line[v]);
        if (x->wave_ovr || x->sr_ovr) cv_rect(160, y + 16, 80, 1, C_RMX);
        /* envelope: four bars, the level, the gate; the pulse width */
        for (i = 0; i < 4; i++) {
            int val = i == 0 ? ad >> 4 : i == 1 ? ad & 15 : i == 2 ? sr >> 4 : sr & 15, hh = 1 + val * 16 / 15;
            int ovr = x->adsr_ovr & (1 << i);
            cv_rect(124 + i * 9, y + 42 - 16, 7, 17, C_LINE);
            cv_rect(124 + i * 9, y + 43 - hh, 7, hh, ovr ? C_RMX : vc_mid[v]);
        }
        cv_text(124, y + 36, &FONT_XS, "", C_DIM);
        cv_rect(164, y + 24, 7, 17, C_LINE);
        cv_rect(164, y + 41 - env * 16 / 255, 7, env * 16 / 255 + 1, gate ? vc_line[v] : C_GRAY);
        cv_text(176, y + 20, &FONT_XS, "PW", x->pw_ovr ? C_RMX : C_DIM);
        cv_rect(194, y + 24, 44, 5, C_LINE);
        cv_rect(194, y + 24, (int)(pw * 44u / (SID_PW_MAX + 1u)), 5, x->pw_ovr ? C_RMX : vc_mid[v]);
        if (ctrl & SID_CTRL_PULSE) cv_rect(194 + (int)(pw * 44u / (SID_PW_MAX + 1u)) - 1, y + 23, 2, 7, vc_line[v]);
        cv_rect(176, y + 32, 62, 1, C_LINE);
        {
            char f[8];
            f[0] = "0123456789ABCDEF"[(freq >> 12) & 15]; f[1] = "0123456789ABCDEF"[(freq >> 8) & 15];
            f[2] = "0123456789ABCDEF"[(freq >> 4) & 15]; f[3] = "0123456789ABCDEF"[freq & 15]; f[4] = 0;
            cv_text(176, y + 34, &FONT_XS, "F", C_DIM);
            cv_text(184, y + 34, &FONT_XS, f, C_DIM);
            cv_text(212, y + 34, &FONT_XS, gate ? "GATE" : "", vc_line[v]);
        }
    }
    cv_blit(0, 36);
}

static int ui_cut_changed(void)
{
    return rx.cutoff_mode == RX_CUT_OFFSET ? rx.cutoff_off != 0 : rx.cutoff_scale != RX_CUTOFF_ONE;
}
static void ui_cut_text(char *b)            /* "x1.25" or "+64" */
{
    if (rx.cutoff_mode == RX_CUT_SCALE) { ui_scale(b, rx.cutoff_scale); return; }
    if (rx.cutoff_off >= 0) *b++ = '+';
    ui_num(b, rx.cutoff_off);
}

static void label_block(int x, int y, const char *s, int remixed)   /* a label; gold block when remixed */
{
    if (remixed) {                          /* (FONT_XS's letters: rows y + 3 .. y + 11) */
        cv_rect(x - 2, y + 1, text_w(&FONT_XS, s) + 4, 13, C_RMX);
        cv_text(x, y, &FONT_XS, s, C_BLACK);
    } else cv_text(x, y, &FONT_XS, s, C_GRAY);
}

static void draw_filter(void)               /* rows 180 .. 215 */
{
    uint32_t cut = sid_fc(rx.out), res = rx.out[SID_ADDR_RES_FILT] >> SID_FILT_RES_SHIFT, mode = rx.out[SID_ADDR_MODE_VOL];
    uint8_t route = rx.out[SID_ADDR_RES_FILT] & SID_FILT_VOICES;
    char b[16];
    int i;
    cv_begin(240, 36, C_BLACK);
    label_block(4, 2, "CUT", ui_cut_changed());   /* the cutoff, gold when remixed */
    cv_rect(28, 4, 74, 7, C_LINE);          /* as the resonance's bars below: 28 .. 101 */
    cv_rect(28, 4, (int)(cut * 74u / (SID_FC_MAX + 1u)), 7, ui_cut_changed() ? C_RMX : C_HI);
    ui_num(b, (int)cut);
    cv_text(110, 2, &FONT_XS, b, C_DIM);
    if (ui_cut_changed()) { ui_cut_text(b); cv_text(140, 2, &FONT_XS, b, C_RMX); }
    label_block(4, 20, "RES", rx.res_off != 0);
    for (i = 0; i < SID_NIBBLE_MAX; i++) cv_rect(28 + i * 5, 22, 4, 8, (uint32_t)i < res ? (rx.res_off ? C_RMX : C_HI) : C_LINE);
    badge(110, 19, "LP", mode & SID_MODE_LP, 16);
    badge(128, 19, "BP", mode & SID_MODE_BP, 16);
    badge(146, 19, "HP", mode & SID_MODE_HP, 16);
    if (rx.fmode_ovr) cv_rect(110, 33, 52, 1, C_RMX);     /* the type frozen by the remix */
    badge(164, 19, "3OFF", mode & SID_MODE_3OFF, 28);
    for (i = 0; i < 3; i++) {
        b[0] = (char)('1' + i); b[1] = 0; badge(196 + i * 13, 19, b, route & (1 << i), 12);
        if (rx.v[i].filt_ovr) cv_rect(196 + i * 13, 33, 12, 1, C_RMX);   /* a routing bit frozen */
    }
    cv_text(196, 2, &FONT_XS, "VOL", C_GRAY);
    ui_num(b, mode & SID_MODE_VOL);
    cv_text(222, 2, &FONT_XS, b, C_HI);
    cv_rect(0, 35, 240, 1, C_LINE);
    cv_blit(0, 180);
}

static int st_tag(int x, const char *name)  /* the status line: the page, inverted (dark on a light block) */
{
    int w = text_w(&FONT_XS, name);
    cv_rect(x - 2, 7, w + 5, 13, C_AMB);    /* the letters are rows 9 .. 17: two rows around them */
    return cv_text(x, 6, &FONT_XS, name, C_BLACK) + 8;
}

static int st_field(int x, const char *label, const char *value, uint16_t c)   /* the status line: the next x */
{
    if (label) x = cv_text(x, 6, &FONT_XS, label, C_DIM) + 3;
    return cv_text(x, 6, &FONT_XS, value, c) + 7;
}

static int st_kfield(int x, int knob, const char *label, const char *value, uint16_t c)   /* "K1 PW 384" */
{
    char k[3] = {'K', (char)('0' + knob), 0};
    x = cv_text(x, 6, &FONT_XS, k, C_DIM) + 3;
    if (label) x = cv_text(x, 6, &FONT_XS, label, C_DIM) + 3;
    return cv_text(x, 6, &FONT_XS, value, c) + 5;
}

static void draw_status(void)               /* rows 216 .. 239 */
{
    char b[48], t[12];
    uint32_t now = plat_ms();
    cv_begin(240, 24, C_BLACK);
    if (now < ui.msg_until) {
        cv_text(4, 4, &FONT_S, ui.msg, C_WHITE);
    } else if (ui.mode == UI_SONGS) {
        int px = 4;                         /* the settings by their knobs, SAVE keeps them */
        if (set.model == 0) {               /* the file's: the selected song's chip, and AUTO */
            uint32_t size = 0;
            const uint8_t *f = song_count ? song_file(ui.sel, &size, 0, 0) : 0;
            sidfile_t sf;
            int is8580 = f && sidfile_parse(&sf, f, size) == 0 && sidfile_model(&sf, 0) == SID_MODEL_8580;
            px = st_kfield(px, 1, "SID", is8580 ? "8580" : "6581", C_GRAY) - 2;
            px = cv_text(px, 6, &FONT_XS, "AUTO", C_DIM) + 11;
        } else px = st_kfield(px, 1, "SID", set.model == 1 ? "6581" : "8580", C_GRAY) + 6;
        px = st_kfield(px, 2, "OUT", set.output == 0 ? "EMU" : set.output == 1 ? "ASID" : "BOTH", C_GRAY) + 6;
        st_kfield(px, 3, "USB", set.usb4 ? "5CH" : "MIX", C_GRAY);
    } else {
        const rx_voice_t *x = &rx.v[ui.voice];
        int px = 4;                         /* the fields one by one: a dim label, its value, a gap */
        if (ui.page == PG_FX || ui.page == PG_ENV || ui.page == PG_LFO) {   /* a voice's page: which voice */
            b[0] = 'V'; b[1] = (char)('1' + ui.voice); b[2] = 0;
            px = st_field(px, 0, b, C_GRAY);
        }
        px = st_tag(px + 2, PG_NAME[ui.page]);
        if (ui.page == PG_FX) {             /* each value by its knob: K1, K2 ... (as SONGS) */
            ui_num(t, x->pw_ovr ? x->pw_val : remix_tune_pw(&rx, ui.voice));
            px = st_kfield(px, 1, "PW", t, x->pw_ovr ? C_RMX : C_GRAY);
            t[0] = 0; if (x->oct >= 0) ui_cat(t, "+"); ui_num(t + ui_strlen(t), x->oct);
            px = st_kfield(px, 2, "OCT", t, x->oct ? C_RMX : C_GRAY);
            t[0] = 0; if (x->fine >= 0) ui_cat(t, "+"); ui_num(t + ui_strlen(t), x->fine);
            px = st_kfield(px, 3, "FINE", t, x->fine ? C_RMX : C_GRAY);
        } else if (ui.page == PG_ENV) {
            uint8_t ad = rx.out[SID_ADDR_VOICE(ui.voice) + SID_ADDR_AD], sr = rx.out[SID_ADDR_VOICE(ui.voice) + SID_ADDR_SR];
            ui_num(t, ad >> 4); px = st_kfield(px, 1, "A", t, (x->adsr_ovr & RX_ADSR_A) ? C_RMX : C_GRAY);
            ui_num(t, ad & 15); px = st_kfield(px, 2, "D", t, (x->adsr_ovr & RX_ADSR_D) ? C_RMX : C_GRAY);
            ui_num(t, sr >> 4); px = st_kfield(px, 3, "S", t, (x->adsr_ovr & RX_ADSR_S) ? C_RMX : C_GRAY);
            ui_num(t, sr & 15); px = st_kfield(px, 4, "R", t, (x->adsr_ovr & RX_ADSR_R) ? C_RMX : C_GRAY);
        } else if (ui.page == PG_LFO && eng_deck_off) {
            px = st_field(px, 0, "RSID: NO LFO", C_DIM);
        } else if (ui.page == PG_LFO) {
            int on = x->lfo_target != RX_LFO_OFF && x->lfo_depth;
            uint16_t c = on ? C_RMX : C_GRAY, hz = RX_LFO_CHZ[x->lfo_rate < RX_LFO_NRATE ? x->lfo_rate : 0];
            if (hz < 1000) {                /* 0.05 .. 9.3: two digits; 10 .. 20 whole */
                t[0] = (char)('0' + hz / 100); t[1] = '.';
                if (hz < 100) { t[2] = (char)('0' + hz / 10); t[3] = (char)('0' + hz % 10); t[4] = 0; }
                else { t[2] = (char)('0' + hz / 10 % 10); t[3] = 0; }
            } else ui_num(t, hz / 100);
            ui_cat(t, "HZ");
            px = st_kfield(px, 1, 0, t, c);
            ui_num(t, x->lfo_depth); ui_cat(t, "%"); px = st_kfield(px, 2, 0, t, c);
            px = st_kfield(px, 3, 0, LFO_TGT_NAME[x->lfo_target < RX_LFO_NTARGET ? x->lfo_target : 0], c);
            px = st_kfield(px, 4, 0, LFO_SHAPE_NAME[x->lfo_shape < RX_LFO_NSHAPE ? x->lfo_shape : 0], c);
        } else if (ui.page == PG_GLO && eng_deck_off) {
            px = st_field(px, 0, "RSID: 100%, NO FADER", C_DIM);
        } else if (ui.page == PG_GLO) {
            int v = eng_vinyl, a = v < 0 ? -v : v;
            ui_num(t, eng_tempo); ui_cat(t, "%");
            px = st_kfield(px, 1, "TEMPO", t, eng_tempo != 100 ? C_RMX : C_GRAY);
            t[0] = v < 0 ? '-' : '+'; t[1] = 0; ui_num(t + 1, a / 10); ui_cat(t, ".");
            { char dd[2] = {(char)('0' + a % 10), 0}; ui_cat(t, dd); } ui_cat(t, "%");
            px = st_kfield(px, 2, "PITCH", t, v ? C_RMX : C_GRAY);
        } else {
            ui_cut_text(t); px = st_kfield(px, 1, "CUT", t, ui_cut_changed() ? C_RMX : C_GRAY);
            t[0] = 0; if (rx.res_off >= 0) ui_cat(t, "+"); ui_num(t + ui_strlen(t), rx.res_off);
            px = st_kfield(px, 2, "RES", t, rx.res_off ? C_RMX : C_GRAY);
        }
        (void)px;                           /* (the output and the load: the top bar, tools/fm1_sid.py stats) */
    }
    cv_blit(0, 216);
}

static void ui_leds(void)
{
    uint32_t b = 0, k = 0;
    int i, l;
    if (eng_playing) b |= 1u << B_PLAY;
    if (ui.mode == UI_PLAYER) {
        uint32_t held = plat_buttons();
        b |= 1u << VOICE_BTN[ui.voice];
        if ((held & (1u << B_SEL)) && ui.voice_held < 0) {     /* SEL held: the routing and the filter type */
            b &= ~(1u << VOICE_BTN[ui.voice]);
            for (i = 0; i < 3; i++) if (rx.out[SID_ADDR_RES_FILT] & (1 << i)) b |= 1u << VOICE_BTN[i];
            if (rx.out[SID_ADDR_MODE_VOL] & SID_MODE_LP) b |= 1u << B_LFO;
            if (rx.out[SID_ADDR_MODE_VOL] & SID_MODE_BP) b |= 1u << B_EDIT;
            if (rx.out[SID_ADDR_MODE_VOL] & SID_MODE_HP) b |= 1u << B_GLO;
            b |= 1u << B_SEL;
        } else if (ui.voice_held >= 0) {
            uint8_t ctrl = rx.out[SID_ADDR_VOICE(ui.voice_held) + SID_ADDR_CTRL];
            for (i = 0; i < 6; i++) if (ctrl & WAVE_BIT[i]) b |= 1u << WAVE_BTN[i];
            if (rx.v[ui.voice_held].mute) b |= 1u << B_REC;
        } else {
            b |= 1u << PG_BTN[ui.page];
            if (rx.v[ui.voice].mute) b |= 1u << B_REC;
        }
        if (ui.octave < 0) b |= 1u << B_OCTDN;
        if (ui.octave > 0) b |= 1u << B_OCTUP;
        if ((l = voice_lane(ui.voice)) >= 0) {        /* the sounding key, on the keys' range */
            if (lane[l].sound - 1 - KEY_BASE >= 0 && lane[l].sound - 1 - KEY_BASE < NKEYS) k |= 1u << (lane[l].sound - 1 - KEY_BASE);
        } else if (rx.out[SID_ADDR_VOICE(ui.voice) + SID_ADDR_CTRL] & SID_CTRL_GATE) {       /* the selected voice's note on the keys; off their range:
                                                         * the OCT button towards it flashes */
            int note = note_of_freq(sid_freq(rx.out, ui.voice));
            int n = note - KEY_BASE - 12 * ui.octave;
            uint32_t flash = (plat_ms() / 150u) & 1u;
            if (note && n >= 0 && n < NKEYS) k |= 1u << n;
            else if (note && n < 0) b = flash ? b | 1u << B_OCTDN : b & ~(1u << B_OCTDN);
            else if (note) b = flash ? b | 1u << B_OCTUP : b & ~(1u << B_OCTUP);
        }
    }
    ui.lit_keys = k;
    plat_leds(b, k);
}

/* ------------------------------------------------------------- intro --- */
/* The boot intro, about 3 s: raster bars in the three voice colours glide behind "SiDJ", whose
 * pixel-block letters drop in one by one (S i D in the voices' green, red, blue, J in white);
 * a scope trace draws itself across, morphing pulse / saw / triangle; the tagline types itself
 * in; a moment later the version and the credit fade in under it; a CRT switch-off ends it. Any button, key or knob ends it at once (and does its job).
 * Drawn full screen in two bands, about 30 times a second; the brand colours are fixed. */
#define INTRO_MS 4000u
#define CREDIT_MS 2950u                 /* the credit: a moment after the tagline is typed (2625 ms) */
#ifndef SIDJ_VERSION                    /* (the builds set it: --release X.Y.Z, else tools/version.sh) */
#define SIDJ_VERSION "1.0.0"
#endif
#define LOGO_CELL 8
static const int16_t SIN64[64] = {0, 25, 50, 74, 98, 121, 142, 162, 181, 198, 213, 226, 237, 245, 251, 255, 256, 255, 251, 245, 237, 226, 213, 198, 181, 162, 142, 121, 98, 74, 50, 25,
    0, -25, -50, -74, -98, -121, -142, -162, -181, -198, -213, -226, -237, -245, -251, -255, -256, -255, -251, -245, -237, -226, -213, -198, -181, -162, -142, -121, -98, -74, -50, -25};
static int isin(uint32_t a) { return SIN64[a & 63]; }        /* a: 64 steps a turn, result * 256 */
/* the letters, 8 rows each, a byte per row (bit 7 = the left cell) */
static const struct { uint8_t w; uint8_t row[8]; } LOGO[4] = {
    {7, {0x7C, 0xC6, 0xC0, 0x7C, 0x06, 0x06, 0xC6, 0x7C}},   /* S */
    {2, {0xC0, 0x00, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0, 0xC0}},   /* i */
    {7, {0xF8, 0xCC, 0xC6, 0xC6, 0xC6, 0xC6, 0xCC, 0xF8}},   /* D */
    {6, {0x0C, 0x0C, 0x0C, 0x0C, 0x0C, 0xCC, 0xCC, 0x78}},   /* J */
};
static const uint16_t LOGO_COL[4] = {BRAND_G, BRAND_R, BRAND_B, RGB(240, 240, 244)};

static void draw_logo_small(int x, int y, int cell)   /* the logo in cell-sized blocks, its colours */
{
    int k, row, col;
    for (k = 0; k < 4; k++) {
        for (row = 0; row < 8; row++)
            for (col = 0; col < LOGO[k].w; col++)
                if (LOGO[k].row[row] & (0x80 >> col)) cv_rect(x + col * cell, y + row * cell, cell, cell, LOGO_COL[k]);
        x += (LOGO[k].w + 1) * cell;
    }
}

static uint16_t mix565(uint16_t a, uint16_t b, uint32_t q8)  /* a towards b by q8 / 256 */
{
    int32_t ar = a >> 11, ag = (a >> 5) & 63, ab = a & 31, br = b >> 11, bg = (b >> 5) & 63, bb = b & 31;
    ar += ((br - ar) * (int32_t)q8) / 256; ag += ((bg - ag) * (int32_t)q8) / 256; ab += ((bb - ab) * (int32_t)q8) / 256;
    return (uint16_t)(ar << 11 | ag << 5 | ab);
}

static int logo_drop(uint32_t t, int k)    /* letter k's height above its place, pixels */
{
    int32_t start = 250 + 130 * k, p;
    if ((int32_t)t < start) return 1000;
    p = (int32_t)(t - (uint32_t)start) * 1024 / 460;             /* 0 .. 1024 over 460 ms */
    if (p >= 1024) return 0;
    if (p < 768) { int32_t q = 768 - p; return (int)(140 * q / 768 * q / 768); }   /* the fall, slowing */
    return (int)((10 * isin((uint32_t)(p - 768) * 32 / 256)) / 256);   /* one small hop */
}

static void intro_band(uint32_t t, int by, int bh)
{
    int k, x, row, col, cx, i;
    cv_begin(240, (uint32_t)bh, C_BLACK);
    if (t >= INTRO_MS - 260u) {                              /* the CRT switch-off: a line that shrinks */
        uint32_t q = (t - (INTRO_MS - 260u)) * 256u / 260u, w = 240u - 240u * q / 256u;
        if (by <= 119 && by + bh > 119) cv_rect(120 - (int)w / 2, 119 - by, (int)w, 2, mix565(RGB(255, 255, 255), C_BLACK, q));
        return;
    }
    for (k = 0; k < 3; k++) {                                /* raster bars, behind it all */
        static const uint16_t bc[3] = {BRAND_G, BRAND_R, BRAND_B};
        int yc = 92 + (54 * isin(t / 22 + (uint32_t)k * 21)) / 256;
        for (i = -9; i <= 9; i++) {
            uint32_t q = (uint32_t)(i < 0 ? -i : i) * 26u;   /* bright in the middle, dark at the edges */
            cv_rect(0, yc + i - by, 240, 1, mix565(mix565(bc[k], C_BLACK, 150), C_BLACK, q));
        }
    }
    cx = (240 - 25 * LOGO_CELL) / 2;                         /* the logo: 7 + 1 + 2 + 1 + 7 + 1 + 6 cells */
    for (k = 0; k < 4; k++) {
        int lift = logo_drop(t, k), y0 = 60 - lift, pass;
        if (lift < 1000)
            for (pass = 0; pass < 2; pass++)                 /* the letter's shadow first, then its face */
                for (row = 0; row < 8; row++)
                    for (col = 0; col < LOGO[k].w; col++)
                        if (LOGO[k].row[row] & (0x80 >> col)) {
                            int px = cx + col * LOGO_CELL, py = y0 + row * LOGO_CELL;
                            if (!pass) cv_rect(px + 3, py + 3 - by, LOGO_CELL - 1, LOGO_CELL - 1, RGB(10, 10, 14));
                            else cv_rect(px, py - by, LOGO_CELL - 1, LOGO_CELL - 1,
                                         mix565(RGB(255, 255, 255), LOGO_COL[k], (uint32_t)(64 + row * 24)));
                        }
        cx += (LOGO[k].w + 1) * LOGO_CELL;
    }
    if (t > 900) {                                           /* the scope, drawn across, morphing */
        int reach = (int)((t - 900) * 2 / 5), shape = (int)((t / 600) % 3), yb = 162, prev = 0;
        if (reach > 236) reach = 236;
        for (x = 2; x < 2 + reach; x++) {
            uint32_t ph = ((uint32_t)x * 3u + t / 3u) & 255u;
            int v = shape == 0 ? (ph < 128 ? 12 : -12) : shape == 1 ? 12 - (int)ph * 24 / 256
                  : (ph < 128 ? -12 + (int)ph * 24 / 128 : 12 - ((int)ph - 128) * 24 / 128);
            int y = yb - v;
            for (i = 1; i <= 6; i++) cv_rect(x, y + i - by, 1, 1, mix565(RGB(90, 120, 255), C_BLACK, (uint32_t)(i * 40)));
            if (x > 2) cv_line(x - 1, prev - by, x, y - by, RGB(235, 240, 255));
            prev = y;
        }
    }
    if (t > 1500) {                                          /* the tagline, typed */
        static const char tag[] = "SID PLAYER  +  LIVE REMIX";
        char b[sizeof tag];
        uint32_t n = (t - 1500) / 45, j;
        for (j = 0; j < n && j < sizeof tag - 1; j++) b[j] = tag[j];
        b[j] = 0;
        cv_text((240 - text_w(&FONT_S, tag)) / 2, 192 - by, &FONT_S, b, RGB(200, 204, 212));
    }
    if (t > CREDIT_MS) {                                     /* the version and the credit, small, fading in */
        static const char cr[] = "v" SIDJ_VERSION " \xb7 by Thomas Jansson";
        uint32_t q = t - CREDIT_MS >= 250u ? 256u : (t - CREDIT_MS) * 256u / 250u;
        cv_text((240 - text_w(&FONT_XS, cr)) / 2, 224 - by, &FONT_XS, cr, mix565(C_BLACK, RGB(96, 100, 110), q));
    }
}

static void draw_intro(uint32_t t)
{
    intro_band(t, 0, 144);
    cv_blit(0, 0);
    intro_band(t, 144, 96);
    cv_blit(0, 144);
}

static void intro_end(void)
{
    ui.intro = 0;
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui.dirty_top = ui.dirty_list = ui.dirty_voices = ui.dirty_filter = ui.dirty_status = 1;
}

static void ui_frame(void)                  /* ~60 times a second */
{
    uint32_t now = plat_ms();
    ui_input();
    if (ui.intro) {
        if (ui.activity || ui.load_wait || now - ui.intro_t0 >= INTRO_MS) intro_end();
        else {
            if (now - ui.intro_at >= 33u) { ui.intro_at = now; draw_intro(now - ui.intro_t0); }
            ui.activity = 0;
            ui_leds();
            return;
        }
    }
    ui.activity = 0;
    if (ui.load_wait && !eng_file.pending) {
        ui.load_wait = 0;
        if (!eng_load_rc && set.output && eng_deck_off) ui_say("RSID: NO ASID, EMULATED SID ONLY", 0);
        if (eng_load_rc) { ui.cur = -1; ui_say(eng_load_rc == -5 ? "BASIC TUNE: NOT SUPPORTED" : eng_load_rc == -6 ? "2SID / 3SID TUNE: NOT SUPPORTED" : "CANNOT PLAY THIS FILE", 0); }
        ui.dirty_top = ui.dirty_list = 1;
    }
    if (eng_loaded && eng_playing && ui.cur >= 0 && !ui.ended) {   /* the end of the song: stop and wait for SELECT */
        const uint16_t *len = 0; int nlen = 0;
        song_file(ui.cur, 0, &len, &nlen);
        if (len && ui.subsong <= nlen && len[ui.subsong - 1] && eng_elapsed_ds() >= len[ui.subsong - 1]) {
            ui.ended = 1;
            eng_post(EV_STOP, 0, 0, 0);
            ui_say("ENDED: SELECT PICKS THE NEXT", 0);
            ui.dirty_top = 1;
        }
    }
    if (eng_loaded && !eng_playing && c64.fault && ui.cur >= 0 && !ui.ended) { ui.ended = 1; ui_say("THE TUNE CRASHED", 0); }
    if (ui.mode == UI_SONGS) {
        if (ui.dirty_top || now - ui.top_at > 1000u) { ui.dirty_top = 0; ui.top_at = now; draw_top(); }
        if (ui.dirty_list) { ui.dirty_list = 0; draw_list(); }
    } else {
        if (ui.dirty_top || now - ui.top_at > 500u || remix_sig() != ui.top_sig) { ui.dirty_top = 0; ui.top_at = now; draw_top(); }
        if (ui.dirty_voices || now - ui.voices_at >= 33u) { ui.dirty_voices = 0; ui.voices_at = now; draw_voices(); }
        if (ui.dirty_filter || now - ui.filter_at > 100u) { ui.dirty_filter = 0; ui.filter_at = now; draw_filter(); }
    }
    if (ui.dirty_status || now - ui.status_at > 500u) { ui.dirty_status = 0; ui.status_at = now; draw_status(); }
    ui_leds();
}

static void ui_input_only(void) { ui_input(); }

static void songs_changed(void)
{
    ui_say("SONGS UPDATED", 0);
    if (ui.sel >= song_count) ui.sel = song_count ? song_count - 1 : 0;
    song_ord_n = -1;                        /* the list's order again */
    ui.cur = -1;                            /* the index moved: the loaded entry is unknown */
    ui.dirty_top = ui.dirty_list = 1;
}
