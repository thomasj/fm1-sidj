/* SPDX-License-Identifier: GPL-3.0-only */
/* The C64 for SID tunes, see c64.h. Integer only. */
#include "c64.h"
#include "c64regs.h"
#include "sidreg.h"

/* ------------------------------------------------------- the KERNAL replacement --- */
/* Sparse: only the bytes a tune may reach are defined, at the real KERNAL's addresses so that
 * tunes jumping into the KERNAL's interrupt code ($EA31, $EA7E, $EA81) land where they expect.
 * Everything else reads as RTS, so a KERNAL call does nothing and returns. */
typedef struct { uint16_t addr; uint8_t len; const uint8_t *bytes; } rom_range_t;
static const uint8_t ROM_EA31[] = {0x4c, 0x7e, 0xea};                       /* JMP $EA7E */
static const uint8_t ROM_EA7E[] = {0xad, 0x0d, 0xdc,                        /* LDA $DC0D: ack CIA 1 */
                                   0x68, 0xa8, 0x68, 0xaa, 0x68, 0x40};     /* $EA81: PLA TAY PLA TAX PLA RTI */
static const uint8_t ROM_FE43[] = {0x78, 0x6c, 0x18, 0x03,                  /* NMI: SEI, JMP ($0318) */
                                   0x48, 0x8a, 0x48, 0x98, 0x48,            /* $FE47: PHA TXA PHA TYA PHA */
                                   0xad, 0x0d, 0xdd, 0x4c, 0xbc, 0xfe};     /* LDA $DD0D, JMP $FEBC */
static const uint8_t ROM_FE66[] = {0x4c, 0xbc, 0xfe};                       /* BRK default: JMP $FEBC */
static const uint8_t ROM_FEBC[] = {0x68, 0xa8, 0x68, 0xaa, 0x68, 0x40};     /* PLA TAY PLA TAX PLA RTI */
static const uint8_t ROM_FF48[] = {0x48, 0x8a, 0x48, 0x98, 0x48, 0xba,      /* IRQ / BRK entry */
                                   0xbd, 0x04, 0x01, 0x29, 0x10, 0xf0, 0x03,
                                   0x6c, 0x16, 0x03, 0x6c, 0x14, 0x03};
static const uint8_t ROM_FFE4[] = {0xa9, 0x00, 0x18, 0x60};                 /* GETIN: no key */
static const uint8_t ROM_FFFA[] = {0x43, 0xfe, 0xe2, 0xfc, 0x48, 0xff};     /* NMI, RESET, IRQ vectors */
static const rom_range_t ROM[] = {
    {0xea31, sizeof ROM_EA31, ROM_EA31}, {0xea7e, sizeof ROM_EA7E, ROM_EA7E},
    {0xfe43, sizeof ROM_FE43, ROM_FE43}, {0xfe66, sizeof ROM_FE66, ROM_FE66},
    {0xfebc, sizeof ROM_FEBC, ROM_FEBC}, {0xff48, sizeof ROM_FF48, ROM_FF48},
    {0xffe4, sizeof ROM_FFE4, ROM_FFE4}, {0xfffa, sizeof ROM_FFFA, ROM_FFFA},
};

static uint8_t rom_read(const c64_t *c, uint16_t addr)
{
    unsigned i;
    if (addr >= 0xea34 && addr < 0xea7e) return 0xea;       /* NOPs down to the handler's end */
    if (addr == 0xfce2) return 0x4c;                        /* RESET: JMP driver */
    if (addr == 0xfce3) return (uint8_t)c->driver;
    if (addr == 0xfce4) return (uint8_t)(c->driver >> 8);
    for (i = 0; i < sizeof ROM / sizeof ROM[0]; i++)
        if (addr >= ROM[i].addr && addr < ROM[i].addr + ROM[i].len)
            return ROM[i].bytes[addr - ROM[i].addr];
    return 0x60;                                            /* RTS */
}

/* ----------------------------------------------------------------- CIA --- */
static void cia_reset(c64_cia_t *k)
{
    k->ta = k->tb = k->ta_latch = k->tb_latch = 0xffff;
    k->cra = k->crb = k->icr_mask = k->icr_data = 0;
    k->pra = k->prb = 0xff;
    k->ddra = k->ddrb = 0;
    k->sdr = 0;
    k->irq = 0;
    k->tod[0] = k->tod[1] = k->tod[2] = 0; k->tod[3] = 1;   /* 1:00:00.0 AM, running */
    k->tod_alarm[0] = k->tod_alarm[1] = k->tod_alarm[2] = k->tod_alarm[3] = 0;
    k->tod_latched = k->tod_stopped = k->tod_pins = 0;
    k->tod_acc = 0;
}

static uint8_t bcd_inc(uint8_t v) { return (uint8_t)((v & 0x0f) == 9 ? (v & 0xf0) + 0x10 : v + 1); }

