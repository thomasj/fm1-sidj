/* SPDX-License-Identifier: GPL-3.0-only */
/* SiDJ engine: see engine.h. Compiled as its own unit at -O2 (tools/build.py). */
#include "engine.h"
#include "plat.h"
#include <stddef.h>
#include "beat.c"
void *memcpy(void *dst, const void *src, size_t n);

c64_t c64;
uint8_t c64_mem[65536] ENG_POOL;
sid_t eng_sid[C64_NSID];
remix_t rx;
asid_t eng_asid;
volatile uint8_t eng_output;
uint8_t eng_regs[C64_NSID][SID_NREG];
volatile uint8_t eng_playing, eng_loaded, eng_ntsc;
volatile uint8_t eng_tempo = 100;
volatile int8_t eng_vinyl;
volatile uint8_t eng_deck_off;
volatile uint8_t eng_usb4;
int16_t *eng_stem_out;
volatile uint8_t eng_busy;
static uint32_t tempo_rem;                  /* the chunk length's remainder at a tempo, in % cycles */
volatile uint32_t eng_log_lost;
int16_t eng_scope[3][ENG_SCOPE_N] ENG_POOL;
volatile uint32_t eng_scope_w;
volatile uint32_t eng_scope_n;
volatile uint8_t eng_scope_stems;
sidfile_t eng_file_info;
volatile int eng_load_rc;
eng_file_t eng_file ENG_POOL;

typedef struct { uint8_t type, a, b, rsv; uint32_t pkt; } ev_t;
#define EVQ 64u
static ev_t evq[EVQ];
static volatile uint32_t ev_w, ev_r;

/* the current chunk: the C64's writes after the remix, placed at their cycles; chip OUT_FRAME
 * marks where the frame changed (the ASID sink flushes there) */
#define OUT_MAX (C64_LOG_MAX * 2 + 64)
#define OUT_FRAME 0xff
#define OUT_CLOCK 0xfe                      /* a MIDI clock pulse (F8) */
#define OUT_START 0xfd                      /* MIDI Start (FA), just before the first pulse */
typedef struct { uint16_t cycle; uint8_t chip, reg, val; } out_t;
static out_t out[OUT_MAX];
static int nout, out_i;
static uint32_t chunk_len, chunk_pos;
#define CHUNK_CYCLES 2048u
static int16_t tap[128 * 3];
static uint8_t model_ovr;                   /* 0 file, 1 6581, 2 8580 */
static uint8_t cut_mode;                    /* the cutoff knob's mode, kept across songs */

/* the output gain, Q14, ramped 10 ms either way: play, pause and a song change never step the
 * output (the SID's own volume and DC offset would click); what waits for the fade-out */
#define GAIN_ONE 16384
#define GAIN_STEP (GAIN_ONE / 441 + 1)
enum { FADE_NONE, FADE_STOP, FADE_LOAD };
static int32_t gain, gain_to;
static uint8_t after_fade;
static volatile uint8_t load_req;           /* the main loop is loading (eng_poll); the ISR renders silence */
static uint8_t load_song, load_opts;

/* the MIDI clock (ENG_OUT_CLOCK): a load first runs the tune ahead, silent (the C64 only), until its
 * beat is found (detect), then starts it again with the clock on its first row. The pulses go into
 * the chunk at their tune cycles, so the tempo and the fader move them with the music */
volatile uint8_t eng_clk_on;
volatile uint16_t eng_clk_bpm_x10;
volatile uint32_t eng_clk_pulses;
volatile uint8_t eng_clk_tried;
volatile uint8_t eng_clk_ratio = ENG_CLK_X1;
volatile uint32_t eng_clk_first_cyc;
volatile int16_t eng_clk_offset_ms;
volatile uint8_t eng_clk_click;
volatile uint8_t eng_clk_manual;
volatile uint32_t eng_calls_min_x10;
eng_clk_grid_t eng_clk_preset;
static int32_t clk_offset_cyc;              /* the offset in the tune's cycles */
#define CLICK_N 1024
static int16_t click_tab[CLICK_N];          /* a short decaying tone (eng_init) */
static uint32_t click_i = CLICK_N;          /* where the click sounding is in it (CLICK_N: none) */
static uint8_t clk_ratio_now;               /* the ratio the pulses go at */
static uint32_t clk_base, clk_sched;        /* the spacing at x1 (/256); pulses scheduled since the Start */
static struct { uint8_t valid, found; uint16_t bpm_x10; uint32_t tick, call, base, off; } det_last;   /* the last look (ENG_LOAD_AGAIN) */
static uint32_t tune_cyc;                   /* the tune's cycles since the load (before the tempo) */
static uint8_t clk_on, clk_start, clk_paused, clk_resolve;   /* clk_resolve: the first pulse's call, its cycle not yet known */
static uint32_t clk_call, clk_tick, clk_off;   /* the first pulse's player call; C64 cycles a call; cycles from the first SID write to call 0 */
static uint32_t rt_frame;                   /* the frame of the block eng_render is at (the clock's timing) */
/* following a clock that comes in: the tune's own pulses (as above, not sent) and the clock's are
 * paired by number, and the tune's speed is set from the two spacings and steered by the
 * difference; the tune's first pulse pairs with the clock's nearest bar */
#define FOL_N 64u
#define FOL_ALIVE (PLAT_TICKS_HZ / 2u)     /* a clock not heard for half a second has stopped */
#define FOL_GAIN 48                         /* a difference is steered out over about two beats */
#define FOL_STEER 410                       /* ... at most 10 % faster or slower */
volatile uint8_t eng_follow;
volatile uint16_t eng_follow_bpm_x10;
volatile int32_t eng_follow_err_us;
static uint32_t fol_te[FOL_N], fol_ti[FOL_N];   /* the clock's pulses' times, by number; the tune's */
static uint32_t fol_ne, fol_ni, fol_np;     /* the clock's numbers seen (the last + 1), the tune's, the tune's next to pair */
static uint32_t fol_last, fol_pe;           /* the clock's last pulse (ticks); its spacing (ticks, /256) */
#define FOL_W 128u                          /* the spacing: measured over the last bar (96 spacings), ... */
#define FOL_WS 12u                          /* ... or from the last 12 when they differ from it ... */
#define FOL_WJ 4u                           /* ... by more than 4 %: a new tempo */
#define FOL_WM 24u                          /* ... or the last beat's, against the rest of the bar ... */
#define FOL_WD 5u                           /* ... off it by more than 0.5 % (a millisecond's jitter moves it 0.4 %) ... */
#define FOL_WK 12u                          /* ... for this many pulses in a row: a small change of tempo */
static uint32_t fol_w[FOL_W], fol_wc, fol_wn;   /* the last pulses' times (counted or not): written, how many held */
static uint32_t fol_dev, fol_hold_t;      /* pulses the last beat has been off the bar; when a held clock's song played */
static uint8_t fol_held, fol_hold_play;     /* a Stop: the clock's tempo kept while the song is paused (a second's play without it: let go) */
static uint8_t fol_gap;                     /* the bar started after a gap: the last tempo kept until it differs */
static int32_t fol_shift;                   /* the clock's number paired with the tune's 0 */
static uint8_t fol_heard, fol_paired, fol_prev;   /* fol_paired: fol_shift chosen; fol_prev: fol_last a pulse */
static uint32_t fol_q12 = 4096u;            /* the tune's speed while following (4096: as written) */
static uint32_t clk_next, clk_frac, clk_period;   /* the next pulse (tune cycles, + /256), the spacing (/256) */

/* a complete SysEx to USB-MIDI; a message the ring cannot take whole is dropped whole */
static void asid_emit(void *ctx, const uint8_t *m, int n)
{
    uint32_t pk[16];
    int k = asid_usb_packets(m, n, pk, 16);
    (void)ctx;
    if (!k || plat_midi_out_free() < k) return;
    plat_midi_out_msg(pk, k);
}

