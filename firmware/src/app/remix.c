/* SPDX-License-Identifier: GPL-3.0-only */
/* The remix, see remix.h. Integer only. */
#include "remix.h"

/* 2^(n/12) in Q12, n = 0 .. 11 */
static const uint16_t SEMI_Q12[12] = {4096, 4340, 4598, 4871, 5161, 5468, 5793, 6137, 6502, 6889, 7298, 7732};
/* 2^(c/1200) in Q15, c = -50 .. 50 cents: the fine offset */
static const uint16_t CENT_Q15[2 * 50 + 1] = {
    31835, 31854, 31872, 31890, 31909, 31927, 31946, 31964, 31983, 32001,
    32020, 32038, 32057, 32075, 32094, 32112, 32131, 32149, 32168, 32186,
    32205, 32224, 32242, 32261, 32280, 32298, 32317, 32336, 32354, 32373,
    32392, 32410, 32429, 32448, 32467, 32485, 32504, 32523, 32542, 32560,
    32579, 32598, 32617, 32636, 32655, 32673, 32692, 32711, 32730, 32749,
    32768, 32787, 32806, 32825, 32844, 32863, 32882, 32901, 32920, 32939,
    32958, 32977, 32996, 33015, 33034, 33053, 33072, 33091, 33110, 33130,
    33149, 33168, 33187, 33206, 33225, 33245, 33264, 33283, 33302, 33322,
    33341, 33360, 33379, 33399, 33418, 33437, 33457, 33476, 33495, 33515,
    33534, 33553, 33573, 33592, 33611, 33631, 33650, 33670, 33689, 33709,
    33728
};
static const uint32_t LFO_INC[RX_LFO_NRATE] = {   /* f * 2^32 / 44100, f = 0.05 * 400^(i/63) Hz */
    4870u, 5355u, 5890u, 6477u, 7124u, 7834u, 8616u, 9476u,
    10421u, 11461u, 12604u, 13862u, 15245u, 16766u, 18439u, 20278u,
    22301u, 24526u, 26973u, 29665u, 32624u, 35879u, 39459u, 43396u,
    47726u, 52487u, 57724u, 63483u, 69817u, 76783u, 84444u, 92869u,
    102135u, 112325u, 123531u, 135856u, 149411u, 164318u, 180712u, 198742u,
    218571u, 240378u, 264361u, 290737u, 319744u, 351646u, 386730u, 425315u,
    467749u, 514417u, 565742u, 622187u, 684263u, 752534u, 827615u, 910188u,
    1000999u, 1100870u, 1210706u, 1331500u, 1464346u, 1610446u, 1771123u, 1947831u
};
const uint16_t RX_LFO_CHZ[RX_LFO_NRATE] = {      /* the same rates in 0.01 Hz, two digits */
    5u, 5u, 6u, 7u, 7u, 8u, 9u, 10u, 11u, 12u, 13u, 14u,
    16u, 17u, 19u, 21u, 23u, 25u, 28u, 30u, 33u, 37u, 41u, 45u,
    49u, 54u, 59u, 65u, 72u, 79u, 87u, 95u, 100u, 120u, 130u, 140u,
    150u, 170u, 190u, 200u, 220u, 250u, 270u, 300u, 330u, 360u, 400u, 440u,
    480u, 530u, 580u, 640u, 700u, 770u, 850u, 930u, 1000u, 1100u, 1200u, 1400u,
    1500u, 1700u, 1800u, 2000u
};
static const uint16_t QSIN[65] = {               /* sin(pi/2 * i/64) in Q15 */
    0, 804, 1608, 2410, 3212, 4011, 4808, 5602, 6393, 7179, 7962, 8739, 9512,
    10278, 11039, 11793, 12539, 13279, 14010, 14732, 15446, 16151, 16846, 17530, 18204, 18868,
    19519, 20159, 20787, 21403, 22005, 22594, 23170, 23731, 24279, 24811, 25329, 25832, 26319,
    26790, 27245, 27683, 28105, 28510, 28898, 29268, 29621, 29956, 30273, 30571, 30852, 31113,
    31356, 31580, 31785, 31971, 32137, 32285, 32412, 32521, 32609, 32678, 32728, 32757, 32767
};

