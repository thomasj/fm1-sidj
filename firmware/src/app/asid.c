/* SPDX-License-Identifier: GPL-3.0-only */
/* The ASID sink, see asid.h. */
#include "asid.h"

/* the SID register of each ASID register id: the protocol's order (SID_* offsets; the three
 * control registers, 0x04 0x0b 0x12, twice) */
static const uint8_t ASID_REG[ASID_NSLOT] = {
    0x00, 0x01, 0x02, 0x03, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0c, 0x0d, 0x0e, 0x0f,
    0x10, 0x11, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x04, 0x0b, 0x12, 0x04, 0x0b, 0x12};
/* the first ASID register id of each SID register */
static const uint8_t SLOT_OF[ASID_NREG] = {
    0, 1, 2, 3, 22, 4, 5, 6, 7, 8, 9, 23, 10, 11, 12, 13, 14, 15, 24, 16, 17, 18, 19, 20, 21};
static const uint8_t CMD_OF[ASID_NCHIP] = {0x4e, 0x50, 0x51};

void asid_init(asid_t *a, asid_emit_t emit, void *ctx)
{
    uint32_t i;
    for (i = 0; i < sizeof *a; i++) ((uint8_t *)a)[i] = 0;
    a->emit = emit;
    a->ctx = ctx;
}

void asid_reset(asid_t *a, int nchip)
{
    int c, i;
    a->nchip = nchip < 0 ? 0 : nchip > ASID_NCHIP ? ASID_NCHIP : nchip;
    for (c = 0; c < ASID_NCHIP; c++) {
        asid_chip_t *x = &a->c[c];
        for (i = 0; i < ASID_NREG; i++) { x->known[i] = 0; x->img[i] = 0; }
        for (i = 0; i < ASID_NSLOT; i++) { x->slot[i] = 0; x->set[i] = 0; }
    }
}

static int is_control(uint8_t reg) { return reg < SID_ADDR_FC_LO && reg % SID_VOICE_REGS == SID_ADDR_CTRL; }

static void emit(asid_t *a, const uint8_t *m, int n) { if (a->emit) a->emit(a->ctx, m, n); }

static void flush_chip(asid_t *a, int c)
{
    asid_chip_t *x = &a->c[c];
    uint8_t m[ASID_MSG_MAX];
    int s, n = 11, any = 0;
    m[0] = 0xf0; m[1] = 0x2d; m[2] = CMD_OF[c];
    for (s = 3; s < 11; s++) m[s] = 0;
    for (s = 0; s < ASID_NSLOT; s++) {
        uint8_t reg = ASID_REG[s], v = x->slot[s];
        if (!x->set[s]) {
            if (s >= ASID_NREG || x->known[reg]) continue;
            v = 0;                                      /* never sent: the receiver gets a zero */
        }
        x->set[s] = 0;
        if (s < ASID_NREG && x->known[reg] && x->img[reg] == v) continue;   /* the receiver has it */
        m[3 + s / 7] |= (uint8_t)(1u << (s % 7));
        if (v & 0x80) m[7 + s / 7] |= (uint8_t)(1u << (s % 7));
        m[n++] = (uint8_t)(v & 0x7f);
        x->img[reg] = v;
        x->known[reg] = 1;
        any = 1;
    }
    if (!any) return;
    m[n++] = 0xf7;
    emit(a, m, n);
    a->msgs++;
}

void asid_write(asid_t *a, int chip, uint8_t reg, uint8_t val)
{
    asid_chip_t *x;
    if (chip < 0 || chip >= a->nchip || reg >= ASID_NREG) return;
    x = &a->c[chip];
    if (is_control(reg)) {
        int s1 = 22 + reg / 7, s2 = s1 + 3;
        if (!x->set[s1]) {
            if (x->known[reg] && x->img[reg] == val) return;        /* a no-op on the chip */
            x->slot[s1] = val; x->set[s1] = 1;
        } else if (!x->set[s2]) {
            if (x->slot[s1] == val) return;
            x->slot[s2] = val; x->set[s2] = 1;
        } else if (x->slot[s2] != val) {                            /* a third value: this frame goes now */
            flush_chip(a, chip);
            x->slot[s1] = val; x->set[s1] = 1;
        }
        return;
    }
    x->slot[SLOT_OF[reg]] = val;
    x->set[SLOT_OF[reg]] = 1;
}

void asid_frame(asid_t *a)
{
    int c;
    for (c = 0; c < a->nchip; c++) flush_chip(a, c);
}

void asid_start(asid_t *a) { static const uint8_t m[4] = {0xf0, 0x2d, 0x4c, 0xf7}; emit(a, m, 4); }
void asid_stop(asid_t *a) { static const uint8_t m[4] = {0xf0, 0x2d, 0x4d, 0xf7}; emit(a, m, 4); }
void asid_sid_type(asid_t *a, int chip, int is_8580)
{
    uint8_t m[6] = {0xf0, 0x2d, 0x32, 0, 0, 0xf7};
    if (chip < 0 || chip >= ASID_NCHIP) return;
    m[3] = (uint8_t)chip;
    m[4] = (uint8_t)(is_8580 ? 1 : 0);
    emit(a, m, 6);
}

int asid_usb_packets(const uint8_t *msg, int n, uint32_t *pkt, int max)
{
    int i = 0, k = 0;
    if ((n + 2) / 3 > max) return 0;
    while (i < n) {
        int left = n - i, take = left >= 3 ? 3 : left;
        uint32_t cin = take == 3 && left > 3 ? 4u : take == 3 ? 7u : 4u + (uint32_t)take;   /* 4 continues; 5 6 7 end */
        pkt[k++] = cin | (uint32_t)msg[i] << 8 | (take > 1 ? (uint32_t)msg[i + 1] << 16 : 0u) |
                   (take > 2 ? (uint32_t)msg[i + 2] << 24 : 0u);
        i += take;
    }
    return k;
}
