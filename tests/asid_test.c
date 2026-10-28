/* SPDX-License-Identifier: GPL-3.0-only */
/* The ASID sink against the protocol: the message layout (masks, MSBs, data in ASID order),
 * a full frame after a reset, nothing for an unchanged register or an empty frame, the second
 * control slot for a gate off and on, an early flush on a third control value, the chip
 * commands 0x4E / 0x50 / 0x51, start / stop / SID type, and the USB-MIDI packets. */
#include "../firmware/src/app/asid.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint8_t last[64], msgs[16][64];
static int last_n, nmsg, msg_n[16];
static void emit(void *ctx, const uint8_t *m, int n)
{
    (void)ctx;
    memcpy(last, m, (size_t)n); last_n = n;
    if (nmsg < 16) { memcpy(msgs[nmsg], m, (size_t)n); msg_n[nmsg] = n; }
    nmsg++;
}
static void clear(void) { nmsg = 0; last_n = 0; }

/* decode a data message into regs[25] (the TherapSID / spec way); returns the chip or -1 */
static int decode(const uint8_t *m, int n, uint8_t *regs, uint8_t *written)
{
    static const uint8_t MAP[28] = {0x00, 0x01, 0x02, 0x03, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0c, 0x0d, 0x0e, 0x0f,
                                    0x10, 0x11, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x04, 0x0b, 0x12, 0x04, 0x0b, 0x12};
    int id = 0, di = 11, mb, bit, chip;
    if (n < 12 || m[0] != 0xf0 || m[1] != 0x2d || m[n - 1] != 0xf7) return -1;
    chip = m[2] == 0x4e ? 0 : m[2] == 0x50 ? 1 : m[2] == 0x51 ? 2 : -1;
    if (chip < 0) return -1;
    for (mb = 0; mb < 4; mb++)
        for (bit = 0; bit < 7; bit++, id++)
            if (m[3 + mb] & (1 << bit)) {
                uint8_t v = m[di++];
                if (m[7 + mb] & (1 << bit)) v |= 0x80;
                if (di > n - 1) return -1;
                regs[MAP[id]] = v;
                written[MAP[id]]++;
            }
    return di == n - 1 ? chip : -1;
}