/* the C64's frequency of C-0 (16.35 Hz) in Q8 register units: f * 2^24 / clock */
#define C0_PAL_Q8 71267u                    /* 16.3516 * 16777216 / 985248 = 278.39 */
#define C0_NTSC_Q8 68656u                   /* 16.3516 * 16777216 / 1022727 = 268.19 */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* whether every register derives as the tune wrote it (no override, offset or LFO offset in
 * effect): then emit takes the tune's value without deriving (a sample player writes the chip
 * tens of thousands of times a second). Everything that changes the derivation ends in
 * remix_refresh, remix_reset or remix_lfo_tick, which call this. */
static void update_neutral(remix_t *rx)
{
    int v, n = rx->res_off == 0 && !rx->fmode_ovr && rx->lfo_cut == 0 &&
               (rx->cutoff_mode == RX_CUT_OFFSET ? rx->cutoff_off == 0 : rx->cutoff_scale == RX_CUTOFF_ONE);
    for (v = 0; v < RX_NV && n; v++) {
        const rx_voice_t *x = &rx->v[v];
        n = !x->wave_ovr && !x->sr_ovr && !x->mute && !x->pw_ovr && !x->adsr_ovr && !x->oct && !x->fine &&
            !x->lfo_pw && !x->lfo_cents && !x->key_on && !x->shift_on && !x->filt_ovr;
    }
    rx->neutral = (uint8_t)n;
}

void remix_init(remix_t *rx)
{
    uint32_t i;
    for (i = 0; i < sizeof *rx; i++) ((uint8_t *)rx)[i] = 0;
    rx->cutoff_scale = RX_CUTOFF_ONE;
    update_neutral(rx);
}

void remix_reset(remix_t *rx)
{
    int v;
    for (v = 0; v < RX_NV; v++) {
        rx_voice_t *x = &rx->v[v];
        x->wave_ovr = 0; x->sr_ovr = 0; x->mute = 0; x->pw_ovr = 0; x->adsr_ovr = 0;
        x->oct = 0; x->fine = 0;
        x->lfo_target = RX_LFO_OFF; x->lfo_shape = RX_LFO_SIN; x->lfo_rate = 0; x->lfo_depth = 0;
        x->lfo_pw = 0; x->lfo_cents = 0; x->key_on = 0; x->shift_on = 0; x->filt_ovr = 0;
    }
    rx->lfo_cut = 0;
    rx->cutoff_scale = RX_CUTOFF_ONE;
    rx->cutoff_off = 0;
    rx->res_off = 0;
    rx->fmode_ovr = 0;
    update_neutral(rx);
}

uint16_t remix_note_freq(int midi_note, int ntsc)
{
    int n = clampi(midi_note, 12, 12 + 8 * 12 - 1) - 12, oct = n / 12, semi = n % 12;
    uint32_t f = ((uint32_t)(ntsc ? C0_NTSC_Q8 : C0_PAL_Q8) * SEMI_Q12[semi]) >> 12;   /* Q8 */
    f = (f << oct) >> 8;
    return f > 0xffff ? 0xffff : (uint16_t)f;
}

uint16_t remix_tune_freq(const remix_t *rx, int v) { return sid_freq(rx->tune, v); }
uint16_t remix_tune_pw(const remix_t *rx, int v) { return sid_pw(rx->tune, v); }
uint16_t remix_tune_cutoff(const remix_t *rx) { return sid_fc(rx->tune); }
int remix_voice_wave(const remix_t *rx, int v) { return rx->out[SID_ADDR_VOICE(v) + SID_ADDR_CTRL] & RX_WAVE_MASK; }