static void tod_tick(c64_cia_t *k)          /* a tenth of a second on */
{
    uint8_t h;
    if ((k->tod[0] = (uint8_t)((k->tod[0] + 1) & 0x0f)) < 10) goto alarm;
    k->tod[0] = 0;
    if ((k->tod[1] = bcd_inc(k->tod[1])) < 0x60) goto alarm;
    k->tod[1] = 0;
    if ((k->tod[2] = bcd_inc(k->tod[2])) < 0x60) goto alarm;
    k->tod[2] = 0;
    h = k->tod[3] & 0x1f;                   /* 1 .. 12, the PM bit flipping on the way to 12 */
    if (h == 0x11) k->tod[3] = (uint8_t)((k->tod[3] & 0x80) ^ 0x80) | 0x12;
    else if (h == 0x12) k->tod[3] = (uint8_t)((k->tod[3] & 0x80) | 0x01);
    else k->tod[3] = (uint8_t)((k->tod[3] & 0x80) | bcd_inc(h));
alarm:
    if (k->tod[0] == k->tod_alarm[0] && k->tod[1] == k->tod_alarm[1] && k->tod[2] == k->tod_alarm[2] && k->tod[3] == k->tod_alarm[3])
        k->icr_data |= 0x04;
}

static void tod_run(c64_cia_t *k)           /* the pin pulses the cycles counted add up to */
{
    if (!k->tod_period) return;
    while (k->tod_acc >= k->tod_period) {
        k->tod_acc -= k->tod_period;
        if (k->tod_stopped) continue;
        if (++k->tod_pins >= ((k->cra & 0x80) ? 5u : 6u)) { k->tod_pins = 0; tod_tick(k); }   /* CRA bit 7: a 50 Hz pin */
    }
}

/* a timer counting down `n` ticks: returns the underflows */
static uint32_t timer_count(uint16_t *t, uint16_t latch, uint8_t *cr, uint32_t n)
{
    uint32_t under = 0;
    while (n) {
        if (*t >= n) { *t = (uint16_t)(*t - n); return under; }
        n -= (uint32_t)*t + 1u;                             /* the tick after 0 reloads */
        under++;
        *t = latch;
        if (*cr & CIA_CR_ONESHOT) { *cr &= (uint8_t)~CIA_CR_START; return under; }   /* one-shot: stops */
    }
    return under;
}

static void cia_clock(c64_cia_t *k, uint32_t cyc)
{
    uint32_t ua = 0, ub = 0, mode_b = (k->crb & CIA_CRB_INMODE) >> CIA_CRB_INMODE_SHIFT;
    if ((k->cra & (CIA_CRA_INMODE | CIA_CR_START)) == CIA_CR_START) ua = timer_count(&k->ta, k->ta_latch, &k->cra, cyc);
    if (k->crb & CIA_CR_START) {
        if (mode_b == 0) ub = timer_count(&k->tb, k->tb_latch, &k->crb, cyc);
        else if (mode_b == CIA_CRB_COUNT_TA && ua) ub = timer_count(&k->tb, k->tb_latch, &k->crb, ua);
    }
    if (ua) k->icr_data |= CIA_ICR_TA;
    if (ub) k->icr_data |= CIA_ICR_TB;
    k->tod_acc += cyc;
    tod_run(k);
    k->irq = (k->icr_data & k->icr_mask) ? 1 : 0;
}

static uint8_t cia_read(c64_cia_t *k, uint8_t reg)
{
    uint8_t v;
    switch (reg & (CIA_SPAN - 1)) {
    case CIA_ADDR_PRA: return k->pra | (uint8_t)~k->ddra;
    case CIA_ADDR_PRB: return k->prb | (uint8_t)~k->ddrb;
    case CIA_ADDR_DDRA: return k->ddra;
    case CIA_ADDR_DDRB: return k->ddrb;
    case CIA_ADDR_TA_LO: return (uint8_t)k->ta;
    case CIA_ADDR_TA_HI: return (uint8_t)(k->ta >> 8);
    case CIA_ADDR_TB_LO: return (uint8_t)k->tb;
    case CIA_ADDR_TB_HI: return (uint8_t)(k->tb >> 8);
    case CIA_ADDR_SDR: return k->sdr;
    case CIA_ADDR_TOD: case CIA_ADDR_TOD + 1: case CIA_ADDR_TOD + 2: case CIA_ADDR_TOD + 3: {
        int r = (reg & (CIA_SPAN - 1)) - CIA_ADDR_TOD;
        tod_run(k);
        if (r == 3 && !k->tod_latched) { k->tod_latch[0] = k->tod[0]; k->tod_latch[1] = k->tod[1]; k->tod_latch[2] = k->tod[2]; k->tod_latch[3] = k->tod[3]; k->tod_latched = 1; }
        v = k->tod_latched ? k->tod_latch[r] : k->tod[r];
        if (r == 0) k->tod_latched = 0;     /* the tenths end the latch */
        return v;
    }
    case CIA_ADDR_ICR:
        v = (uint8_t)(k->icr_data | ((k->icr_data & k->icr_mask) ? CIA_ICR_ANY : 0));
        k->icr_data = 0;
        k->irq = 0;
        return v;
    case CIA_ADDR_CRA: return k->cra;
    case CIA_ADDR_CRB: return k->crb;
    default: return 0;
    }
}

