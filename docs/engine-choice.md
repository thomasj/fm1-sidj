# Why the engine is a port of reSID 0.16 and not libresidfp

The obvious candidate for the SID engine was libresidfp (reSIDfp), the most accurate open
emulator of the chip. The FM-1's CPU (JieLi AC79, pi32v2 core) decided against it: reSIDfp and
the older reSID differ exactly in how much floating point and memory they need.

## The target

- pi32v2: a 32-bit core at 360 MHz with a single-precision FPU and no double precision
  ([platform.md](platform.md); the FPU was found only later, and it changes nothing below, since
  reSIDfp's filter model is double precision). Integer multiply and 32x32-to-64 multiply are
  single instructions. A divide is one instruction too, but slow: in the per-sample code the
  waveform averages' divides turned out to be the SID engine's largest single cost on the FM-1.
- RAM is a few hundred kilobytes; code and constant tables run from flash (XIP).
- Audio out at 44.1 kHz, so one SID clock (985248 Hz PAL) is about 22.3 cycles per sample.

## libresidfp (reSIDfp 1.3.0a) as it is

Measured and read from the source:

| | |
| --- | --- |
| Filter model tables | both models built at start: about 21.9 MB of RAM (mixer 3.5 MiB, summer 2.5 MiB, volume 2 MiB, resonance 2 MiB per model, plus opamp and 6581 transistor tables); 5.2 million Newton-Raphson solves in `double` at startup |
| Per SID cycle | about 195 to 215 integer ops plus 27 (8580) to 45 (6581) floating-point ops, 11 virtual calls, 15 random lookups into multi-megabyte tables |
| Clocking | one cycle at a time only; `clock(delta_t)` was removed. Pipelines (noise shift, pulse delay, envelope LFSR) assume single-cycle steps |
| Resampler | two-pass sinc, 97 to 153 multiply-accumulates per SID cycle |
| Host cost | 20 to 34 ns per SID cycle on an Apple M4 Pro (2 to 3 percent of one core) |
| C++ | C++11 or later, exceptions, `std::thread`, `std::map`, smart pointers, `<cmath>` incl. `tanh` per clipped sample |

On a 360 MHz in-order core without double precision that is 20 to 50 times over budget, and the
tables do not fit in any case. Making it fit would mean: build only one model, precompute and
downsample the opamp tables into flash, rewrite the ~40 FP glue ops in fixed point, drop the
sinc resampler, and then still run every one of the ~985000 cycles per second at ~200 integer
ops each. That is a new engine that borrows reSIDfp's tables, not a port.

## reSID 0.16 (Dag Lem, 2004)

- Entirely integer: 24-bit accumulators, 15-bit rate counters, an integer state-variable
  filter (`w0` and `1024/Q` tables, 14-bit fixed point), 8-bit sampled combined-waveform
  tables (8 x 4 KB), 2 x 2048-entry cutoff tables.
- Delta clocking: `clock(delta_t)` on the waveform generator (accumulator += freq * delta,
  counting bit-19 edges for the noise shifts), on the envelope (rate-counter arithmetic) and
  on the filter (steps of at most 8 cycles for stability). Its `SAMPLE_FAST` mode clocks the
  chip once per output sample. This is what made reSID real-time on 1990s PCs.
- Cost in the port: about 2.5 KB of pi32v2 code as first ported (about 6 KB now, with SiDJ's
  averaging), under 1 KB of state, 200 KB of constant tables (could be 8-bit packed smaller),
  and on the order of a few hundred integer ops per output sample, so tens of MIPS at 44.1 kHz
  rather than hundreds.
- What it lacks compared with reSIDfp: the transistor-level 6581 filter distortion, noise
  combined with other waveforms (returns 0), the cycle-exact pipelines, and the anti-aliased
  resampling. For a player these are refinements, not blockers.

## Decision

`sid/sid.c` is a C port of reSID 0.16's delta-cycle paths, kept structurally identical to the
original so that `tests/sid_ref_test.cc` can check it sample for sample against the vendored
C++ (`third_party/resid-0.16`). It passes bit-exactly on both chip models with random register
writes at 32, 44.1 and 48 kHz.

## Since then

Done on the device:

- **Averaging against aliasing.** The sawtooth and the pulse are averaged exactly over each
  sample's cycles, and the combined waveforms over the table positions the sample went
  through, from running sums of the tables in RAM (32 KB; two look-ups a run). Taken once a
  sample, the spiky tables popped audibly (The Last Ninja, The Elephant, Bogstihyde). Measured
  on the FM-1 with all three voices on combined waveforms: once a sample 56-64 % of the audio
  budget, a look-up every other cycle 67-75 %, the running sums 61-71 %. Above 85 % load the
  combined-waveform averaging steps aside, and it comes back below 75 %. Each voice takes only
  the average its waveform shows, and a step that crosses no pulse edge needs no divide.
- **The 8580's floating waveform DAC**, after reSIDfp's measurements (`sid_set_floating_dac`,
  off by default to keep the test bit-exact), for tunes that play samples with the test bit.

Still open, in order of value per effort:

1. Noise combined with other waveforms, and the improved combined-waveform tables, from
   reSID 1.0-pre (`github.com/daglem/reSID`, GPL-2.0-or-later); its wave and envelope
   `clock(delta_t)` paths are compatible with this engine's structure.
2. A better 6581 filter curve or a fixed-point port of reSIDfp's 8580 integrator, which is
   integer apart from one scaling factor, if the CPU budget allows per-cycle filter clocking.
3. A cheap integer decimator (the chip clocked at 2x or 4x the sample rate, then a short
   half-band filter), if bright tunes still alias audibly despite the averaging.
