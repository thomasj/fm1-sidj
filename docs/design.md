# SiDJ design

What the firmware is, what it is built from, how it works, and the decisions taken along the
way. Nothing here comes from the device's vendor or from anyone's employer. The user manual
is the README; the FM-1 itself is described in [platform.md](platform.md), and the choice of
SID engine in [engine-choice.md](engine-choice.md).

## Goal

The FM-1 as a SID file player. A .sid file (PSID or RSID, versions 1 to 4, as in the High
Voltage SID Collection) is loaded into an emulated C64's memory, its init and play routines
run on an emulated 6510, and every write the tune makes to the SID is sent, with its cycle,
to an *output*:

| Output | Transport |
| --- | --- |
| emulated SID (a fixed-point port of reSID 0.16) | the FM-1's audio jack, and USB audio to a computer: the mix, or the mix and each voice |
| ASID (SysEx, one message per chip and frame) | USB-MIDI to real SID hardware (a SidStation, a TherapSID, a TeensyROM, a USBSID-Pico) through a MIDI host |
| both at once | the emulation for monitoring, ASID to the real chip |
| SID+CLK: the emulated SID and a MIDI clock | the audio jack and USB audio as above, and a MIDI clock in time with the tune on USB-MIDI |

On the way to the output the writes pass through the *remix*: the panel's overrides of pitch,
waveform, envelope, pulse width and filter ([README, Remix](../README.md#remix)).

## What it is built from

| Piece | Origin | Licence |
| --- | --- | --- |
| Platform: linker script, crt0, HAL headers, lcd, gfx, libc, usb (device), midi_uart, storage, ota, the update loader, the package tools and the installer | [Felucca](https://github.com/hugelton/Felucca) by way of [X0X](https://github.com/charlesvestal/fm1-x0x) | GPL-3.0-only |
| Boot, ISRs, main loop, panel map, the `plat.h` boundary, the host simulator and the browser emulator pattern | after X0X | GPL-3.0-only |
| SID engine `sid/` (reSID 0.16 in fixed-point C, bit-exact test against the original) | written for the FM-1 | GPL-2.0-or-later |
| Everything else: the 6510, the C64, the SID file loader, the player, the remix, ASID, the song store, the UI | written for SiDJ | GPL-3.0-only |

[LICENSING.md](../LICENSING.md) has the file-by-file list.

## The player environment

A SID player has to cope with anything in HVSC, so the C64 is more than a routine called once
per frame:

- **Interrupts in the 6510 core**: IRQ and NMI lines, the I flag, the vectors at $FFFA /
  $FFFE. RSID tunes return from init and live in their own interrupt handlers; PSID tunes
  are called by the player, but many install an IRQ handler of their own anyway.
- **Memory banking**: $00 / $01 and the KERNAL / BASIC / I/O / character ROM windows, so
  that a tune's writes to $D400 reach the SID only when I/O is banked in, and reads of the
  ROM areas return something sensible.
- **A minimal KERNAL replacement**, written for this project (no ROM images): reset and
  interrupt vectors, an IRQ entry that saves the registers and jumps through $0314, an NMI
  entry that returns, the default $0314 / $0318 handlers that acknowledge the CIA and VIC
  and RTI, and the few zero-page and vector defaults tunes rely on. Any other KERNAL address
  reads as RTS, so a tune that calls a KERNAL routine gets a harmless return. BASIC tunes are
  refused.
- **CIA 1 and 2 timers and the VIC raster interrupt**: a CIA-speed PSID tune is called at the
  timer's rate (the KERNAL's default of about 60 Hz unless init changes it, multi-speed tunes
  up to at least 38x), a raster tune at the frame rate; RSID tunes get both as real
  interrupts. The VIC's bad lines stall the CPU.
- **Cycle placement**: an I/O access happens at its own cycle in the instruction, so a timer
  read is what a real CIA shows then; an NMI that arrives in an instruction's last cycle, or
  from a store to CIA 2, waits one more instruction, as on the 6510; RTS and RTI make their
  dummy read, and CIA 2's serial register keeps what was written. NMI sample players depend on
  all of it (an RTI kept at $DD0C acknowledges the CIA by reading $DD0D).
- **PAL and NTSC**: 985248 Hz and 19656 cycles per frame, or 1022727 Hz and 17095. The
  SID's sampling follows the clock. The file's flags choose.
- **One SID**: 2SID and 3SID tunes are refused when added and when played. Measured on the
  FM-1 (October 2026), the 6510 and the C64 cost about 25 % of the audio budget and each
  emulated SID about 28 %: one SID ran at about 53 %, two at 81 to 93 % with peaks over 100 %,
  three overloaded it.
  The C64 core still maps a second and third SID (the host tools use them for the reference
  check), but the firmware never loads such a file.

The C64 runs continuously in cycle-counted chunks of 2048 cycles rather than being called
once per frame: PSID and RSID tunes share one path, a driver assembled into free RAM calling
play from the interrupt the file asks for. Time passes lazily: the CIAs and the VIC catch up
at the next event, or at an I/O access's own cycle.

## Outputs

A chunk's output is the write log, `(cycle, chip, register, value)` in order, after the remix.

- **Emulated**: each write lands on the emulated SID at its cycle while the samples between
  are rendered, 24-bit stereo out of the I2S buffer.
- **USB audio**: the same samples as a class-compliant UAC1 input, 16-bit at 44.1 kHz,
  resampled to the computer's USB clock so nothing drifts. USB:MIX sends the mix; USB:5CH adds
  the three voices as stems (each voice's share of the filter is kept, so the three add up to
  the mix). It comes at the chip's own level, before MASTER.
- **ASID** (`firmware/src/app/asid.c`): the writes of a frame packed into one SysEx per chip
  after the ASID protocol (<https://github.com/thomasj/asid-protocol>): masks, MSBs and data
  in ASID register order (the sink can also send 0x50 / 0x51 for a second and third chip,
  unused since SiDJ plays one SID), 0x32 with the chip's model at load, 0x4C / 0x4D at play /
  pause. Only registers that changed since the receiver last got them are sent; a gate off
  and on within a frame uses the second control slot, and a third value flushes early. The
  frame ends exactly at the raster wrap (the C64 reports the cycle). A resume after a pause
  sends the whole image, since receivers reset at 0x4D. RSID tunes are never sent (they drive
  the whole C64 at their own cycles, often with samples); they play on the emulated SID.

## The MIDI clock

The full behaviour is in [midi.md](midi.md#midi-clock); the design in short:

- **The beat comes from the tune** (`firmware/src/app/beat.c`): at load the C64 runs ahead
  silently, the player's calls are counted by their bursts of writes, and each voice's note
  starts are noted by call. The grid is the longest row (or swing pair of rows) nearly all note
  starts land on; the beat is four rows or, for a tune in threes, six; halved or doubled into
  70..180 BPM. The tune then starts over, so the clock begins with its first row.
- **Pulses at their cycles**: the engine puts each pulse into the chunk's write log at its tune
  cycle, so it is rendered at the sample it belongs to. TIMER5 sends each byte when that sample
  plays, not when the block is rendered, and USB carries it at its next poll.
- **Kept clocks** (`clkstore.c`): the tempo found, its ratio and offset, or a tempo set by hand,
  per song and subsong by MD5, in a flash object of their own, written at the next quiet moment.
- **Following a clock**: incoming pulses are timestamped as they arrive. The tune's own pulses
  (not sent) are paired with them by number; the tune's speed is the ratio of the two spacings,
  steered by the timing error, so its beats land on the clock's. The incoming spacing is measured
  over a bar against USB's jitter.

## Songs on the device

HVSC files are mostly 2 to 20 KB. The store is a pool of 384 KB with a small index: name, author, released
(the header's own strings), songs, default song, the song lengths per subsong, and the file. The
index and the files are read in place through the XIP window. The pool is append-only with an
in-place delete flag; compacting fetches, wipes and re-sends. The firmware never moves data.

| Flash | Size | What |
| --- | --- | --- |
| 0x93000-0xF2FFF | 384 KB | the songs |
| 0xF3000-0xF4FFF | 8 KB | the kept clocks |
| 0xF5000-0xF6FFF | 8 KB | the panel calibration |
| 0xF7000-0xFBFFF | 20 KB | the update loader's staging |
| 0xFC000-0xFDFFF | 8 KB | the settings |

Up to 1.3 the pool was 0xA0000-0xDBFFF, with the clocks at 0x97000, the calibration at 0xDC000
and the staging at 0xE0000. A new boot copies the clocks and the calibration up and erases a loader
left at the old place; songs stay at 0xA0000 until the pool is compacted.

From 0xEA000 the flash is M-VAVE's patch area. The pool's last 36 KB, the clocks, the calibration
and the staging are all inside it: the price of one continuous pool. SiDJ does not ask before using
it, since installing custom firmware is at the user's own risk, and the README says saved patches
can be lost.

The staging's address is free: the update record names it, and the chip's boot code follows. It has
to stay above 0xA9000, or installing V16 would overwrite its own update record, and a pulled cable
could not resume.

A computer compresses a song before sending it: the first 0x7E bytes stay plain, since the list
reads the PSID header in place, and the rest is raw deflate, about 55 % of the size on HVSC tunes.
The FM-1 unpacks it when the song loads (`app/inflate.c`), in the main loop, while the audio goes on. Nothing is
compressed on the device; `get` and the web page's read give the file back as it was added.

Song lengths come from HVSC's `Songlengths.md5`: the sender looks the file's MD5 up and sends
the lengths with the file, so the player can show the time and stop at the end of the song;
it then waits for the user to pick the next one with SELECT (or play it again with PLAY). A
file without a known length plays until the user skips it.

The transfer is SysEx over USB-MIDI, `F0 7D 'S' cmd payload F7` with the payload packed 7
bytes into 8: info, entry, put (begin, data, end with a CRC), get, delete, wipe, play, and for
testing stats, output, rx and usb. `tools/fm1_sid.py` and the web page
(`web/site/fm1songs.js`) both speak it. A screen reader's frames, `F0 7D 53 52 ...`, share the
prefix and go to `announce.c` first.

## The screen reader

For [fm1-reader](https://github.com/charlesvestal/fm1-reader): `announce.c` (fm1-base's, unchanged)
answers a reader's HELLO with SiDJ's name and the link to its words file, and sends nothing until one
comes. `speak()` in `ui.c` compares the screen with the last frame and says what changed: the screen
and page with their values on WHERE or a change, a knob's value as it turns (only the turned knob's,
not values the tune moves itself), the song list's row, the song loaded, play and pause, and the
remix's switches; every message goes too, warnings kept whole. The status line's fields are recorded
as they are drawn, so what is said is what is shown. The screen's text goes as it is: `firmware/words.tsv`,
built by `gen_words.py` into `web/words/<fingerprint>.json` and published with the site, says how to
read it. Nothing is said while ASID streams (a song playing with output ASID or both), so a real
chip's timing comes first; and `usb.c` finishes an ASID message before a reader's starts, so neither
is cut.

## The user interface

Two modes, SONGS (the list in the store) and PLAYER (the tune on screen: three voices with a
scope each, the filter, the remix's state), SEQ between them. The controls are in the README.
Loading another song starts clean: every override off, the tempo at 100 % and the vinyl fader
at 0. The same song loaded again (from its start, or another subsong) keeps all of them.

## Host side

The whole app builds on the Mac as a simulator driven by a script (audio to WAV, screen to
PNG, the lights to text, expectations that make scenarios tests); a `sidplay` host program
renders a .sid to WAV. The browser emulator (`web/emu`) is the same simulator as WebAssembly
with the panel on a page: songs added from the page, the ASID stream to Web MIDI.

## Tests

- The SID engine against reSID 0.16, sample by sample.
- 6510: Klaus Dormann's functional test plus an interrupt test.
- The SID file parser, the C64 (interrupts, timers, banking, the KERNAL replacement), the
  remix, the ASID sink, the storage, the encoder, the beat finder, the song unpacker against
  zlib, the rescue tool against a fake update mode.
- Scenarios in the simulator (the songs, the player, the remix, ASID, the MIDI clock out and
  in, the screen reader with `said` checks), with HVSC tunes when HVSC is on the machine (the files are not committed: HVSC's own
  terms).
- The update path against the package: the update entry, the loader, the installer.
- The browser emulator headless in node.
- What the screen reader can say (`speakdump` and every scenario's SAYs) through fm1-reader's audit:
  real words, no symbols, warnings that say what did not happen.
- The C64 against libsidplayfp's own C64 (README, Verification): the SID write sequences of
  random HVSC tunes compared, with a logging SID in place of the emulation.

## Build

Docker with the JieLi toolchain (Linux x86-64 only), the AC79 SDK's three boot files, Python
with Pillow and fonttools; mido and python-rtmidi for the tools. See the README.

## How the FM-1 reaches a real SID

The FM-1 is a USB *device*: Felucca's HAL drives the AC791N's USB controller in device mode
only, and the TRS jack is MIDI in only. So the tune's writes leave as ASID on the USB-MIDI
port, and the receiver (a SidStation, a TherapSID, a TeensyROM, a USBSID-Pico, any ASID
player) hangs off a **USB MIDI host**: a MIDI host box, or a computer, phone or Raspberry Pi
routing the port.
Nothing is written for any one receiver; the same SysEx stream serves them all.

- What is lost: ASID is per frame, with no cycle positions within it, so digi tunes and tunes
  that depend on write order within a frame sound different from the emulation. For most of
  HVSC the difference is small.
- The FM-1 as a USB host was considered and dropped: the SDK's host layer is binary-only,
  which a GPL firmware cannot link (a register-level driver would have to be written, as
  Felucca did for device mode), and whether the board can supply VBUS is unknown.

Tested on a SidStation.

## Decisions

- **The C64 runs continuously** (above) rather than being called once per frame, and the
  KERNAL replacement is sparse (bytes at the real addresses, RTS elsewhere) rather than an
  8 KB image.
- **The reference check**: libsidplayfp built from source with a logging SID in place of the
  emulation prints the SID writes its own C64 makes; the (register, value) sequences are
  compared with this player's. Audio envelopes were compared first and found too sensitive to
  the SID emulations' differences to say anything about the environment.
- **Bad lines** are emulated (whole, at the start of the line) after the reference showed an
  RSID digi tune losing interrupts to them.
- **Cycle placement** (above) came from Dabke 8580, which hung: its NMI jumps through CIA 1's
  timer and returns through CIA 2's serial register. Coffee Bunker, Friday Night at the Ward and
  Beat Cut were broken the same way; all four match the reference now.
- **The floating waveform DAC** is a SiDJ option on the reSID port, off by default, so the
  bit-exact test against reSID 0.16 stands.
- **Profile on the device**: the Mac's timings misled. A temporary tick counter in the audio
  interrupt showed the C64 and the SID at about a third of the budget each for a sample-heavy
  tune; the divides in the waveform averages were the largest single cost.
- **The CPU clock after V16's boot loader**: M-VAVE's V16 leaves the chip at 320 MHz, V15's at
  360, and a custom firmware keeps whichever the FM-1 last had. SiDJ sets V15's clock up at boot
  ([platform.md](platform.md)), so its load figures hold on every FM-1.
- **Not yet**: streaming from the computer (a jukebox: a folder played file by file through a
  RAM slot, without filling the flash).