static uint16_t voice_freq(const remix_t *rx, int v)
{
    const rx_voice_t *x = &rx->v[v];
    uint32_t f = remix_tune_freq(rx, v);
    int k, oct, semi, c, cs;
    if ((rx->tune[SID_ADDR_VOICE(v) + SID_ADDR_CTRL] & SID_CTRL_WAVES) == SID_CTRL_NOISE) return (uint16_t)f;   /* pure noise keeps its pitch */
    if (x->key_on) return x->key_freq;
    c = x->fine + x->lfo_cents;             /* cents: whole semitones, then -50 .. 50 */
    cs = c >= 0 ? (c + 49) / 100 : -((-c + 49) / 100);
    c -= cs * 100;
    if (c) f = (f * CENT_Q15[clampi(c, -50, 50) + 50]) >> 15;   /* up to 0x10770: fits */
    k = x->oct * 12 + cs + (x->shift_on ? x->shift_semi : 0);
    if (!k) return f > 0xffff ? 0xffff : (uint16_t)f;
    oct = k >= 0 ? k / 12 : -((-k + 11) / 12);
    semi = k - oct * 12;
    f = (f * SEMI_Q12[semi]) >> 12;
    if (oct > 0) f <<= oct; else f >>= -oct;
    return f > 0xffff ? 0xffff : (uint16_t)f;
}

static uint16_t voice_pw(const remix_t *rx, int v)
{
    int pw = rx->v[v].pw_ovr ? (int)(rx->v[v].pw_val & SID_PW_MAX) : (int)remix_tune_pw(rx, v);
    return (uint16_t)clampi(pw + rx->v[v].lfo_pw, 0, SID_PW_MAX);
}

static uint16_t filt_cutoff(const remix_t *rx)  /* scaled (never below 0) or offset, clipped */
{
    uint32_t c;
    if (rx->cutoff_mode == RX_CUT_OFFSET) return (uint16_t)clampi((int)remix_tune_cutoff(rx) + rx->cutoff_off + rx->lfo_cut, 0, SID_FC_MAX);
    c = ((uint32_t)remix_tune_cutoff(rx) * rx->cutoff_scale) / RX_CUTOFF_ONE;
    return (uint16_t)clampi((int)(c > SID_FC_MAX ? SID_FC_MAX : c) + rx->lfo_cut, 0, SID_FC_MAX);
}

uint8_t remix_derive(const remix_t *rx, uint8_t reg)
{
    uint8_t t = rx->tune[reg];
    if (reg < SID_ADDR_FC_LO) {
        int v = reg / SID_VOICE_REGS;
        const rx_voice_t *x = &rx->v[v];
        switch (reg % SID_VOICE_REGS) {
        case SID_ADDR_FREQ_LO: return (uint8_t)voice_freq(rx, v);
        case SID_ADDR_FREQ_HI: return (uint8_t)(voice_freq(rx, v) >> 8);
        case SID_ADDR_PW_LO: return (uint8_t)voice_pw(rx, v);
        case SID_ADDR_PW_HI: return (uint8_t)(voice_pw(rx, v) >> 8);
        case SID_ADDR_CTRL:
            if (x->wave_ovr && (t & RX_WAVE_TONAL))     /* the chosen waveform instead of the tune's (pure noise stays) */
                t = (uint8_t)((t & ~SID_CTRL_WAVES) | (x->wave_val & SID_CTRL_WAVES));
            if (x->sr_ovr)                              /* sync and ring, frozen */
                t = (uint8_t)((t & ~(SID_CTRL_SYNC | SID_CTRL_RING)) | (x->sr_val & (SID_CTRL_SYNC | SID_CTRL_RING)));
            if (x->mute) t &= (uint8_t)~SID_CTRL_WAVES;      /* no waveform: silence; gate, sync, ring kept */
            return t;
        case SID_ADDR_AD:
            return (uint8_t)(((x->adsr_ovr & RX_ADSR_A) ? x->a << 4 : t & 0xf0) | ((x->adsr_ovr & RX_ADSR_D) ? x->d : t & 0x0f));
        case SID_ADDR_SR:
            return (uint8_t)(((x->adsr_ovr & RX_ADSR_S) ? x->s << 4 : t & 0xf0) | ((x->adsr_ovr & RX_ADSR_R) ? x->r : t & 0x0f));
        default: return t;
        }
    }
    if (reg == SID_ADDR_FC_LO) return (uint8_t)(filt_cutoff(rx) & ((1 << SID_FC_LO_BITS) - 1));
    if (reg == SID_ADDR_FC_HI) return (uint8_t)(filt_cutoff(rx) >> SID_FC_LO_BITS);
    if (reg == SID_ADDR_RES_FILT) {
        int v, route = t & SID_FILT_VOICES, res = clampi((t >> SID_FILT_RES_SHIFT) + rx->res_off, 0, SID_NIBBLE_MAX);
        for (v = 0; v < RX_NV; v++)
            if (rx->v[v].filt_ovr) route = (route & ~(1 << v)) | ((rx->v[v].filt_val & 1) << v);
        return (uint8_t)((res << SID_FILT_RES_SHIFT) | (t & SID_FILT_EXT) | route);
    }
    if (reg == SID_ADDR_MODE_VOL && rx->fmode_ovr) return (uint8_t)((t & ~SID_MODE_FILTERS) | (rx->fmode_val & SID_MODE_FILTERS));
    return t;
}

