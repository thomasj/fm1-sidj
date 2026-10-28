// SPDX-License-Identifier: GPL-3.0-only
// The site's song module (web/site/fm1songs.js) against the device's own song store: the
// simulator (build/emu/sidj.wasm, which runs firmware/src/app/songs.c) stands in for the
// FM-1 on the other end of the MIDI cable. Add, list, fetch back, delete, compact, play, wipe;
// and HVSC's Songlengths.md5 parsed as the page parses it, when HVSC is on this machine.
//   node tests/site_test.mjs SID_FILE SID_FILE2 [HVSC_DIR]
import { readFileSync, existsSync } from "node:fs";
import { createHash } from "node:crypto";
import { Songs, parseSonglengths, md5, hex, crc16, sidInfo, addMissing } from "../web/site/fm1songs.js";

let fails = 0;
const check = (ok, what) => { console.log(`  ${ok ? "ok  " : "FAIL"} ${what}`); if (!ok) fails++; };
const [sid1, sid2, hvsc] = process.argv.slice(2);

let mem;
const wasm = readFileSync(new URL("../build/emu/sidj.wasm", import.meta.url));
const wasi = new Proxy({ clock_time_get: (id, p, out) => { new DataView(mem.buffer).setBigUint64(out, 0n, true); return 0; } },
                       { get: (t, k) => (k in t ? t[k] : () => 0) });
const { instance } = await WebAssembly.instantiate(wasm, { wasi_snapshot_preview1: wasi, env: new Proxy({}, { get: () => () => 0 }) });
const ex = instance.exports;
mem = ex.memory;
if (ex._initialize) ex._initialize();
ex.web_boot();
for (let i = 0; i < 20; i++) ex.web_render(128);

// the MIDI cable: a whole SysEx message in, the device run until it answers, its answers out
const transport = {
  onSysex: null,
  send(bytes) {
    const body = Uint8Array.from(bytes.slice(1, -1));
    new Uint8Array(mem.buffer, ex.web_sx_in_buf(), body.length).set(body);
    if (ex.web_sx_in(body.length)) throw new Error("the device has a frame pending");
    setImmediate(() => {
      for (let i = 0; i < 400 && !ex.web_sx_out_len(); i++) ex.web_render(128);
      const n = ex.web_sx_out_len(), out = new Uint8Array(mem.buffer, ex.web_sx_out(), n).slice();
      ex.web_sx_out_clear();
      for (let i = 0; i < n;) { const e = out.indexOf(0xF7, i); if (e < 0) break; this.onSysex(out.subarray(i, e + 1)); i = e + 1; }
    });
  },
};

// the helpers against node's
const bytes1 = new Uint8Array(readFileSync(sid1)), bytes2 = new Uint8Array(readFileSync(sid2));
check(hex(md5(bytes1)) === createHash("md5").update(bytes1).digest("hex"), "md5 of a .sid file == node's");
check(crc16(new TextEncoder().encode("123456789")) === 0x31C3, "crc16 (XMODEM) of 123456789");
check(sidInfo(bytes1) && sidInfo(bytes1).name.length > 0 && sidInfo(new Uint8Array(10)) === null, "the header's strings");

