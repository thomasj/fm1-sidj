# The FM-1 platform, as learned from Felucca, X0X and FoMni

Notes gathered from reading the three open firmwares (all GPL-3.0). File references are to
the Felucca tree unless marked `x0x/`. Nothing here comes from the vendor.

## SoC

- **JieLi AC791N** ("wl82" in the SDK), **pi32v2** core, 32-bit little-endian. Registers
  r0-r15; control registers icfg/psr/sp/ssp/usp/rets/reti (`firmware/crt0.S`).
- **Clock**: measured by X0X at PLL 540 MHz, system clock **360 MHz**, HSB 180 MHz, LSB
  (peripherals) 60 MHz. About 8160 CPU cycles per output sample at 44.1 kHz.
- **FPU**: single precision in hardware with `-mcpu=r3 -mfprev1` (divide yes, no sqrt, no FMA).
  `double` is software and X0X's build fails if a soft-double routine gets linked. The divide-
  by-zero trap also fires on float divides and must stay off. Felucca is integer only.
- **RAM**: 512 KiB at 0x01C00000. Layout (`firmware/app.ld`): RAMTEXT 24 KiB (flash driver,
  runs while XIP is off), RAM **96 KiB** for .data/.bss, POOL **336 KiB** for large NOLOAD
  buffers (section `.pool`), user stack ~24 KiB, supervisor/ISR stack ~8 KiB, NOINIT ~15 KiB
  (crash record, settings, survives reset), boot state and vector table at the top.
- **Flash**: 1 MiB SPI NOR (JEDEC 0x856014), 4 KiB sectors, 256 B pages. Code and rodata run
  in place (XIP) through a 32 B-line cache; flash offset `off` appears at
  `0x02000000 + off - 0x4000`. App slot 0x4000-0x93000 (app.bin at +0x120, max 581,564 B),
  SFC-encrypted with chip key 0x980F. Above 0x93000: VM region, Felucca's data
  0x97000-0xE0000, OTA staging 0xE0000-0xE5000, settings 0xFC000-0xFF000.
- **Caches**: 32 KiB I-cache, 32 KiB D-cache; 578 KiB SRAM and a second core per the SDK,
  unused by Felucca.
- **Costs to know**: a divide is one instruction but many cycles, so keep it out of per-sample
  code. Code runs from flash through the I-cache, so a hot path that touches a lot of code costs
  more on the device than on a Mac: profile on the device.

## Audio

- ALNK0 I2S to an external codec, double-buffered DMA. Buffer: two halves of `int32`
  interleaved L,R; samples are 24-bit in the int32 (full scale 2^23). Felucca renders Q15 and
  shifts left 7 for a -6 dBFS ceiling.
- Rate 44.1 kHz nominal (actually 44,117.6 Hz: 24 MHz / 544 TIMER4 ticks per frame).
- Half-buffer size: Felucca 128 frames (2.9 ms), FoMni 256, X0X 512 (a heavy block overran 256
  about 1 in 300). SiDJ uses 512, rendered as two blocks of 256.
- IRQ: ALNK0 is vector 11, priority 3. ISRs cannot be plain C (the compiler emits no `csync`
  before `rti`), so `hal/fm1_isr.S` wraps `fm1_alnk0_irq()` and `fm1_timer5_irq()`.
  Pattern (`src/audio.c`): `p = fm1_audio_pending(); fm1_audio_ack_aux(p); if (p & FM1_AUDIO_HALF)
  { half = fm1_audio_free_half(); render into abuf[half*HALF_WORDS]; fm1_audio_ack_half(); }`.
  TIMER5 (10 kHz, priority 4) nests above audio for the key scan.
- MASTER volume pot on SARADC channel 4, 10-bit, polled.

## Input and LEDs

