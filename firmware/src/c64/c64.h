/* SPDX-License-Identifier: GPL-3.0-only */
/* A C64 for SID tunes: a 6510 with interrupts, 64 KB of RAM, memory banking through $01, CIA 1
 * and CIA 2 (timers A and B, their interrupts), the VIC's raster counter, raster interrupt and
 * bad-line stalls,
 * one to three SIDs, and a small KERNAL replacement written for this project (the interrupt
 * entries, the default handlers, the vectors; every other ROM byte reads as RTS). A tune is
 * started by a driver placed in free RAM: it sets the machine up as the KERNAL would, calls
 * init, and for PSID files calls play from the CIA or raster interrupt; RSID files run on their
 * own. The CPU runs continuously in cycle-counted chunks; every SID write is logged with its
 * cycle so that an output can place it exactly. No hardware here: the device, the simulator and
 * the tests all use it. */
#pragma once
#include <stdint.h>
#include "cpu6502.h"
#include "sidfile.h"

#define C64_LOG_MAX 512                     /* SID writes remembered per run */
#define C64_PAL_HZ 985248u
#define C64_NTSC_HZ 1022727u
#define C64_NSID 3

typedef struct { uint16_t cycle; uint8_t chip, reg, val; } c64_write_t;   /* cycle: from the run's start */

typedef struct {
    uint16_t ta, tb, ta_latch, tb_latch;
    uint8_t cra, crb, icr_mask, icr_data;
    uint8_t pra, prb, ddra, ddrb;
    uint8_t sdr;                            /* the serial data register (a tune may keep code there: RTI) */
    /* the time-of-day clock (BCD tenths, seconds, minutes, hours with the PM bit), stepped by the
     * 50 / 60 Hz pin; a read of the hours latches until the tenths, a write of the hours stops it
     * until the tenths; with CRB bit 7 the writes go to the alarm */
    uint8_t tod[4], tod_latch[4], tod_alarm[4], tod_latched, tod_stopped, tod_pins;
    uint32_t tod_acc, tod_period;           /* cycles since the last pin pulse; cycles a pulse */
    uint8_t irq;                           /* the chip's interrupt output */
} c64_cia_t;

typedef struct c64 {
    cpu6502_t cpu;
    uint8_t *mem;                           /* 64 KB, the caller's */
    uint32_t clock_hz;
    uint16_t cycles_per_line, lines;
    uint8_t ntsc;
    /* VIC: the raster counter and its interrupt; the other registers are plain storage */
    uint16_t raster, raster_cmp, line_acc;
    uint8_t vic_en, vic_flag, den, vic_reg[0x40];
    uint32_t frames;                        /* frames since the tune started */
    c64_cia_t cia[2];
    /* SIDs: chip 0 at $D400 (mirrored through $D7FF), 1 and 2 where the file puts them */
    int nsid;
    uint16_t sid_addr[C64_NSID];
    uint8_t (*sid_read)(void *ctx, int chip, uint8_t reg);   /* $D41B / $D41C come from the output */
    void *sid_ctx;
    c64_write_t log[C64_LOG_MAX];
    int nlog;
    uint32_t log_lost, run_start;
    uint32_t frame_cycle;                   /* the cycle (from the run's start) the raster wrapped at, else -1 */
    uint32_t acc, next;                     /* cycles not yet applied to the chips; cycles until the next event */
    uint32_t ahead;                         /* cycles of the running instruction the chips already have (an I/O access in it) */
    /* the tune */
    uint16_t driver;                        /* where the driver went */
    uint8_t song, cia_speed, has_play, bank, bank_play;   /* $01 for init and for play */
    int fault;                              /* -1: the CPU jammed (KIL) */
} c64_t;

void c64_init(c64_t *c, uint8_t *mem);
/* load a parsed file and start song (1-based) on a PAL or NTSC machine */
void c64_load(c64_t *c, const sidfile_t *f, int song, int ntsc);
/* run at least `cycles` cycles (the last instruction may overrun); the SID writes land in log[]
 * with their cycle from the run's start, and frame_cycle says where the frame changed (a run
 * is far shorter than a frame, so at most once); returns the cycles run, 0 after a fault */
uint32_t c64_run(c64_t *c, uint32_t cycles);
uint32_t c64_frame_cycles(const c64_t *c);
/* the current value of a SID register as the tune last wrote it is not kept here: the
 * output keeps the register image (remix.c) */
