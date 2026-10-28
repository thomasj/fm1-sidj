// SPDX-License-Identifier: GPL-2.0-or-later
// Bit-exact comparison of the C SID engine (sid/sid.c) with the original
// reSID 0.16 (third_party/resid-0.16), both driven with the same register
// writes and clocked with the same SAMPLE_FAST method.
#include "../third_party/resid-0.16/sid.h"   // reSID 0.16 reference
#include "../sid/sid.h"                        // the C SID engine
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// reSID 0.16 lives in the global namespace with a class also called SID;
// the C engine's struct is sid_t, so both can be included together.

static uint32_t rng_state = 0x12345678;
static uint32_t rnd() { rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5; return rng_state; }

static int compare_run(chip_model model, int sample_hz, int frames, int seed, int frame_cycles, bool random_regs)
{
    rng_state = seed;
    SID ref;
    ref.set_chip_model(model);
    ref.set_sampling_parameters(985248, SAMPLE_FAST, sample_hz);
    ref.reset();

    sid_t mine;
    sid_init(&mine);
    sid_set_model(&mine, model == MOS6581 ? SID_MOS6581 : SID_MOS8580);
    sid_set_sampling(&mine, 985248, sample_hz);
    sid_reset(&mine);

    // A plausible starting patch on all three voices so that something sounds.
    static const unsigned char patch[25] = {
        0x16, 0x0b, 0x00, 0x08, 0x41, 0x09, 0xa8,   // voice 1: pulse
        0x2c, 0x16, 0x00, 0x00, 0x21, 0x0a, 0xc9,   // voice 2: saw
        0x58, 0x2c, 0x00, 0x00, 0x81, 0x00, 0xf0,   // voice 3: noise
        0x00, 0x40, 0xf7, 0x1f };                   // filter: cutoff, res + all voices, LP + vol 15
    for (int r = 0; r < 25; r++) { ref.write(r, patch[r]); sid_write(&mine, r, patch[r]); }

    short ref_buf[4096], my_buf[4096];
    long total = 0, mismatches = 0;
    for (int fr = 0; fr < frames; fr++) {
        if (random_regs) {
            int writes = rnd() % 4;
            for (int w = 0; w < writes; w++) {
                unsigned char r = rnd() % 0x19, v = rnd() & 0xff;
                ref.write(r, v); sid_write(&mine, r, v);
            }
        }
        cycle_count d1 = frame_cycles; int d2 = frame_cycles;
        int n1 = ref.clock(d1, ref_buf, 4096);
        int n2 = sid_clock_render(&mine, &d2, my_buf, 4096, 1);
        if (n1 != n2 || d1 != d2) { printf("  frame %d: sample count %d vs %d, rest %d vs %d\n", fr, n1, n2, d1, d2); return 1; }
        for (int i = 0; i < n1; i++) {
            total++;
            if (ref_buf[i] != my_buf[i]) {
                if (mismatches < 5) printf("  frame %d sample %d: ref %d mine %d\n", fr, i, ref_buf[i], my_buf[i]);
                mismatches++;
            }
        }
        // Readable registers must agree too.
        for (int r = 0x19; r <= 0x1c; r++)
            if (ref.read(r) != sid_read(&mine, r)) { printf("  frame %d: read $%02x differs\n", fr, r); return 1; }
    }
    printf("  %s %d Hz %s: %ld samples, %ld mismatches\n", model == MOS6581 ? "6581" : "8580", sample_hz,
           random_regs ? "random writes" : "static patch", total, mismatches);
    return mismatches != 0;
}

int main()
{
    int fail = 0;
    printf("sid_ref_test: the C SID engine vs reSID 0.16\n");
    fail |= compare_run(MOS6581, 44100, 200, 1, 19705, false);
    fail |= compare_run(MOS8580, 44100, 200, 2, 19705, false);
    fail |= compare_run(MOS6581, 44100, 2000, 3, 19705, true);
    fail |= compare_run(MOS8580, 48000, 2000, 4, 19705, true);
    fail |= compare_run(MOS6581, 32000, 2000, 5, 1000, true);
    printf(fail ? "FAIL\n" : "PASS\n");
    return fail;
}
