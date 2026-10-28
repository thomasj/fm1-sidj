/* SPDX-License-Identifier: GPL-3.0-only */
/* 6510 core, see cpu6502.h. Integer only. */
#include "cpu6502.h"

#define RD(addr) cpu_read(c, (uint16_t)(addr))
#define WR(addr, v) cpu_write(c, (uint16_t)(addr), (uint8_t)(v))

/* A data access comes after the operands, at pc - op_pc cycles into the instruction (one cycle
 * per byte fetched; indexed and indirect modes spend more, so there it is a little early). A
 * tune that reads a CIA timer as a cycle count (a jump through $DC04 on an NMI) needs it. */
static inline uint8_t cpu_read(cpu6502_t *c, uint16_t addr)
{
    if (addr < c->read_lo)
        return c->mem[addr];
    return c->read_hook(c->ctx, addr, c->cycles + (uint16_t)(c->pc - c->op_pc));
}

static inline void cpu_write(cpu6502_t *c, uint16_t addr, uint8_t v)
{
    if (addr >= c->hook_lo && addr <= c->hook_hi)
        c->write_hook(c->ctx, addr, v, c->cycles + (uint16_t)(c->pc - c->op_pc));   /* the hook decides what reaches RAM */
    else
        c->mem[addr] = v;
}

static inline uint8_t cpu_fetch(cpu6502_t *c)  /* the opcode and operands: one cycle each */
{
    uint16_t a = c->pc++;
    if (a < c->read_lo)
        return c->mem[a];
    return c->read_hook(c->ctx, a, c->cycles + (uint16_t)(a - c->op_pc));
}
#define FETCH() cpu_fetch(c)

void cpu_init(cpu6502_t *c, uint8_t *mem)
{
    c->mem = mem;
    c->hook_lo = 1; c->hook_hi = 0;         /* an empty write window */
    c->read_lo = 65536u;                     /* every read from RAM */
    c->write_hook = 0; c->read_hook = 0; c->ctx = 0;
    c->irq_line = 0; c->nmi_pending = 0;
    c->a = c->x = c->y = 0;
    c->sp = 0xfd;
    c->p = CPU_U | CPU_I;
    c->pc = 0; c->op_pc = 0;
    c->cycles = 0;
    c->jammed = 0;
}

void cpu_reset(cpu6502_t *c)
{
    c->sp = 0xfd;
    c->p = CPU_U | CPU_I;
    c->pc = (uint16_t)(c->mem[0xfffc] | c->mem[0xfffd] << 8);
    c->jammed = 0;
}

static inline void set_nz(cpu6502_t *c, uint8_t v)
{
    c->p = (uint8_t)((c->p & ~(CPU_N | CPU_Z)) | (v & CPU_N) | (v ? 0 : CPU_Z));
}

static inline void push(cpu6502_t *c, uint8_t v) { c->mem[0x100 | c->sp] = v; c->sp--; }
static inline uint8_t pull(cpu6502_t *c) { c->sp++; return c->mem[0x100 | c->sp]; }

/* ---- addressing: each returns the effective address; *pen gets 1 on a page crossing (reads) */
static inline uint16_t am_zp(cpu6502_t *c) { return FETCH(); }
static inline uint16_t am_zpx(cpu6502_t *c) { return (uint8_t)(FETCH() + c->x); }
static inline uint16_t am_zpy(cpu6502_t *c) { return (uint8_t)(FETCH() + c->y); }
static inline uint16_t am_abs(cpu6502_t *c) { uint16_t a = (uint16_t)(FETCH() | FETCH() << 8); return a; }
static inline uint16_t am_absi(cpu6502_t *c, uint8_t idx, int *pen)
{
    uint16_t b = am_abs(c), a = (uint16_t)(b + idx);
    *pen = (b & 0xff00) != (a & 0xff00);
    return a;
}
static inline uint16_t am_indx(cpu6502_t *c)
{
    uint8_t z = (uint8_t)(FETCH() + c->x);
    return (uint16_t)(c->mem[z] | c->mem[(uint8_t)(z + 1)] << 8);
}
static inline uint16_t am_indy(cpu6502_t *c, int *pen)
{
    uint8_t z = FETCH();
    uint16_t b = (uint16_t)(c->mem[z] | c->mem[(uint8_t)(z + 1)] << 8), a = (uint16_t)(b + c->y);
    *pen = (b & 0xff00) != (a & 0xff00);
    return a;
}