const songs = new Songs(transport);
let info = await songs.info();
check(info.count === 0 && info.size === 0x3C000 && info.used === 0, `info: an empty store of ${info.size} bytes`);
let progress = 0;
await songs.put(bytes1, [600, 1234], (p) => { progress = p; });
check(progress === 1, "put: progress to 100 %");
await songs.put(bytes2, []);
info = await songs.info();
check(info.count === 2 && info.used > bytes1.length + bytes2.length, `two songs in, ${info.used} bytes used`);
const list = await songs.list();
check(list[0].name === sidInfo(bytes1).name && list[0].author === sidInfo(bytes1).author, `entry 1: ${list[0].name} / ${list[0].author}`);
check(list[0].lens.join() === "600,1234" && list[1].lens.length === 0, "the song lengths came along");
const back = await songs.get(0, list[0].size);
check(back.length === bytes1.length && back.every((b, i) => b === bytes1[i]), "get: the file comes back byte for byte");
await songs.play(1, 0);
for (let i = 0; i < 100; i++) ex.web_render(128);
check((ex.web_state() & 3) === 3 && (ex.web_state() >> 8) === 2, "play: the second song is loaded and playing");
await songs.delete(0);
check((await songs.list()).length === 1, "delete: one song left");
const usedBefore = (await songs.info()).used;
check(await songs.compact() === 1, "compact: the live song fetched and sent back");
const after = await songs.info();
check(after.count === 1 && after.used < usedBefore && (await songs.entry(0)).name === sidInfo(bytes2).name, `compact: ${usedBefore} -> ${after.used} bytes used`);
let refused = false;
try { await songs.put(new Uint8Array(200), []); } catch { refused = true; }
check(refused, "put: not a .sid file is refused before sending");
// a 2SID tune: refused before sending, and by the device itself when a sender does not check
const two = Uint8Array.from(bytes1);
two[5] = 3; two[0x7A] = 0x50;                           // version 3, a second SID at $D500
check(sidInfo(two).sids === 2, "sidInfo: a version 3 header with $D500 is a 2SID tune");
refused = false;
try { await songs.put(two, []); } catch (e) { refused = /2SID/.test(e.message); }
check(refused, "put: a 2SID tune is refused before sending");
{
  const before = (await songs.info()).count;
  const hdr = [two.length & 0xFF, (two.length >> 8) & 0xFF, 0, 0, 0, ...md5(two)];
  await songs.ask(4 - 1, hdr);                                                  // PUT_BEGIN
  for (let off = 0; off < two.length; off += 256)
    await songs.ask(4, [off & 0xFF, off >> 8, 0, ...two.subarray(off, off + 256)]);   // PUT_DATA
  const c = crc16(two), st = (await songs.ask(5, [c & 0xFF, c >> 8]))[0];      // PUT_END
  check(st === 7 && (await songs.info()).count === before, `the device refuses a 2SID tune itself (status ${st}), the store unchanged`);
}
await songs.wipe();
check((await songs.info()).count === 0, "wipe: the store is empty");

// the tunes that come with the site: PSID / RSID files the device takes, named and timed as songs.json says
const starters = JSON.parse(readFileSync(new URL("../web/site/songs/songs.json", import.meta.url), "utf8"));
for (const e of starters) {
  const b = new Uint8Array(readFileSync(new URL(`../web/site/songs/${e.file}`, import.meta.url))), info = sidInfo(b);
  check(info && info.name === e.name && info.author === e.author && e.lens.length === info.songs && b.length <= 61440,
        `songs/${e.file}: ${e.name}, ${e.lens.length} length(s)`);
}

// after an install the page adds the bundled tunes the FM-1 does not have yet, and only those
const tunes = starters.map((e) => ({ ...e, bytes: new Uint8Array(readFileSync(new URL(`../web/site/songs/${e.file}`, import.meta.url))) }));
check(await addMissing(songs, tunes) === 2 && (await songs.info()).count === 2, "addMissing: both bundled tunes added to an empty store");
check(await addMissing(songs, tunes) === 0 && (await songs.info()).count === 2, "addMissing: run again, nothing added twice");
await songs.delete(0);
check(await addMissing(songs, tunes) === 1 && (await songs.list()).filter(Boolean).length === 2, "addMissing: a deleted one comes back, alone");
await songs.wipe();

if (hvsc && existsSync(`${hvsc}/DOCUMENTS/Songlengths.md5`)) {
  const sl = parseSonglengths(readFileSync(`${hvsc}/DOCUMENTS/Songlengths.md5`, "latin1"));
  for (const e of starters) {
    const b = new Uint8Array(readFileSync(new URL(`../web/site/songs/${e.file}`, import.meta.url)));
    check((sl.byMd5.get(hex(md5(b))) || []).join() === e.lens.join(), `songs/${e.file}: the lengths are HVSC's`);
  }
  const commando = sl.entries.find((e) => e.path === "/MUSICIANS/H/Hubbard_Rob/Commando.sid");
  check(sl.entries.length > 50000 && sl.byMd5.size >= sl.entries.length * 0.99, `Songlengths.md5: ${sl.entries.length} paths`);
  if (commando) {
    const file = new Uint8Array(readFileSync(`${hvsc}${commando.path}`));
    check(hex(md5(file)) === commando.md5 && commando.lens.length === sidInfo(file).songs, `Commando: md5 matches, ${commando.lens.length} lengths`);
  }
} else {
  console.log("  skip Songlengths.md5 (no HVSC)");
}
console.log(fails ? "site_test FAIL" : "site_test PASS");
process.exit(fails ? 1 : 0);
