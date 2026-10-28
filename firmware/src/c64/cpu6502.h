/* SPDX-License-Identifier: GPL-3.0-only */
/* A 6510 (6502) core for running SID tunes: all documented opcodes with their
 * cycle counts (page-crossing and branch penalties included), decimal mode, the usual
 * undocumented opcodes (LAX, SAX, DCP, ISB, SLO, RLA, SRE, RRA, ANC, ALR, ARR, SBX, the NOPs),
 * KIL jams, IRQ and NMI. Memory is 64 KB of RAM with hooks for the top of the address space
 * (banking, I/O, the KERNAL replacement: c64.c). Written for SiDJ;
 * checked against Klaus Dormann's 6502 functional test (tests/cpu_test.c). */
#pragma once
#include <stdint.h>

typedef struct cpu6502 {
    uint16_t pc;
    uint8_t a, x, y, sp, p;
    uint32_t cycles;                        /* total cycles executed */
    uint8_t *mem;                           /* 64 KB of RAM */
    /* the bus: writes inside [hook_lo, hook_hi] go to write_hook (which stores to RAM itself if
     * it wants to), reads at or above read_lo to read_hook; everything else is RAM */
    uint16_t hook_lo, hook_hi;
    uint32_t read_lo;
    void (*write_hook)(void *ctx, uint16_t addr, uint8_t v, uint32_t cycle);
    uint8_t (*read_hook)(void *ctx, uint16_t addr, uint32_t cycle);
    void *ctx;
    uint8_t jammed;                         /* a KIL opcode was executed */
    uint8_t irq_line;                       /* level: taken before the next instruction when I is clear */
    uint8_t nmi_pending;                    /* edge: set it, the core clears it when taken; CPU_NMI_LATE
                                             * (it came in an instruction's last cycle) waits one more */
    uint16_t op_pc;                         /* where the instruction began: a hook's cycle is `cycles`
                                             * plus the access's place in it (exact for fetches and for
                                             * zero page / absolute operands, early on indexed ones) */
} cpu6502_t;

#define CPU_NMI_LATE 2                      /* nmi_pending: taken after the next instruction */
#define CPU_MAX_OP_CYCLES 8                /* no instruction takes longer (7 for an indexed read-modify-write) */

enum { CPU_C = 0x01, CPU_Z = 0x02, CPU_I = 0x04, CPU_D = 0x08, CPU_B = 0x10, CPU_U = 0x20, CPU_V = 0x40, CPU_N = 0x80 };

void cpu_init(cpu6502_t *c, uint8_t *mem);
void cpu_reset(cpu6502_t *c);               /* pc from the vector at $FFFC, sp = $FD, I set */
int cpu_step(cpu6502_t *c);                 /* one instruction; returns its cycles (0 when jammed) */
/* JSR addr with the registers given, run until the matching RTS (or max_cycles, or a jam);
 * returns the cycles spent, or -1 if it did not return in time */
int cpu_call(cpu6502_t *c, uint16_t addr, uint8_t a, uint8_t x, uint8_t y, uint32_t max_cycles);
