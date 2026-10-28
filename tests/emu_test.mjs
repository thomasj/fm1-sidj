// SPDX-License-Identifier: GPL-3.0-only
// The browser emulator without a browser (node): build/emu/sidj.wasm instantiated as the
// worklet does, booted, a .sid file put in the store as the page does, PLAY pressed: audio
// comes out, the screen is drawn, the settings save, and with ALGORITHM at ASID the MIDI out
// stream is ASID SysEx. Also the page's MD5 (for Songlengths.md5) against node's.
//   node tests/emu_test.mjs SID_FILE
import { readFileSync } from "node:fs";
import { createHash } from "node:crypto";

let fails = 0;
const check = (ok, what) => { console.log(`  ${ok ? "ok  " : "FAIL"} ${what}`); if (!ok) fails++; };
const sidPath = process.argv[2];
const wasm = readFileSync(new URL("../build/emu/sidj.wasm", import.meta.url));
const page = readFileSync(new URL("../web/emu/index.html", import.meta.url), "utf8");

// the page's MD5, taken from the page itself
const src = page.slice(page.indexOf("const MD5_K"), page.indexOf("const hex"));
const md5 = new Function(src + "; return md5;")();
for (const s of ["", "a", "abc", "x".repeat(55), "y".repeat(56), "z".repeat(64), "w".repeat(1000)]) {
  const u8 = new TextEncoder().encode(s);
  check(Buffer.from(md5(u8)).toString("hex") === createHash("md5").update(u8).digest("hex"), `md5 of ${s.length} bytes`);
}

let mem;
const wasi = new Proxy({ clock_time_get: (id, p, out) => { new DataView(mem.buffer).setBigUint64(out, 0n, true); return 0; } },
                       { get: (t, k) => (k in t ? t[k] : () => 0) });
const { instance } = await WebAssembly.instantiate(wasm, { wasi_snapshot_preview1: wasi, env: new Proxy({}, { get: () => () => 0 }) });
const ex = instance.exports;
mem = ex.memory;
if (ex._initialize) ex._initialize();
ex.web_master(4096);
ex.web_boot();
const BTN = { PLAY: 10, SEQ: 9 };
const render = (frames) => { let peak = 0; for (let n = 0; n < frames; n += 128) { ex.web_render(128); const l = new Float32Array(mem.buffer, ex.web_out_l(), 128); for (const v of l) peak = Math.max(peak, Math.abs(v)); } return peak; };
const tap = (bit) => { ex.web_buttons(1 << bit); render(4410); ex.web_buttons(0); render(4410); };

render(4410);
check(ex.web_song_count() === 0, "boots with an empty store");
const bytes = new Uint8Array(readFileSync(sidPath));
new Uint8Array(mem.buffer, ex.web_song_buf(), bytes.length).set(bytes);
new Uint8Array(mem.buffer, ex.web_song_md5(), 16).set(createHash("md5").update(bytes).digest());
new Uint16Array(mem.buffer, ex.web_song_lens(), 1).set([600]);
check(ex.web_song_put(bytes.length, 1, 0) === 0, "a .sid file goes into the store");
check(ex.web_song_count() === 1 && ex.web_pool_used() > bytes.length, "the store has it");
new Uint8Array(mem.buffer, ex.web_song_buf(), 4).set([1, 2, 3, 4]);
check(ex.web_song_put(4, 0, 0) === -1, "not a .sid file: refused");

const blits0 = ex.web_blits();
tap(BTN.PLAY);
const peak = render(44100 * 2);
check(peak > 0.01, `PLAY: audio comes out (peak ${peak.toFixed(3)})`);
check(ex.web_blits() > blits0, "the screen is drawn");

// ALGORITHM one click: ASID only; the stream is SysEx F0 2D ... F7, and the audio is silent
ex.web_midi_out_clear();
ex.web_enc(1, 1);
render(4410);
const quiet = render(44100);
const n = ex.web_midi_out_len(), out = new Uint8Array(mem.buffer, ex.web_midi_out(), n).slice();
let msgs = 0, data = 0, bad = 0;
for (let i = 0; i < n;) {
  if (out[i] !== 0xF0) { bad++; break; }
  const end = out.indexOf(0xF7, i);
  if (end < 0 || out[i + 1] !== 0x2D) { bad++; break; }
  msgs++;
  if (out[i + 2] === 0x4E) data++;
  i = end + 1;
}
check(bad === 0 && data > 30, `ASID: ${msgs} SysEx messages, ${data} of them 0x4E register data`);
check(quiet < 0.001, "ASID only: the emulator is silent");
check(ex.web_store_writes() === 0, "nothing saved yet");
ex.web_enc(1, 2);                                   // back to the emulated SID
tap(BTN.SEQ);
ex.web_buttons(1 << 7); render(4410); ex.web_buttons(0); render(4410);   // SAVE in SONGS
check(ex.web_store_writes() === 1 && ex.web_store_len(0) > 0, "SAVE keeps the settings for the page");
ex.web_songs_clear();
check(ex.web_song_count() === 0, "the store can be emptied");
console.log(fails ? "emu_test FAIL" : "emu_test PASS");
process.exit(fails ? 1 : 0);