static void cia_write(c64_cia_t *k, uint8_t reg, uint8_t v)
{
    switch (reg & (CIA_SPAN - 1)) {
    case CIA_ADDR_PRA: k->pra = v; break;
    case CIA_ADDR_PRB: k->prb = v; break;
    case CIA_ADDR_DDRA: k->ddra = v; break;
    case CIA_ADDR_DDRB: k->ddrb = v; break;
    case CIA_ADDR_TA_LO: k->ta_latch = (uint16_t)((k->ta_latch & 0xff00) | v); break;
    case CIA_ADDR_TA_HI:
        k->ta_latch = (uint16_t)((k->ta_latch & 0x00ff) | (uint16_t)v << 8);
        if (!(k->cra & CIA_CR_START)) k->ta = k->ta_latch;  /* stopped: the counter follows */
        break;
    case CIA_ADDR_TB_LO: k->tb_latch = (uint16_t)((k->tb_latch & 0xff00) | v); break;
    case CIA_ADDR_TB_HI:
        k->tb_latch = (uint16_t)((k->tb_latch & 0x00ff) | (uint16_t)v << 8);
        if (!(k->crb & CIA_CR_START)) k->tb = k->tb_latch;
        break;
    case CIA_ADDR_SDR: k->sdr = v; break;
    case CIA_ADDR_TOD: case CIA_ADDR_TOD + 1: case CIA_ADDR_TOD + 2: case CIA_ADDR_TOD + 3: {
        static const uint8_t MASK[4] = {0x0f, 0x7f, 0x7f, 0x9f};
        int r = (reg & (CIA_SPAN - 1)) - CIA_ADDR_TOD;
        tod_run(k);
        if (k->crb & 0x80) { k->tod_alarm[r] = v & MASK[r]; break; }   /* CRB bit 7: the alarm */
        k->tod[r] = v & MASK[r];
        if (r == 3) k->tod_stopped = 1;     /* the hours stop it ... */
        if (r == 0) { k->tod_stopped = 0; k->tod_pins = 0; }   /* ... until the tenths */
        break;
    }
    case CIA_ADDR_ICR:
        if (v & CIA_ICR_SET) k->icr_mask |= v & CIA_ICR_SOURCES;
        else k->icr_mask &= (uint8_t)~(v & CIA_ICR_SOURCES);
        k->irq = (k->icr_data & k->icr_mask) ? 1 : 0;
        break;
    case CIA_ADDR_CRA:
        if (v & CIA_CR_LOAD) k->ta = k->ta_latch;
        k->cra = v & (uint8_t)~CIA_CR_LOAD;
        break;
    case CIA_ADDR_CRB:
        if (v & CIA_CR_LOAD) k->tb = k->tb_latch;
        k->crb = v & (uint8_t)~CIA_CR_LOAD;
        break;
    default: break;
    }
}

/* ----------------------------------------------------------------- bus --- */
static void irq_update(c64_t *c)
{
    c->cpu.irq_line = (uint8_t)(c->cia[0].irq | ((c->vic_flag & c->vic_en) ? 1u : 0u));
}

static int sid_chip(const c64_t *c, uint16_t addr)
{
    int i;
    for (i = 1; i < c->nsid; i++)
        if ((addr & ~(SID_NREG - 1)) == c->sid_addr[i]) return i;
    if (addr >= C64_ADDR_SID && addr < C64_ADDR_COLOR) return 0;
    return -1;
}

static int io_mapped(const c64_t *c)         /* I/O at $D000: CHAREN, and a ROM in (all RAM otherwise) */
{
    uint8_t b = c->mem[C64_ADDR_PORT] & C64_PORT_BANK;
    return (b & (C64_PORT_LORAM | C64_PORT_HIRAM)) != 0 && (b & C64_PORT_CHAREN) != 0;
}

static void c64_flush(c64_t *c);
static uint32_t next_event(const c64_t *c);

/* An I/O access `cycle` falls inside the running instruction: the chips get the cycles up to it
 * first (the rest of the instruction follows when it ends), so a timer read there reads what a
 * real one would at that cycle. A cycle not inside an instruction (a call from outside the
 * CPU) changes nothing. */
static void catch_up(c64_t *c, uint32_t cycle)
{
    uint32_t n = cycle - c->cpu.cycles;
    if (n > c->ahead && n < CPU_MAX_OP_CYCLES) { c->acc += n - c->ahead; c->ahead = n; }
}