static void midi_rt(uint8_t b)              /* a MIDI real-time byte: a one-byte USB-MIDI packet */
{
    if (!eng_follow && plat_midi_out_free() > 0) plat_midi_out(0x0Fu | (uint32_t)b << 8);
}

static void clock_off(void)
{
    if (clk_on && !clk_paused && !clk_start) midi_rt(0xFC);   /* (not yet started: nothing to stop) */
    clk_on = 0;
    eng_clk_on = 0;
}

static int asid_out(void) { return eng_output == 1 || eng_output == 2; }

/* the tune's beat, from its first seconds run ahead (the C64 only: no chips, no sound). A note start:
 * a voice starts to sound (gate on, test bit off, a waveform). Player calls are counted by the bursts
 * of writes they make (a gap of 2500 cycles between), each numbered by its start rounded to whole
 * calls from where the calls fall (the third burst's place in the call), so writes late in a call,
 * or an init at another place in the frame, do not split a row. The grid: the last found in the
 * first 12 s that held at two checks a second apart (from 4 s): a longer look puts right an early
 * guess (a tune whose notes first fit a row half as long). *off: cycles from the first write to
 * call 0's start */
#define DETECT_FROM_S 4u
#define DETECT_MAX_S 12u
#define BURST_GAP 2500u
static beat_t det_beat;
static int detect(beat_grid_t *g, uint32_t *tick_out, uint32_t *off)
{
    uint32_t cyc = 0, w0 = 0, hz = c64.clock_hz, next = hz * DETECT_FROM_S, i, last = 0, bursts = 0, cur = 0, phase = 0;
    uint8_t snd[3] = {0, 0, 0}, have0 = 0, seen = 0, found = 0;
    beat_grid_t prev;
    beat_reset(&det_beat);
    while (cyc < hz * DETECT_MAX_S) {
        uint32_t len = c64_run(&c64, CHUNK_CYCLES), tick;
        if (!len) break;                    /* (the tune ended or jammed) */
        tick = c64.cia_speed ? (uint32_t)c64.cia[0].ta_latch + 1u : c64_frame_cycles(&c64);
        for (i = 0; i < (uint32_t)c64.nlog; i++) {
            const c64_write_t *w = &c64.log[i];
            uint32_t at = cyc + w->cycle;
            if (!have0) { w0 = last = at; have0 = 1; }
            if (!bursts || at - last > BURST_GAP) {   /* a new player call */
                if (++bursts == 3) phase = (at - w0) % tick;
                cur = (at - w0 + tick - phase + tick / 2) / tick - 1;
            }
            last = at;
            if (w->chip == 0 && (w->reg == 4 || w->reg == 11 || w->reg == 18)) {
                int v = w->reg / 7;
                uint8_t s = (w->val & 0x09) == 0x01 && (w->val & 0xF0);
                if (s && !snd[v]) beat_note(&det_beat, cur);
                snd[v] = s;
            }
        }
        cyc += len;
        if ((cyc & 0x3FFFFu) < len) plat_keepalive();   /* (a few seconds of this in the main loop) */
        if (cyc >= next) {
            beat_grid_t now;
            next += hz;
            if (beat_find(&det_beat, (uint32_t)((uint64_t)hz * 60u / tick), &now)) {
                if (seen && now.a == prev.a && now.b == prev.b && now.rpb2 == prev.rpb2 && now.anchor == prev.anchor) {
                    *g = now; *tick_out = tick; *off = phase; found = 1;
                }
                prev = now; seen = 1;
            } else seen = 0;
        }
    }
    return found;
}

static int chip_is_8580(int i) { return model_ovr ? model_ovr == 2 : sidfile_model(&eng_file_info, i) == SID_MODEL_8580; }

static void asid_types(void)
{
    int i;
    for (i = 0; i < c64.nsid; i++) asid_sid_type(&eng_asid, i, chip_is_8580(i));
}

static uint8_t sid_read_cb(void *ctx, int chip, uint8_t reg) { (void)ctx; return sid_read(&eng_sid[chip], reg); }

static uint32_t chip_clock(void)            /* the chips' clock: the tune's, on the vinyl fader */
{
    uint32_t clk = eng_ntsc ? C64_NTSC_HZ : C64_PAL_HZ;
    if (eng_deck_off) return clk;
    return clk / 1000u * (uint32_t)(1000 + eng_vinyl) + clk % 1000u * (uint32_t)(1000 + eng_vinyl) / 1000u;
}

static int32_t fol_div(int32_t a, uint32_t m, uint32_t d)   /* a x m / d, signed (no signed 64-bit divide here) */
{
    uint32_t q = (uint32_t)((uint64_t)(a < 0 ? -(uint32_t)a : (uint32_t)a) * m / d);
    return a < 0 ? -(int32_t)q : (int32_t)q;
}

static uint32_t call_cycles(void)          /* the player's call spacing: its CIA timer, else the frame */
{
    return c64.cia_speed ? (uint32_t)c64.cia[0].ta_latch + 1u : c64_frame_cycles(&c64);
}

static uint32_t calls_min_x10(void) { return (uint32_t)((uint64_t)c64.clock_hz * 600u / call_cycles()); }

static uint32_t manual_beat(uint32_t bpm_x10)   /* a tempo set by hand: the nearest beat of whole player calls */
{
    uint32_t n = (calls_min_x10() + bpm_x10 / 2u) / bpm_x10;
    return n ? n : 1u;
}

static uint32_t manual_base(uint32_t bpm_x10)   /* its pulse spacing (tune cycles, /256): the beat / 24 */
{
    return bpm_x10 ? (uint32_t)((uint64_t)manual_beat(bpm_x10) * call_cycles() * 256u / 24u) : 0u;
}

static uint32_t fol_ff(void)                /* the speed that puts the tune's pulse spacing on the clock's (Q12) */
{
    uint64_t pn = (uint64_t)clk_period * PLAT_TICKS_HZ / chip_clock();   /* the tune's, as written (ticks, /256) */
    return fol_pe ? (uint32_t)(pn * 4096u / fol_pe) : 4096u;
}

static void fol_speed(uint32_t q12)
{
    uint32_t lo = ENG_TEMPO_MIN * 4096u / 100u, hi = (uint32_t)eng_speed_cap() * 4096u / 100u;
    fol_q12 = q12 < lo ? lo : q12 > hi ? hi : q12;
}

static void fol_reset(void)                 /* a load: the tune's pulses from 0, paired anew */
{
    fol_ni = fol_np = 0;
    fol_paired = 0;
}

