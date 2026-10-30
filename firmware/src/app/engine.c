/* SPDX-License-Identifier: GPL-3.0-only */
/* SiDJ engine: see engine.h. Compiled as its own unit at -O2 (tools/build.py). */
#include "engine.h"
#include "plat.h"
#include <stddef.h>
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

/* a complete SysEx to USB-MIDI; a message the ring cannot take whole is dropped whole */
static void asid_emit(void *ctx, const uint8_t *m, int n)
{
    uint32_t pk[16];
    int k = asid_usb_packets(m, n, pk, 16), i;
    (void)ctx;
    if (!k || plat_midi_out_free() < k) return;
    for (i = 0; i < k; i++) plat_midi_out(pk[i]);
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
static int asid_on(void) { return eng_output && !eng_deck_off; }

static uint8_t vol_off(uint8_t mode_vol) { return (uint8_t)(mode_vol & ~SID_MODE_VOL); }   /* the mode, silent */

/* a write to a chip. While paused the volume stays at 0 on the emulated chips (eng_regs keeps
 * the tune's value), and nothing goes to the ASID receiver: the resume sends the whole image */
static void sid_apply(int chip, uint8_t reg, uint8_t val)
{
    if (chip == OUT_FRAME) { if (asid_on() && eng_playing) asid_frame(&eng_asid); return; }   /* the frame changed */
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
    c64_load(&c64, &eng_file_info, song ? song : eng_file_info.start_song, ntsc);
    if (eng_output && eng_deck_off) asid_stop(&eng_asid);   /* an RSID: the receiver falls silent */
    else if (eng_output) {                  /* the receiver starts from a clean image */
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
            gain_to = GAIN_ONE;             /* from silence */
        }
        break;
    case EV_STOP:                           /* fades out, then stop_now() */
        if (eng_playing && after_fade == FADE_NONE) { after_fade = FADE_STOP; gain_to = 0; }
        break;
    case EV_USB4:                           /* USB audio's format changed: the cap with it */
        eng_usb4 = e->a ? 1 : 0;
        deck_clamp();
        break;
    case EV_OUTPUT: {
        int old = asid_on(), now;
        eng_output = e->a > 2 ? 0 : e->a;
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
        if (eng_tempo != 100 && !eng_deck_off) {   /* the C64's cycles onto the chips' time: at 200 % a
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
    while (ev_r != ev_w) {
        eng_apply(&evq[ev_r % EVQ]);
        ev_r++;
        if (load_req) { for (k = 0; k < 2 * n; k++) lr[k] = 0; return; }
    }
    if (!eng_playing && gain == 0 && after_fade == FADE_NONE) {   /* paused: the chips idle */
        for (k = 0; k < 2 * n; k++) lr[k] = 0;
        return;
    }
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