static uint8_t io_read(c64_t *c, uint16_t addr, uint32_t cycle)
{
    int chip;
    catch_up(c, cycle);
    c64_flush(c);
    if (addr < C64_ADDR_SID) {                              /* VIC, mirrored every $40 */
        uint8_t r = addr & (VIC_SPAN - 1);
        switch (r) {
        case VIC_ADDR_CTRL1: return (uint8_t)((c->vic_reg[VIC_ADDR_CTRL1] & ~VIC_CTRL1_RST8) | ((c->raster >> 1) & VIC_CTRL1_RST8));
        case VIC_ADDR_RASTER: return (uint8_t)c->raster;
        case VIC_ADDR_IRQ: return (uint8_t)(0x70 | c->vic_flag | ((c->vic_flag & c->vic_en) ? VIC_IRQ_ANY : 0));   /* unused bits read 1 */
        case VIC_ADDR_IRQ_EN: return (uint8_t)(0xf0 | c->vic_en);
        default: return r < VIC_NREG ? c->vic_reg[r] : 0xff;
        }
    }
    if (addr >= C64_ADDR_CIA1 && addr < C64_ADDR_CIA2) {
        uint8_t v = cia_read(&c->cia[0], (uint8_t)addr);
        if ((addr & (CIA_SPAN - 1)) == CIA_ADDR_ICR) irq_update(c);           /* the read acknowledged it: the IRQ line drops now,
                                                             * not at the next clock step (or an RTI right after
                                                             * takes a phantom interrupt) */
        return v;
    }
    if (addr >= C64_ADDR_CIA2 && addr < C64_ADDR_IO_EXP) return cia_read(&c->cia[1], (uint8_t)addr);
    if (addr >= C64_ADDR_COLOR && addr < C64_ADDR_CIA1) return c->mem[addr] | 0xf0;   /* colour RAM: 4 bits */
    chip = sid_chip(c, addr);
    if (chip >= 0) {
        uint8_t reg = addr & (SID_NREG - 1);
        if (reg == SID_ADDR_POTX || reg == SID_ADDR_POTY) return 0xff;   /* no paddles */
        if ((reg == SID_ADDR_OSC3 || reg == SID_ADDR_ENV3) && c->sid_read) return c->sid_read(c->sid_ctx, chip, reg);
        return 0;
    }
    return 0xff;                                            /* open I/O */
}

static void io_write(c64_t *c, uint16_t addr, uint8_t v, uint32_t cycle)
{
    int chip;
    if (addr < C64_ADDR_SID || (addr >= C64_ADDR_CIA1 && addr < C64_ADDR_IO_EXP)) {   /* the VIC and the CIAs
                                                             * catch up first; a SID write only needs its cycle */
        catch_up(c, cycle);
        c64_flush(c);
    }
    if (addr < C64_ADDR_SID) {
        uint8_t r = addr & (VIC_SPAN - 1);
        switch (r) {
        case VIC_ADDR_CTRL1:
            c->vic_reg[VIC_ADDR_CTRL1] = v;
            c->raster_cmp = (uint16_t)((c->raster_cmp & 0xff) | ((v & VIC_CTRL1_RST8) << 1));
            if (c->raster == VIC_DEN_LINE && (v & VIC_CTRL1_DEN)) c->den = VIC_CTRL1_DEN;
            break;
        case VIC_ADDR_RASTER: c->raster_cmp = (uint16_t)((c->raster_cmp & 0x100) | v); break;
        case VIC_ADDR_IRQ: c->vic_flag &= (uint8_t)~(v & VIC_IRQ_SOURCES); irq_update(c); break;
        case VIC_ADDR_IRQ_EN: c->vic_en = v & VIC_IRQ_SOURCES; irq_update(c); break;
        default: if (r < VIC_NREG) c->vic_reg[r] = v; break;
        }
        c->next = next_event(c);
        return;
    }
    if (addr >= C64_ADDR_CIA1 && addr < C64_ADDR_CIA2) { cia_write(&c->cia[0], (uint8_t)addr, v); irq_update(c); c->next = next_event(c); return; }
    if (addr >= C64_ADDR_CIA2 && addr < C64_ADDR_IO_EXP) {
        uint8_t was = c->cia[1].irq;
        cia_write(&c->cia[1], (uint8_t)addr, v);
        if (c->cia[1].irq && !was && !c->cpu.nmi_pending)   /* (the mask set on a pending source): a store */
            c->cpu.nmi_pending = CPU_NMI_LATE;  /* writes in its last cycle, the NMI comes after the next one */
        c->next = next_event(c);
        return;
    }
    if (addr >= C64_ADDR_COLOR && addr < C64_ADDR_CIA1) { c->mem[addr] = v; return; }
    chip = sid_chip(c, addr);
    if (chip >= 0) {
        uint8_t reg = addr & (SID_NREG - 1);
        if (reg >= SID_NWRITE) return;
        if (c->nlog < C64_LOG_MAX) {
            c64_write_t *w = &c->log[c->nlog++];
            w->cycle = (uint16_t)(cycle - c->run_start);
            w->chip = (uint8_t)chip;
            w->reg = reg;
            w->val = v;
        } else {
            c->log_lost++;
        }
    }
}

static uint8_t bus_read(void *ctx, uint16_t addr, uint32_t cycle)
{
    c64_t *c = ctx;
    uint8_t b = c->mem[C64_ADDR_PORT] & C64_PORT_BANK;
    if (addr >= C64_ADDR_KERNAL) return (b & C64_PORT_HIRAM) ? rom_read(c, addr) : c->mem[addr];
    if (addr >= C64_ADDR_IO) {
        if ((b & (C64_PORT_LORAM | C64_PORT_HIRAM)) == 0) return c->mem[addr];
        if (b & C64_PORT_CHAREN) return io_read(c, addr, cycle);
        return 0;                                           /* the character ROM: not here */
    }
    if (addr < C64_ADDR_BASIC_END && (b & (C64_PORT_LORAM | C64_PORT_HIRAM)) == (C64_PORT_LORAM | C64_PORT_HIRAM)
        && c->cpu.pc == (uint16_t)(addr + 1u))   /* BASIC: code fetched there is RTS; data read there */
        return 0x60;                            /* is the RAM below, as in libsidplayfp (a table kept there) */
    return c->mem[addr];
}

