/* SPDX-License-Identifier: GPL-3.0-only */
/* The C64 environment with tunes assembled here: a PSID played by the raster interrupt at the
 * frame rate, one by the CIA timer at the rate its init sets, an RSID tune running its own
 * raster interrupt through the KERNAL chain ($0314, $EA81), memory banking keeping SID writes
 * out of the chip when I/O is banked out, an NMI from CIA 2 through the KERNAL's NMI entry, and
 * a jam reported as a fault. Also writes build/host/synth.sid, a PSID file the simulator
 * scenarios can play without HVSC. */
#include "../firmware/src/c64/c64.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

static uint8_t mem[65536], file[512];
static c64_t c64;

static void be16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }

/* a PSID v2 file around a program at $1000: init at $1000, play at `play` */
static unsigned psid(const char *magic, const uint8_t *prog, unsigned n, unsigned play, unsigned speed, unsigned flags)
{
    memset(file, 0, sizeof file);
    memcpy(file, magic, 4);
    be16(file + 4, 2); be16(file + 6, 0x7c); be16(file + 8, 0); be16(file + 0x0a, 0x1000); be16(file + 0x0c, play);
    be16(file + 0x0e, 1); be16(file + 0x10, 1); file[0x12] = 0; file[0x13] = 0; file[0x14] = 0; file[0x15] = (uint8_t)speed;
    memcpy(file + 0x16, "SiDJ test tune", 14); memcpy(file + 0x36, "tests/c64_test.c", 16); memcpy(file + 0x56, "2026", 4);
    be16(file + 0x76, flags);
    file[0x7c] = 0x00; file[0x7d] = 0x10;
    memcpy(file + 0x7e, prog, n);
    return 0x7e + n;
}

static uint32_t run_seconds(int secs)       /* run the C64 for whole seconds of its clock; returns SID writes seen */
{
    uint32_t cycles = c64.clock_hz * (uint32_t)secs, done = 0, writes = 0;
    while (done < cycles) {
        uint32_t got = c64_run(&c64, 2048);
        if (!got) break;
        done += got;
        writes += (uint32_t)c64.nlog;
    }
    return writes;
}