static int emit(remix_t *rx, uint8_t reg, rx_out_t *o, int force)
{
    uint8_t v = rx->neutral ? rx->tune[reg] : remix_derive(rx, reg);   /* (neutral: the same, without deriving) */
    if (!force && rx->out_valid[reg] && rx->out[reg] == v) return 0;
    rx->out[reg] = v;
    rx->out_valid[reg] = 1;
    o->reg = reg;
    o->val = v;
    return 1;
}

/* a 16-bit field (frequency, pulse width, cutoff): both bytes as transformed; the byte the
 * tune wrote (written; -1 none) always goes, the other only if it changed */
static int emit_pair(remix_t *rx, uint8_t lo, rx_out_t *o, int written)
{
    int n = emit(rx, lo, o, written == lo);
    return n + emit(rx, (uint8_t)(lo + 1), o + n, written == lo + 1);
}

int remix_write(remix_t *rx, uint8_t reg, uint8_t val, rx_out_t *o)
{
    int n;
    if (reg >= SID_NWRITE) return 0;
    rx->tune[reg] = val;
    if (reg < SID_ADDR_FC_LO) {
        uint8_t voice = (uint8_t)(reg - reg % SID_VOICE_REGS);
        switch (reg % SID_VOICE_REGS) {
        case SID_ADDR_FREQ_LO: case SID_ADDR_FREQ_HI: return emit_pair(rx, (uint8_t)(voice + SID_ADDR_FREQ_LO), o, reg);
        case SID_ADDR_PW_LO: case SID_ADDR_PW_HI: return emit_pair(rx, (uint8_t)(voice + SID_ADDR_PW_LO), o, reg);
        case SID_ADDR_CTRL:                                  /* always passed (a gate is an event); the pitch may change
                                                         * hands between noise and the key or the offsets, so the
                                                         * frequency pair goes first */
            n = emit_pair(rx, (uint8_t)(voice + SID_ADDR_FREQ_LO), o, -1);
            return n + emit(rx, reg, o + n, 1);
        default: return emit(rx, reg, o, 1);
        }
    }
    if (reg == SID_ADDR_FC_LO || reg == SID_ADDR_FC_HI) return emit_pair(rx, SID_ADDR_FC_LO, o, reg);
    return emit(rx, reg, o, 1);
}