int main(void)
{
    asid_t a;
    uint8_t regs[25], wr[25];
    uint32_t pk[16];
    int i, n;
    asid_init(&a, emit, 0);
    /* start / stop / type */
    asid_start(&a); CHECK(last_n == 4 && last[0] == 0xf0 && last[1] == 0x2d && last[2] == 0x4c && last[3] == 0xf7);
    asid_stop(&a); CHECK(last_n == 4 && last[2] == 0x4d);
    asid_sid_type(&a, 1, 1); CHECK(last_n == 6 && last[2] == 0x32 && last[3] == 1 && last[4] == 1 && last[5] == 0xf7);
    /* after a reset the first frame carries all 25 registers, the tune's writes merged in */
    clear();
    asid_reset(&a, 1);
    asid_write(&a, 0, 0x01, 0x95); asid_write(&a, 0, 0x04, 0x41); asid_write(&a, 0, 0x18, 0x0f);
    asid_frame(&a);
    CHECK(nmsg == 1 && last[2] == 0x4e);
    CHECK(last_n == 3 + 8 + 25 + 1);
    CHECK(last[3] == 0x7f && last[4] == 0x7f && last[5] == 0x7f && last[6] == 0x0f);   /* ids 0 .. 24 */
    CHECK(last[7] == 0x02 && last[8] == 0 && last[9] == 0 && last[10] == 0);           /* only reg 1 has its MSB set */
    memset(regs, 0xee, 25); memset(wr, 0, 25);
    CHECK(decode(last, last_n, regs, wr) == 0);
    CHECK(regs[1] == 0x95 && regs[4] == 0x41 && regs[0x18] == 0x0f && regs[0] == 0 && regs[0x17] == 0);
    for (i = 0; i < 25; i++) CHECK(wr[i] == 1);
    /* every register, each with its own value, comes back on its own register */
    clear();
    for (i = 0; i < 25; i++) asid_write(&a, 0, (uint8_t)i, (uint8_t)(0x80 + i * 5));
    asid_frame(&a);
    memset(regs, 0xee, 25); memset(wr, 0, 25);
    CHECK(nmsg == 1 && decode(last, last_n, regs, wr) == 0);
    for (i = 0; i < 25; i++) CHECK(regs[i] == (uint8_t)(0x80 + i * 5) && wr[i] == 1);
    for (i = 0; i < 25; i++) asid_write(&a, 0, (uint8_t)i, (uint8_t)i);
    asid_frame(&a);
    for (i = 0; i < 25; i++) asid_write(&a, 0, (uint8_t)i, 0);
    asid_frame(&a);
    clear();
    asid_write(&a, 0, 0x01, 0x95); asid_write(&a, 0, 0x04, 0x41); asid_write(&a, 0, 0x18, 0x0f);
    asid_frame(&a);
    /* an empty frame sends nothing, and so does a register written with what the receiver has */
    clear();
    asid_frame(&a); CHECK(nmsg == 0);
    asid_write(&a, 0, 0x01, 0x95); asid_write(&a, 0, 0x04, 0x41);
    asid_frame(&a); CHECK(nmsg == 0);
    /* a frame with two registers: ids in ASID order, voice 2's frequency hi is id 7 (mask byte 1, bit 0) */
    asid_write(&a, 0, 0x08, 0x12); asid_write(&a, 0, 0x00, 0x80);
    asid_frame(&a);
    CHECK(nmsg == 1 && last_n == 3 + 8 + 2 + 1);
    CHECK(last[3] == 0x01 && last[4] == 0x01 && last[5] == 0 && last[6] == 0);
    CHECK(last[7] == 0x01 && last[8] == 0);
    CHECK(last[11] == 0x00 && last[12] == 0x12);
    /* a gate off and on within the frame: the control register twice, slots 22 and 25 */
    clear();
    asid_write(&a, 0, 0x04, 0x40); asid_write(&a, 0, 0x04, 0x40); asid_write(&a, 0, 0x04, 0x41);
    asid_frame(&a);
    CHECK(nmsg == 1 && last_n == 3 + 8 + 2 + 1);
    CHECK(last[6] == 0x12 && last[11] == 0x40 && last[12] == 0x41);     /* ids 22 and 25: bits 1 and 4 of mask byte 3 */
    memset(wr, 0, 25); decode(last, last_n, regs, wr); CHECK(wr[4] == 2 && regs[4] == 0x41);
    /* a third value flushes the frame early: two messages, the gate sequence intact */
    clear();
    asid_write(&a, 0, 0x04, 0x40); asid_write(&a, 0, 0x04, 0x41); asid_write(&a, 0, 0x04, 0x40);
    CHECK(nmsg == 1 && last[11] == 0x40 && last[12] == 0x41);
    asid_frame(&a);
    CHECK(nmsg == 2 && last_n == 3 + 8 + 1 + 1 && last[6] == 0x02 && last[11] == 0x40);
    /* the last write of a frame wins for the other registers */
    clear();
    asid_write(&a, 0, 0x16, 0x10); asid_write(&a, 0, 0x16, 0x20);
    asid_frame(&a);
    CHECK(nmsg == 1 && last[11] == 0x20);
    /* three chips: 0x4E, 0x50, 0x51; writes to a chip beyond nchip are dropped */
    clear();
    asid_reset(&a, 3);
    asid_write(&a, 2, 0x18, 0x1f);
    asid_frame(&a);
    CHECK(nmsg == 3 && msgs[0][2] == 0x4e && msgs[1][2] == 0x50 && msgs[2][2] == 0x51);
    memset(wr, 0, 25); CHECK(decode(msgs[2], msg_n[2], regs, wr) == 2 && regs[0x18] == 0x1f);
    clear();
    asid_reset(&a, 1);
    asid_frame(&a);
    asid_write(&a, 1, 0x18, 0x1f); asid_write(&a, 2, 0x18, 0x1f);
    asid_frame(&a); CHECK(nmsg == 1);
    /* USB-MIDI packets: 4 = continues, 5 / 6 / 7 end with 1 / 2 / 3 bytes */
    n = asid_usb_packets((const uint8_t[]){0xf0, 0x2d, 0x4c, 0xf7}, 4, pk, 16);
    CHECK(n == 2 && pk[0] == (0x04u | 0xf0u << 8 | 0x2du << 16 | 0x4cu << 24) && pk[1] == (0x05u | 0xf7u << 8));
    n = asid_usb_packets((const uint8_t[]){0xf0, 0x2d, 0x32, 0, 1, 0xf7}, 6, pk, 16);
    CHECK(n == 2 && (pk[1] & 0xff) == 7 && pk[1] == (0x07u | 0x00u << 8 | 0x01u << 16 | 0xf7u << 24));
    n = asid_usb_packets((const uint8_t[]){0xf0, 0x2d, 0x4c, 0x00, 0xf7}, 5, pk, 16);
    CHECK(n == 2 && (pk[1] & 0xff) == 6);
    CHECK(asid_usb_packets(last, 40, pk, 13) == 0);
    printf(fails ? "asid_test FAIL\n" : "asid_test PASS\n");
    return fails != 0;
}