static void fol_take(void)                  /* the clock's pulses that came in */
{
    uint32_t t, k;
    while (plat_clock_in(&t, &k)) {
        if (k == PLAT_CLK_STOP) {           /* a Stop: the clock paused, not gone */
            fol_held = 1;
            fol_hold_play = 0;
            continue;
        }
        if (k != PLAT_CLK_FREE) fol_held = 0;   /* (pulses after the Stop, already on their way, leave it held) */
        if (k == PLAT_CLK_START) {          /* a Start: numbered from 0, paired anew */
            fol_ne = 0;
            fol_paired = fol_prev = 0;
        } else {
            uint32_t d = t - fol_last;      /* the spacing: over the last bar (a pulse lands a USB frame early or late) */
            if (!fol_prev || d >= PLAT_TICKS_HZ / 4u) { fol_wn = fol_dev = 0; fol_gap = 1; }   /* (after a gap: anew) */
            fol_w[fol_wc++ % FOL_W] = t;
            if (fol_wn < 96u + 1u) fol_wn++;
            if (fol_wn >= 2u) {
                uint32_t n = fol_wn - 1u;
                uint64_t pl = (uint64_t)(t - fol_w[(fol_wc - 1u - n) % FOL_W]) * 256u / n;
                if (n > FOL_WS) {           /* the last few against it: a new tempo, the bar anew from them */
                    uint64_t ps = (uint64_t)(t - fol_w[(fol_wc - 1u - FOL_WS) % FOL_W]) * 256u / FOL_WS;
                    if (ps * 100u > pl * (100u + FOL_WJ) || ps * 100u < pl * (100u - FOL_WJ)) { fol_wn = FOL_WS + 1u; pl = ps; fol_dev = 0; }
                }
                if (n > 2u * FOL_WM) {      /* the last beat steadily off the rest of the bar: a small change, the bar anew from it */
                    uint32_t tm = fol_w[(fol_wc - 1u - FOL_WM) % FOL_W];
                    uint64_t pm = (uint64_t)(t - tm) * 256u / FOL_WM;
                    uint64_t po = (uint64_t)(tm - fol_w[(fol_wc - 1u - n) % FOL_W]) * 256u / (n - FOL_WM);
                    if (pm * 1000u > po * (1000u + FOL_WD) || pm * 1000u < po * (1000u - FOL_WD)) {
                        if (++fol_dev >= FOL_WK) { fol_wn = FOL_WM + 1u; pl = pm; fol_dev = 0; }
                    } else fol_dev = 0;
                }
                if (!fol_pe || !fol_gap || n >= 96u ||   /* after a gap (a Stop), the last tempo until the new one differs */
                    (n >= FOL_WS && (pl * 100u > (uint64_t)fol_pe * (100u + FOL_WJ) || pl * 100u < (uint64_t)fol_pe * (100u - FOL_WJ))) ||
                    (n >= FOL_WM && (pl * 1000u > (uint64_t)fol_pe * (1000u + 2u * FOL_WD) || pl * 1000u < (uint64_t)fol_pe * (1000u - 2u * FOL_WD)))) {
                    fol_pe = (uint32_t)pl;
                    fol_gap = 0;
                }
            }
            if (k != PLAT_CLK_FREE) { fol_te[k % FOL_N] = t; fol_ne = k + 1; }
            fol_prev = 1;
        }
        fol_last = t;
        fol_heard = 1;
        if (!fol_paired && eng_follow && fol_pe) fol_speed(fol_ff());
    }
    if (fol_held && eng_playing) {          /* the song plays and the stopped clock does not come back: let go */
        if (!fol_hold_play) { fol_hold_play = 1; fol_hold_t = plat_ticks(); }
        else if (plat_ticks() - fol_hold_t > PLAT_TICKS_HZ) fol_held = 0;
    }
    eng_follow = clk_on && fol_heard && ((int32_t)(plat_ticks() - fol_last) < (int32_t)FOL_ALIVE || fol_held);   /* (a Start alone: the clock to come) */
    if (eng_follow && fol_pe) eng_follow_bpm_x10 = (uint16_t)(((uint64_t)25u * PLAT_TICKS_HZ * 256u + fol_pe / 2u) / fol_pe);   /* 60 x 10 / 24 */
}

static void fol_pair(void)                  /* each pair: the speed */
{
    while (fol_paired && fol_np < fol_ni) {
        int32_t j = (int32_t)fol_np + fol_shift;
        int32_t e, c;
        if (j < 0 || (uint32_t)j >= fol_ne) break;   /* (the clock's still to come) */
        if (fol_ni - fol_np > FOL_N || fol_ne - (uint32_t)j > FOL_N) { fol_paired = 0; break; }   /* lost: paired anew */
        e = (int32_t)(fol_ti[fol_np % FOL_N] - fol_te[(uint32_t)j % FOL_N]);
        eng_follow_err_us = e / (int32_t)(PLAT_TICKS_HZ / 1000000u);
        c = fol_div(e, 4096u * 256u / FOL_GAIN, fol_pe);
        c = c < -FOL_STEER ? -FOL_STEER : c > FOL_STEER ? FOL_STEER : c;
        fol_speed((uint32_t)((uint64_t)fol_ff() * (uint32_t)(4096 + c) / 4096u));
        fol_np++;
    }
}

static void fol_pulse(uint32_t n, uint32_t t)   /* the tune's pulse n plays at t */
{
    fol_ti[n % FOL_N] = t;
    fol_ni = n + 1;
    if (!fol_paired && fol_heard && fol_pe) {   /* the clock's nearest bar */
        int32_t x = (int32_t)fol_ne - 1 + fol_div((int32_t)(t - fol_last), 256u, fol_pe) - (int32_t)n;
        fol_shift = (x >= 0 ? x + 48 : x - 47) / 96 * 96;
        if ((int32_t)n + fol_shift < 0) fol_shift += 96;
        fol_np = n;
        fol_paired = 1;
    }
    fol_pair();
}

static void chips_clock(void)               /* the vinyl fader moved: the chips run at the new clock */
{
    int i;
    for (i = 0; i < C64_NSID; i++) {
        int32_t off = eng_sid[i].sample_offset;
        sid_set_sampling(&eng_sid[i], chip_clock(), ENG_RATE);
        eng_sid[i].sample_offset = off;     /* no step in the sampling */
    }
}

static void sids_setup(int ntsc)
{
    int i;
    for (i = 0; i < C64_NSID; i++) {
        int m = model_ovr ? model_ovr : (sidfile_model(&eng_file_info, i) == SID_MODEL_8580 ? 2 : 1);
        sid_init(&eng_sid[i]);
        sid_set_model(&eng_sid[i], m == 2 ? SID_MOS8580 : SID_MOS6581);
        (void)ntsc;                         /* (eng_ntsc is set: chip_clock) */
        sid_set_sampling(&eng_sid[i], chip_clock(), ENG_RATE);
        sid_set_averaging(&eng_sid[i], 1);
        sid_set_floating_dac(&eng_sid[i], 1);   /* no waveform keeps the level: the 8580's sample trick */
    }
    eng_sid[0].tap = tap;
}

void eng_init(void)
{
    int i, j;
    c64_init(&c64, c64_mem);
    c64.sid_read = sid_read_cb;
    remix_init(&rx);
    asid_init(&eng_asid, asid_emit, 0);
    eng_output = 0;
    gain = gain_to = 0;
    after_fade = FADE_NONE;
    load_req = 0;
    for (i = 0; i < C64_NSID; i++) for (j = 0; j < SID_NREG; j++) eng_regs[i][j] = 0;
    for (i = 0; i < 3; i++) for (j = 0; j < ENG_SCOPE_N; j++) eng_scope[i][j] = 0;
    eng_scope_w = 0;
    eng_scope_n = 0;
    model_ovr = 0;
    sids_setup(0);
    eng_playing = eng_loaded = eng_ntsc = 0;
    eng_log_lost = 0;
    eng_file.pending = 0;
    eng_load_rc = 0;
    nout = out_i = 0;
    chunk_len = chunk_pos = 0;
    ev_w = ev_r = 0;
    {                                       /* the click: 3 kHz, falling off over ~10 ms (a sine by rotation) */
        float c = 0.9070138f, s = 0.4211900f, x = 1.0f, y = 0.0f, a = 14000.0f;   /* cos, sin of 2 pi 3000 / 44100 */
        for (i = 0; i < CLICK_N; i++) {
            float nx = x * c - y * s;
            y = x * s + y * c; x = nx;
            click_tab[i] = (int16_t)(y * a);
            a *= 0.9975f;
        }
    }
}

int eng_post(uint8_t type, uint8_t a, uint8_t b, uint32_t pkt)
{
    uint32_t w = ev_w;
    if (w - ev_r >= EVQ)
        return -1;
    evq[w % EVQ].type = type;
    evq[w % EVQ].a = a;
    evq[w % EVQ].b = b;
    evq[w % EVQ].pkt = pkt;
    __asm__ volatile("" ::: "memory");
    ev_w = w + 1;
    return 0;
}

/* ASID goes out (ASID or BOTH), unless the tune is an RSID: those drive the whole C64 at its own
 * cycles (often digis), which ASID's frames cannot carry, so they play on the emulated SID */
static int asid_on(void) { return asid_out() && !eng_deck_off; }

