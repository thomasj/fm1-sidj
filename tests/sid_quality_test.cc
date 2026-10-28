// SPDX-License-Identifier: GPL-2.0-or-later
// How close the fast (delta-clocked) engine gets to reSID 0.16 clocked every cycle
// (SAMPLE_INTERPOLATE), on the waveforms where undersampling hurts: narrow pulses. Prints the
// RMS error relative to the cycle-exact output, for the plain fast method (reSID's own
// SAMPLE_FAST, which sid.c reproduces bit for bit) and for sid.c with pulse averaging on.
#include "../third_party/resid-0.16/sid.h"
#include "../sid/sid.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const int N = 44100;
static short truth[44100], fast[44100], avg[44100];

static void program(SID *ref, sid_t *mine, int pw, int freq, int wave)
{
    unsigned char regs[25] = {0};
    regs[0] = freq & 0xff; regs[1] = freq >> 8; regs[2] = pw & 0xff; regs[3] = pw >> 8;
    regs[4] = (unsigned char)(wave << 4 | 1); regs[5] = 0x00; regs[6] = 0xf0;
    regs[0x18] = 0x0f;
    for (int r = 0; r < 25; r++) { if (ref) ref->write(r, regs[r]); if (mine) sid_write(mine, r, regs[r]); }
}

static double rel_rms(const short *x, const short *t, int n)
{
    double e = 0, s = 0;
    for (int i = 2000; i < n; i++) { double d = (double)x[i] - t[i]; e += d * d; s += (double)t[i] * t[i]; }
    return sqrt(e / s);
}

/* amplitude flutter: the standard deviation of the 10 ms RMS over its mean, in percent */
static double flutter(const short *x, int n)
{
    double m = 0, m2 = 0; int k = 0;
    for (int i = 2205; i + 441 <= n; i += 441) {
        double e = 0;
        for (int j = 0; j < 441; j++) e += (double)x[i + j] * x[i + j];
        e = sqrt(e / 441); m += e; m2 += e * e; k++;
    }
    m /= k; m2 /= k;
    return 100 * sqrt(m2 - m * m > 0 ? m2 - m * m : 0) / m;
}

/* harmonic levels: mean |dB difference| from the truth over the harmonics below 18 kHz */
static double harm_err(const short *x, const short *t, int n, int freq_reg)
{
    double f0 = freq_reg * 985248.0 / 16777216.0, err = 0; int k = 0;
    for (int h = 1; h * f0 < 18000 && h <= 48; h++) {
        double xr = 0, xi = 0, tr = 0, ti = 0;
        for (int i = 4410; i < n; i++) {
            double a = 2 * M_PI * h * f0 * i / 44100.0, c = cos(a), sn = sin(a);
            xr += x[i] * c; xi += x[i] * sn; tr += t[i] * c; ti += t[i] * sn;
        }
        double lx = 10 * log10(xr * xr + xi * xi + 1), lt = 10 * log10(tr * tr + ti * ti + 1);
        err += fabs(lx - lt); k++;
    }
    return err / k;
}

/* inter-harmonic noise: the energy away from the harmonics (where aliasing lands), relative to
 * the whole, in dB; a 32768-point Hann-windowed FFT, harmonics masked +-4 bins */
static void fft(double *re, double *im, int n)
{
    for (int i = 1, j = 0; i < n; i++) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) { double t = re[i]; re[i] = re[j]; re[j] = t; t = im[i]; im[i] = im[j]; im[j] = t; }
    }
    for (int len = 2; len <= n; len <<= 1) {
        double ang = -2 * M_PI / len, wr = cos(ang), wi = sin(ang);
        for (int i = 0; i < n; i += len) {
            double cr = 1, ci = 0;
            for (int j = 0; j < len / 2; j++) {
                double ur = re[i + j], ui = im[i + j];
                double vr = re[i + j + len / 2] * cr - im[i + j + len / 2] * ci;
                double vi = re[i + j + len / 2] * ci + im[i + j + len / 2] * cr;
                re[i + j] = ur + vr; im[i + j] = ui + vi;
                re[i + j + len / 2] = ur - vr; im[i + j + len / 2] = ui - vi;
                double t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
            }
        }
    }
}
static double noise_db(const short *x, int freq_reg)
{
    static double re[32768], im[32768];
    const int n = 32768;
    double f0 = freq_reg * 985248.0 / 16777216.0, total = 0, noise = 0;
    for (int i = 0; i < n; i++) {
        double w = 0.5 - 0.5 * cos(2 * M_PI * i / n);
        re[i] = x[4000 + i] * w; im[i] = 0;
    }
    fft(re, im, n);
    for (int b = 1; b < n / 2; b++) {
        double p = re[b] * re[b] + im[b] * im[b], fb = b * 44100.0 / n;
        double h = fb / f0, dist = fabs(h - floor(h + 0.5)) * f0 * n / 44100.0;   /* bins to the nearest harmonic */
        total += p;
        if (dist > 4) noise += p;
    }
    return 10 * log10(noise / total);
}