/* ---- operations on a value */
static inline void op_adc(cpu6502_t *c, uint8_t v)
{
    unsigned cin = c->p & CPU_C, r;
    if (c->p & CPU_D) {
        unsigned lo = (c->a & 0x0f) + (v & 0x0f) + cin, hi = (c->a >> 4) + (v >> 4);
        unsigned bin = c->a + v + cin;
        if (lo > 9) { lo += 6; hi++; }
        c->p = (uint8_t)((c->p & ~(CPU_N | CPU_V | CPU_Z | CPU_C)) | ((bin & 0xff) ? 0 : CPU_Z));
        c->p |= (uint8_t)((hi << 4) & CPU_N);
        if (((c->a ^ (hi << 4)) & ~(c->a ^ v) & 0x80)) c->p |= CPU_V;
        if (hi > 9) hi += 6;
        if (hi > 15) c->p |= CPU_C;
        c->a = (uint8_t)((hi << 4) | (lo & 0x0f));
        return;
    }
    r = c->a + v + cin;
    c->p = (uint8_t)(c->p & ~(CPU_V | CPU_C));
    if (~(c->a ^ v) & (c->a ^ r) & 0x80) c->p |= CPU_V;
    if (r > 0xff) c->p |= CPU_C;
    c->a = (uint8_t)r;
    set_nz(c, c->a);
}

static inline void op_sbc(cpu6502_t *c, uint8_t v)
{
    unsigned cin = c->p & CPU_C, r = c->a - v - (1 - cin);
    if (c->p & CPU_D) {
        int lo = (c->a & 0x0f) - (v & 0x0f) - (1 - (int)cin), hi = (c->a >> 4) - (v >> 4);
        if (lo < 0) { lo -= 6; hi--; }
        if (hi < 0) hi -= 6;
        c->p = (uint8_t)(c->p & ~(CPU_V | CPU_C));
        if ((c->a ^ v) & (c->a ^ r) & 0x80) c->p |= CPU_V;
        if (!(r & 0x100)) c->p |= CPU_C;
        set_nz(c, (uint8_t)r);
        c->a = (uint8_t)(((hi & 0x0f) << 4) | (lo & 0x0f));
        return;
    }
    c->p = (uint8_t)(c->p & ~(CPU_V | CPU_C));
    if ((c->a ^ v) & (c->a ^ r) & 0x80) c->p |= CPU_V;
    if (!(r & 0x100)) c->p |= CPU_C;
    c->a = (uint8_t)r;
    set_nz(c, c->a);
}

static inline void op_cmp(cpu6502_t *c, uint8_t reg, uint8_t v)
{
    unsigned r = reg - v;
    c->p = (uint8_t)((c->p & ~CPU_C) | ((r & 0x100) ? 0 : CPU_C));
    set_nz(c, (uint8_t)r);
}

static inline uint8_t op_asl(cpu6502_t *c, uint8_t v) { c->p = (uint8_t)((c->p & ~CPU_C) | (v >> 7)); v <<= 1; set_nz(c, v); return v; }
static inline uint8_t op_lsr(cpu6502_t *c, uint8_t v) { c->p = (uint8_t)((c->p & ~CPU_C) | (v & 1)); v >>= 1; set_nz(c, v); return v; }
static inline uint8_t op_rol(cpu6502_t *c, uint8_t v) { uint8_t ci = c->p & CPU_C; c->p = (uint8_t)((c->p & ~CPU_C) | (v >> 7)); v = (uint8_t)(v << 1 | ci); set_nz(c, v); return v; }
static inline uint8_t op_ror(cpu6502_t *c, uint8_t v) { uint8_t ci = c->p & CPU_C; c->p = (uint8_t)((c->p & ~CPU_C) | (v & 1)); v = (uint8_t)(v >> 1 | ci << 7); set_nz(c, v); return v; }
static inline void op_bit(cpu6502_t *c, uint8_t v) { c->p = (uint8_t)((c->p & ~(CPU_N | CPU_V | CPU_Z)) | (v & (CPU_N | CPU_V)) | ((c->a & v) ? 0 : CPU_Z)); }

static inline int branch(cpu6502_t *c, int cond)
{
    int8_t off = (int8_t)FETCH();
    if (!cond) return 2;
    {
        uint16_t t = (uint16_t)(c->pc + off);
        int pen = (t & 0xff00) != (c->pc & 0xff00);
        c->pc = t;
        return 3 + pen;
    }
}

