/* SPDX-License-Identifier: GPL-3.0-only
 * After X0X's host simulator (charlesvestal/fm1-x0x, GPL-3.0-only). */
/* SiDJ on the host: the whole app (UI, settings, song store, engine, C64, SIDs) run
 * against a simulated FM-1 driven by a script. Audio goes to a WAV, the screen to PNGs, the
 * lights to a text dump. The device-only parts (HAL, USB, OTA, flash driver) are replaced by
 * plat.h implemented here and a song pool in RAM.
 *
 *   sidj_host SCRIPT [OUTDIR]
 *
 * The same file, built with SIDJ_WEB, is the core of the browser emulator (web/emu): there
 * the script runner and the file outputs are left out and the page drives the device.
 *
 * Script, one command per line (# comments):
 *   wait MS                      run the device for MS milliseconds
 *   press BTN | release BTN | tap BTN   (BTN: FX SEL ENV LFO EDIT GLO HOME SAVE ARP SEQ PLAY REC OCT- OCT+)
 *   key K down|up | tapkey K     (K: 0..26 = F3..G5, or w0..w15 white keys, b0..b10 black keys)
 *   turn ENC N [MS]              (ENC: SELECT ALGO PRESET K1 K2 K3 K4; N detents, signed, MS apart, 80 = slow)
 *   midi ST D1 D2                MIDI in (hex bytes, e.g. 90 45 64), as from USB or the MIDI IN jack
 *   busy 0|1                     the load valve, as the device sets it (the scopes fall back to the tap)
 *   master N                     MASTER pot 0..4096
 *   sid FILE [LEN_DS ...]        a .sid file into the song pool, with song lengths in deciseconds
 *   songdel K                    delete entry K (0-based) as the transfer's DELETE does
 *   songraw SIZE                 a SIZE-byte file into the song pool (any bytes: lays out entries exactly)
 *   poolwrite OFF LEN BYTE       LEN bytes of the pool from OFF set to BYTE, as another firmware's
 *                                image writes over it (no erase; OFF from the pool's start; 0x.. hex)
 *   wav FILE | wavstop           start / stop recording the output
 *   asid FILE                    keep the raw MIDI out stream (the ASID SysEx) in FILE
 *   shot FILE.png                save the screen
 *   leds                         print the lit buttons and keys
 *   expect WHAT VALUE            mode, playing, loaded, cur, sel, subsong, nsongs, voice, page, octave,
 *                                frames, elapsed, songs, fault, cutoff, regN, tuneN, peak_db_min/max;
 *                                per voice V (0..2): muteV, keyonV, waveV, pwovrV, freqV, ctrlV,
 *                                pwvalV, octV, adsrV, wovrV, srovrV, srvalV, filtovrV (-1: the tune's),
 *                                tunefreqV, shiftV, fineV, lfotargetV, lfodepthV, lforateV, lfoshapeV,
 *                                lfopwV, lfocentsV, lfoabsV; lfocut, lfocutabs, tempo, vinyl, dframes
 *                                and dscope (the frames, the scopes' samples since `mark`); the remix: keymode, cutmode, scale, cutoff_off,
 *                                route, tuneroute, routexor, fmode, tunefmode, fmodexor, fmodeovr,
 *                                fmodeval, res; the output:
 *                                output, asid_msgs, asid_starts, asid_stops, asid_types, asid_bad,
 *                                asid_double, asid_vol_at_stop, asid_diffC, asid_seenC (chip C)
 *                                (VALUE may be <N or >N); exit 1 on a mismatch
 *   peakreset                    forget the peak
 *   mark                         remember the frame count and the scopes' (expect dframes, dscope)
 *   reboot                       re-run boot from the simulated flash (persistence test)
 *   echo TEXT
 */
#define SIDJ_HOST 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#define __attribute__(x)

/* ------------------------------------------------------------ screen --- */
static uint16_t fb[240 * 240];
static void lcd_sync(void) {}
static void lcd_fill(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c)
{
    uint32_t i, j;
    uint16_t sc = (uint16_t)(((c >> 8) & 0xFFu) | ((c & 0xFFu) << 8));
    for (j = y; j < y + h && j < 240u; j++)
        for (i = x; i < x + w && i < 240u; i++)
            fb[j * 240u + i] = sc;
}
static uint32_t blits;
static void lcd_blit(uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint16_t *px)
{
    uint32_t i, j;
    blits++;
    for (j = 0; j < h; j++)
        for (i = 0; i < w; i++)
            if (x + i < 240u && y + j < 240u)
                fb[(y + j) * 240u + x + i] = px[j * w + i];
}
#include "../firmware/src/gfx.c"

/* ---------------------------------------------------------- platform --- */
#include "../firmware/src/app/plat.h"
#include "../firmware/src/app/engine.h"