- 11 x 6 key matrix behind two 74HC595 shift registers (`hal/fm1_input.h`), 41 ids: 0-13
  buttons, **14-40 the 27 note keys F3..G5** (16 white, 11 black). 7 quadrature encoders
  (4 transitions per detent, no push switches). The decoder (`hal/fm1_encoder.h`) counts
  transitions from the rest state seen at power-on and emits whole cycles on returning to it,
  so a click is one step slow or fast; it relearns the rest state only after a second still
  elsewhere, since learning from short pauses lost slow single clicks (Felucca #23).
- Button labels and default ids (`src/panel.c`): OCT-=0, OCT+=1, FX=2, SCL=3, ENV=4, LFO=5,
  EDIT=6, GLO=7, HOME=8, SAVE=9, ARP=10, SEQ=11, PLAY=12, REC=13. Encoders: SELECT=0,
  ALGORITHM=1, KNOB1-4=2-5, PRESETS=6.
- Scan: `fm1_input_tick()` from TIMER5, one column per tick, frame 1.1 ms; press after 2 closed
  frames, release after 8 open. Main-loop API: `fm1_in.notes`, `fm1_in.buttons`,
  `fm1_input_edges()`, `fm1_input_note_edges()`, `fm1_enc_take(e)`.
- 41 LEDs, one per key/button, multiplexed on the matrix columns (1/11 duty). Dimming and
  breathing are software (`fm1_led[]`, `fm1_led_dim[]`, `fm1_led_breath[]`).

## Display

- ST7789-class **240 x 240 RGB565** IPS on SPI1 at 12 MHz, backlight PA2 active-low. Pixel
  data by SPI DMA from RAM, big-endian pixels (byte-swap).
- No full framebuffer: `src/gfx.c` draws into an off-screen canvas of 240 x 124 pixels in
  `.pool` and blits strips (`cv_begin`, `cv_rect`, `cv_text`, `cv_blit`, ...). A full frame
  would take ~77 ms to push, so UIs redraw strips.
- Fonts: Felucca rasterises Inter Tight at build time into 4-bit alpha glyphs
  (`tools/gen_aa_font.py`, needs Pillow and fontTools). X0X uses a simpler bitmap font
  generator (`x0x/tools/gen_font.py`).

## Boot, package, update

- Mask ROM → SPL (`uboot.boot` from the SDK, in the flash head) → jumps to app.bin at XIP
  0x02000120. The image must start with `goto +4; rts` (bytes `04 81 80 00`). `crt0.S` sets
  the stacks and calls `fm1_cstart()`, which arms the watchdog (~8 s), keeps a boot-loop
  guard in `.noinit`, installs 128 vectors, zeroes .bss and .pool, copies .data and .ram_text,
  arms the hardware guards and calls `fm1_main()`.
- Link: `ld -T app.ld crt0.o fm1_vec.o fm1_isr.o app.o`, **no libc, libm or compiler-rt**.
  The firmware provides its own `memset`/`memcpy`/`memcmp` (`src/libc.c`). Flags:
  `-Os -ffunction-sections -fno-builtin`; X0X adds `-mcpu=r3 -mfprev1 -ffp-contract=off`.
- `.fwsc` package (`tools/fm1pkg_make.py`): a 0x93000-byte flash image (head with JLFS entries
  and the SPL, app area with app.bin, `cfg_tool.bin`, `eq_cfg_hw.bin`, SFC-encrypted) and the
  update loader `ota.bin`, in a UFW container for chip "AC791N" with an identity string that
  the installers check (`FM-1_64000000` for a development build; a SiDJ release X.Y.Z is
  `FM-1_64XXYYZZ`: 64 is SiDJ's own prefix, apart from every other firmware's, see
  [thefirmwarehub.com](https://thefirmwarehub.com/kb/)).
- Install over USB-MIDI SysEx (`tools/fm1_install.py`, `src/ota.c`): the running app stages
  the RAM-only loader (`firmware/loader/`) at 0xE0000, writes an update record, resets; the
  SPL runs the loader, which pulls the image from the host in 512 B reads and writes sectors
  0x4000-0x93000, verifying each. M-VAVE's packages go back the same way with their own loader
  (V15's and V16's are the same), which rewrites the whole flash image head included: V15's is
  0x93000 bytes, V16's 0xA9000 (into the song store), and their heads differ, so `ota.c` takes a
  head unlike the FM-1's only when it is one of those two (by CRC).
- Safety nets: 2 failed boots → X0X's safe mode (USB and OTA only; Felucca goes straight to
  the chip's UBOOT), 4 failed boots → UBOOT, OCT- and OCT+ held together for 5 s → UBOOT, a
  crash screen with vector / PC for 4 s, then a reboot. UBOOT enumerates as USB 4C4A:8057
  "WL80UBOOT"; [FM-1 Transporter](https://github.com/kurogedelic/FM-1-transporter) rewrites
  flash as a last resort.

## USB

- Full-speed device, polled at 2 kHz from TIMER5 (no USB IRQ). VID 0x1209 PID 0x0001
  (pid.codes test id). Classes: USB-MIDI (EP1), optional CDC console, optional UAC1 audio
  input to the host (16-bit, 44.1 kHz; SiDJ sends stereo, or five channels with the voices).
- MIDI: 4-byte USB-MIDI packets in 64-deep rings, `midi_in_q` consumed by the app,
  `midi_out_event()` to send. TRS MIDI IN on UART1 (PH8, 31250 baud).

## Host simulation

- Felucca's tests `#include` the firmware sources on the Mac after `#define __attribute__(x)`,
  with the HAL register macro redirected to RAM arrays where the real HAL code is tested.
- X0X's `host/x0x_host.c` is the cleanest template: a `plat.h` boundary (ms, buttons, keys,
  encoders, master, LEDs, MIDI, storage) implemented by `plat_fm1.c` on the device and by the
  host program on the Mac, driven by a script (`press`, `key`, `turn`, `wav`, `shot`), output
  as WAV and PNG screenshots. `-ffp-contract=off` keeps host and device float results equal.
- The web emulators build the same sources with Emscripten into an AudioWorklet.

## What an app must provide

`fm1_cstart`, `fm1_main`, `fm1_timer5_irq`, `fm1_alnk0_irq`, `fm1_fault`, the audio buffer,
and the hooks of the platform files it includes (`usb.c`: `fm1_ms`, `RING_PUBLISH`,
`HALF_FRAMES`, the `FELUCCA_*` switches, `bootguard`; `storage.c`: `st_read/erase/prog`;
`ota.c`: `ota_*` hooks; `gfx.c`: fonts, palettes, `lcd_*`).

Minimal platform file set from Felucca: `app.ld`, `crt0.S`, `hal/*.h`, `hal/*.S`,
`src/{libc,lcd,gfx,usb}.c`, optionally `panel.c`, `midi_uart.c`, `storage*.c`, `ota*.c`,
`loader/*`; tools `build.py` (trimmed), `fm1pkg_make.py`, `lz4blk.py`, `fm1_install.py`.