static inline uint8_t op_inc1(cpu6502_t *c, uint8_t v) { (void)c; return (uint8_t)(v + 1); }
static inline uint8_t op_inc(cpu6502_t *c, uint8_t v) { v++; set_nz(c, v); return v; }
static inline uint8_t op_dec(cpu6502_t *c, uint8_t v) { v--; set_nz(c, v); return v; }
static inline uint8_t op_dec1(cpu6502_t *c, uint8_t v) { (void)c; return (uint8_t)(v - 1); }

/* read-modify-write at an address with an operation. The 6502 writes the unmodified value back
 * first, then the result; on an I/O register that first write is an event (INC $D019 and
 * ASL $D019 acknowledge the VIC's interrupt that way), so it is made for hooked addresses. */
#define RMW(addr, OP) do { uint16_t a_ = (addr); uint8_t v_ = RD(a_); \
    if (a_ >= c->hook_lo && a_ <= c->hook_hi) WR(a_, v_); v_ = OP(c, v_); WR(a_, v_); } while (0)

static int take_interrupt(cpu6502_t *c, uint16_t vector)
{
    push(c, (uint8_t)(c->pc >> 8)); push(c, (uint8_t)c->pc);
    push(c, (uint8_t)((c->p | CPU_U) & ~CPU_B));
    c->p |= CPU_I;
    c->pc = (uint16_t)(RD(vector) | RD(vector + 1) << 8);
    c->cycles += 7;
    return 7;
}