int main(void)
{
    sidfile_t f;
    unsigned n;
    /* ---- a PSID played at the frame rate: init sets the volume, play counts in $1100 and writes $D400 */
    static const uint8_t prog_vbi[] = {
        0xa9, 0x0f, 0x8d, 0x18, 0xd4, 0x60,                     /* $1000: LDA #$0F, STA $D418, RTS */
        0xea, 0xea, 0xea, 0xea, 0xea, 0xea, 0xea, 0xea, 0xea, 0xea,
        0xee, 0x00, 0x11, 0xad, 0x00, 0x11, 0x8d, 0x00, 0xd4, 0x60   /* $1010: INC $1100, LDA $1100, STA $D400, RTS */
    };
    n = psid("PSID", prog_vbi, sizeof prog_vbi, 0x1010, 0, 0x04);
    {
        FILE *w = fopen("build/host/synth.sid", "wb");
        if (w) { fwrite(file, 1, n, w); fclose(w); }
    }
    CHECK(sidfile_parse(&f, file, n) == 0);
    c64_init(&c64, mem);
    c64_load(&c64, &f, 1, 0);
    CHECK(c64.driver >= 0x0400 && c64.driver < 0x1000);
    CHECK(c64.has_play && !c64.cia_speed && c64.bank == 0x37);
    {
        uint32_t writes = run_seconds(2);
        printf("  VBI: %u frames, play ran %u times, %u SID writes\n", c64.frames, mem[0x1100], writes);
        CHECK(c64.frames >= 99 && c64.frames <= 101);
        CHECK(mem[0x1100] >= 99 && mem[0x1100] <= 101);
        CHECK(writes == 1u + mem[0x1100]);
        CHECK(!c64.fault);
    }
    /* NTSC: 60 frames a second */
    c64_load(&c64, &f, 1, 1);
    run_seconds(1);
    printf("  NTSC: %u frames, play ran %u times\n", c64.frames, mem[0x1100]);
    CHECK(c64.frames >= 59 && c64.frames <= 61 && mem[0x1100] >= 59 && mem[0x1100] <= 61);

    /* ---- a PSID at CIA speed whose init sets timer A to 100 Hz */
    static const uint8_t prog_cia[] = {
        0xa9, 0x0f, 0x8d, 0x18, 0xd4,                           /* $1000: LDA #$0F, STA $D418 */
        0xa9, 0x7b, 0x8d, 0x04, 0xdc, 0xa9, 0x26, 0x8d, 0x05, 0xdc,   /* LDA #<9851, STA $DC04, LDA #>9851, STA $DC05 */
        0xa9, 0x11, 0x8d, 0x0e, 0xdc, 0x60,                     /* LDA #$11, STA $DC0E, RTS */
        0xee, 0x00, 0x11, 0x60                                  /* $1015: INC $1100, RTS */
    };
    n = psid("PSID", prog_cia, sizeof prog_cia, 0x1015, 1, 0x04);
    CHECK(sidfile_parse(&f, file, n) == 0 && sidfile_cia(&f, 1));
    c64_load(&c64, &f, 1, 0);
    CHECK(c64.cia_speed);
    run_seconds(1);
    printf("  CIA: play ran %u times in a second (timer %u)\n", mem[0x1100], c64.cia[0].ta_latch);
    CHECK(mem[0x1100] >= 99 && mem[0x1100] <= 101);
    /* the KERNAL's default rate when init leaves the timer alone: 60 Hz */
    n = psid("PSID", prog_vbi, sizeof prog_vbi, 0x1010, 1, 0x04);
    CHECK(sidfile_parse(&f, file, n) == 0);
    c64_load(&c64, &f, 1, 0);
    run_seconds(1);
    printf("  CIA default: play ran %u times in a second\n", mem[0x1100]);
    CHECK(mem[0x1100] >= 59 && mem[0x1100] <= 61);

    /* ---- an RSID tune: init installs a raster interrupt through $0314, the handler ends in $EA81 */
    static const uint8_t prog_rsid[] = {
        0x78,                                                   /* $1000: SEI */
        0xa9, 0x7f, 0x8d, 0x0d, 0xdc, 0xad, 0x0d, 0xdc,         /* CIA 1 interrupts off, acknowledged */
        0xa9, 0x27, 0x8d, 0x14, 0x03, 0xa9, 0x10, 0x8d, 0x15, 0x03,   /* $0314 = $1027 */
        0xa9, 0x64, 0x8d, 0x12, 0xd0,                           /* raster line 100 */
        0xad, 0x11, 0xd0, 0x29, 0x7f, 0x8d, 0x11, 0xd0,         /* $D011 bit 7 clear */
        0xa9, 0x01, 0x8d, 0x1a, 0xd0,                           /* raster interrupt on */
        0x58, 0x60,                                             /* CLI, RTS */
        0xee, 0x00, 0x11,                                       /* $1027: INC $1100 */
        0xa9, 0x01, 0x8d, 0x19, 0xd0,                           /* ack the raster */
        0x4c, 0x81, 0xea                                        /* JMP $EA81 */
    };
    n = psid("RSID", prog_rsid, sizeof prog_rsid, 0, 0, 0x04);
    CHECK(sidfile_parse(&f, file, n) == 0 && f.rsid);
    c64_load(&c64, &f, 1, 0);
    CHECK(!c64.has_play);
    run_seconds(2);
    printf("  RSID raster: the handler ran %u times in two seconds\n", mem[0x1100]);
    CHECK(mem[0x1100] >= 99 && mem[0x1100] <= 101);
    CHECK(!c64.fault);

    /* ---- banking: with $01 = $34 a store to $D400 lands in RAM, not on the SID; $35 reaches the SID */
    c64_init(&c64, mem);
    memset(mem, 0, sizeof mem);
    mem[1] = 0x37;
    {
        static const uint8_t prog[] = {0xa9, 0x34, 0x85, 0x01, 0xa9, 0x55, 0x8d, 0x00, 0xd4,   /* LDA #$34, STA $01, LDA #$55, STA $D400 */
                                       0xa9, 0x35, 0x85, 0x01, 0xa9, 0x66, 0x8d, 0x01, 0xd4,   /* LDA #$35, STA $01, LDA #$66, STA $D401 */
                                       0xa9, 0x37, 0x85, 0x01, 0x60};
        memcpy(mem + 0x1000, prog, sizeof prog);
        c64.nlog = 0;
        CHECK(cpu_call(&c64.cpu, 0x1000, 0, 0, 0, 1000) > 0);
        CHECK(mem[0xd400] == 0x55 && mem[0xd401] == 0x00);
        CHECK(c64.nlog == 1 && c64.log[0].reg == 1 && c64.log[0].val == 0x66);
        /* reads: the KERNAL stub when banked in (an RTS everywhere else), RAM when banked out */
        mem[1] = 0x37; mem[0xe000] = 0x12;
        CHECK(c64.cpu.read_hook(&c64, 0xe000, 0) == 0x60 && c64.cpu.read_hook(&c64, 0xfffe, 0) == 0x48 && c64.cpu.read_hook(&c64, 0xffff, 0) == 0xff);
        mem[1] = 0x35;
        CHECK(c64.cpu.read_hook(&c64, 0xe000, 0) == 0x12);
        mem[1] = 0x37;
    }

    /* ---- an NMI from CIA 2 timer A (one shot) through the KERNAL's NMI entry and ($0318) */
    {
        static const uint8_t prog[] = {
            0x78,                                                   /* $1000: SEI */
            0xa9, 0x24, 0x8d, 0x18, 0x03, 0xa9, 0x10, 0x8d, 0x19, 0x03,   /* $0318 = $1024 */
            0xa9, 0x81, 0x8d, 0x0d, 0xdd,                           /* CIA 2 timer A NMI on */
            0xa9, 0x40, 0x8d, 0x04, 0xdd, 0xa9, 0x00, 0x8d, 0x05, 0xdd,   /* timer A = 64 */
            0xa9, 0x19, 0x8d, 0x0e, 0xdd,                           /* one shot, load, start */
            0x4c, 0x1f, 0x10,                                       /* $101f: JMP $101F */
            0xea, 0xea,
            0xee, 0x00, 0x11, 0xad, 0x0d, 0xdd, 0x40                /* $1024: INC $1100, LDA $DD0D, RTI */
        };
        memset(mem, 0, sizeof mem);
        mem[1] = 0x37;
        memcpy(mem + 0x1000, prog, sizeof prog);
        c64_init(&c64, mem);
        mem[1] = 0x37;
        c64.cpu.pc = 0x1000;
        c64_run(&c64, 2000);
        printf("  NMI: the handler ran %u times (one shot)\n", mem[0x1100]);
        CHECK(mem[0x1100] == 1);
        CHECK(!c64.fault);
    }

    /* ---- a jam is a fault */
    memset(mem, 0, sizeof mem);
    mem[1] = 0x37;
    mem[0x1000] = 0x02;                                             /* KIL */
    c64_init(&c64, mem);
    c64.cpu.pc = 0x1000;
    CHECK(c64_run(&c64, 100) == 0 && c64.fault == -1);
    CHECK(c64_run(&c64, 100) == 0);

    printf(fails ? "c64_test FAIL\n" : "c64_test PASS\n");
    return fails != 0;
}
