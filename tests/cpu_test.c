/* SPDX-License-Identifier: GPL-3.0-only */
/* The 6510 core against Klaus Dormann's 6502 functional test (GPL-3.0-or-later,
 * third_party/6502_functional_test): the binary loads at $0000, starts at $0400 and loops on
 * itself at the success address when every test has passed; any other self-loop is a failure
 * whose address names the test in the .lst file. Also checks cpu_call() and the SID write hook. */
#include "../firmware/src/c64/cpu6502.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t mem[65536];
static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static int hook_calls;
static uint16_t hook_addr;
static uint8_t hook_val;
static void whook(void *ctx, uint16_t addr, uint8_t v, uint32_t cycle) { (void)ctx; (void)cycle; hook_calls++; hook_addr = addr; hook_val = v; }
static uint8_t rhook(void *ctx, uint16_t addr, uint32_t cycle) { (void)ctx; (void)cycle; return addr == 0xd41b ? 0x5a : 0; }

int main(void)
{
    cpu6502_t c;
    FILE *f = fopen("third_party/6502_functional_test/6502_functional_test.bin", "rb");
    if (!f || fread(mem, 1, 65536, f) != 65536) { printf("cannot read the functional test binary\n"); return 1; }
    fclose(f);
    cpu_init(&c, mem);
    c.pc = 0x0400;
    {
        uint16_t last = 0;
        unsigned long steps = 0;
        for (;;) {
            last = c.pc;
            if (!cpu_step(&c)) { printf("  jammed at $%04x (opcode %02x)\n", c.pc, mem[c.pc]); fails++; break; }
            steps++;
            if (c.pc == last) break;         /* a self-loop: success or a trapped failure */
            if (steps > 200000000UL) { printf("  did not finish\n"); fails++; break; }
        }
        printf("  functional test stopped at $%04x after %lu instructions, %u cycles\n", c.pc, steps, c.cycles);
        CHECK(c.pc == 0x3469);               /* the success loop of the stock build */
    }

    /* cpu_call: a routine that stores A to $D418 (hooked), reads $D41B (hooked) into X, RTS */
    memset(mem, 0, sizeof mem);
    {
        static const uint8_t prog[] = { 0x8d, 0x18, 0xd4, 0xae, 0x1b, 0xd4, 0x60 };   /* STA $D418; LDX $D41B; RTS */
        memcpy(mem + 0x1000, prog, sizeof prog);
        cpu_init(&c, mem);
        c.hook_lo = 0xd400; c.hook_hi = 0xd41c; c.read_lo = 0xd400; c.write_hook = whook; c.read_hook = rhook;
        int cyc = cpu_call(&c, 0x1000, 0x0f, 0, 0, 1000);
        CHECK(cyc == 4 + 4 + 6);
        CHECK(hook_calls == 1 && hook_addr == 0xd418 && hook_val == 0x0f);
        CHECK(c.x == 0x5a);
        CHECK(c.sp == 0xfd);
        /* a routine that never returns is cut off */
        mem[0x2000] = 0x4c; mem[0x2001] = 0x00; mem[0x2002] = 0x20;                 /* JMP $2000 */
        CHECK(cpu_call(&c, 0x2000, 0, 0, 0, 500) == -1);
    }
    printf(fails ? "cpu_test FAIL\n" : "cpu_test PASS\n");
    return fails != 0;
}