static uint32_t now_ms, held_btn, held_keys, master = 4096;
static int32_t enc_acc[NE];
static uint32_t lit_btn, lit_keys;

uint32_t plat_ms(void) { return now_ms; }
uint32_t plat_buttons(void) { return held_btn; }
uint32_t plat_keys(void) { return held_keys; }
int32_t plat_enc(int role) { int32_t v = enc_acc[role]; enc_acc[role] = 0; return v; }
uint32_t plat_master(void) { return master; }
void plat_leds(uint32_t b, uint32_t k) { lit_btn = b; lit_keys = k; }
static uint32_t midi_q[64], mq_w, mq_r;   /* the scenario's "midi" lines, as USB-MIDI packets */
int plat_midi_in(uint32_t *pkt) { if (mq_r == mq_w) return 0; *pkt = midi_q[mq_r++ % 64u]; return 1; }

/* MIDI out: the SysEx stream reassembled; ASID messages decoded (the spec's layout) into
 * asid_img[] for the expectations, and written raw to the "asid FILE" capture */
static FILE *asid_file;
static uint8_t sx_buf[512];
static int sx_n;
static uint32_t asid_msgs, asid_starts, asid_stops, asid_types_n, asid_bad, asid_double;   /* double: messages with a control register twice */
static int asid_vol_at_stop = -1;           /* the receiver's chip 0 volume when 0x4D came */
static uint32_t asid_seen[3];               /* registers received since the last 0x4D, a bit each */
static uint8_t asid_img[3][32];
#ifdef SIDJ_WEB
static void web_sysex(const uint8_t *m, int n);    /* web/emu: the stream to Web MIDI */
#endif
static void asid_sysex(const uint8_t *m, int n)
{
    static const uint8_t MAP[28] = {0x00, 0x01, 0x02, 0x03, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0c, 0x0d, 0x0e, 0x0f,
                                    0x10, 0x11, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x04, 0x0b, 0x12, 0x04, 0x0b, 0x12};
    int chip, id = 0, di = 11, mb, bit;
    if (asid_file) fwrite(m, 1, (size_t)n, asid_file);
#ifdef SIDJ_WEB
    web_sysex(m, n);
#endif
    if (n < 4 || m[0] != 0xf0 || m[1] != 0x2d || m[n - 1] != 0xf7) { asid_bad++; return; }
    if (m[2] == 0x4c) { asid_starts++; return; }
    if (m[2] == 0x4d) {                     /* as TherapSID and the SidStation: the chips reset */
        asid_vol_at_stop = asid_img[0][SID_ADDR_MODE_VOL] & SID_MODE_VOL;
        memset(asid_img, 0, sizeof asid_img);
        memset(asid_seen, 0, sizeof asid_seen);
        asid_stops++;
        return;
    }
    if (m[2] == 0x32) { asid_types_n++; return; }
    chip = m[2] == 0x4e ? 0 : m[2] == 0x50 ? 1 : m[2] == 0x51 ? 2 : -1;
    if (chip < 0 || n < 12) { asid_bad++; return; }
    for (mb = 0; mb < 4; mb++)
        for (bit = 0; bit < 7; bit++, id++)
            if (m[3 + mb] & (1 << bit)) {
                if (di >= n - 1) { asid_bad++; return; }
                asid_img[chip][MAP[id]] = (uint8_t)(m[di++] | ((m[7 + mb] >> bit & 1) << 7));
                asid_seen[chip] |= 1u << MAP[id];
            }
    if (m[6] & 0x70) asid_double++;         /* ids 25 .. 27 */
    if (di != n - 1) asid_bad++;
    asid_msgs++;
}
void plat_midi_out(uint32_t pkt)
{
    int cin = (int)(pkt & 0xf), k = cin == 4 || cin == 7 ? 3 : cin == 6 ? 2 : cin == 5 ? 1 : 0, i;
    if (!k) return;
    for (i = 0; i < k && sx_n < (int)sizeof sx_buf; i++) sx_buf[sx_n++] = (uint8_t)(pkt >> (8 + 8 * i));
    if (cin != 4) { asid_sysex(sx_buf, sx_n); sx_n = 0; }
}
int plat_midi_out_free(void) { return 256; }
static int asid_diff(int chip)             /* registers of the ASID image that differ from the chip's */
{
    int i, d = 0;
    for (i = 0; i < 25; i++)
        if (asid_img[chip][i] != eng_regs[chip][i]) { d++; printf("  asid chip %d reg %02x: image %02x, chip %02x\n", chip, i, asid_img[chip][i], eng_regs[chip][i]); }
    return d;
}