static void bus_write(void *ctx, uint16_t addr, uint8_t v, uint32_t cycle)
{
    c64_t *c = ctx;
    if (io_mapped(c)) io_write(c, addr, v, cycle);
    else c->mem[addr] = v;
}

/* --------------------------------------------------------------- clock --- */
/* Time passes lazily: the cycles an instruction took are only added up (c64_clock) until the
 * earliest thing that can happen, a timer underflow or the next raster line, is due, or until
 * the tune touches I/O (c64_flush); then the CIAs and the VIC catch up at once. Interrupts
 * are still seen at the instruction boundary where they fall due.
 *
 * The VIC steals the bus for about 40 cycles on every bad line (a line whose low three bits
 * equal the vertical scroll, in the display window, with the display enabled at line $30):
 * the CPU stalls. Here the stall lands whole at the start of the line, which keeps the timers,
 * the raster and the tune's own cycle counting in step with a real machine to within a line. */
static uint32_t next_event(const c64_t *c)
{
    uint32_t n = (uint32_t)c->cycles_per_line - c->line_acc, i;
    if (n == 0) n = 1;
    for (i = 0; i < 2; i++) {
        const c64_cia_t *k = &c->cia[i];
        if ((k->cra & (CIA_CRA_INMODE | CIA_CR_START)) == CIA_CR_START && (uint32_t)k->ta + 1u < n) n = (uint32_t)k->ta + 1u;
        if ((k->crb & (CIA_CRB_INMODE | CIA_CR_START)) == CIA_CR_START && (uint32_t)k->tb + 1u < n) n = (uint32_t)k->tb + 1u;
    }
    return n;
}

static void c64_flush(c64_t *c)
{
    uint32_t cyc = c->acc, stall = 0, after = 0;
    int wrapped = 0;
    if (cyc < c->next) {                    /* nothing falls due (every I/O access between events):
                                             * the running timers and the raster count on, no more */
        int i;
        if (!cyc) return;
        c->acc = 0;
        c->next -= cyc;
        c->line_acc = (uint16_t)(c->line_acc + cyc);
        for (i = 0; i < 2; i++) {
            c64_cia_t *k = &c->cia[i];
            if ((k->cra & (CIA_CRA_INMODE | CIA_CR_START)) == CIA_CR_START) k->ta = (uint16_t)(k->ta - cyc);
            if ((k->crb & (CIA_CRB_INMODE | CIA_CR_START)) == CIA_CR_START) k->tb = (uint16_t)(k->tb - cyc);
            k->tod_acc += cyc;              /* (the time of day steps when read or at the next event) */
        }
        return;
    }
    c->acc = 0;
    if (cyc) {
        uint8_t was = c->cia[1].irq;
        cia_clock(&c->cia[0], cyc);
        cia_clock(&c->cia[1], cyc);
        if (c->cia[1].irq && !was) c->cpu.nmi_pending = 1;
        c->line_acc = (uint16_t)(c->line_acc + cyc);
        while (c->line_acc >= c->cycles_per_line) {
            c->line_acc = (uint16_t)(c->line_acc - c->cycles_per_line);
            if (wrapped) after += c->cycles_per_line;
            if (++c->raster >= c->lines) { c->raster = 0; c->frames++; wrapped = 1; after = 0; }
            if (c->raster == c->raster_cmp) c->vic_flag |= VIC_IRQ_RASTER;
            if (c->raster == VIC_DEN_LINE) c->den = (uint8_t)(c->vic_reg[VIC_ADDR_CTRL1] & VIC_CTRL1_DEN);   /* DEN at line $30 enables the frame's bad lines */
            if (c->raster >= VIC_DEN_LINE && c->raster <= VIC_LAST_BAD_LINE && c->den &&
                (c->raster & VIC_CTRL1_YSCROLL) == (c->vic_reg[VIC_ADDR_CTRL1] & VIC_CTRL1_YSCROLL))
                stall += VIC_BAD_LINE_STALL;
        }
        if (wrapped)                                        /* where in the run the frame changed */
            c->frame_cycle = c->cpu.cycles + c->ahead - c->run_start - (c->line_acc + after);
        irq_update(c);
    }
    c->next = next_event(c);
    if (stall) {
        c->cpu.cycles += stall;
        c->acc = stall;
        c64_flush(c);                                       /* the time passes for the chips too */
    }
}

/* whether CIA 2 interrupts (an NMI) in the cycles lo+1 .. hi from the last catch-up: its timers
 * as they stand count down from there */
