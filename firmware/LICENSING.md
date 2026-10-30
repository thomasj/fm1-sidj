# SiDJ licensing

SiDJ is free software. The firmware as a whole is licensed under the GNU General Public
License, version 3 only (`GPL-3.0-only`, text in `LICENSE`). The SID engine in `sid/` is a
derivative of reSID 0.16 and stays under reSID's licence, GPL-2.0-or-later, which is
compatible with distributing the combined firmware under GPL-3.0. If you distribute
SiDJ, or firmware derived from it, you must give your recipients its complete
corresponding source under the same licence.

Copyright (C) 2026 Thomas Jansson, except as listed below. Much of the code was written with AI
coding tools at the author's direction, then reviewed and tested by the author; it is licensed
as a whole under the terms above.

## Where the code comes from

| What | Origin | Licence |
| --- | --- | --- |
| `sid/sid.c`, `sid/sid.h` | C port of reSID 0.16 by Dag Lem (<http://www.zimmers.net/anonftp/pub/cbm/crossplatform/emulators/resid/>), Copyright (C) 2004 Dag Lem, written for the FM-1 by Thomas Jansson. SiDJ's additions, each off by default: waveform averaging, the voice tap and stems, the floating waveform DAC, the per-voice output mute | GPL-2.0-or-later |
| `sid/sid_tables.c` | generated from reSID 0.16's combined-waveform samples and filter curves by `tools/gen_sid_tables.cc` | GPL-2.0-or-later |
| `third_party/resid-0.16/` | reSID 0.16, unmodified except for the removal of autotools helper scripts; the reference the tests compare against | GPL-2.0-or-later (`third_party/licenses/GPL-2.0.txt`) |
| `firmware/app.ld`, `firmware/crt0.S`, `firmware/hal/`, `firmware/loader/`, `firmware/src/{libc,lcd,gfx,usb,midi_uart,storage,ota}.c` | [Felucca](https://github.com/hugelton/Felucca), Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments, as changed in [X0X](https://github.com/charlesvestal/fm1-x0x) by charlesvestal (taken at X0X 03bbd4d, 2026-10-07); SiDJ's changes are listed below | GPL-3.0-only |
| `firmware/src/app/{panel,plat_fm1,main_fm1}.c`, `firmware/src/app/plat.h`, `firmware/src/sidj.c`, `host/sidj_host.c` | after X0X's `panel.c`, `plat_fm1.c`, `main_fm1.c`, `plat.h`, `x0x.c` and `host/x0x_host.c`, after Felucca's `main.c` / `audio.c` / `panel.c` | GPL-3.0-only |
| `tools/build.py`, `build.sh`, `tools/fm1pkg_make.py`, `tools/lz4blk.py`, `tools/get_toolchain.sh`, `tools/fm1_install.py`, `tools/fm1_rescue.*`, `tools/fm1_debug.py`, `tools/gen_font.py`, `tests/{ota_test,ldr_test,storage_test,enc_test}.c`, `tests/install_test.py`, `tests/target_budget.py` | Felucca, by way of X0X; `build.py` adapted | GPL-3.0-only |
| `firmware/src/c64/cpu6502.c` and its header | SiDJ: the documented and undocumented opcodes with cycle counts, IRQ / NMI and the bus hooks | GPL-3.0-only |
| `firmware/src/c64/{c64,sidfile}.c` and headers, `firmware/src/app/{engine,remix,ui,songs,settings}.c` and headers, `host/sidplay.c`, `tools/fm1_sid.py`, `tests/{sidfile,c64,remix}_test.c`, `tests/scenarios/`, `docs/` | SiDJ | GPL-3.0-only |
| `web/emu/` (`sidj_web.c`, `worklet.js`, `index.html`, `build.sh`) | after X0X's browser emulator (`x0x_web.c`, its worklet and page) | GPL-3.0-only |
| `web/site/fm1ota.js`, `web/site/fm1pkg.js` | Felucca's web updater, Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments, unchanged | GPL-3.0-only |
| `web/site/index.html`, `tools/make_site.py` | SiDJ, after Felucca's installer page and `make_site.py` | GPL-3.0-only |
| `web/site/fm1songs.js`, `tests/{emu_test,site_test}.mjs`, `tests/scenarios/store.in` | SiDJ | GPL-3.0-only |
| `tests/{sid_ref_test,sid_quality_test}.cc`, `tools/gen_sid_tables.*` | SiDJ | GPL-2.0-or-later (they include reSID) |

### Changed from Felucca and X0X

- `usb.c`: USB audio's five-channel mode with the voices' channel names, its descriptors patched
  in RAM, the audio streaming interface listed before MIDI (as in Felucca 1.5.1), and SiDJ's
  USB names.
- `gfx.c`: one theme, DEBUT, with the voices' colours and the remix gold.
- `tools/gen_font.py`: Latin-1 in the text sizes, and plain digits when Pillow has no raqm.
- `tools/fm1pkg_make.py`: SiDJ's package identity as the default.
- `hal/fm1_encoder.h` (new): the encoder part of `fm1_input.h`, with Felucca's later detent
  fix; `hal/fm1_input.h` uses it.
- `hal/fm1_sys.h`: the VDC14 / SYSVDD supply raise, from X0X's later commits.
- `ota.c`, `loader/ldr_core.c`: a package whose flash.bin is not this board's size is refused
  (from Felucca 1.6.1).

Everything else from them is unchanged.

## The tunes that come with the site

`web/site/songs/Debut_and_Credit.sid` and `web/site/songs/TwinCross.sid` are Thomas Jansson's
(tubesockor's) own tunes, as released in HVSC and on CSDb, included by their author so the web
page and the emulator have something to play. They are music, not part of the firmware, and not
under the GPL: free to play and to share unmodified with SiDJ; for anything else, ask the author.

## Third-party material

| What | Licence | Where |
| --- | --- | --- |
| Barlow Semi Condensed by Jeremy Tribby: the screen's font, rasterised at build time | SIL OFL 1.1 | `assets/fonts/BarlowSemiCondensed-*.ttf`, `assets/fonts/Barlow-OFL.txt` |
| Terminus by Dimitar Zhekov: the 12-pixel monospace face, always built in, and an alternative font set (`SIDJ_FONTSET=terminus`) | SIL OFL 1.1 | `assets/fonts/ter-*.bdf`, `assets/fonts/Terminus-LICENSE.txt` |
| 6502 functional test by Klaus Dormann (<https://github.com/Klaus2m5/6502_65C02_functional_tests>): the binary the 6510 core is checked against by `tests/cpu_test.c`; host tests only, not in the firmware | GPL-3.0-or-later | `third_party/6502_functional_test/` |
| JieLi AC79 SDK (<https://gitee.com/Jieli-Tech/fw-AC79_AIoT_SDK>): three unmodified files go into every `.fwsc` package, `uboot.boot` (first-stage boot loader), `cfg_tool.bin` (chip configuration) and `eq_cfg_hw.bin` (hardware EQ table); read from an SDK checkout at build time, not in this tree | Apache-2.0 | `third_party/licenses/Apache-2.0.txt` |

## The C64 environment

No Commodore ROM is used or needed. The KERNAL's part in playing a tune (the interrupt
entries, the default handlers, the vectors, a harmless return for everything else) is a small
replacement written for this project (`firmware/src/c64/c64.c`), placed at the real KERNAL's
addresses so tunes that jump into the KERNAL's interrupt code land where they expect. The
tune's own code comes from the user's .sid file, loaded as data at run time; it is not part of
the firmware image. The HVSC file format description (`SID_file_format.txt`) was the reference
for `sidfile.c`; no player source was used.

## ASID and the remix

The ASID output is written from the protocol description
(<https://github.com/thomasj/asid-protocol>). The remix is new code for SiDJ, building on the ASID
and remix ideas the author wrote for [TherapSID](https://github.com/twistedelectrons/TherapSID).

## What is not here

- No M-VAVE material: no vendor code or data is linked or included. The installer's "return to
  official" path installs a package you downloaded yourself.
- No code or material from the author's employer. Everything in this tree is either the
  open-source work listed above or written for this project.
- No Commodore ROMs, and no SID tunes apart from the author's two above: the tunes are the
  user's files (HVSC's are not redistributed here; the tests play the ones on your disk when
  they are there).
- Felucca's and X0X's non-GPL assets (icon set, panel photo, drum samples) are not part of
  this tree.

## Trademarks

"M-VAVE" and "FM-1" are trademarks of their owners. "SID" and "Commodore" are trademarks of their
owners. SiDJ is independent firmware and is not affiliated with or endorsed by any of
them, nor by Hügelton Instruments, the authors of X0X, the High Voltage SID Collection, or the
makers of the SidStation, TherapSID, TeensyROM or USBSID-Pico.