static uint8_t vol_off(uint8_t mode_vol) { return (uint8_t)(mode_vol & ~SID_MODE_VOL); }   /* the mode, silent */

/* a write to a chip. While paused the volume stays at 0 on the emulated chips (eng_regs keeps
 * the tune's value), and nothing goes to the ASID receiver: the resume sends the whole image */
static void sid_apply(int chip, uint8_t reg, uint8_t val)
{
    if (chip == OUT_FRAME) { if (asid_on() && eng_playing) asid_frame(&eng_asid); return; }   /* the frame changed */
    if (chip == OUT_CLOCK) {
        if (!eng_playing) return;
        if (eng_clk_click && eng_clk_pulses % 24u == 0) click_i = 0;   /* a beat */
        if (eng_follow) fol_pulse(eng_clk_pulses, plat_frame_ticks(rt_frame));
        else plat_midi_rt(0xF8, rt_frame);
        eng_clk_pulses++;
        return;
    }
    if (chip == OUT_START) { if (!eng_follow) plat_midi_rt(0xFA, rt_frame); eng_clk_pulses = 0; fol_reset(); return; }
    sid_write(&eng_sid[chip], reg, reg == SID_ADDR_MODE_VOL && !eng_playing ? vol_off(val) : val);
    eng_regs[chip][reg] = val;
    if (asid_on() && eng_playing) asid_write(&eng_asid, chip, reg, val);
}

/* the receiver's image is unknown (it may have reset or muted its chips at 0x4D, or never had
 * one): the whole image goes out as it stands, the volume at 0 while paused */
static void asid_resync(void)
{
    int i, j;
    asid_reset(&eng_asid, c64.nsid);
    for (i = 0; i < c64.nsid; i++)
        for (j = 0; j < ASID_NREG; j++)
            asid_write(&eng_asid, i, (uint8_t)j, j == SID_ADDR_MODE_VOL && !eng_playing ? vol_off(eng_regs[i][j]) : eng_regs[i][j]);
    asid_frame(&eng_asid);
}

/* an override changed: what it changes on the chip goes out now */
static void rx_push(int voice)
{
    rx_out_t o[SID_NWRITE];
    int n = remix_refresh(&rx, voice, o, SID_NWRITE), i;
    for (i = 0; i < n; i++) sid_apply(0, o[i].reg, o[i].val);
}