static int nmi_in(const c64_t *c, uint32_t lo, uint32_t hi)
{
    const c64_cia_t *k = &c->cia[1];
    uint32_t u;
    if ((k->icr_mask & CIA_ICR_TA) && (k->cra & (CIA_CRA_INMODE | CIA_CR_START)) == CIA_CR_START) {
        u = (uint32_t)k->ta + 1u;
        if (u > lo && u <= hi) return 1;
    }
    if ((k->icr_mask & CIA_ICR_TB) && (k->crb & (CIA_CRB_INMODE | CIA_CR_START)) == CIA_CR_START) {
        u = (uint32_t)k->tb + 1u;
        if (u > lo && u <= hi) return 1;
    }
    return (k->icr_mask & ~(CIA_ICR_TA | CIA_ICR_TB)) != 0;   /* another source: as before, to be safe */
}

/* The last cycles of an instruction. The 6510 looks at its interrupt inputs before an
 * instruction's last cycle: an NMI that comes in that cycle waits for one more instruction. A
 * tune that times itself by the NMI's lateness (a jump through a CIA timer) needs that. */
static inline void c64_clock(c64_t *c, uint32_t cyc)
{
    uint8_t pending;
    if (c->acc + cyc < c->next) { c->acc += cyc; return; }
    if (!nmi_in(c, c->acc + cyc - 1, c->acc + cyc)) { c->acc += cyc; c64_flush(c); return; }   /* no NMI to place */
    if (cyc > 1) { c->acc += cyc - 1; c64_flush(c); }   /* (cyc: what an I/O access in it left, down to the last) */
    pending = c->cpu.nmi_pending;
    c->acc += 1;
    c64_flush(c);
    if (c->cpu.nmi_pending && !pending) c->cpu.nmi_pending = CPU_NMI_LATE;
}

/* -------------------------------------------------------------- driver --- */
/* The driver, hand-assembled into RAM at c->driver (about 100 bytes). It does what the KERNAL
 * and a player would: stack, bank, interrupts off, CIA 1 timer A at the KERNAL's 60 Hz, the
 * vectors, then init and an idle loop with interrupts on. For a PSID file with a play address
 * the interrupt (CIA timer or raster) calls play with the bank set as the player wants it. */
static int driver_fits(const sidfile_t *f, uint32_t page)
{
    uint32_t a = page << 8, lo = f->load, hi = (uint32_t)f->load + f->data_len;
    if (a < 0x0200 || a >= C64_ADDR_IO) return 0;
    if (a + 0x100 > lo && a < hi) return 0;
    return 1;
}

static uint16_t driver_place(const sidfile_t *f)
{
    uint32_t p;
    if (f->start_page && f->start_page != 0xff && driver_fits(f, f->start_page)) return (uint16_t)(f->start_page << 8);
    for (p = 0x04; p < C64_ADDR_IO >> 8; p++)
        if (driver_fits(f, p)) return (uint16_t)(p << 8);
    for (p = 0x02; p < 0x04; p++)
        if (driver_fits(f, p)) return (uint16_t)(p << 8);
    return 0x0200;                                          /* nowhere free: the BASIC input buffer */
}

