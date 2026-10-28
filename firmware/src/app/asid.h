/* SPDX-License-Identifier: GPL-3.0-only */
/* The ASID sink: the SID writes of a frame packed into one SysEx per chip, after the ASID
 * protocol (Elektron's, extended: github.com/thomasj/asid-protocol). F0 2D <cmd> ... F7, the
 * data messages 0x4E (SID 1), 0x50 (SID 2), 0x51 (SID 3): four mask bytes, four MSB bytes,
 * then the low seven bits of each masked register in ASID order. The order carries the
 * frequencies, pulse widths and envelopes first, then the filter, then the three control
 * registers, and a second slot for each control register, so a gate can go off and on again
 * within one frame. A third distinct control value flushes the message early, so a tune that
 * retriggers faster than the frame loses nothing. A register that would be written with what
 * the receiver already holds is left out; an empty frame sends nothing. Pure: the caller
 * supplies the frame clock and the transport. */
#pragma once
#include <stdint.h>
#include "../c64/sidreg.h"

#define ASID_NCHIP 3
#define ASID_NSLOT 28
#define ASID_NREG SID_NWRITE                /* SID registers 0 .. 0x18 */
#define ASID_MSG_MAX 40                     /* F0 2D cmd mask[4] msb[4] data[28] F7 */

typedef void (*asid_emit_t)(void *ctx, const uint8_t *msg, int n);   /* a complete F0 .. F7 */

typedef struct {
    uint8_t img[ASID_NREG];                 /* the receiver's registers, as sent */
    uint8_t known[ASID_NREG];               /* img[] has been sent at least once */
    uint8_t slot[ASID_NSLOT];               /* this frame's values, by ASID register id */
    uint8_t set[ASID_NSLOT];
} asid_chip_t;

typedef struct {
    asid_chip_t c[ASID_NCHIP];
    int nchip;
    asid_emit_t emit;
    void *ctx;
    uint32_t msgs;                          /* data messages sent */
} asid_t;

void asid_init(asid_t *a, asid_emit_t emit, void *ctx);
/* the receiver's image is unknown: the next frame of each of the nchip chips carries every
 * register, zero unless the tune writes it first */
void asid_reset(asid_t *a, int nchip);
void asid_write(asid_t *a, int chip, uint8_t reg, uint8_t val);   /* a write, in the tune's order */
void asid_frame(asid_t *a);                 /* the frame ended: send what changed */
void asid_start(asid_t *a);                 /* 0x4C */
void asid_stop(asid_t *a);                  /* 0x4D */
void asid_sid_type(asid_t *a, int chip, int is_8580);   /* 0x32 */
/* a message as USB-MIDI event packets (cable 0); returns the count, 0 if max is too small */
int asid_usb_packets(const uint8_t *msg, int n, uint32_t *pkt, int max);