static int lfo_wave(rx_voice_t *x, uint32_t *seed, int wrapped)   /* -32767 .. 32767 */
{
    uint32_t p = x->lfo_phase >> 16;        /* 0 .. 65535 */
    int q, i, fr, s;
    switch (x->lfo_shape) {
    case RX_LFO_TRI: return p < 32768u ? (int)(p * 2u) - 32767 : 32767 - (int)((p - 32768u) * 2u);
    case RX_LFO_SAW: return (int)p - 32768 + (p == 0);
    case RX_LFO_SQR: return p < 32768u ? 32767 : -32767;
    case RX_LFO_RND:
        if (wrapped) { *seed = *seed * 1664525u + 1013904223u; x->lfo_val = (int16_t)((int)(*seed >> 16) - 32768 + ((*seed >> 16) == 0)); }
        return x->lfo_val;
    default:                                /* the sine from a quarter, interpolated */
        q = (int)(p >> 14); i = (int)((p >> 8) & 63); fr = (int)(p & 255);
        if (q & 1) { i = 63 - i; fr = 255 - fr; }
        s = QSIN[i] + (((QSIN[i + 1] - QSIN[i]) * fr) >> 8);
        return q & 2 ? -s : s;
    }
}

int remix_lfo_tick(remix_t *rx, uint32_t n, rx_out_t *o, int max)
{
    int v, m = 0, cut = 0, moved_cut = 0, moved = 0;
    for (v = 0; v < RX_NV; v++) {
        rx_voice_t *x = &rx->v[v];
        int16_t pw = 0, cents = 0;
        if (x->lfo_target != RX_LFO_OFF && x->lfo_depth) {
            uint32_t before = x->lfo_phase;
            int w, d = x->lfo_depth;
            x->lfo_phase += LFO_INC[x->lfo_rate < RX_LFO_NRATE ? x->lfo_rate : RX_LFO_NRATE - 1] * n;
            w = lfo_wave(x, &rx->lfo_seed, x->lfo_phase < before);
            if (x->lfo_target == RX_LFO_PW) pw = (int16_t)(((w * d / RX_LFO_DEPTH_MAX) * 2048) >> 15);
            else if (x->lfo_target == RX_LFO_PITCH) cents = (int16_t)((w * (d * d * 12 / RX_LFO_DEPTH_MAX)) >> 15);
            else cut += ((w * d / RX_LFO_DEPTH_MAX) * SID_FC_MAX) >> 15;
        }
        if (pw != x->lfo_pw) {
            x->lfo_pw = pw;
            moved = 1;                      /* (a moved offset: the neutrality is looked at again) */
            if (m + 2 <= max) m += emit_pair(rx, (uint8_t)(SID_ADDR_VOICE(v) + SID_ADDR_PW_LO), o + m, -1);
        }
        if (cents != x->lfo_cents) {
            x->lfo_cents = cents;
            moved = 1;
            if (m + 2 <= max) m += emit_pair(rx, (uint8_t)(SID_ADDR_VOICE(v) + SID_ADDR_FREQ_LO), o + m, -1);
        }
    }
    cut = clampi(cut, -SID_FC_MAX, SID_FC_MAX);
    if (cut != rx->lfo_cut) { rx->lfo_cut = (int16_t)cut; moved_cut = 1; }
    if (moved_cut && m + 2 <= max) m += emit_pair(rx, SID_ADDR_FC_LO, o + m, -1);
    if (moved || moved_cut) update_neutral(rx);
    return m;
}

int remix_refresh(remix_t *rx, int v, rx_out_t *o, int max)
{
    int n = 0, i, lo, hi;
    if (v < 0) { lo = 0; hi = SID_NWRITE; }
    else { lo = SID_ADDR_VOICE(v); hi = SID_ADDR_VOICE(v) + SID_VOICE_REGS; }
    update_neutral(rx);
    for (i = lo; i < hi && n < max; i++)
        n += emit(rx, (uint8_t)i, o + n, 0);
    return n;
}