static void driver_emit(c64_t *c, const sidfile_t *f, int song)
{
    uint8_t *m = c->mem;
    uint16_t d = c->driver, p = d, irq_k, irq_r, loop, v[4] = {0, 0, 0, 0};
    uint16_t timer = c->ntsc ? 0x4295 : 0x4025;             /* the KERNAL's 60 Hz jiffy, PAL / NTSC */
#define B(x) (m[p++] = (uint8_t)(x))
#define LDA_IMM(v) do { B(0xa9); B(v); } while (0)
#define STA_ABS(a) do { B(0x8d); B((a) & 0xff); B((a) >> 8); } while (0)
#define LDA_ABS(a) do { B(0xad); B((a) & 0xff); B((a) >> 8); } while (0)
    B(0x78); B(0xd8);                                       /* SEI, CLD */
    B(0xa2); B(0xff); B(0x9a);                              /* LDX #$FF, TXS */
#define VIC(r) (C64_ADDR_VIC + (r))
#define CIA1(r) (C64_ADDR_CIA1 + (r))
#define CIA2(r) (C64_ADDR_CIA2 + (r))
    LDA_IMM(c->bank); B(0x85); B(C64_ADDR_PORT);            /* STA $01 */
    LDA_IMM(0x00); STA_ABS(VIC(VIC_ADDR_IRQ_EN));           /* VIC interrupts off */
    LDA_IMM(CIA_ICR_SOURCES); STA_ABS(CIA1(CIA_ADDR_ICR)); STA_ABS(CIA2(CIA_ADDR_ICR));   /* CIA interrupts off */
    LDA_ABS(CIA1(CIA_ADDR_ICR)); LDA_ABS(CIA2(CIA_ADDR_ICR));   /* and acknowledged */
    LDA_IMM(timer & 0xff); STA_ABS(CIA1(CIA_ADDR_TA_LO));
    LDA_IMM(timer >> 8); STA_ABS(CIA1(CIA_ADDR_TA_HI));
    LDA_IMM(CIA_CR_LOAD | CIA_CR_START); STA_ABS(CIA1(CIA_ADDR_CRA));   /* timer A: load, start, continuous */
    if (c->has_play) {
        /* the vectors are patched once the handlers' addresses are known (below) */
        irq_k = 0; irq_r = 0;
        v[0] = p + 1; LDA_IMM(0); STA_ABS(C64_ADDR_IRQ_VEC); v[1] = p + 1; LDA_IMM(0); STA_ABS(C64_ADDR_IRQ_VEC + 1);
        v[2] = p + 1; LDA_IMM(0); STA_ABS(C64_ADDR_IRQ); v[3] = p + 1; LDA_IMM(0); STA_ABS(C64_ADDR_IRQ + 1);
        if (c->cia_speed) {
            LDA_IMM(CIA_ICR_SET | CIA_ICR_TA); STA_ABS(CIA1(CIA_ADDR_ICR));   /* timer A interrupts */
        } else {
            LDA_IMM(0x1b); STA_ABS(VIC(VIC_ADDR_CTRL1));    /* (the KERNAL's value: DEN, 25 rows, scroll 3) */
            LDA_IMM(0x00); STA_ABS(VIC(VIC_ADDR_RASTER));   /* raster interrupt at line 0 */
            LDA_IMM(VIC_IRQ_RASTER); STA_ABS(VIC(VIC_ADDR_IRQ_EN));
        }
    } else {
        LDA_IMM(CIA_ICR_SET | CIA_ICR_TA); STA_ABS(CIA1(CIA_ADDR_ICR));   /* as the KERNAL leaves it */
    }
    if (f->rsid) { B(0xa2); B(0x01); B(0xa4); B(C64_ADDR_PORT); }   /* LDX #1, LDY $01: an RSID's init gets the
                                                             * registers libsidplayfp gives it (a 256-byte intro counts on them) */
    LDA_IMM(song - 1); B(0x20); B(f->init & 0xff); B(f->init >> 8);   /* JSR init */
    B(0x58);                                                /* CLI */
    loop = p;
    B(0x4c); B(loop & 0xff); B(loop >> 8);                  /* loop: JMP loop */
    if (c->has_play) {
        irq_k = p;                                          /* via $FF48 / ($0314): registers saved */
        B(0xa5); B(C64_ADDR_PORT); B(0x48);                 /* LDA $01, PHA */
        LDA_IMM(c->bank_play); B(0x85); B(C64_ADDR_PORT);   /* STA $01: play's bank */
        B(0x20); B(f->play & 0xff); B(f->play >> 8);        /* JSR play */
        B(0x68); B(0x85); B(C64_ADDR_PORT);                 /* PLA, STA $01 */
        LDA_ABS(CIA1(CIA_ADDR_ICR));                        /* ack CIA 1 */
        LDA_IMM(VIC_IRQ_RASTER); STA_ABS(VIC(VIC_ADDR_IRQ));   /* ack the raster */
        B(0x68); B(0xa8); B(0x68); B(0xaa); B(0x68); B(0x40);   /* PLA TAY PLA TAX PLA RTI */
        irq_r = p;                                          /* via RAM $FFFE: save the registers first */
        B(0x48); B(0x8a); B(0x48); B(0x98); B(0x48);
        B(0x4c); B(irq_k & 0xff); B(irq_k >> 8);
        m[v[0]] = (uint8_t)irq_k; m[v[1]] = (uint8_t)(irq_k >> 8);   /* the vector stores above */
        m[v[2]] = (uint8_t)irq_r; m[v[3]] = (uint8_t)(irq_r >> 8);
    }
#undef VIC
#undef CIA1
#undef CIA2
#undef B
#undef LDA_IMM
#undef STA_ABS
#undef LDA_ABS
}

static uint8_t bank_for(uint16_t addr)     /* $01 for a routine at addr: the ROMs out from under it */
{
    if (addr < C64_ADDR_BASIC) return C64_PORT_CASSETTE | C64_PORT_LORAM | C64_PORT_HIRAM | C64_PORT_CHAREN;
    if (addr < C64_ADDR_IO) return C64_PORT_CASSETTE | C64_PORT_HIRAM | C64_PORT_CHAREN;   /* BASIC out */
    if (addr < C64_ADDR_KERNAL) return C64_PORT_CASSETTE | C64_PORT_CHAREN;               /* all RAM */
    return C64_PORT_CASSETTE | C64_PORT_LORAM | C64_PORT_CHAREN;                           /* I/O, no ROMs */
}

/* ------------------------------------------------------------- the C64 --- */
void c64_init(c64_t *c, uint8_t *mem)
{
    uint32_t i;
    for (i = 0; i < sizeof *c; i++) ((uint8_t *)c)[i] = 0;
    c->mem = mem;
    cpu_init(&c->cpu, mem);
    c->cpu.hook_lo = C64_ADDR_IO;
    c->cpu.hook_hi = C64_ADDR_KERNAL - 1;
    c->cpu.read_lo = C64_ADDR_BASIC;
    c->cpu.write_hook = bus_write;
    c->cpu.read_hook = bus_read;
    c->cpu.ctx = c;
    c->nsid = 1;
    c->sid_addr[0] = C64_ADDR_SID;
    c->clock_hz = C64_PAL_HZ;
    c->cycles_per_line = 63;
    c->lines = 312;
}

