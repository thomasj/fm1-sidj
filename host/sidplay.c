/* SPDX-License-Identifier: GPL-3.0-only */
/* SiDJ on the host: play a .sid file through the C64 and the SID engine to a WAV, and
 * report what happened (frames, interrupts, SID writes, faults). The player's correctness tests
 * use it; so does listening on the Mac.
 *
 *   sidplay FILE.sid [-s SONG] [-t SECONDS] [-n] [-m 6581|8580] [-o OUT.wav] [-l] [-q] [-f]
 *     -n NTSC;  -l log every SID write (cycle, chip, register, value) to stdout;  -q no WAV;
 *     -f the SID's filter off (to compare with other emulations' unfiltered output) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../firmware/src/c64/c64.h"
#include "../firmware/src/c64/c64regs.h"
#include "../firmware/src/c64/sidreg.h"
#include "../sid/sid.h"

static uint8_t mem[65536];
static c64_t c64;
static sid_t sids[C64_NSID];

static uint8_t sidr(void *ctx, int chip, uint8_t reg) { (void)ctx; return sid_read(&sids[chip], reg); }

static void wav_header(FILE *f, uint32_t frames)
{
    uint32_t bytes = frames * 4, v;
    uint16_t h[2];
    fseek(f, 0, SEEK_SET);
    fwrite("RIFF", 1, 4, f); v = 36 + bytes; fwrite(&v, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
    v = 16; fwrite(&v, 4, 1, f);
    h[0] = 1; h[1] = 2; fwrite(h, 2, 2, f);
    v = 44100; fwrite(&v, 4, 1, f); v = 44100 * 4; fwrite(&v, 4, 1, f);
    h[0] = 4; h[1] = 16; fwrite(h, 2, 2, f);
    fwrite("data", 1, 4, f); fwrite(&bytes, 4, 1, f);
}

int main(int argc, char **argv)
{
    const char *path = 0, *out = "sidplay.wav";
    int song = 0, seconds = 30, ntsc = -1, model = 0, log = 0, quiet = 0, nofilter = 0, i, rc;
    uint8_t *buf;
    long len;
    FILE *f, *w = 0;
    sidfile_t sf;
    uint32_t writes = 0, frames_out = 0, chunk_len = 0, pos = 0, log_i = 0, nlog = 0, c0 = 0;
    uint32_t sid_writes_by_reg[SID_NREG] = {0};
    int16_t tmp[256];
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-s") && i + 1 < argc) song = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) seconds = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else if (!strcmp(argv[i], "-m") && i + 1 < argc) model = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-n")) ntsc = 1;
        else if (!strcmp(argv[i], "-l")) log = 1;
        else if (!strcmp(argv[i], "-q")) quiet = 1;
        else if (!strcmp(argv[i], "-f")) nofilter = 1;
        else path = argv[i];
    }
    if (!path) { fprintf(stderr, "usage: sidplay FILE.sid [-s SONG] [-t SECONDS] [-n] [-m 6581|8580] [-o OUT.wav] [-l] [-q]\n"); return 2; }
    f = fopen(path, "rb");
    if (!f) { perror(path); return 2; }
    fseek(f, 0, SEEK_END); len = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)len);
    if (fread(buf, 1, (size_t)len, f) != (size_t)len) { fprintf(stderr, "read error\n"); return 2; }
    fclose(f);
    rc = sidfile_parse(&sf, buf, (uint32_t)len);
    if (rc) { fprintf(stderr, "%s: not a usable SID file (%d)\n", path, rc); return 1; }
    if (!song) song = sf.start_song;
    if (ntsc < 0) ntsc = sidfile_clock(&sf) == SID_CLOCK_NTSC;
    printf("%s\n  %s / %s / %s\n  %s v%d, load $%04x init $%04x play $%04x, %d song(s), start %d, %s, %s, %d SID(s)%s\n",
           path, sf.name, sf.author, sf.released, sf.rsid ? "RSID" : "PSID", sf.version, sf.load, sf.init, sf.play,
           sf.songs, sf.start_song, sidfile_cia(&sf, song) ? "CIA" : "VBI",
           ntsc ? "NTSC" : "PAL", sidfile_nsids(&sf), sf.basic ? ", BASIC (unsupported)" : "");
    c64_init(&c64, mem);
    c64.sid_read = sidr;
    for (i = 0; i < C64_NSID; i++) {
        int m = model ? model : (sidfile_model(&sf, i) == SID_MODEL_8580 ? 8580 : 6581);
        sid_init(&sids[i]);
        sid_set_model(&sids[i], m == 8580 ? SID_MOS8580 : SID_MOS6581);
        sid_set_sampling(&sids[i], ntsc ? C64_NTSC_HZ : C64_PAL_HZ, 44100);
        sid_set_averaging(&sids[i], 1);
        sid_set_floating_dac(&sids[i], 1);
        if (nofilter) sid_enable_filter(&sids[i], 0);
    }
    c64_load(&c64, &sf, song, ntsc);
    printf("  driver at $%04x, bank $%02x, song %d\n", c64.driver, c64.bank, song);
    if (!quiet) { w = fopen(out, "wb"); if (w) { static const uint8_t z[44]; fwrite(z, 1, 44, w); } }
    while (frames_out < (uint32_t)seconds * 44100u) {
        uint32_t target;
        int d, got, want = 128;
        if (pos >= chunk_len) {
            chunk_len = c64_run(&c64, 2048);
            if (!chunk_len) { printf("  FAULT: the CPU jammed at $%04x after %u frames\n", c64.cpu.pc, c64.frames); break; }
            nlog = (uint32_t)c64.nlog; log_i = 0; pos = 0; c0 = c64.run_start;
        }
        while (log_i < nlog && c64.log[log_i].cycle <= pos) {
            const c64_write_t *x = &c64.log[log_i++];
            sid_write(&sids[x->chip], x->reg, x->val);
            writes++;
            sid_writes_by_reg[x->reg]++;
            if (log) printf("%10u %d $%02x %02x\n", c0 + x->cycle, x->chip, x->reg, x->val);
        }
        target = log_i < nlog ? c64.log[log_i].cycle : chunk_len;
        d = (int)(target - pos);
        got = sid_clock_render(&sids[0], &d, tmp, want, 1);
        for (i = 1; i < c64.nsid; i++) { int d2 = (int)(target - pos); sid_clock_render(&sids[i], &d2, tmp + 128, want, 1); }
        pos = target - (uint32_t)d;
        if (w) {
            for (i = 0; i < got; i++) {
                int16_t s[2];
                int l = tmp[i], r = tmp[i];
                if (c64.nsid >= 2) { l = tmp[i]; r = tmp[128 + i]; }
                s[0] = (int16_t)l; s[1] = (int16_t)r;
                fwrite(s, 2, 2, w);
            }
        }
        frames_out += (uint32_t)got;
    }
    if (w) { wav_header(w, frames_out); fclose(w); }
    printf("  %u frames, %u cycles, %u SID writes (%u lost), raster line %u, CIA1 A latch %u (%s)\n",
           c64.frames, c64.cpu.cycles, writes, c64.log_lost, c64.raster, c64.cia[0].ta_latch,
           (c64.cia[0].cra & CIA_CR_START) ? "running" : "stopped");
    {
        unsigned v, freq = 0, pw = 0, filt = 0;
        for (v = 0; v < SID_NVOICE; v++) {
            freq += sid_writes_by_reg[SID_ADDR_VOICE(v) + SID_ADDR_FREQ_LO] + sid_writes_by_reg[SID_ADDR_VOICE(v) + SID_ADDR_FREQ_HI];
            pw += sid_writes_by_reg[SID_ADDR_VOICE(v) + SID_ADDR_PW_LO] + sid_writes_by_reg[SID_ADDR_VOICE(v) + SID_ADDR_PW_HI];
        }
        for (v = SID_ADDR_FC_LO; v <= SID_ADDR_MODE_VOL; v++) filt += sid_writes_by_reg[v];
        printf("  writes: ctrl %u %u %u, freq %u, vol/filter %u, pw %u\n",
               sid_writes_by_reg[SID_ADDR_VOICE(0) + SID_ADDR_CTRL], sid_writes_by_reg[SID_ADDR_VOICE(1) + SID_ADDR_CTRL],
               sid_writes_by_reg[SID_ADDR_VOICE(2) + SID_ADDR_CTRL], freq, filt, pw);
    }
    if (w) printf("  -> %s\n", out);
    free(buf);
    return c64.fault ? 1 : 0;
}