static uint8_t store[OBJ_NOBJ][PLAT_STORE_MAX];
static int store_len[OBJ_NOBJ] = {-1, -1, -1, -1, -1, -1, -1};
static uint32_t store_writes;               /* saves so far (the browser keeps the objects) */
int plat_store_load(uint32_t obj, void *dst, uint32_t max)
{
    int n = obj < OBJ_NOBJ ? store_len[obj] : -1;
    if (n < 0) return -1;
    if ((uint32_t)n > max) n = (int)max;
    memcpy(dst, store[obj], (size_t)n);
    return n;
}
int plat_store_save(uint32_t obj, const void *src, uint32_t len)
{
    if (obj >= OBJ_NOBJ || len > PLAT_STORE_MAX) return -1;
    memcpy(store[obj], src, len);
    store_len[obj] = (int)len;
    store_writes++;
    return 0;
}
static uint32_t cpu_pct;
uint32_t plat_cpu_pct(void) { return cpu_pct; }
uint32_t plat_xruns(void) { return 0; }
void plat_usb_audio(uint32_t s[6]) { int i; for (i = 0; i < 6; i++) s[i] = 0; }
void plat_master_stats(uint32_t s[5]) { int i; for (i = 0; i < 5; i++) s[i] = 0; }
static int usb_wide;
void plat_usb_audio_mode(int wide) { usb_wide = wide; }
void plat_cpu_stats(uint32_t *mean_x10, uint32_t *peak_x10) { *mean_x10 = *peak_x10 = cpu_pct * 10u; }
void plat_supplies(uint8_t s[2]) { s[0] = s[1] = 0; }
void plat_stop_for_flash(void) { eng_post(EV_STOP, 0, 0, 0); }   /* (no flash here: nothing to wait for) */

/* the song pool: 240 KB of simulated flash */
#include "../firmware/src/app/settings.c"
static uint8_t pool[0x3C000];
static const uint8_t *song_ptr(uint32_t off) { return pool + (off - 0xA0000u); }
static int song_blank(uint32_t off, uint32_t n) { uint32_t i; for (i = 0; i < n; i++) if (pool[off - 0xA0000u + i] != 0xff) return 0; return 1; }
static int song_erase_sector(uint32_t off) { memset(pool + (off - 0xA0000u), 0xff, 4096); return 0; }
static int song_prog(uint32_t off, const uint8_t *src, uint32_t n) { uint32_t i; for (i = 0; i < n; i++) pool[off - 0xA0000u + i] &= src[i]; return memcmp(pool + (off - 0xA0000u), src, n) ? -1 : 0; }
/* the song transfer (songs.c, as tools/fm1_sid.py and the web page speak it): one SysEx frame
 * in at a time (the bytes between F0 and F7), the answers out as whole messages */
static uint8_t sxi_buf[1024], sxo_buf[8192];
static uint32_t sxi_n, sxo_n;
static int sxi_ready;
static int ota_frame_get(const uint8_t **p, uint32_t *n) { if (!sxi_ready) return 0; *p = sxi_buf; *n = sxi_n; return 1; }
static void ota_frame_done(void) { sxi_ready = 0; }
static int ota_wire_send(const uint8_t *p, uint32_t n)
{
    if (sxo_n + n > sizeof sxo_buf) return -1;
    memcpy(sxo_buf + sxo_n, p, n);
    sxo_n += n;
    return 0;
}
static void songs_changed(void);
static int songs_play_request(int k, int subsong);
#include "../firmware/src/app/songs.c"
#include "../firmware/src/app/ui.c"