static void lfo_run(uint32_t n)             /* the LFOs on by n samples: what they moved goes out now */
{
    rx_out_t o[12];
    int m, i;
    if (eng_deck_off) return;               /* an RSID: no LFOs */
    m = remix_lfo_tick(&rx, n, o, 12);
    for (i = 0; i < m; i++) sid_apply(0, o[i].reg, o[i].val);
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int eng_speed_cap(void) { return eng_usb4 ? 100 : ENG_TEMPO_MAX; }

static void deck_clamp(void)                /* tempo x (1 + a raised fader) within the cap */
{
    int cap = eng_speed_cap(), v = eng_vinyl;
    if (eng_tempo > cap) eng_tempo = (uint8_t)cap;
    if (v > 0 && v > cap * 1000 / eng_tempo - 1000) v = cap * 1000 / eng_tempo - 1000;
    if (v != eng_vinyl) { eng_vinyl = (int8_t)(v > 0 ? v : 0); chips_clock(); }
}

static void rx_event(uint8_t p, uint8_t voice, int32_t val)
{
    rx_voice_t *x = &rx.v[voice < RX_NV ? voice : 0];
    switch (p) {
    case RX_P_KEY:                          /* val: MIDI note, 0 = released */
        if (val > 0) { x->key_on = 1; x->key_freq = remix_note_freq((int)val, eng_ntsc); }
        else x->key_on = 0;
        rx_push(voice);
        break;
    case RX_P_WAVE_TOGGLE:                  /* val: the bit. A waveform replaces the tune's (again: the tune's back);
                                             * sync and ring toggle, frozen from the first press */
        if (val & SID_CTRL_WAVES) {
            if (x->wave_ovr && x->wave_val == (uint8_t)val) x->wave_ovr = 0;
            else { x->wave_ovr = 1; x->wave_val = (uint8_t)val; }
        } else {
            uint8_t sr = SID_CTRL_SYNC | SID_CTRL_RING, t = rx.tune[SID_ADDR_VOICE(voice) + SID_ADDR_CTRL];
            x->sr_val = (uint8_t)((x->sr_val & x->sr_ovr) | (t & sr & ~x->sr_ovr));   /* the free bits as the tune has them */
            x->sr_ovr = sr;
            x->sr_val ^= (uint8_t)(val & sr);
        }
        rx_push(voice);
        break;
    case RX_P_MUTE_TOGGLE:
        x->mute = !x->mute;
        rx_push(voice);
        break;
    case RX_P_SOLO: {                       /* the voice alone; pressed on a soloed voice: all back */
        int v, solo = !x->mute;
        for (v = 0; v < RX_NV; v++) if (v != (int)voice && !rx.v[v].mute) solo = 0;
        for (v = 0; v < RX_NV; v++) rx.v[v].mute = (uint8_t)(solo ? 0 : v != (int)voice);
        rx_push(-1);
        break;
    }
    case RX_P_FILT_TOGGLE:
        if (!x->filt_ovr) { x->filt_ovr = 1; x->filt_val = (uint8_t)(rx.tune[SID_ADDR_RES_FILT] >> (voice < RX_NV ? voice : 0) & 1); }
        x->filt_val ^= 1;
        rx_push(-1);
        break;
    case RX_P_FMODE_TOGGLE:
        rx.fmode_val = (uint8_t)((rx.fmode_val & rx.fmode_ovr) | (rx.tune[SID_ADDR_MODE_VOL] & SID_MODE_FILTERS & ~rx.fmode_ovr));
        rx.fmode_ovr = SID_MODE_FILTERS;
        rx.fmode_val ^= (uint8_t)(val & SID_MODE_FILTERS);
        rx_push(-1);
        break;
    case RX_P_KEY_SHIFT:
        if (val & 0x100) { x->shift_on = 1; x->shift_semi = (int8_t)(val & 0xff); }
        else x->shift_on = 0;
        rx_push(voice);
        break;
    case RX_P_LFO_RATE: case RX_P_LFO_DEPTH: case RX_P_LFO_TARGET: case RX_P_LFO_SHAPE:
        if (eng_deck_off) break;            /* an RSID: no LFOs */
        if (p != RX_P_LFO_RATE) goto lfo_more;
        x->lfo_rate = (uint8_t)clampi(x->lfo_rate + val, 0, RX_LFO_NRATE - 1); break;
    lfo_more:
        if (p == RX_P_LFO_DEPTH) x->lfo_depth = (uint8_t)clampi(x->lfo_depth + val, 0, RX_LFO_DEPTH_MAX);
        else if (p == RX_P_LFO_TARGET) x->lfo_target = (uint8_t)clampi(x->lfo_target + val, 0, RX_LFO_NTARGET - 1);
        else if (p == RX_P_LFO_SHAPE) x->lfo_shape = (uint8_t)clampi(x->lfo_shape + val, 0, RX_LFO_NSHAPE - 1);
        break;
    case RX_P_TEMPO: {                      /* within the cap with the fader: at it, the fader yields nothing */
        int cap = eng_speed_cap(), t;
        if (eng_deck_off) break;
        t = clampi(eng_tempo + val, ENG_TEMPO_MIN, cap);
        if (eng_vinyl > 0 && t > cap * 1000 / (1000 + eng_vinyl)) t = cap * 1000 / (1000 + eng_vinyl);
        eng_tempo = (uint8_t)(t > eng_tempo || val < 0 ? t : eng_tempo > t ? t : eng_tempo);
        break;
    }
    case RX_P_VINYL: {                      /* down freely; up only while tempo x fader stays within the cap */
        int v;
        if (eng_deck_off) break;
        v = clampi(eng_vinyl + val, -ENG_VINYL_MAX, ENG_VINYL_MAX);
        if (v > 0 && v > eng_speed_cap() * 1000 / eng_tempo - 1000) v = eng_speed_cap() * 1000 / eng_tempo - 1000;
        if (v < eng_vinyl || val > 0) eng_vinyl = (int8_t)(v > eng_vinyl || val < 0 ? v : eng_vinyl);
        chips_clock();
        break;
    }
    case RX_P_CUTOFF_MODE:
        cut_mode = (uint8_t)(val == RX_CUT_OFFSET ? RX_CUT_OFFSET : RX_CUT_SCALE);
        rx.cutoff_mode = cut_mode;
        rx_push(-1);
        break;
    case RX_P_PW:                           /* val: a delta on the current value */
        if (!x->pw_ovr) { x->pw_ovr = 1; x->pw_val = remix_tune_pw(&rx, voice); }
        x->pw_val = (uint16_t)clampi((int)x->pw_val + val, 0, SID_PW_MAX);
        rx_push(voice);
        break;
    case RX_P_OCT: x->oct = (int8_t)clampi(x->oct + val, -RX_OCT_MAX, RX_OCT_MAX); rx_push(voice); break;
    case RX_P_FINE: x->fine = (int8_t)clampi(x->fine + val, -RX_FINE_MAX, RX_FINE_MAX); rx_push(voice); break;   /* cents */
    case RX_P_A: case RX_P_D: case RX_P_S: case RX_P_R: {
        uint8_t *f = p == RX_P_A ? &x->a : p == RX_P_D ? &x->d : p == RX_P_S ? &x->s : &x->r;
        uint8_t bit = (uint8_t)(1u << (p - RX_P_A)), t = rx.tune[SID_ADDR_VOICE(voice) + (p <= RX_P_D ? SID_ADDR_AD : SID_ADDR_SR)];
        if (!(x->adsr_ovr & bit)) {
            x->adsr_ovr |= bit;
            *f = (uint8_t)((p == RX_P_A || p == RX_P_S) ? t >> 4 : t & 15);
        }
        *f = (uint8_t)clampi(*f + val, 0, SID_NIBBLE_MAX);
        rx_push(voice);
        break;
    }
    case RX_P_CUTOFF:                       /* val: clicks (x4 when fast); the scale's step grows with it */
        if (rx.cutoff_mode == RX_CUT_OFFSET) rx.cutoff_off = (int16_t)clampi(rx.cutoff_off + val * 16, -SID_FC_MAX, SID_FC_MAX);
        else {
            int step = rx.cutoff_scale / 16;
            if (step < 4) step = 4;
            rx.cutoff_scale = (uint16_t)clampi((int)rx.cutoff_scale + val * step, 0, RX_CUTOFF_MAX);
        }
        rx_push(-1);
        break;
    case RX_P_RES: rx.res_off = (int8_t)clampi(rx.res_off + val, -SID_NIBBLE_MAX, SID_NIBBLE_MAX); rx_push(-1); break;
    default: break;
    }
}

static void bit_set(uint8_t *ovr, uint8_t *v, uint8_t bit, int32_t val)   /* -1 the tune's, 0 off, 1 on */
{
    if (val < 0) { *ovr &= (uint8_t)~bit; return; }
    *ovr |= bit;
    *v = (uint8_t)(val ? *v | bit : *v & ~bit);
}

static void rx_set(uint8_t p, uint8_t voice, int32_t val)   /* EV_RX_SET: the value itself, as MIDI CCs send it */
{
    rx_voice_t *x = &rx.v[voice < RX_NV ? voice : 0];
    int v;
    switch (p) {
    case RX_P_WAVE_TOGGLE:                  /* a waveform bit, 0 the tune's */
        x->wave_ovr = (val & SID_CTRL_WAVES) != 0;
        x->wave_val = (uint8_t)(val & SID_CTRL_WAVES);
        rx_push(voice);
        break;
    case RX_P_SYNC: case RX_P_RING:
        bit_set(&x->sr_ovr, &x->sr_val, p == RX_P_SYNC ? SID_CTRL_SYNC : SID_CTRL_RING, val);
        rx_push(voice);
        break;
    case RX_P_MUTE_TOGGLE: x->mute = val != 0; rx_push(voice); break;
    case RX_P_SOLO:                         /* 1: the voice alone; 0: all back */
        for (v = 0; v < RX_NV; v++) rx.v[v].mute = (uint8_t)(val && v != (int)voice);
        rx_push(-1);
        break;
    case RX_P_FILT_TOGGLE: x->filt_ovr = val >= 0; x->filt_val = val > 0; rx_push(-1); break;
    case RX_P_FMODE_TOGGLE: bit_set(&rx.fmode_ovr, &rx.fmode_val, voice & SID_MODE_FILTERS, val); rx_push(-1); break;
    case RX_P_PW:
        x->pw_ovr = val >= 0;
        if (val >= 0) x->pw_val = (uint16_t)clampi(val, 0, SID_PW_MAX);
        rx_push(voice);
        break;
    case RX_P_OCT: x->oct = (int8_t)clampi(val, -RX_OCT_MAX, RX_OCT_MAX); rx_push(voice); break;
    case RX_P_FINE: x->fine = (int8_t)clampi(val, -RX_FINE_MAX, RX_FINE_MAX); rx_push(voice); break;
    case RX_P_A: case RX_P_D: case RX_P_S: case RX_P_R: {
        uint8_t *f = p == RX_P_A ? &x->a : p == RX_P_D ? &x->d : p == RX_P_S ? &x->s : &x->r;
        uint8_t bit = (uint8_t)(1u << (p - RX_P_A));
        if (val < 0) x->adsr_ovr &= (uint8_t)~bit;
        else { x->adsr_ovr |= bit; *f = (uint8_t)clampi(val, 0, SID_NIBBLE_MAX); }
        rx_push(voice);
        break;
    }
    case RX_P_LFO_RATE: case RX_P_LFO_DEPTH: case RX_P_LFO_TARGET: case RX_P_LFO_SHAPE:
        if (eng_deck_off) break;            /* an RSID: no LFOs */
        if (p == RX_P_LFO_RATE) x->lfo_rate = (uint8_t)clampi(val, 0, RX_LFO_NRATE - 1);
        else if (p == RX_P_LFO_DEPTH) x->lfo_depth = (uint8_t)clampi(val, 0, RX_LFO_DEPTH_MAX);
        else if (p == RX_P_LFO_TARGET) x->lfo_target = (uint8_t)clampi(val, 0, RX_LFO_NTARGET - 1);
        else x->lfo_shape = (uint8_t)clampi(val, 0, RX_LFO_NSHAPE - 1);
        break;
    case RX_P_CUTOFF:
        if (rx.cutoff_mode == RX_CUT_OFFSET) rx.cutoff_off = (int16_t)clampi(val, -SID_FC_MAX, SID_FC_MAX);
        else rx.cutoff_scale = (uint16_t)clampi(val, 0, RX_CUTOFF_MAX);
        rx_push(-1);
        break;
    case RX_P_RES: rx.res_off = (int8_t)clampi(val, -SID_NIBBLE_MAX, SID_NIBBLE_MAX); rx_push(-1); break;
    case RX_P_TEMPO: rx_event(p, 0, val - eng_tempo); break;   /* as a step: the cap applies the same way */
    case RX_P_VINYL: rx_event(p, 0, val - eng_vinyl); break;
    case RX_P_CUTOFF_MODE: rx_event(p, 0, val); break;
    default: break;
    }
}

static void do_load(uint8_t song, uint8_t opts)
{
    int ntsc, i, j, paused = (opts & ENG_LOAD_PAUSED) != 0;
    eng_playing = 0;
    eng_loaded = 0;
    eng_load_rc = sidfile_parse(&eng_file_info, eng_file.data, eng_file.len);
    if (eng_load_rc == 0 && eng_file_info.basic) eng_load_rc = -5;
    if (eng_load_rc == 0 && sidfile_nsids(&eng_file_info) > 1) eng_load_rc = -6;   /* one SID only: two or three overload the FM-1 */
    if (eng_load_rc) { eng_file.pending = 0; return; }
    ntsc = (opts & 1) ? 1 : (opts & 2) ? 0 : sidfile_clock(&eng_file_info) == SID_CLOCK_NTSC;
    model_ovr = (opts >> 2) & 3;
    eng_ntsc = (uint8_t)ntsc;
    eng_deck_off = eng_file_info.rsid;      /* (before sids_setup: the chips' clock) */
    eng_tempo = 100;                        /* the deck starts clean too, like the remix */
    eng_vinyl = 0;
    sids_setup(ntsc);
    remix_init(&rx);                        /* a new song starts clean (the cutoff mode stays) */
    rx.cutoff_mode = cut_mode;
    for (i = 0; i < C64_NSID; i++) for (j = 0; j < SID_NREG; j++) eng_regs[i][j] = 0;
    clock_off();                            /* (Stop, if it ran) */
    tune_cyc = 0;
    fol_reset();
    eng_clk_bpm_x10 = 0;
    eng_clk_tried = 0;
    eng_clk_manual = 0;
    if (!(opts & ENG_LOAD_AGAIN)) { eng_clk_ratio = ENG_CLK_X1; eng_clk_offset_ms = 0; clk_offset_cyc = 0; }   /* (again: what was put right stays) */
    if (eng_clk_preset.valid && eng_output == ENG_OUT_CLOCK && !eng_file_info.rsid && !paused) {   /* the song's saved tempo: no look */
        det_last.valid = det_last.found = 1;
        det_last.tick = eng_clk_preset.tick; det_last.call = eng_clk_preset.call; det_last.off = eng_clk_preset.off;
        det_last.bpm_x10 = eng_clk_preset.bpm_x10;
        det_last.base = eng_clk_preset.tick ? eng_clk_preset.base : manual_base(eng_clk_preset.bpm_x10);
        eng_clk_ratio = eng_clk_preset.ratio < ENG_CLK_RATIOS ? eng_clk_preset.ratio : ENG_CLK_X1;
        eng_clk_offset_ms = eng_clk_preset.offset_ms;
        clk_offset_cyc = (int32_t)((int64_t)eng_clk_offset_ms * (int64_t)c64.clock_hz / 1000);
        opts |= ENG_LOAD_AGAIN;             /* (no look below) */
    }
    eng_clk_preset.valid = 0;
    clk_ratio_now = eng_clk_ratio;
    c64_load(&c64, &eng_file_info, song ? song : eng_file_info.start_song, ntsc);
    if (eng_output == ENG_OUT_CLOCK && !eng_deck_off && !paused) {   /* the beat first, then the tune again from its start */
        beat_grid_t g;
        uint32_t tick, off;
        eng_clk_tried = 1;
        if (!((opts & ENG_LOAD_AGAIN) && det_last.valid)) {   /* (again: the last look's answer) */
            det_last.found = (uint8_t)detect(&g, &tick, &off);
            det_last.valid = 1;
            if (det_last.found) {
                det_last.tick = tick;
                det_last.call = g.anchor;
                det_last.off = off;
                /* 24 pulses a beat; a beat is rpb2 * (a + b) / 4 calls: rpb2 * (a + b) * tick / 96 cycles */
                det_last.base = (uint32_t)((uint64_t)g.rpb2 * (g.a + g.b) * tick * 256u / 96u);
                det_last.bpm_x10 = (uint16_t)beat_bpm_x10(&g, (uint32_t)((uint64_t)c64.clock_hz * 60u / tick));
            }
            c64_load(&c64, &eng_file_info, song ? song : eng_file_info.start_song, ntsc);
        }
        if (det_last.found) {
            clk_tick = det_last.tick;
            clk_call = det_last.call;
            clk_off = det_last.off;
            clk_base = det_last.base;
            clk_period = (uint32_t)((uint64_t)clk_base * ENG_CLK_RATIO[clk_ratio_now][1] / ENG_CLK_RATIO[clk_ratio_now][0]);
            clk_sched = 0;
            clk_frac = 0;
            clk_on = clk_start = clk_resolve = 1;
            eng_clk_on = 1;
            eng_clk_bpm_x10 = det_last.bpm_x10;
            eng_clk_manual = clk_tick == 0;   /* (set by hand: from the first write, at x1) */
        }
    } else det_last.valid = 0;              /* (paused, or no clock: the next look is a new one) */
    if (asid_out() && eng_deck_off) asid_stop(&eng_asid);   /* an RSID: the receiver falls silent */
    else if (asid_out()) {                  /* the receiver starts from a clean image */
        asid_reset(&eng_asid, c64.nsid);
        asid_types();
        if (!paused) asid_start(&eng_asid);   /* (paused: EV_PLAY starts it) */
    }
    nout = out_i = 0;
    chunk_len = chunk_pos = 0;
    eng_log_lost = 0;
    eng_file.pending = 0;
    eng_loaded = 1;
    eng_playing = !paused;
    clk_paused = !eng_playing;              /* (paused: EV_PLAY sends Start) */
}

static void clk_manual(uint32_t bpm_x10)   /* no tempo found: the clock at a tempo set by hand */
{
    if (!eng_loaded || eng_output != ENG_OUT_CLOCK || eng_deck_off) return;
    if (!bpm_x10) {                         /* none */
        clock_off();
        det_last.found = 0;
        eng_clk_bpm_x10 = 0;
        eng_clk_manual = 0;
        return;
    }
    det_last.valid = det_last.found = 1;    /* (a load again starts it from the first write) */
    det_last.tick = det_last.call = det_last.off = 0;
    bpm_x10 = calls_min_x10() / manual_beat(bpm_x10);   /* (the tempo the beat of whole calls has) */
    det_last.bpm_x10 = (uint16_t)bpm_x10;
    det_last.base = manual_base(bpm_x10);
    eng_clk_bpm_x10 = (uint16_t)bpm_x10;
    eng_clk_manual = 1;
    eng_clk_ratio = clk_ratio_now = ENG_CLK_X1;
    clk_base = clk_period = det_last.base;  /* (running: the pulses still to come at the new spacing) */
    if (!clk_on) {                          /* from now: Start with the next pulse */
        clk_tick = clk_call = clk_off = 0;
        clk_next = tune_cyc;
        clk_frac = clk_sched = 0;
        clk_resolve = 0;
        clk_on = clk_start = 1;
        clk_paused = !eng_playing;
        eng_clk_on = 1;
    }
}

static void stop_now(void)                  /* the pause itself, once the output is silent */
{
    int i;
    if (!eng_playing) return;
    eng_playing = 0;
    for (i = 0; i < c64.nsid; i++) sid_write(&eng_sid[i], SID_ADDR_MODE_VOL, vol_off(eng_regs[i][SID_ADDR_MODE_VOL]));
    if (asid_on()) {                        /* the frame so far, the volume to 0 (for receivers that
                                             * ignore 0x4D), then 0x4D */
        asid_frame(&eng_asid);
        for (i = 0; i < c64.nsid; i++) asid_write(&eng_asid, i, SID_ADDR_MODE_VOL, vol_off(eng_regs[i][SID_ADDR_MODE_VOL]));
        asid_frame(&eng_asid);
        asid_stop(&eng_asid);
    }
    if (clk_on && !clk_paused) { if (!clk_start) midi_rt(0xFC); clk_paused = 1; }   /* Stop; the play after it: Continue */
}

static void load_start(void)                /* the output is silent: hand the load to eng_poll */
{
    eng_playing = 0;
    eng_loaded = 0;
    after_fade = FADE_NONE;
    gain = gain_to = 0;
    __asm__ volatile("" ::: "memory");
    load_req = 1;
}

void eng_poll(void)
{
    if (!load_req) return;
    do_load(load_song, load_opts);          /* the ISR renders silence and touches nothing meanwhile */
    gain = 0;
    gain_to = eng_playing ? GAIN_ONE : 0;   /* the new tune fades in */
    __asm__ volatile("" ::: "memory");
    load_req = 0;
}

static void eng_apply(const ev_t *e)
{
    int i;
    switch (e->type) {
    case EV_LOAD:                           /* the tune playing fades out first; the load runs in eng_poll */
        if (!eng_file.pending) break;
        load_song = e->a; load_opts = e->b;
        if (eng_playing && gain > 0) { after_fade = FADE_LOAD; gain_to = 0; }
        else load_start();
        break;
    case EV_PLAY:
        if (eng_playing && after_fade == FADE_STOP) { after_fade = FADE_NONE; gain_to = GAIN_ONE; break; }   /* pause undone mid-fade */
        if (eng_loaded && !eng_playing) {
            eng_playing = 1;
            for (i = 0; i < c64.nsid; i++) sid_write(&eng_sid[i], SID_ADDR_MODE_VOL, eng_regs[i][SID_ADDR_MODE_VOL]);
            if (asid_on()) { asid_start(&eng_asid); asid_resync(); }
            if (clk_on && clk_paused) { if (!clk_start) midi_rt(0xFB); clk_paused = 0; }   /* Continue (or Start, still to come) */
            fol_paired = 0;                 /* (following: to the clock's nearest bar again) */
            gain_to = GAIN_ONE;             /* from silence */
        }
        break;
    case EV_STOP:                           /* fades out, then stop_now() */
        if (eng_playing && after_fade == FADE_NONE) { after_fade = FADE_STOP; gain_to = 0; }
        break;
    case EV_CLOCK_RATIO:
        if (e->a < ENG_CLK_RATIOS) eng_clk_ratio = e->a;
        break;
    case EV_CLOCK_OFFSET: {                 /* the pulses still to come move by the change */
        int32_t ms = clampi((int32_t)e->pkt, -ENG_CLK_OFFSET_MAX, ENG_CLK_OFFSET_MAX);
        int32_t to = (int32_t)((int64_t)ms * (int64_t)c64.clock_hz / 1000), d = to - clk_offset_cyc;
        eng_clk_offset_ms = (int16_t)ms;
        clk_offset_cyc = to;
        if (clk_on && !clk_resolve) {
            clk_next += (uint32_t)d;
            if ((int32_t)(clk_next - tune_cyc) < 0) clk_next = tune_cyc;   /* (earlier than now: from now) */
        }
        break;
    }
    case EV_CLOCK_MANUAL:
        clk_manual(e->pkt);
        break;
    case EV_USB4:                           /* USB audio's format changed: the cap with it */
        eng_usb4 = e->a ? 1 : 0;
        deck_clamp();
        break;
    case EV_OUTPUT: {
        int old = asid_on(), now;
        eng_output = e->a > 3 ? 0 : e->a;
        if (eng_output != ENG_OUT_CLOCK) clock_off();   /* (to the clock: from the next load, which finds the beat) */
        now = asid_on();
        if (!old && now && eng_loaded) {    /* the receiver gets the whole image as it stands */
            asid_types();
            if (eng_playing) asid_start(&eng_asid);
            asid_resync();
        }
        if (old && !now) asid_stop(&eng_asid);
        break;
    }
    case EV_MODEL:                          /* a = 0 file, 1 6581, 2 8580: the chips switch in place */
        model_ovr = e->a;
        for (i = 0; i < C64_NSID; i++) {
            int m = model_ovr ? model_ovr : (sidfile_model(&eng_file_info, i) == SID_MODEL_8580 ? 2 : 1);
            sid_set_model(&eng_sid[i], m == 2 ? SID_MOS8580 : SID_MOS6581);
        }
        if (asid_on() && eng_loaded) asid_types();
        break;
    case EV_RX:
        rx_event(e->a, e->b, (int32_t)e->pkt);
        break;
    case EV_RX_SET:
        rx_set(e->a, e->b, (int32_t)e->pkt);
        break;
    case EV_RX_RESET:
        remix_reset(&rx);
        rx_push(-1);
        eng_tempo = 100;
        if (eng_vinyl) { eng_vinyl = 0; chips_clock(); }
        break;
    default:
        break;
    }
}

static void fade_done(void)                 /* the fade-out ended: what waited for it */
{
    uint8_t a = after_fade;
    after_fade = FADE_NONE;
    if (a == FADE_STOP) stop_now();
    else if (a == FADE_LOAD) load_start();
}

static void next_chunk(void)
{
    int i;
    nout = out_i = 0;
    chunk_pos = 0;
    if (eng_playing) {
        int marked = 0;
        chunk_len = c64_run(&c64, CHUNK_CYCLES);
        if (!chunk_len) {                   /* a jam: the tune is over */
            eng_playing = 0;
            clock_off();
            chunk_len = CHUNK_CYCLES;
            return;
        }
        for (i = 0; i <= c64.nlog; i++) {
            const c64_write_t *w = &c64.log[i];
            if (!marked && c64.frame_cycle != 0xffffffffu && (i == c64.nlog || w->cycle >= c64.frame_cycle) && nout < OUT_MAX) {
                out[nout].cycle = (uint16_t)c64.frame_cycle; out[nout].chip = OUT_FRAME;
                out[nout].reg = out[nout].val = 0;
                nout++;
                marked = 1;
            }
            if (i == c64.nlog) break;
            if (w->chip == 0) {
                rx_out_t o[4];
                int n = remix_write(&rx, w->reg, w->val, o), k;
                for (k = 0; k < n && nout < OUT_MAX; k++) {
                    out[nout].cycle = w->cycle; out[nout].chip = 0;
                    out[nout].reg = o[k].reg; out[nout].val = o[k].val;
                    nout++;
                }
            } else if (nout < OUT_MAX) {
                out[nout].cycle = w->cycle; out[nout].chip = w->chip;
                out[nout].reg = w->reg; out[nout].val = w->val;
                nout++;
            }
        }
        eng_log_lost = c64.log_lost;
        if (clk_on && clk_resolve && c64.nlog) {   /* the first SID write: the calls as detect counted them */
            uint32_t f = tune_cyc + c64.log[0].cycle;
            clk_next = f + clk_off + clk_call * clk_tick;
            if (clk_offset_cyc >= 0 || (uint32_t)-clk_offset_cyc <= clk_next - f) clk_next += (uint32_t)clk_offset_cyc;   /* (not before the tune) */
            eng_clk_first_cyc = clk_next;
            clk_resolve = 0;
            if (!clk_tick && det_last.bpm_x10) {   /* set by hand: the tune's call spacing is known by now */
                det_last.bpm_x10 = (uint16_t)(calls_min_x10() / manual_beat(det_last.bpm_x10));
                det_last.base = clk_base = manual_base(det_last.bpm_x10);
                clk_period = (uint32_t)((uint64_t)clk_base * ENG_CLK_RATIO[clk_ratio_now][1] / ENG_CLK_RATIO[clk_ratio_now][0]);
                eng_clk_bpm_x10 = det_last.bpm_x10;
            }
        }
        while (clk_on && !clk_paused && !clk_resolve && (int32_t)(clk_next - tune_cyc) < (int32_t)chunk_len && nout + 2 <= OUT_MAX) {
            uint32_t at = (int32_t)(clk_next - tune_cyc) > 0 ? clk_next - tune_cyc : 0;   /* the pulses in this chunk, in place */
            int k = nout, add = clk_start ? 2 : 1;
            while (k > 0 && out[k - 1].cycle > at) { out[k - 1 + add] = out[k - 1]; k--; }
            if (clk_start) { out[k].cycle = (uint16_t)at; out[k].chip = OUT_START; out[k].reg = out[k].val = 0; k++; clk_start = 0; }
            out[k].cycle = (uint16_t)at; out[k].chip = OUT_CLOCK; out[k].reg = out[k].val = 0;
            nout += add;
            if (++clk_sched % 24u == 1u && eng_clk_ratio != clk_ratio_now && eng_clk_ratio < ENG_CLK_RATIOS) {
                clk_ratio_now = eng_clk_ratio;  /* a new ratio from a beat's first pulse: the beats stay in step */
                clk_period = (uint32_t)((uint64_t)clk_base * ENG_CLK_RATIO[clk_ratio_now][1] / ENG_CLK_RATIO[clk_ratio_now][0]);
            }
            clk_frac += clk_period;
            clk_next += clk_frac >> 8;
            clk_frac &= 255u;
        }
        tune_cyc += chunk_len;
        if (eng_follow && !eng_deck_off) {  /* following: the speed the clock sets (Q12), the tempo aside */
            uint32_t t = fol_q12, l = chunk_len * 4096u + tempo_rem;
            for (i = 0; i < nout; i++) out[i].cycle = (uint16_t)((uint32_t)out[i].cycle * 4096u / t);
            chunk_len = l / t;
            tempo_rem = l % t;
            if (!chunk_len) chunk_len = 1;
        } else if (eng_tempo != 100 && !eng_deck_off) {   /* the C64's cycles onto the chips' time: at 200 % a
                                             * chunk of the tune takes half the chips' cycles */
            uint32_t t = eng_tempo, l = chunk_len * 100u + tempo_rem;
            for (i = 0; i < nout; i++) out[i].cycle = (uint16_t)((uint32_t)out[i].cycle * 100u / t);
            chunk_len = l / t;
            tempo_rem = l % t;
            if (!chunk_len) chunk_len = 1;
        }
    } else {
        chunk_len = CHUNK_CYCLES;           /* silence runs on: the chips keep their state */
    }
}

void eng_render(int32_t *lr, int n, uint32_t master_q12)
{
    int16_t tmp[3][128];
    static int16_t stem[128 * 3];
    int16_t *so = eng_stem_out;
    int k, stems = so != 0 || (!eng_busy && !eng_deck_off);   /* USB audio's voices, or the scopes' */
    sid_set_stems(&eng_sid[0], stems);      /* (their filters start from rest when they come on) */
    eng_scope_stems = (uint8_t)stems;
    sid_set_comb_averaging(&eng_sid[0], !eng_busy);
    for (k = 0; k < 3; k++) eng_sid[0].vmute[k] = rx.v[k].mute;   /* a muted voice silent at the chip's output
                                                         * too (its waveform cleared still leaves the DC an
                                                         * envelope makes); the register mute stays for ASID */
    eng_sid[0].stem = stem;
    if (so) for (k = 0; k < 3 * n; k++) so[k] = 0;
    if (load_req) {                         /* eng_poll is loading: hands off the C64 and the chips */
        for (k = 0; k < 2 * n; k++) lr[k] = 0;
        return;
    }
    if (eng_loaded) eng_calls_min_x10 = calls_min_x10();   /* (the UI's hand-set tempo steps) */
    while (ev_r != ev_w) {
        eng_apply(&evq[ev_r % EVQ]);
        ev_r++;
        if (load_req) { for (k = 0; k < 2 * n; k++) lr[k] = 0; return; }
    }
    fol_take();                             /* (paused too: the clock's spacing kept up) */
    if (!eng_playing && gain == 0 && after_fade == FADE_NONE) {   /* paused: the chips idle */
        for (k = 0; k < 2 * n; k++) lr[k] = 0;
        return;
    }
    rt_frame = 0;
    while (n > 0) {
        uint32_t target;
        int want = n > 128 ? 128 : n, d, got, i, c;
        if (chunk_pos >= chunk_len)
            next_chunk();
        while (out_i < nout && out[out_i].cycle <= chunk_pos) {
            sid_apply(out[out_i].chip, out[out_i].reg, out[out_i].val);
            out_i++;
        }
        target = out_i < nout ? out[out_i].cycle : chunk_len;
        if (target > chunk_len) target = chunk_len;
        d = (int)(target - chunk_pos);
        got = sid_clock_render(&eng_sid[0], &d, tmp[0], want, 1);
        for (c = 1; c < c64.nsid; c++) {
            int d2 = (int)(target - chunk_pos);
            sid_clock_render(&eng_sid[c], &d2, tmp[c], want, 1);
        }
        chunk_pos = target - (uint32_t)d;
        for (i = 0; i < got; i++) {
            int32_t l, r, s0 = ((int32_t)tmp[0][i] * (int32_t)master_q12) >> 5;   /* 16-bit -> 24-bit at -6 dBFS */
            uint32_t w = eng_scope_w;
            const int16_t *sc = stems ? stem : tap;   /* each voice as it sounds, else before the filter */
            eng_scope[0][w] = sc[i * 3 + 0];
            eng_scope[1][w] = sc[i * 3 + 1];
            eng_scope[2][w] = sc[i * 3 + 2];
            eng_scope_n++;
            eng_scope_w = eng_scope_n % ENG_SCOPE_N;
            if (c64.nsid >= 2) {
                int32_t s1 = ((int32_t)tmp[1][i] * (int32_t)master_q12) >> 5;
                l = s0; r = s1;
                if (c64.nsid >= 3) {
                    int32_t s2 = ((int32_t)tmp[2][i] * (int32_t)master_q12) >> 6;
                    l += s2; r += s2;
                }
            } else {
                l = r = s0;
            }
            if (gain != gain_to) gain = gain < gain_to ? (gain + GAIN_STEP > gain_to ? gain_to : gain + GAIN_STEP)
                                                       : (gain - GAIN_STEP < gain_to ? gain_to : gain - GAIN_STEP);
            if (gain != GAIN_ONE) { l = ((l >> 8) * gain) >> 6; r = ((r >> 8) * gain) >> 6; }   /* 32 bits: 16 x 14 */
            if (eng_output == 1 && !eng_deck_off) l = r = 0;   /* ASID only: the chips run for the
                                             * screen (scopes, envelopes), silent at the jack and on USB */
            if (click_i < CLICK_N) {        /* the clock's click, on its beat */
                int32_t c = ((int32_t)click_tab[click_i++] * (int32_t)master_q12) >> 5;
                l += c; r += c;
            }
            lr[0] = l;
            lr[1] = r;
            lr += 2;
            if (so && !(eng_output == 1 && !eng_deck_off)) {   /* the stems: the same gain as the mix */
                int v;
                for (v = 0; v < 3; v++) so[v] = gain == GAIN_ONE ? stem[i * 3 + v] : (int16_t)((stem[i * 3 + v] * gain) >> 14);
                so += 3;
            }
        }
        n -= got;
        rt_frame += (uint32_t)got;
        if (eng_playing && got > 0) lfo_run((uint32_t)got);
    }
    if (after_fade != FADE_NONE && gain == 0) fade_done();
}

uint32_t eng_elapsed_ds(void)
{
    /* frames * cycles per frame * 10 / clock, in 32 bits: frames * 10 / (clock / frame cycles) */
    uint32_t fc = c64_frame_cycles(&c64);
    if (!fc) return 0;
    return (uint32_t)(((uint64_t)c64.frames * fc * 10u) / c64.clock_hz);
}

int eng_clk_get(eng_clk_grid_t *g)
{
    if (!det_last.valid || !det_last.found) return 0;
    g->valid = 1; g->ratio = eng_clk_ratio; g->offset_ms = eng_clk_offset_ms;
    g->tick = (uint16_t)det_last.tick; g->call = (uint16_t)det_last.call; g->off = (uint16_t)det_last.off;
    g->bpm_x10 = det_last.bpm_x10; g->base = det_last.base;
    return det_last.tick <= 0xFFFFu && det_last.call <= 0xFFFFu && det_last.off <= 0xFFFFu;
}