static int run(const char *name, int pw, int freq, int wave, double *out_fast, double *out_avg)
{
    SID ref;
    ref.set_chip_model(MOS8580);
    ref.set_sampling_parameters(985248, SAMPLE_INTERPOLATE, 44100);
    ref.reset();
    program(&ref, 0, pw, freq, wave);
    for (int i = 0; i < N;) { cycle_count d = 100000; i += ref.clock(d, truth + i, N - i); }

    SID ref_fast;
    ref_fast.set_chip_model(MOS8580);
    ref_fast.set_sampling_parameters(985248, SAMPLE_FAST, 44100);
    ref_fast.reset();
    program(&ref_fast, 0, pw, freq, wave);
    for (int i = 0; i < N;) { cycle_count d = 100000; i += ref_fast.clock(d, fast + i, N - i); }

    sid_t mine;
    sid_init(&mine);
    sid_set_model(&mine, SID_MOS8580);
    sid_set_sampling(&mine, 985248, 44100);
    sid_set_averaging(&mine, 1);
    program(0, &mine, pw, freq, wave);
    sid_render(&mine, avg, N);

    *out_fast = harm_err(fast, truth, N, freq);
    *out_avg = harm_err(avg, truth, N, freq);
    printf("  %-16s harmonics off by: fast %4.1f dB  avg %4.1f dB | noise between them: truth %5.1f dB  fast %5.1f dB  avg %5.1f dB | flutter: truth %3.1f %%  fast %3.1f %%  avg %3.1f %%\n",
           name, *out_fast, *out_avg, noise_db(truth, freq), noise_db(fast, freq), noise_db(avg, freq),
           flutter(truth, N), flutter(fast, N), flutter(avg, N));
    return 0;
}

int main()
{
    double f, a, worst_fast = 0, worst_avg = 0;
    printf("sid_quality_test: the fast engine vs reSID 0.16 clocked every cycle (the truth)\n");
    struct { const char *name; int pw, freq, wave; } cases[] = {
        {"pulse 50 %, A4", 0x800, 7493, 4},
        {"pulse 12 %, A4", 0x200, 7493, 4},
        {"pulse 3 %, A4", 0x080, 7493, 4},
        {"pulse 1.5 %, A4", 0x040, 7493, 4},
        {"pulse 1.5 %, A5", 0x040, 14985, 4},
        {"pulse 97 %, A4", 0xf80, 7493, 4},
        {"saw, A4", 0x800, 7493, 2},
        {"saw, A2", 0x800, 1873, 2},
        {"saw, A6", 0x800, 29970, 2},
        {"triangle, A4", 0x800, 7493, 1},
    };
    int ok = 1;
    for (auto &c : cases) {
        run(c.name, c.pw, c.freq, c.wave, &f, &a);
        if (c.wave == 4) { if (f > worst_fast) worst_fast = f; if (a > worst_avg) worst_avg = a; }
        if (c.wave == 4 || c.wave == 2)               /* averaged pulse and saw: less noise than the truth */
            ok &= noise_db(avg, c.freq) <= noise_db(truth, c.freq) + 0.5;
    }
    ok &= worst_avg < worst_fast / 2;
    printf("  worst pulse harmonic error: fast %.1f dB, averaged %.1f dB\n", worst_fast, worst_avg);
    printf(ok ? "PASS\n" : "FAIL (averaging should halve the worst pulse error and not add noise)\n");
    return !ok;
}