uint32_t c64_frame_cycles(const c64_t *c) { return (uint32_t)c->cycles_per_line * c->lines; }

void c64_load(c64_t *c, const sidfile_t *f, int song, int ntsc)
{
    uint32_t i;
    uint8_t *m = c->mem;
    for (i = 0; i < 65536; i++) m[i] = 0;
    c->ntsc = (uint8_t)(ntsc != 0);
    c->clock_hz = ntsc ? C64_NTSC_HZ : C64_PAL_HZ;
    c->cycles_per_line = ntsc ? 65 : 63;
    c->lines = ntsc ? 263 : 312;
    c->raster = 0; c->raster_cmp = 0; c->line_acc = 0;
    c->vic_en = 0; c->vic_flag = 0; c->den = VIC_CTRL1_DEN;
    for (i = 0; i < sizeof c->vic_reg; i++) c->vic_reg[i] = 0;
    c->vic_reg[VIC_ADDR_CTRL1] = 0x1b; c->vic_reg[VIC_ADDR_CTRL2] = 0xc8; c->vic_reg[VIC_ADDR_MEM] = 0x15;   /* the KERNAL's */
    c->frames = 0;
    cia_reset(&c->cia[0]);
    cia_reset(&c->cia[1]);
    c->cia[0].tod_period = c->cia[1].tod_period = c->clock_hz / (ntsc ? 60u : 50u);   /* the mains pin */
    c->nsid = sidfile_nsids(f);
    c->sid_addr[0] = C64_ADDR_SID;
    c->sid_addr[1] = f->sid2;
    c->sid_addr[2] = f->sid3;
    c->nlog = 0; c->log_lost = 0; c->run_start = 0; c->frame_cycle = 0xffffffffu;
    c->acc = 0; c->next = 1;
    c->fault = 0;
    if (song < 1) song = 1;
    if (song > f->songs) song = f->songs;
    c->song = (uint8_t)song;
    c->cia_speed = (uint8_t)sidfile_cia(f, song);
    c->has_play = (uint8_t)(!f->rsid && f->play != 0);
    /* the bank for a PSID tune's init and play: where each routine lives decides what is
     * mapped while it runs (BASIC out above $A000, I/O only above $E000), as players do */
    c->bank = f->rsid ? C64_PORT_POWER_ON : bank_for(f->init);
    c->bank_play = f->rsid ? C64_PORT_POWER_ON : bank_for(f->play);
    /* what a running KERNAL leaves in RAM that tunes read (before the tune's data, which wins):
     * no key pressed (its keyboard scan writes $40 = none to $C5 and $CB; a tune waiting for the
     * keys to be released would otherwise wait for ever), an empty keyboard buffer, no shift
     * keys, and the PAL / NTSC flag at $02A6 that tunes pick their speed by */
    m[0x00c5] = 0x40; m[0x00cb] = 0x40; m[0x00c6] = 0x00; m[0x028d] = 0x00;
    m[0x02a6] = ntsc ? 0x00 : 0x01;
    for (i = 0; i < f->data_len && f->load + i < 65536u; i++) m[f->load + i] = f->data[i];
    m[C64_ADDR_DDR] = 0x2f;                                 /* the port's directions, as the KERNAL sets them */
    m[C64_ADDR_PORT] = C64_PORT_POWER_ON;
    m[C64_ADDR_IRQ_VEC] = 0x31; m[C64_ADDR_IRQ_VEC + 1] = 0xea;   /* IRQ: the KERNAL's handler */
    m[C64_ADDR_BRK_VEC] = 0x66; m[C64_ADDR_BRK_VEC + 1] = 0xfe;   /* BRK */
    m[C64_ADDR_NMI_VEC] = 0x47; m[C64_ADDR_NMI_VEC + 1] = 0xfe;   /* NMI */
    m[C64_ADDR_NMI] = 0x43; m[C64_ADDR_NMI + 1] = 0xfe;     /* the vectors in RAM too, for a tune */
    m[C64_ADDR_RESET] = 0xe2; m[C64_ADDR_RESET + 1] = 0xfc; /* that banks the KERNAL out but keeps them */
    m[C64_ADDR_IRQ] = 0x48; m[C64_ADDR_IRQ + 1] = 0xff;
    c->driver = driver_place(f);
    driver_emit(c, f, song);
    cpu_reset(&c->cpu);
    c->cpu.pc = c->driver;
    c->cpu.cycles = 0;
    c->cpu.irq_line = 0; c->cpu.nmi_pending = 0;
}

uint32_t c64_run(c64_t *c, uint32_t cycles)
{
    uint32_t t0 = c->cpu.cycles;
    c->run_start = t0;
    c->nlog = 0;
    c->frame_cycle = 0xffffffffu;
    if (c->fault) return 0;
    c->ahead = 0;
    while ((int32_t)(c->cpu.cycles - t0) < (int32_t)cycles) {
        int cyc = cpu_step(&c->cpu);
        if (!cyc) { c->fault = -1; break; }
        c64_clock(c, (uint32_t)cyc - c->ahead);
        c->ahead = 0;
    }
    c64_flush(c);
    return c->cpu.cycles - t0;
}
