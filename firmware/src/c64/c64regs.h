/* SPDX-License-Identifier: GPL-3.0-only */
/* The C64's addresses and the registers of its I/O chips, as the emulation uses them (the
 * SID's are in sidreg.h).
 *   C64_ADDR_*   an address in the C64's memory map
 *   C64_PORT_*   the bits of the processor port ($01): which ROMs and I/O are banked in
 *   VIC_ADDR_*   a VIC-II register, as an offset from $D000 (repeating every $40)
 *   VIC_CTRL1_*, VIC_IRQ_*   the bits of its control and interrupt registers
 *   CIA_ADDR_*   a CIA register, as an offset from the chip's base (repeating every $10)
 *   CIA_CR_*, CIA_ICR_*      the bits of its timer control and interrupt control registers */
#pragma once

enum {
    C64_ADDR_DDR = 0x0000,                  /* the processor port's data directions */
    C64_ADDR_PORT = 0x0001,                 /* the processor port: the banking */
    C64_ADDR_IRQ_VEC = 0x0314,              /* the KERNAL's RAM vectors: IRQ, BRK, NMI */
    C64_ADDR_BRK_VEC = 0x0316,
    C64_ADDR_NMI_VEC = 0x0318,
    C64_ADDR_BASIC = 0xa000,                /* the BASIC ROM, $A000 .. $BFFF */
    C64_ADDR_BASIC_END = 0xc000,
    C64_ADDR_IO = 0xd000,                   /* the I/O area, $D000 .. $DFFF: */
    C64_ADDR_VIC = 0xd000,                  /*   the VIC-II */
    C64_ADDR_SID = 0xd400,                  /*   the SID (and room for more, to $D7FF) */
    C64_ADDR_COLOR = 0xd800,                /*   colour RAM, 4 bits */
    C64_ADDR_CIA1 = 0xdc00,                 /*   CIA 1: the IRQ */
    C64_ADDR_CIA2 = 0xdd00,                 /*   CIA 2: the NMI */
    C64_ADDR_IO_EXP = 0xde00,               /*   the expansion port's I/O, open here */
    C64_ADDR_KERNAL = 0xe000,               /* the KERNAL ROM, $E000 .. $FFFF */
    C64_ADDR_NMI = 0xfffa,                  /* the 6510's vectors: NMI, RESET, IRQ */
    C64_ADDR_RESET = 0xfffc,
    C64_ADDR_IRQ = 0xfffe
};

enum {                                      /* C64_ADDR_PORT */
    C64_PORT_LORAM = 0x01,                  /* BASIC in */
    C64_PORT_HIRAM = 0x02,                  /* KERNAL in */
    C64_PORT_CHAREN = 0x04,                 /* I/O in (else the character ROM) */
    C64_PORT_BANK = 0x07,
    C64_PORT_CASSETTE = 0x30,               /* the cassette lines, as the KERNAL leaves them */
    C64_PORT_POWER_ON = 0x37                /* BASIC, KERNAL, I/O */
};

enum {
    VIC_ADDR_CTRL1 = 0x11,                  /* raster bit 8, DEN, the vertical scroll */
    VIC_ADDR_RASTER = 0x12,                 /* the raster line (compare on write) */
    VIC_ADDR_CTRL2 = 0x16,
    VIC_ADDR_MEM = 0x18,
    VIC_ADDR_IRQ = 0x19,                    /* the interrupt flags (a write of 1 clears) */
    VIC_ADDR_IRQ_EN = 0x1a,                 /* the interrupt enables */
    VIC_NREG = 0x2f,                        /* above: unused, read $FF */
    VIC_SPAN = 0x40                         /* the registers repeat every $40 */
};
enum {                                      /* VIC_ADDR_CTRL1 */
    VIC_CTRL1_YSCROLL = 0x07,
    VIC_CTRL1_DEN = 0x10,                   /* the display enabled */
    VIC_CTRL1_RST8 = 0x80                   /* the raster line's bit 8 */
};
enum {                                      /* VIC_ADDR_IRQ, VIC_ADDR_IRQ_EN */
    VIC_IRQ_RASTER = 0x01,
    VIC_IRQ_SOURCES = 0x0f,
    VIC_IRQ_ANY = 0x80                      /* read: an enabled flag is set */
};
#define VIC_DEN_LINE 0x30                   /* DEN is sampled on this line: bad lines this frame */
#define VIC_LAST_BAD_LINE 0xf7
#define VIC_BAD_LINE_STALL 40               /* the cycles a bad line takes from the CPU */

enum {
    CIA_ADDR_PRA, CIA_ADDR_PRB,             /* the ports and their directions */
    CIA_ADDR_DDRA, CIA_ADDR_DDRB,
    CIA_ADDR_TA_LO, CIA_ADDR_TA_HI,         /* timer A: the counter on read, the latch on write */
    CIA_ADDR_TB_LO, CIA_ADDR_TB_HI,         /* timer B */
    CIA_ADDR_TOD = 0x08,                    /* the time of day: tenths, seconds, minutes, hours (BCD) */
    CIA_ADDR_SDR = 0x0c,                    /* the serial data: reads back what was written */
    CIA_ADDR_ICR = 0x0d,                   /* the interrupt control */
    CIA_ADDR_CRA = 0x0e,                    /* timer A's and B's control */
    CIA_ADDR_CRB = 0x0f,
    CIA_SPAN = 0x10                         /* the registers repeat every $10 */
};
enum {                                      /* CIA_ADDR_CRA, CIA_ADDR_CRB */
    CIA_CR_START = 0x01,
    CIA_CR_ONESHOT = 0x08,
    CIA_CR_LOAD = 0x10,                     /* strobe: the latch into the counter now */
    CIA_CRA_INMODE = 0x20,                  /* timer A counts CNT edges (not clocks) */
    CIA_CRB_INMODE = 0x60,                  /* timer B counts clocks (0), CNT, A's underflows (2), ... */
    CIA_CRB_INMODE_SHIFT = 5,
    CIA_CRB_COUNT_TA = 2
};
enum {                                      /* CIA_ADDR_ICR */
    CIA_ICR_TA = 0x01, CIA_ICR_TB = 0x02,   /* the timers' underflows */
    CIA_ICR_SOURCES = 0x1f,
    CIA_ICR_SET = 0x80,                     /* write: set the given mask bits (else clear them) */
    CIA_ICR_ANY = 0x80                      /* read: an enabled source fired */
};