/* ---------------------------------------------------------- outputs --- */
static FILE *wav;
static uint32_t wav_frames;
static float peak_out;
static uint32_t mark_frames, mark_scope;
#ifndef SIDJ_WEB
static void png_write(const char *path)
{
    static uint8_t raw[240 * (1 + 240 * 3)];
    FILE *f = fopen(path, "wb");
    uint32_t crc_t[256], i, j, k, n = 0, a = 1, b = 0, pos, len = sizeof raw;
    uint8_t hdr[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    if (!f) { fprintf(stderr, "cannot write %s\n", path); return; }
    for (i = 0; i < 256; i++) { uint32_t c = i; for (k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_t[i] = c; }
    for (j = 0; j < 240; j++) {
        raw[n++] = 0;
        for (i = 0; i < 240; i++) {
            uint16_t p = fb[j * 240 + i];
            p = (uint16_t)((p >> 8) | (p << 8));
            raw[n++] = (uint8_t)(((p >> 11) & 31) * 255 / 31);
            raw[n++] = (uint8_t)(((p >> 5) & 63) * 255 / 63);
            raw[n++] = (uint8_t)((p & 31) * 255 / 31);
        }
    }
    for (i = 0; i < len; i++) { a = (a + raw[i]) % 65521; b = (b + a) % 65521; }
#define PUT32(v) do { uint8_t q_[4] = {(uint8_t)((v) >> 24), (uint8_t)((v) >> 16), (uint8_t)((v) >> 8), (uint8_t)(v)}; fwrite(q_, 1, 4, f); } while (0)
    fwrite(hdr, 1, 8, f);
    {
        uint8_t ih[17] = {'I', 'H', 'D', 'R', 0, 0, 0, 240, 0, 0, 0, 240, 8, 2, 0, 0, 0};
        uint32_t c = 0xFFFFFFFFu;
        PUT32(13u); fwrite(ih, 1, 17, f);
        for (i = 0; i < 17; i++) c = crc_t[(c ^ ih[i]) & 255] ^ (c >> 8);
        PUT32(~c);
    }
    {
        uint32_t nblk = (len + 65534) / 65535, zlen = 2 + len + nblk * 5 + 4, c = 0xFFFFFFFFu;
        uint8_t *z = malloc(zlen + 4), *q = z;
        memcpy(q, "IDAT", 4); q += 4; *q++ = 0x78; *q++ = 0x01;
        for (pos = 0; pos < len;) {
            uint32_t m = len - pos > 65535 ? 65535 : len - pos;
            *q++ = pos + m >= len ? 1 : 0; *q++ = (uint8_t)m; *q++ = (uint8_t)(m >> 8); *q++ = (uint8_t)~m; *q++ = (uint8_t)(~m >> 8);
            memcpy(q, raw + pos, m); q += m; pos += m;
        }
        *q++ = (uint8_t)(b >> 8); *q++ = (uint8_t)b; *q++ = (uint8_t)(a >> 8); *q++ = (uint8_t)a;
        PUT32(zlen); fwrite(z, 1, zlen + 4, f);
        for (i = 0; i < zlen + 4; i++) c = crc_t[(c ^ z[i]) & 255] ^ (c >> 8);
        PUT32(~c); free(z);
    }
    {
        uint8_t ie[4] = {'I', 'E', 'N', 'D'};
        uint32_t c = 0xFFFFFFFFu;
        PUT32(0u); fwrite(ie, 1, 4, f);
        for (i = 0; i < 4; i++) c = crc_t[(c ^ ie[i]) & 255] ^ (c >> 8);
        PUT32(~c);
    }
    fclose(f);
}

static void wav_open(const char *path) { static const uint8_t h[44] = {0}; wav = fopen(path, "wb"); if (wav) fwrite(h, 1, 44, wav); wav_frames = 0; }
static void wav_close(void)
{
    uint32_t bytes = wav_frames * 4, v;
    if (!wav) return;
    fseek(wav, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, wav); v = 36 + bytes; fwrite(&v, 4, 1, wav); fwrite("WAVEfmt ", 1, 8, wav);
    v = 16; fwrite(&v, 4, 1, wav);
    { uint16_t fmt[2] = {1, 2}; uint32_t r[2] = {44100, 44100 * 4}; uint16_t al[2] = {4, 16}; fwrite(fmt, 2, 2, wav); fwrite(r, 4, 2, wav); fwrite(al, 2, 2, wav); }
    fwrite("data", 1, 4, wav); fwrite(&bytes, 4, 1, wav);
    fclose(wav); wav = 0;
}
#endif /* SIDJ_WEB */

/* --------------------------------------------------------- the device --- */
static double audio_due_ms, render_ns_total, render_budget_ns_total;
static uint32_t ui_due;
static double now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (double)t.tv_sec * 1e9 + (double)t.tv_nsec; }

#ifdef SIDJ_WEB
static void web_audio(const int32_t *blk, uint32_t n);
#endif

static void boot(void)
{
    eng_init();
    settings_load();
    songs_scan();
    lcd_fill(0, 0, 240, 240, C_BLACK);
    ui_init();
}

static void run_ms(uint32_t ms)
{
    static int32_t blk[512];
    uint32_t i;
    for (i = 0; i < ms; i++) {
        now_ms++;
        while (audio_due_ms <= (double)now_ms) {
            double t0 = now_ns(), dt;
            uint32_t k;
            eng_render(blk, 256, master);
            dt = now_ns() - t0;
            render_ns_total += dt;
            render_budget_ns_total += 256.0 * 1e9 / 44100.0;
            cpu_pct = (uint32_t)(100.0 * dt / (256.0 * 1e9 / 44100.0));
#ifdef SIDJ_WEB
            web_audio(blk, 256);                     /* the browser build: to the AudioWorklet */
#endif
            for (k = 0; k < 256; k++) {
                float l = (float)blk[2 * k] / 8388608.0f, r = (float)blk[2 * k + 1] / 8388608.0f;
                int16_t s[2];
                if (fabsf(l) > peak_out) peak_out = fabsf(l);
                if (fabsf(r) > peak_out) peak_out = fabsf(r);
                s[0] = (int16_t)(l * 32767.0f); s[1] = (int16_t)(r * 32767.0f);
                if (wav) { fwrite(s, 2, 2, wav); wav_frames++; }
            }
            audio_due_ms += 256.0 * 1000.0 / 44100.0;
        }
        eng_poll();                             /* a song load, as the FM-1's main loop does it */
        songs_service();                        /* the main loop's SysEx turn, as on the FM-1 */
        if (now_ms >= ui_due) { ui_due = now_ms + 16; ui_frame(); }
        else ui_input_only();
    }
}

#ifndef SIDJ_WEB                          /* the browser build (web/emu) drives the device itself */
/* ---------------------------------------------------------- script --- */
static const char *const BTN_N[NB] = {"FX", "SEL", "ENV", "LFO", "EDIT", "GLO", "HOME", "SAVE", "ARP", "SEQ", "PLAY", "REC", "OCT-", "OCT+"};
static const char *const ENC_N[NE] = {"SELECT", "ALGO", "PRESET", "K1", "K2", "K3", "K4"};
static const int WHITE_K[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static const int BLACK_K[11] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25};

static int find_name(const char *const *t, int n, const char *s) { int i; for (i = 0; i < n; i++) if (!strcmp(t[i], s)) return i; return -1; }
static int key_of(const char *s)
{
    if (s[0] == 'w') return WHITE_K[atoi(s + 1) % 16];
    if (s[0] == 'b') return BLACK_K[atoi(s + 1) % 11];
    return atoi(s) % NKEYS;
}

static int expect(const char *what, const char *val)
{
    long have, want = atol(val[0] == '<' || val[0] == '>' ? val + 1 : val);
    int ok, v = what[ui_strlen(what) - 1] - '0';
    if (!strncmp(what, "reg", 3) && what[3] == ' ') { have = 0; }
    if (!strcmp(what, "mode")) have = ui.mode;
    else if (!strcmp(what, "playing")) have = eng_playing;
    else if (!strcmp(what, "loaded")) have = eng_loaded;
    else if (!strcmp(what, "cur")) have = ui.cur;
    else if (!strcmp(what, "sel")) have = ui.sel;
    else if (!strcmp(what, "subsong")) have = ui.subsong;
    else if (!strcmp(what, "nsongs")) have = ui.nsongs;
    else if (!strcmp(what, "voice")) have = ui.voice;
    else if (!strcmp(what, "page")) have = ui.page;
    else if (!strcmp(what, "octave")) have = ui.octave;
    else if (!strcmp(what, "frames")) have = (long)c64.frames;
    else if (!strcmp(what, "elapsed")) have = (long)eng_elapsed_ds();
    else if (!strcmp(what, "songs")) have = song_count;
    else if (!strncmp(what, "songsize", 8)) { uint32_t sz = 0; song_file(atoi(what + 8), &sz, 0, 0); have = (long)sz; }
    else if (!strcmp(what, "fault")) have = c64.fault;
    else if (!strcmp(what, "cutoff")) have = sid_fc(rx.out);
    else if (!strcmp(what, "output")) have = eng_output;
    else if (!strcmp(what, "keymode")) have = ui.key_mode;
    else if (!strcmp(what, "cutmode")) have = rx.cutoff_mode;
    else if (!strcmp(what, "route")) have = rx.out[SID_ADDR_RES_FILT] & SID_FILT_VOICES;
    else if (!strcmp(what, "tuneroute")) have = rx.tune[SID_ADDR_RES_FILT] & SID_FILT_VOICES;
    else if (!strcmp(what, "routexor")) have = (rx.out[SID_ADDR_RES_FILT] ^ rx.tune[SID_ADDR_RES_FILT]) & SID_FILT_VOICES;
    else if (!strcmp(what, "fmodexor")) have = (rx.out[SID_ADDR_MODE_VOL] ^ rx.tune[SID_ADDR_MODE_VOL]) & SID_MODE_FILTERS;
    else if (!strncmp(what, "fine", 4) && v >= 0 && v < 3) have = rx.v[v].fine;
    else if (!strncmp(what, "lfotarget", 9) && v >= 0 && v < 3) have = rx.v[v].lfo_target;
    else if (!strncmp(what, "lfodepth", 8) && v >= 0 && v < 3) have = rx.v[v].lfo_depth;
    else if (!strncmp(what, "lforate", 7) && v >= 0 && v < 3) have = rx.v[v].lfo_rate;
    else if (!strncmp(what, "lfoshape", 8) && v >= 0 && v < 3) have = rx.v[v].lfo_shape;
    else if (!strncmp(what, "lfopw", 5) && v >= 0 && v < 3) have = rx.v[v].lfo_pw;
    else if (!strncmp(what, "lfocents", 8) && v >= 0 && v < 3) have = rx.v[v].lfo_cents;
    else if (!strcmp(what, "lfocut")) have = rx.lfo_cut;
    else if (!strcmp(what, "lfocutabs")) have = labs((long)rx.lfo_cut);
    else if (!strncmp(what, "lfoabs", 6) && v >= 0 && v < 3) { long a = labs((long)rx.v[v].lfo_pw), b2 = labs((long)rx.v[v].lfo_cents); have = a > b2 ? a : b2; }
    else if (!strcmp(what, "dframes")) have = (long)(c64.frames - mark_frames);
    else if (!strcmp(what, "dscope")) have = (long)(eng_scope_n - mark_scope);
    else if (!strcmp(what, "tempo")) have = eng_tempo;
    else if (!strcmp(what, "usb4")) have = usb_wide + 10 * eng_usb4;   /* the format and the engine's cap: 11 or 0 */
    else if (!strcmp(what, "vinyl")) have = eng_vinyl;
    else if (!strncmp(what, "shift", 5) && v >= 0 && v < 3) have = rx.v[v].shift_on ? rx.v[v].shift_semi : -99;
    else if (!strcmp(what, "fmode")) have = rx.out[SID_ADDR_MODE_VOL] & SID_MODE_FILTERS;
    else if (!strcmp(what, "tunefmode")) have = rx.tune[SID_ADDR_MODE_VOL] & SID_MODE_FILTERS;
    else if (!strcmp(what, "scale")) have = rx.cutoff_scale;
    else if (!strcmp(what, "cutoff_off")) have = rx.cutoff_off;
    else if (!strncmp(what, "tunefreq", 8) && v >= 0 && v < 3) have = remix_tune_freq(&rx, v);
    else if (!strcmp(what, "asid_msgs")) have = (long)asid_msgs;
    else if (!strcmp(what, "asid_starts")) have = (long)asid_starts;
    else if (!strcmp(what, "asid_stops")) have = (long)asid_stops;
    else if (!strcmp(what, "asid_types")) have = (long)asid_types_n;
    else if (!strcmp(what, "asid_bad")) have = (long)asid_bad;
    else if (!strcmp(what, "asid_double")) have = (long)asid_double;
    else if (!strcmp(what, "asid_vol_at_stop")) have = asid_vol_at_stop;
    else if (!strncmp(what, "asid_seen", 9) && v >= 0 && v < 3) { uint32_t s = asid_seen[v]; have = 0; while (s) { have += s & 1u; s >>= 1; } }
    else if (!strncmp(what, "asid_diff", 9) && v >= 0 && v < 3) have = asid_diff(v);
    else if (!strncmp(what, "reg", 3)) have = rx.out[strtol(what + 3, 0, 0) & (SID_NREG - 1)];
    else if (!strncmp(what, "tune", 4)) have = rx.tune[strtol(what + 4, 0, 0) & (SID_NREG - 1)];
    else if (!strncmp(what, "mute", 4) && v >= 0 && v < 3) have = rx.v[v].mute;
    else if (!strncmp(what, "keyon", 5) && v >= 0 && v < 3) have = rx.v[v].key_on;
    else if (!strncmp(what, "scopepp", 7) && v >= 0 && v < 3) {      /* the scope ring's peak to peak */
        int lo = 32767, hi = -32768, k;
        for (k = 0; k < (int)ENG_SCOPE_N; k++) { if (eng_scope[v][k] < lo) lo = eng_scope[v][k]; if (eng_scope[v][k] > hi) hi = eng_scope[v][k]; }
        have = hi - lo;
    }
    else if (!strcmp(what, "scopestems")) have = eng_scope_stems;
    else if (!strncmp(what, "scopeclip", 9) && v >= 0 && v < 3) {    /* per mille of the ring the scope clips */
        int32_t k, n = 0;                     /* (as draw_scope scales it, at the last frame drawn) */
        for (k = 0; k < (int32_t)ENG_SCOPE_N; k++) {
            int32_t t = ((eng_scope[v][k] - scope_avg[v]) * (eng_scope_stems ? scope_gain_q8[v] : 256)) >> 8;
            if (t > 32767 || t < -32767) n++;
        }
        have = n * 1000 / (int32_t)ENG_SCOPE_N;
    }
    else if (!strncmp(what, "scoperaw", 8) && v >= 0 && v < 3) {     /* the ring's largest |sample| */
        int k, m = 0;
        for (k = 0; k < (int)ENG_SCOPE_N; k++) { int a = eng_scope[v][k] < 0 ? -eng_scope[v][k] : eng_scope[v][k]; if (a > m) m = a; }
        have = m;
    }
    else if (!strncmp(what, "wave", 4) && v >= 0 && v < 3) have = rx.out[SID_ADDR_VOICE(v) + SID_ADDR_CTRL] & RX_WAVE_MASK;
    else if (!strncmp(what, "pwovr", 5) && v >= 0 && v < 3) have = rx.v[v].pw_ovr;
    else if (!strncmp(what, "pwval", 5) && v >= 0 && v < 3) have = rx.v[v].pw_ovr ? rx.v[v].pw_val : -1;
    else if (!strncmp(what, "oct", 3) && v >= 0 && v < 3) have = rx.v[v].oct;
    else if (!strncmp(what, "adsr", 4) && v >= 0 && v < 3) have = rx.v[v].adsr_ovr;
    else if (!strncmp(what, "wovr", 4) && v >= 0 && v < 3) have = rx.v[v].wave_ovr ? rx.v[v].wave_val : 0;
    else if (!strncmp(what, "srovr", 5) && v >= 0 && v < 3) have = rx.v[v].sr_ovr;
    else if (!strncmp(what, "srval", 5) && v >= 0 && v < 3) have = rx.v[v].sr_val & rx.v[v].sr_ovr;
    else if (!strncmp(what, "filtovr", 7) && v >= 0 && v < 3) have = rx.v[v].filt_ovr ? rx.v[v].filt_val : -1;
    else if (!strcmp(what, "fmodeovr")) have = rx.fmode_ovr;
    else if (!strcmp(what, "fmodeval")) have = rx.fmode_val & rx.fmode_ovr;
    else if (!strcmp(what, "res")) have = rx.res_off;
    else if (!strncmp(what, "freq", 4) && v >= 0 && v < 3) have = sid_freq(rx.out, v);
    else if (!strncmp(what, "ctrl", 4) && v >= 0 && v < 3) have = rx.out[SID_ADDR_VOICE(v) + SID_ADDR_CTRL];
    else if (!strcmp(what, "peak_db_min") || !strcmp(what, "peak_db_max")) have = peak_out > 0 ? (long)floor(20.0 * log10((double)peak_out)) : -200;
    else { printf("  expect: unknown %s\n", what); return 1; }
    ok = val[0] == '<' ? have < want : val[0] == '>' ? have > want : have == want;
    if (!strcmp(what, "peak_db_min")) ok = have >= want;
    if (!strcmp(what, "peak_db_max")) ok = have <= want;
    printf("  %s %s %ld: %s %ld\n", ok ? "ok  " : "FAIL", what, have, val[0] == '<' || val[0] == '>' ? "bound" : "expected", want);
    return !ok;
}

int main(int argc, char **argv)
{
    FILE *sc;
    char line[512], out[400];
    const char *dir = argc > 2 ? argv[2] : ".";
    int lineno = 0, fails = 0;
    if (argc < 2) { fprintf(stderr, "usage: sidj_host SCRIPT [OUTDIR]\n"); return 2; }
    sc = fopen(argv[1], "r");
    if (!sc) { perror(argv[1]); return 2; }
    memset(pool, 0xff, sizeof pool);
    boot();
    while (fgets(line, sizeof line, sc)) {
        char cmd[32] = {0}, a[256] = {0}, b[64] = {0}, c[64] = {0};
        lineno++;
        if (line[0] == '#' || sscanf(line, "%31s %255s %63s %63s", cmd, a, b, c) < 1) continue;
        if (!strcmp(cmd, "wait")) run_ms((uint32_t)atoi(a));
        else if (!strcmp(cmd, "press") || !strcmp(cmd, "release") || !strcmp(cmd, "tap")) {
            int i = find_name(BTN_N, NB, a);
            if (i < 0) { printf("line %d: no button %s\n", lineno, a); return 2; }
            if (cmd[0] != 'r') held_btn |= 1u << i;
            if (cmd[0] == 't') run_ms(40);
            if (cmd[0] != 'p') held_btn &= ~(1u << i);
            run_ms(cmd[0] == 't' ? 40 : 2);
        } else if (!strcmp(cmd, "key") || !strcmp(cmd, "tapkey")) {
            int k = key_of(a);
            if (cmd[0] == 't') { held_keys |= 1u << k; run_ms(60); held_keys &= ~(1u << k); run_ms(30); }
            else { if (!strcmp(b, "down")) held_keys |= 1u << k; else held_keys &= ~(1u << k); run_ms(2); }
        } else if (!strcmp(cmd, "turn")) {
            int e = find_name(ENC_N, NE, a), n = atoi(b), s = n > 0 ? 1 : -1;
            uint32_t gap = c[0] ? (uint32_t)atoi(c) : 80u;
            if (e < 0) { printf("line %d: no encoder %s\n", lineno, a); return 2; }
            for (; n; n -= s) { enc_acc[e] += s; run_ms(gap); }
        } else if (!strcmp(cmd, "midi")) {               /* midi STATUS D1 D2, hex: 90 3c 64 */
            uint32_t st = (uint32_t)strtoul(a, 0, 16), d1 = (uint32_t)strtoul(b, 0, 16), d2 = (uint32_t)strtoul(c, 0, 16);
            midi_q[mq_w++ % 64u] = st >> 4 | st << 8 | d1 << 16 | d2 << 24;
            run_ms(2);
        } else if (!strcmp(cmd, "busy")) eng_busy = (uint8_t)atoi(a);   /* busy 0|1: as the device's load valve */
        else if (!strcmp(cmd, "master")) master = (uint32_t)atoi(a);
        else if (!strcmp(cmd, "sid")) {                 /* sid FILE [LEN_DS ...] */
            static uint8_t buf[ENG_FILE_MAX];
            uint16_t lens[SONG_MAX_LEN];
            int nlen = 0, rc;
            char *rest = strstr(line, a);
            size_t got;
            FILE *f = fopen(a, "rb");
            if (!f) { printf("line %d: cannot open %s\n", lineno, a); return 2; }
            got = fread(buf, 1, sizeof buf, f);
            fclose(f);
            rest = rest ? rest + strlen(a) : line + strlen(line);
            while (*rest && nlen < SONG_MAX_LEN) { char *end; long v = strtol(rest, &end, 10); if (end == rest) break; lens[nlen++] = (uint16_t)v; rest = end; }
            rc = song_put(buf, (uint32_t)got, 0, lens, nlen);
            if (rc) { printf("line %d: song_put %s: %d\n", lineno, a, rc); return 2; }
            songs_changed();
        } else if (!strcmp(cmd, "songraw")) {
            static uint8_t raw[ENG_FILE_MAX];
            uint32_t n = (uint32_t)strtoul(a, 0, 0), i;
            int rc;
            for (i = 0; i < n && i < sizeof raw; i++) raw[i] = (uint8_t)(i * 7u + 1u);
            rc = song_put(raw, n, 0, 0, 0);
            if (rc) { printf("line %d: song_put %u bytes: %d\n", lineno, (unsigned)n, rc); return 2; }
            songs_changed();
        } else if (!strcmp(cmd, "poolwrite")) {
            uint32_t off = (uint32_t)strtoul(a, 0, 0), n = (uint32_t)strtoul(b, 0, 0);
            if (off + n > sizeof pool) { printf("line %d: poolwrite past the pool\n", lineno); return 2; }
            memset(pool + off, (int)strtoul(c, 0, 0), n);
        } else if (!strcmp(cmd, "songdel")) {
            if (song_delete(atoi(a))) { printf("line %d: song_delete %s failed\n", lineno, a); return 2; }
            songs_changed();
        } else if (!strcmp(cmd, "wav")) { snprintf(out, sizeof out, "%s/%s", dir, a); wav_open(out); }
        else if (!strcmp(cmd, "peakreset")) peak_out = 0.0f;
        else if (!strcmp(cmd, "mark")) { mark_frames = c64.frames; mark_scope = eng_scope_n; }
        else if (!strcmp(cmd, "asid")) { snprintf(out, sizeof out, "%s/%s", dir, a); if (asid_file) fclose(asid_file); asid_file = fopen(out, "wb"); }
        else if (!strcmp(cmd, "wavstop")) wav_close();
        else if (!strcmp(cmd, "shot")) { snprintf(out, sizeof out, "%s/%s", dir, a); png_write(out); }
        else if (!strcmp(cmd, "leds")) {
            int i;
            printf("  leds: buttons");
            for (i = 0; i < NB; i++) if (lit_btn >> i & 1u) printf(" %s", BTN_N[i]);
            printf(" | white ");
            for (i = 0; i < 16; i++) printf("%c", (lit_keys >> WHITE_K[i] & 1u) ? '#' : '.');
            printf(" black ");
            for (i = 0; i < 11; i++) printf("%c", (lit_keys >> BLACK_K[i] & 1u) ? '#' : '.');
            printf("\n");
        } else if (!strcmp(cmd, "expect")) fails += expect(a, b);
        else if (!strcmp(cmd, "reboot")) boot();
        else if (!strcmp(cmd, "echo")) printf("%s", line + 5);
        else { printf("line %d: unknown command %s\n", lineno, cmd); return 2; }
    }
    wav_close();
    if (asid_file) fclose(asid_file);
    printf("host: %u ms simulated, %u blits, render %.1f%% of real time (host CPU)%s\n", now_ms, blits,
           render_budget_ns_total > 0 ? 100.0 * render_ns_total / render_budget_ns_total : 0.0, fails ? ", EXPECTATIONS FAILED" : "");
    return fails ? 1 : 0;
}
#endif /* SIDJ_WEB */