int cpu_step(cpu6502_t *c)
{
    uint8_t op;
    uint16_t a;
    int pen = 0, cyc;
    if (c->jammed) return 0;
    c->op_pc = c->pc;
    if (c->nmi_pending == CPU_NMI_LATE) c->nmi_pending = 1;
    else if (c->nmi_pending) { c->nmi_pending = 0; return take_interrupt(c, 0xfffa); }
    if (c->irq_line && !(c->p & CPU_I)) return take_interrupt(c, 0xfffe);
    op = FETCH();
    switch (op) {
    /* ---- loads / stores */
    case 0xa9: c->a = FETCH(); set_nz(c, c->a); cyc = 2; break;
    case 0xa5: c->a = RD(am_zp(c)); set_nz(c, c->a); cyc = 3; break;
    case 0xb5: c->a = RD(am_zpx(c)); set_nz(c, c->a); cyc = 4; break;
    case 0xad: c->a = RD(am_abs(c)); set_nz(c, c->a); cyc = 4; break;
    case 0xbd: c->a = RD(am_absi(c, c->x, &pen)); set_nz(c, c->a); cyc = 4 + pen; break;
    case 0xb9: c->a = RD(am_absi(c, c->y, &pen)); set_nz(c, c->a); cyc = 4 + pen; break;
    case 0xa1: c->a = RD(am_indx(c)); set_nz(c, c->a); cyc = 6; break;
    case 0xb1: c->a = RD(am_indy(c, &pen)); set_nz(c, c->a); cyc = 5 + pen; break;
    case 0xa2: c->x = FETCH(); set_nz(c, c->x); cyc = 2; break;
    case 0xa6: c->x = RD(am_zp(c)); set_nz(c, c->x); cyc = 3; break;
    case 0xb6: c->x = RD(am_zpy(c)); set_nz(c, c->x); cyc = 4; break;
    case 0xae: c->x = RD(am_abs(c)); set_nz(c, c->x); cyc = 4; break;
    case 0xbe: c->x = RD(am_absi(c, c->y, &pen)); set_nz(c, c->x); cyc = 4 + pen; break;
    case 0xa0: c->y = FETCH(); set_nz(c, c->y); cyc = 2; break;
    case 0xa4: c->y = RD(am_zp(c)); set_nz(c, c->y); cyc = 3; break;
    case 0xb4: c->y = RD(am_zpx(c)); set_nz(c, c->y); cyc = 4; break;
    case 0xac: c->y = RD(am_abs(c)); set_nz(c, c->y); cyc = 4; break;
    case 0xbc: c->y = RD(am_absi(c, c->x, &pen)); set_nz(c, c->y); cyc = 4 + pen; break;
    case 0x85: WR(am_zp(c), c->a); cyc = 3; break;
    case 0x95: WR(am_zpx(c), c->a); cyc = 4; break;
    case 0x8d: WR(am_abs(c), c->a); cyc = 4; break;
    case 0x9d: WR(am_absi(c, c->x, &pen), c->a); cyc = 5; break;
    case 0x99: WR(am_absi(c, c->y, &pen), c->a); cyc = 5; break;
    case 0x81: WR(am_indx(c), c->a); cyc = 6; break;
    case 0x91: WR(am_indy(c, &pen), c->a); cyc = 6; break;
    case 0x86: WR(am_zp(c), c->x); cyc = 3; break;
    case 0x96: WR(am_zpy(c), c->x); cyc = 4; break;
    case 0x8e: WR(am_abs(c), c->x); cyc = 4; break;
    case 0x84: WR(am_zp(c), c->y); cyc = 3; break;
    case 0x94: WR(am_zpx(c), c->y); cyc = 4; break;
    case 0x8c: WR(am_abs(c), c->y); cyc = 4; break;
    /* ---- transfers, stack */
    case 0xaa: c->x = c->a; set_nz(c, c->x); cyc = 2; break;
    case 0xa8: c->y = c->a; set_nz(c, c->y); cyc = 2; break;
    case 0x8a: c->a = c->x; set_nz(c, c->a); cyc = 2; break;
    case 0x98: c->a = c->y; set_nz(c, c->a); cyc = 2; break;
    case 0xba: c->x = c->sp; set_nz(c, c->x); cyc = 2; break;
    case 0x9a: c->sp = c->x; cyc = 2; break;
    case 0x48: push(c, c->a); cyc = 3; break;
    case 0x68: c->a = pull(c); set_nz(c, c->a); cyc = 4; break;
    case 0x08: push(c, (uint8_t)(c->p | CPU_B | CPU_U)); cyc = 3; break;
    case 0x28: c->p = (uint8_t)((pull(c) | CPU_U) & ~CPU_B); cyc = 4; break;
    /* ---- arithmetic, logic */
    case 0x69: op_adc(c, FETCH()); cyc = 2; break;
    case 0x65: op_adc(c, RD(am_zp(c))); cyc = 3; break;
    case 0x75: op_adc(c, RD(am_zpx(c))); cyc = 4; break;
    case 0x6d: op_adc(c, RD(am_abs(c))); cyc = 4; break;
    case 0x7d: op_adc(c, RD(am_absi(c, c->x, &pen))); cyc = 4 + pen; break;
    case 0x79: op_adc(c, RD(am_absi(c, c->y, &pen))); cyc = 4 + pen; break;
    case 0x61: op_adc(c, RD(am_indx(c))); cyc = 6; break;
    case 0x71: op_adc(c, RD(am_indy(c, &pen))); cyc = 5 + pen; break;
    case 0xe9: case 0xeb: op_sbc(c, FETCH()); cyc = 2; break;
    case 0xe5: op_sbc(c, RD(am_zp(c))); cyc = 3; break;
    case 0xf5: op_sbc(c, RD(am_zpx(c))); cyc = 4; break;
    case 0xed: op_sbc(c, RD(am_abs(c))); cyc = 4; break;
    case 0xfd: op_sbc(c, RD(am_absi(c, c->x, &pen))); cyc = 4 + pen; break;
    case 0xf9: op_sbc(c, RD(am_absi(c, c->y, &pen))); cyc = 4 + pen; break;
    case 0xe1: op_sbc(c, RD(am_indx(c))); cyc = 6; break;
    case 0xf1: op_sbc(c, RD(am_indy(c, &pen))); cyc = 5 + pen; break;
    case 0x29: c->a &= FETCH(); set_nz(c, c->a); cyc = 2; break;
    case 0x25: c->a &= RD(am_zp(c)); set_nz(c, c->a); cyc = 3; break;
    case 0x35: c->a &= RD(am_zpx(c)); set_nz(c, c->a); cyc = 4; break;
    case 0x2d: c->a &= RD(am_abs(c)); set_nz(c, c->a); cyc = 4; break;
    case 0x3d: c->a &= RD(am_absi(c, c->x, &pen)); set_nz(c, c->a); cyc = 4 + pen; break;
    case 0x39: c->a &= RD(am_absi(c, c->y, &pen)); set_nz(c, c->a); cyc = 4 + pen; break;
    case 0x21: c->a &= RD(am_indx(c)); set_nz(c, c->a); cyc = 6; break;
    case 0x31: c->a &= RD(am_indy(c, &pen)); set_nz(c, c->a); cyc = 5 + pen; break;
    case 0x09: c->a |= FETCH(); set_nz(c, c->a); cyc = 2; break;
    case 0x05: c->a |= RD(am_zp(c)); set_nz(c, c->a); cyc = 3; break;
    case 0x15: c->a |= RD(am_zpx(c)); set_nz(c, c->a); cyc = 4; break;
    case 0x0d: c->a |= RD(am_abs(c)); set_nz(c, c->a); cyc = 4; break;
    case 0x1d: c->a |= RD(am_absi(c, c->x, &pen)); set_nz(c, c->a); cyc = 4 + pen; break;
    case 0x19: c->a |= RD(am_absi(c, c->y, &pen)); set_nz(c, c->a); cyc = 4 + pen; break;
    case 0x01: c->a |= RD(am_indx(c)); set_nz(c, c->a); cyc = 6; break;
    case 0x11: c->a |= RD(am_indy(c, &pen)); set_nz(c, c->a); cyc = 5 + pen; break;
    case 0x49: c->a ^= FETCH(); set_nz(c, c->a); cyc = 2; break;
    case 0x45: c->a ^= RD(am_zp(c)); set_nz(c, c->a); cyc = 3; break;
    case 0x55: c->a ^= RD(am_zpx(c)); set_nz(c, c->a); cyc = 4; break;
    case 0x4d: c->a ^= RD(am_abs(c)); set_nz(c, c->a); cyc = 4; break;
    case 0x5d: c->a ^= RD(am_absi(c, c->x, &pen)); set_nz(c, c->a); cyc = 4 + pen; break;
    case 0x59: c->a ^= RD(am_absi(c, c->y, &pen)); set_nz(c, c->a); cyc = 4 + pen; break;
    case 0x41: c->a ^= RD(am_indx(c)); set_nz(c, c->a); cyc = 6; break;
    case 0x51: c->a ^= RD(am_indy(c, &pen)); set_nz(c, c->a); cyc = 5 + pen; break;
    case 0xc9: op_cmp(c, c->a, FETCH()); cyc = 2; break;
    case 0xc5: op_cmp(c, c->a, RD(am_zp(c))); cyc = 3; break;
    case 0xd5: op_cmp(c, c->a, RD(am_zpx(c))); cyc = 4; break;
    case 0xcd: op_cmp(c, c->a, RD(am_abs(c))); cyc = 4; break;
    case 0xdd: op_cmp(c, c->a, RD(am_absi(c, c->x, &pen))); cyc = 4 + pen; break;
    case 0xd9: op_cmp(c, c->a, RD(am_absi(c, c->y, &pen))); cyc = 4 + pen; break;
    case 0xc1: op_cmp(c, c->a, RD(am_indx(c))); cyc = 6; break;
    case 0xd1: op_cmp(c, c->a, RD(am_indy(c, &pen))); cyc = 5 + pen; break;
    case 0xe0: op_cmp(c, c->x, FETCH()); cyc = 2; break;
    case 0xe4: op_cmp(c, c->x, RD(am_zp(c))); cyc = 3; break;
    case 0xec: op_cmp(c, c->x, RD(am_abs(c))); cyc = 4; break;
    case 0xc0: op_cmp(c, c->y, FETCH()); cyc = 2; break;
    case 0xc4: op_cmp(c, c->y, RD(am_zp(c))); cyc = 3; break;
    case 0xcc: op_cmp(c, c->y, RD(am_abs(c))); cyc = 4; break;
    case 0x24: op_bit(c, RD(am_zp(c))); cyc = 3; break;
    case 0x2c: op_bit(c, RD(am_abs(c))); cyc = 4; break;
    /* ---- increments, decrements */
    case 0xe6: RMW(am_zp(c), op_inc); cyc = 5; break;
    case 0xf6: RMW(am_zpx(c), op_inc); cyc = 6; break;
    case 0xee: RMW(am_abs(c), op_inc); cyc = 6; break;
    case 0xfe: RMW(am_absi(c, c->x, &pen), op_inc); cyc = 7; break;
    case 0xc6: RMW(am_zp(c), op_dec); cyc = 5; break;
    case 0xd6: RMW(am_zpx(c), op_dec); cyc = 6; break;
    case 0xce: RMW(am_abs(c), op_dec); cyc = 6; break;
    case 0xde: RMW(am_absi(c, c->x, &pen), op_dec); cyc = 7; break;
    case 0xe8: c->x++; set_nz(c, c->x); cyc = 2; break;
    case 0xc8: c->y++; set_nz(c, c->y); cyc = 2; break;
    case 0xca: c->x--; set_nz(c, c->x); cyc = 2; break;
    case 0x88: c->y--; set_nz(c, c->y); cyc = 2; break;
    /* ---- shifts */
    case 0x0a: c->a = op_asl(c, c->a); cyc = 2; break;
    case 0x06: RMW(am_zp(c), op_asl); cyc = 5; break;
    case 0x16: RMW(am_zpx(c), op_asl); cyc = 6; break;
    case 0x0e: RMW(am_abs(c), op_asl); cyc = 6; break;
    case 0x1e: RMW(am_absi(c, c->x, &pen), op_asl); cyc = 7; break;
    case 0x4a: c->a = op_lsr(c, c->a); cyc = 2; break;
    case 0x46: RMW(am_zp(c), op_lsr); cyc = 5; break;
    case 0x56: RMW(am_zpx(c), op_lsr); cyc = 6; break;
    case 0x4e: RMW(am_abs(c), op_lsr); cyc = 6; break;
    case 0x5e: RMW(am_absi(c, c->x, &pen), op_lsr); cyc = 7; break;
    case 0x2a: c->a = op_rol(c, c->a); cyc = 2; break;
    case 0x26: RMW(am_zp(c), op_rol); cyc = 5; break;
    case 0x36: RMW(am_zpx(c), op_rol); cyc = 6; break;
    case 0x2e: RMW(am_abs(c), op_rol); cyc = 6; break;
    case 0x3e: RMW(am_absi(c, c->x, &pen), op_rol); cyc = 7; break;
    case 0x6a: c->a = op_ror(c, c->a); cyc = 2; break;
    case 0x66: RMW(am_zp(c), op_ror); cyc = 5; break;
    case 0x76: RMW(am_zpx(c), op_ror); cyc = 6; break;
    case 0x6e: RMW(am_abs(c), op_ror); cyc = 6; break;
    case 0x7e: RMW(am_absi(c, c->x, &pen), op_ror); cyc = 7; break;
    /* ---- jumps, branches */
    case 0x4c: c->pc = am_abs(c); cyc = 3; break;
    case 0x6c: {                            /* JMP (ind), with the page-wrap bug */
        uint16_t p = am_abs(c);
        c->pc = (uint16_t)(RD(p) | RD((p & 0xff00) | ((p + 1) & 0xff)) << 8);
        cyc = 5;
        break;
    }
    case 0x20: a = am_abs(c); c->pc--; push(c, (uint8_t)(c->pc >> 8)); push(c, (uint8_t)c->pc); c->pc = a; cyc = 6; break;
    /* RTS and RTI read the byte after the opcode first and drop it. It matters when they run
     * from I/O: an NMI handler ending in JMP $DD0C (an RTI kept in CIA 2's serial register)
     * acknowledges the CIA by that read of $DD0D. */
    case 0x60: (void)RD(c->pc); c->pc = pull(c); c->pc = (uint16_t)(c->pc | pull(c) << 8); c->pc++; cyc = 6; break;
    case 0x40: (void)RD(c->pc); c->p =(uint8_t)((pull(c) | CPU_U) & ~CPU_B); c->pc = pull(c); c->pc = (uint16_t)(c->pc | pull(c) << 8); cyc = 6; break;
    case 0x00:                              /* BRK */
        c->pc++;
        push(c, (uint8_t)(c->pc >> 8)); push(c, (uint8_t)c->pc);
        push(c, (uint8_t)(c->p | CPU_B | CPU_U));
        c->p |= CPU_I;
        c->pc = (uint16_t)(RD(0xfffe) | RD(0xffff) << 8);
        cyc = 7;
        break;
    case 0x10: cyc = branch(c, !(c->p & CPU_N)); break;
    case 0x30: cyc = branch(c, c->p & CPU_N); break;
    case 0x50: cyc = branch(c, !(c->p & CPU_V)); break;
    case 0x70: cyc = branch(c, c->p & CPU_V); break;
    case 0x90: cyc = branch(c, !(c->p & CPU_C)); break;
    case 0xb0: cyc = branch(c, c->p & CPU_C); break;
    case 0xd0: cyc = branch(c, !(c->p & CPU_Z)); break;
    case 0xf0: cyc = branch(c, c->p & CPU_Z); break;
    /* ---- flags */
    case 0x18: c->p &= (uint8_t)~CPU_C; cyc = 2; break;
    case 0x38: c->p |= CPU_C; cyc = 2; break;
    case 0x58: c->p &= (uint8_t)~CPU_I; cyc = 2; break;
    case 0x78: c->p |= CPU_I; cyc = 2; break;
    case 0xb8: c->p &= (uint8_t)~CPU_V; cyc = 2; break;
    case 0xd8: c->p &= (uint8_t)~CPU_D; cyc = 2; break;
    case 0xf8: c->p |= CPU_D; cyc = 2; break;
    case 0xea: cyc = 2; break;
    /* ---- undocumented: NOPs of every shape */
    case 0x1a: case 0x3a: case 0x5a: case 0x7a: case 0xda: case 0xfa: cyc = 2; break;
    case 0x80: case 0x82: case 0x89: case 0xc2: case 0xe2: c->pc++; cyc = 2; break;
    case 0x04: case 0x44: case 0x64: c->pc++; cyc = 3; break;
    case 0x14: case 0x34: case 0x54: case 0x74: case 0xd4: case 0xf4: c->pc++; cyc = 4; break;
    case 0x0c: c->pc += 2; cyc = 4; break;
    case 0x1c: case 0x3c: case 0x5c: case 0x7c: case 0xdc: case 0xfc: am_absi(c, c->x, &pen); cyc = 4 + pen; break;
    /* ---- undocumented: LAX, LXA, SAX, SBX, ANC, ALR, ARR */
    case 0xa7: c->a = c->x = RD(am_zp(c)); set_nz(c, c->a); cyc = 3; break;
    case 0xb7: c->a = c->x = RD(am_zpy(c)); set_nz(c, c->a); cyc = 4; break;
    case 0xaf: c->a = c->x = RD(am_abs(c)); set_nz(c, c->a); cyc = 4; break;
    case 0xbf: c->a = c->x = RD(am_absi(c, c->y, &pen)); set_nz(c, c->a); cyc = 4 + pen; break;
    case 0xa3: c->a = c->x = RD(am_indx(c)); set_nz(c, c->a); cyc = 6; break;
    case 0xb3: c->a = c->x = RD(am_indy(c, &pen)); set_nz(c, c->a); cyc = 5 + pen; break;
    case 0x87: WR(am_zp(c), c->a & c->x); cyc = 3; break;
    case 0x97: WR(am_zpy(c), c->a & c->x); cyc = 4; break;
    case 0x8f: WR(am_abs(c), c->a & c->x); cyc = 4; break;
    case 0x83: WR(am_indx(c), c->a & c->x); cyc = 6; break;
    case 0xab: { uint8_t v = FETCH(); c->a = c->x = (uint8_t)((c->a | 0xee) & v); set_nz(c, c->a); cyc = 2; break; }   /* LXA (unstable: the usual $EE) */
    case 0xcb: { unsigned r = (c->a & c->x) - FETCH(); c->p = (uint8_t)((c->p & ~CPU_C) | ((r & 0x100) ? 0 : CPU_C)); c->x = (uint8_t)r; set_nz(c, c->x); cyc = 2; break; }
    case 0x0b: case 0x2b: c->a &= FETCH(); set_nz(c, c->a); c->p = (uint8_t)((c->p & ~CPU_C) | (c->a >> 7)); cyc = 2; break;
    case 0x4b: c->a &= FETCH(); c->a = op_lsr(c, c->a); cyc = 2; break;
    case 0x6b: {
        c->a &= FETCH();
        c->a = (uint8_t)(c->a >> 1 | (c->p & CPU_C) << 7);
        set_nz(c, c->a);
        c->p = (uint8_t)((c->p & ~(CPU_C | CPU_V)) | ((c->a >> 6) & 1 ? CPU_C : 0) | ((((c->a >> 6) ^ (c->a >> 5)) & 1) ? CPU_V : 0));
        cyc = 2;
        break;
    }
    /* ---- undocumented read-modify-write combos */
#define COMBO(opc, ADDR, CY, OP, THEN) case opc: { uint16_t a_ = (ADDR); uint8_t v_ = RD(a_); \
    if (a_ >= c->hook_lo && a_ <= c->hook_hi) WR(a_, v_); v_ = OP(c, v_); WR(a_, v_); THEN; cyc = CY; break; }
#define SLO_T (c->a |= v_, set_nz(c, c->a))
#define RLA_T (c->a &= v_, set_nz(c, c->a))
#define SRE_T (c->a ^= v_, set_nz(c, c->a))
#define RRA_T op_adc(c, v_)
#define DCP_T op_cmp(c, c->a, v_)
#define ISB_T op_sbc(c, v_)
    COMBO(0x07, am_zp(c), 5, op_asl, SLO_T) COMBO(0x17, am_zpx(c), 6, op_asl, SLO_T) COMBO(0x0f, am_abs(c), 6, op_asl, SLO_T)
    COMBO(0x1f, am_absi(c, c->x, &pen), 7, op_asl, SLO_T) COMBO(0x1b, am_absi(c, c->y, &pen), 7, op_asl, SLO_T)
    COMBO(0x03, am_indx(c), 8, op_asl, SLO_T) COMBO(0x13, am_indy(c, &pen), 8, op_asl, SLO_T)
    COMBO(0x27, am_zp(c), 5, op_rol, RLA_T) COMBO(0x37, am_zpx(c), 6, op_rol, RLA_T) COMBO(0x2f, am_abs(c), 6, op_rol, RLA_T)
    COMBO(0x3f, am_absi(c, c->x, &pen), 7, op_rol, RLA_T) COMBO(0x3b, am_absi(c, c->y, &pen), 7, op_rol, RLA_T)
    COMBO(0x23, am_indx(c), 8, op_rol, RLA_T) COMBO(0x33, am_indy(c, &pen), 8, op_rol, RLA_T)
    COMBO(0x47, am_zp(c), 5, op_lsr, SRE_T) COMBO(0x57, am_zpx(c), 6, op_lsr, SRE_T) COMBO(0x4f, am_abs(c), 6, op_lsr, SRE_T)
    COMBO(0x5f, am_absi(c, c->x, &pen), 7, op_lsr, SRE_T) COMBO(0x5b, am_absi(c, c->y, &pen), 7, op_lsr, SRE_T)
    COMBO(0x43, am_indx(c), 8, op_lsr, SRE_T) COMBO(0x53, am_indy(c, &pen), 8, op_lsr, SRE_T)
    COMBO(0x67, am_zp(c), 5, op_ror, RRA_T) COMBO(0x77, am_zpx(c), 6, op_ror, RRA_T) COMBO(0x6f, am_abs(c), 6, op_ror, RRA_T)
    COMBO(0x7f, am_absi(c, c->x, &pen), 7, op_ror, RRA_T) COMBO(0x7b, am_absi(c, c->y, &pen), 7, op_ror, RRA_T)
    COMBO(0x63, am_indx(c), 8, op_ror, RRA_T) COMBO(0x73, am_indy(c, &pen), 8, op_ror, RRA_T)
    COMBO(0xc7, am_zp(c), 5, op_dec1, DCP_T) COMBO(0xd7, am_zpx(c), 6, op_dec1, DCP_T) COMBO(0xcf, am_abs(c), 6, op_dec1, DCP_T)
    COMBO(0xdf, am_absi(c, c->x, &pen), 7, op_dec1, DCP_T) COMBO(0xdb, am_absi(c, c->y, &pen), 7, op_dec1, DCP_T)
    COMBO(0xc3, am_indx(c), 8, op_dec1, DCP_T) COMBO(0xd3, am_indy(c, &pen), 8, op_dec1, DCP_T)
    COMBO(0xe7, am_zp(c), 5, op_inc1, ISB_T) COMBO(0xf7, am_zpx(c), 6, op_inc1, ISB_T) COMBO(0xef, am_abs(c), 6, op_inc1, ISB_T)
    COMBO(0xff, am_absi(c, c->x, &pen), 7, op_inc1, ISB_T) COMBO(0xfb, am_absi(c, c->y, &pen), 7, op_inc1, ISB_T)
    COMBO(0xe3, am_indx(c), 8, op_inc1, ISB_T) COMBO(0xf3, am_indy(c, &pen), 8, op_inc1, ISB_T)
#undef COMBO
    /* ---- the rest: KIL and the unstable ones (SHA, SHX, SHY, TAS, LAS, XAA, LXA): jam */
    default:
        c->jammed = 1;
        c->pc--;
        return 0;
    }
    c->cycles += (uint32_t)cyc;
    return cyc;
}

int cpu_call(cpu6502_t *c, uint16_t addr, uint8_t a, uint8_t x, uint8_t y, uint32_t max_cycles)
{
    uint32_t t0 = c->cycles;
    uint8_t sp0 = c->sp;
    c->a = a; c->x = x; c->y = y;
    /* a return address of $FFFF-1: the RTS that lands on $FFFF ends the call */
    push(c, 0xff); push(c, 0xfe);
    c->pc = addr;
    while (c->pc != 0xffff) {
        if (!cpu_step(c)) return -1;        /* jammed */
        if (c->cycles - t0 > max_cycles) { c->sp = sp0; return -1; }
    }
    return (int)(c->cycles - t0);
}
