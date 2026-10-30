// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Thomas Jansson
//
// The song store of an FM-1 running SiDJ, over Web MIDI: list, add, fetch, delete, play,
// wipe and compact .sid files, as tools/fm1_sid.py does from the command line. Protocol
// (firmware/src/app/songs.c): F0 7D 'S' cmd payload F7, the payload packed 7 bytes into 8 (a
// byte of the seven high bits first); the answer comes back as cmd | 0x40.
// Also HVSC's Songlengths.md5 (md5 of the file -> song lengths) and an MD5 to look files up.

export const INFO = 1, ENTRY = 2, PUT_BEGIN = 3, PUT_DATA = 4, PUT_END = 5, GET = 6, DELETE = 7, WIPE = 8, PLAY = 9;
export const MAX_LENS = 64, FILE_MAX = 61440;
const CHUNK = 256;
const PUT_ERRORS = { 1: "no transfer open", 2: "bytes missing", 3: "CRC mismatch", 4: "too big", 5: "the song store is full", 6: "flash write failed",
                     7: "a 2SID / 3SID tune: SiDJ plays one-SID tunes" };

export function pack7(data) {
  const out = [];
  for (let i = 0; i < data.length; i += 7) {
    const k = Math.min(7, data.length - i);
    let hi = 0;
    for (let j = 0; j < k; j++) hi |= (data[i + j] >> 7) << j;
    out.push(hi);
    for (let j = 0; j < k; j++) out.push(data[i + j] & 0x7F);
  }
  return out;
}

export function unpack7(data) {
  const out = [];
  for (let i = 0; i < data.length;) {
    const hi = data[i++];
    for (let j = 0; j < 7 && i < data.length; j++) out.push(data[i++] | (((hi >> j) & 1) << 7));
  }
  return new Uint8Array(out);
}

export function crc16(data) {
  let c = 0;
  for (const b of data) {
    c ^= b << 8;
    for (let k = 0; k < 8; k++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF;
  }
  return c;
}

const MD5_K = Array.from({ length: 64 }, (_, i) => Math.floor(Math.abs(Math.sin(i + 1)) * 2 ** 32) >>> 0);
const MD5_S = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
export function md5(u8) {
  const n = u8.length, len = (n + 72) & ~63, buf = new Uint8Array(len), dv = new DataView(buf.buffer);
  buf.set(u8); buf[n] = 0x80;
  dv.setUint32(len - 8, (n * 8) >>> 0, true); dv.setUint32(len - 4, Math.floor(n / 0x20000000), true);
  let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  const M = new Uint32Array(16);
  for (let off = 0; off < len; off += 64) {
    for (let j = 0; j < 16; j++) M[j] = dv.getUint32(off + j * 4, true);
    let A = a0, B = b0, C = c0, D = d0;
    for (let i = 0; i < 64; i++) {
      let F, g;
      if (i < 16) { F = (B & C) | (~B & D); g = i; }
      else if (i < 32) { F = (D & B) | (~D & C); g = (5 * i + 1) & 15; }
      else if (i < 48) { F = B ^ C ^ D; g = (3 * i + 5) & 15; }
      else { F = C ^ (B | ~D); g = (7 * i) & 15; }
      F = (F + A + MD5_K[i] + M[g]) >>> 0;
      const s = MD5_S[(i >> 4) * 4 + (i & 3)];
      A = D; D = C; C = B; B = (B + ((F << s) | (F >>> (32 - s)))) >>> 0;
    }
    a0 = (a0 + A) >>> 0; b0 = (b0 + B) >>> 0; c0 = (c0 + C) >>> 0; d0 = (d0 + D) >>> 0;
  }
  const out = new Uint8Array(16), ov = new DataView(out.buffer);
  [a0, b0, c0, d0].forEach((w, i) => ov.setUint32(i * 4, w, true));
  return out;
}
export const hex = (u8) => Array.from(u8, (b) => b.toString(16).padStart(2, "0")).join("");

// HVSC's DOCUMENTS/Songlengths.md5: "; /PATH" lines, then "md5=m:ss[.mmm] ..." -> deciseconds
export function parseSonglengths(text) {
  const byMd5 = new Map(), entries = [], re = /(\d+):(\d+)(?:\.(\d+))?/g;
  let path = null;
  for (const line of text.split(/\r?\n/)) {
    if (line.startsWith("; /")) { path = line.slice(2).trim(); continue; }
    const eq = line.indexOf("=");
    if (eq !== 32) continue;
    const lens = [];
    for (const t of line.slice(eq + 1).matchAll(re)) lens.push((+t[1] * 60 + +t[2]) * 10 + +((t[3] || "0")[0]));
    const key = line.slice(0, 32).toLowerCase();
    byMd5.set(key, lens);
    if (path) { entries.push({ path, md5: key, lens }); path = null; }
  }
  return { byMd5, entries };
}

// the PSID / RSID header's strings and song count, or null
export function sidInfo(u8) {
  if (u8.length < 0x76) return null;
  const magic = String.fromCharCode(u8[0], u8[1], u8[2], u8[3]);
  if (magic !== "PSID" && magic !== "RSID") return null;
  const str = (o) => { let s = ""; for (let i = 0; i < 32 && u8[o + i]; i++) s += String.fromCharCode(u8[o + i]); return s; };
  const ver = u8[4] << 8 | u8[5], ok = (b) => b >= 0x42 && b <= 0xFE && !(b & 1) && !(b >= 0x80 && b <= 0xDF);   // as sidfile.c
  const second = ver >= 3 && ok(u8[0x7A]), third = second && ver >= 4 && ok(u8[0x7B]);
  return { name: str(0x16), author: str(0x36), released: str(0x56), songs: (u8[0x0e] << 8 | u8[0x0f]) || 1, rsid: magic === "RSID",
           sids: 1 + second + third };
}

export const fmtLen = (ds) => `${Math.floor(ds / 600)}:${String(Math.floor(ds / 10) % 60).padStart(2, "0")}`;
const le = (v, n) => Array.from({ length: n }, (_, i) => (v >>> (8 * i)) & 0xFF);
const rd = (b, o, n) => { let v = 0; for (let i = n - 1; i >= 0; i--) v = v * 256 + b[o + i]; return v; };
const str32 = (b, o) => { let s = ""; for (let i = 0; i < 32 && b[o + i]; i++) s += String.fromCharCode(b[o + i]); return s; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
function fail(msg) { return new Error(msg); }

// a transport: send(bytes) for a whole SysEx message, and onSysex(bytes) called with each one
// that arrives. webMidiTransport() makes one from a Web MIDI port pair.
export function webMidiTransport(input, output) {
  const t = {
    onSysex: null,
    send(bytes) { output.send(bytes); },
    close() { input.onmidimessage = null; },
    name: input.name,
  };
  input.onmidimessage = (ev) => { if (ev.data[0] === 0xF0 && t.onSysex) t.onSysex(ev.data); };
  return t;
}

export class Songs {
  constructor(transport) {
    this.t = transport;
    this.waiting = null;                     // {cmd, resolve}
    this.busy = Promise.resolve();
    this.t.onSysex = (d) => {
      if (d.length < 5 || d[1] !== 0x7D || d[2] !== 0x53) return;
      if (this.waiting && d[3] === (0x40 | this.waiting.cmd)) {
        const w = this.waiting;
        this.waiting = null;
        w.resolve(unpack7(Array.from(d.subarray ? d.subarray(4, d.length - 1) : d.slice(4, -1))));
      }
    };
  }

  // one command, its answer; sent again on a timeout
  async ask(cmd, payload = [], timeout = 3000, tries = 3) {
    const run = async () => {
      for (let a = 0; a < tries; a++) {
        const answer = new Promise((resolve) => { this.waiting = { cmd, resolve }; });
        this.t.send([0xF0, 0x7D, 0x53, cmd, ...pack7(payload), 0xF7]);
        let tm;
        const r = await Promise.race([answer, new Promise((res) => { tm = setTimeout(() => res(null), timeout); })]);
        clearTimeout(tm);
        if (r) return r;
        this.waiting = null;
      }
      throw fail(`the FM-1 did not answer (command ${cmd}). Is it running SiDJ, and not busy?`);
    };
    const p = this.busy.then(run, run);
    this.busy = p.catch(() => {});
    return p;
  }

  async info() {
    const r = await this.ask(INFO);
    return { size: rd(r, 0, 4), used: rd(r, 4, 4), count: r[8] | r[9] << 8, fileMax: r.length >= 14 ? rd(r, 10, 4) : FILE_MAX };
  }

  async entry(k) {
    const r = await this.ask(ENTRY, [k & 0xFF, k >> 8]);
    if (r[0]) return null;
    const nlen = r[5], lens = [];
    for (let i = 0; i < nlen; i++) lens.push(r[6 + 2 * i] | r[7 + 2 * i] << 8);
    const s = 6 + 2 * nlen;
    return { k, size: rd(r, 1, 4), lens, name: str32(r, s), author: str32(r, s + 32), released: str32(r, s + 64) };
  }

  async list() {
    const { count } = await this.info();
    const out = [];
    for (let k = 0; k < count; k++) out.push(await this.entry(k));
    return out;
  }

  // a .sid file into the store; lens: song lengths in deciseconds (from Songlengths.md5)
  async put(bytes, lens = [], onProgress = null) {
    const info = sidInfo(bytes);
    if (!info) throw fail("not a PSID / RSID file");
    if (info.sids > 1) throw fail(`a ${info.sids}SID tune: SiDJ plays one-SID tunes`);
    if (bytes.length > FILE_MAX) throw fail(`too big (${bytes.length} bytes; the player takes ${FILE_MAX})`);
    lens = lens.slice(0, MAX_LENS);
    const hdr = [...le(bytes.length, 4), lens.length, ...md5(bytes)];
    for (const x of lens) hdr.push(x & 0xFF, x >> 8);
    if ((await this.ask(PUT_BEGIN, hdr))[0]) throw fail("the FM-1 refused the file");
    for (let off = 0; off < bytes.length; off += CHUNK) {
      const chunk = Array.from(bytes.subarray(off, off + CHUNK));
      let ok = false;
      for (let a = 0; a < 3 && !ok; a++) {
        const r = await this.ask(PUT_DATA, [...le(off, 3), ...chunk], 4000);
        if (r[0] === 0 && rd(r, 1, 3) === off) ok = true;
        else if (r[0] === 1 && (await this.ask(PUT_BEGIN, hdr))[0]) throw fail("the FM-1 refused the file");
      }
      if (!ok) throw fail(`the FM-1 did not take the bytes at ${off}`);
      if (onProgress) onProgress(Math.min(bytes.length, off + CHUNK) / bytes.length);
    }
    const status = (await this.ask(PUT_END, le(crc16(bytes), 2), 30000))[0];
    if (status) throw fail(`the FM-1 did not keep the file: ${PUT_ERRORS[status] || status}`);
  }

  async get(k, size, onProgress = null) {
    const out = new Uint8Array(size);
    for (let got = 0; got < size;) {
      const want = Math.min(CHUNK, size - got);
      const r = await this.ask(GET, [k & 0xFF, k >> 8, ...le(got, 3), ...le(want, 2)]);
      if (r.length < 3 || rd(r, 0, 3) !== got) throw fail("a bad answer while reading the song");
      out.set(r.subarray(3), got);
      got += r.length - 3;
      if (onProgress) onProgress(got / size);
    }
    return out;
  }

  async delete(k) { if ((await this.ask(DELETE, [k & 0xFF, k >> 8]))[0]) throw fail("delete failed"); }
  async play(k, subsong = 0) { if ((await this.ask(PLAY, [k & 0xFF, k >> 8, subsong & 0xFF]))[0]) throw fail("the FM-1 could not play it"); }
  async wipe() { if ((await this.ask(WIPE, [0x57, 0x49, 0x50, 0x45], 60000))[0]) throw fail("wipe failed"); }

  // deleting leaves holes: fetch everything live, wipe, send it all back
  async compact(onProgress = null) {
    const live = await this.list(), kept = [];
    for (const [i, e] of live.entries()) {
      if (!e) continue;
      kept.push({ bytes: await this.get(e.k, e.size), lens: e.lens });
      if (onProgress) onProgress("fetch", (i + 1) / live.length);
    }
    await this.wipe();
    for (const [i, s] of kept.entries()) {
      await this.put(s.bytes, s.lens);
      if (onProgress) onProgress("send", (i + 1) / kept.length);
    }
    return kept.length;
  }
}

// the FM-1 among the Web MIDI ports: an in/out pair that answers INFO. The ports by forEach
// (some iOS Web MIDI browsers' maps have no iterable values()), the output by the same name,
// else the same name without Windows' "MIDIIN2 (...)" / "MIDIOUT2 (...)", else the only one.
// Tried: the names the installer tries (as fm1ota.js, which the emulator page does not load),
// since macOS keeps the port name it first saw: SiDJ can show as "Felucca" or "SLOOP"
const FM1_PORT = /fm-1|sidj|felucca|sloop|clipper|etch|jiant|ota|update|composite|sinco|usb-midi/i;
const ports = (map) => { const a = []; map.forEach((x, y) => a.push(x && typeof x === "object" ? x : y)); return a; };
// the MIDI inputs there are, for a "not found" message: "a, b" or "none"
export function midiInputs(access) {
  const names = ports(access.inputs).filter((p) => p.state !== "disconnected").map((p) => p.name || "?");
  return names.length ? names.join(", ") : "none";
}
function partner(outs, input) {
  const live = outs.filter((o) => o.state !== "disconnected");
  const bare = (n) => (n || "").replace(/^MIDI(?:IN|OUT)(\d*) \((.*)\)$/i, "$2 $1");
  return live.find((o) => o.name === input.name) || live.find((o) => bare(o.name) === bare(input.name)) ||
    (live.length === 1 ? live[0] : null);
}
export async function findFM1(access) {
  const outs = ports(access.outputs);
  for (const input of ports(access.inputs)) {
    if (input.state === "disconnected" || !FM1_PORT.test(input.name || "")) continue;   // never probe other gear
    const output = partner(outs, input);
    if (!output) continue;
    try { await input.open(); await output.open(); } catch { continue; }
    const t = webMidiTransport(input, output), songs = new Songs(t);
    try { await songs.ask(INFO, [], 1500, 2); return songs; } catch { t.close(); }
    await sleep(50);
  }
  return null;
}

// ------------------------------------------------------------------ HVSC on the user's disk
// The C64Music folder (or one that holds it), through the File System Access API (Chrome, Edge):
// its Songlengths.md5 lists every tune's path, MD5 and lengths, so the search needs no walk of
// the 60000 files, and a tune is read only when it is added.
export async function openHvsc(dir) {
  let root = null, docs = null;
  for (const name of ["", "C64Music"]) {
    try { root = name ? await dir.getDirectoryHandle(name) : dir; docs = await root.getDirectoryHandle("DOCUMENTS"); break; } catch { /* the next */ }
  }
  if (!docs) throw fail("there is no DOCUMENTS folder in it: choose the C64Music folder of HVSC");
  const sl = parseSonglengths(await (await (await docs.getFileHandle("Songlengths.md5")).getFile()).text());
  return { root, byMd5: sl.byMd5, index: sl.entries.map((e) => ({ ...e, key: e.path.toLowerCase().replace(/_/g, " ") })) };
}

export async function readHvsc(root, path) {
  const parts = path.split("/").filter(Boolean);
  let h = root;
  for (const p of parts.slice(0, -1)) h = await h.getDirectoryHandle(p);
  return new Uint8Array(await (await (await h.getFileHandle(parts.at(-1))).getFile()).arrayBuffer());
}

// every word in the path (underscores read as spaces), the first max hits; HVSC names its 2SID and
// 3SID tunes so (..._2SID.sid), and those are left out: SiDJ plays one-SID tunes
export const MULTI_SID = /_[23]SID\.sid$/i;
export function searchHvsc(index, query, max = 200) {
  const words = query.toLowerCase().replace(/_/g, " ").split(/\s+/).filter(Boolean), hits = [];
  if (!words.length) return hits;
  for (const e of index) if (!MULTI_SID.test(e.path) && words.every((w) => e.key.includes(w)) && hits.push(e) >= max) break;
  return hits;
}

export function prettyPath(path) {
  const parts = path.split("/").filter(Boolean), file = parts.pop().replace(/\.sid$/i, "").replace(/_/g, " ");
  return { title: file, where: parts.join(" / ").replace(/_/g, " ") };
}

// the folder, remembered between visits: IndexedDB keeps the handle, and the browser asks once
// more for access (one click) when it is used again
function kv() {
  return new Promise((res, rej) => {
    const r = indexedDB.open("sidj", 1);
    r.onupgradeneeded = () => r.result.createObjectStore("kv");
    r.onsuccess = () => res(r.result);
    r.onerror = () => rej(r.error);
  });
}
export async function rememberFolder(handle) {
  try {
    const db = await kv();
    await new Promise((res, rej) => { const t = db.transaction("kv", "readwrite"); t.objectStore("kv").put(handle, "hvsc"); t.oncomplete = res; t.onerror = () => rej(t.error); });
  } catch { /* not kept: picked again next time */ }
}
export async function recallFolder() {
  try {
    const db = await kv();
    return await new Promise((res) => { const r = db.transaction("kv").objectStore("kv").get("hvsc"); r.onsuccess = () => res(r.result || null); r.onerror = () => res(null); });
  } catch { return null; }
}
export async function folderAccess(handle) {
  if (await handle.queryPermission({ mode: "read" }) === "granted") return true;
  return await handle.requestPermission({ mode: "read" }) === "granted";
}

// the tunes that come with the site (songs/songs.json: file, name, author, released, lens)
export async function starterTunes(base = "songs/") {
  const get = async (f) => { const r = await fetch(base + f); if (!r.ok) throw fail(`${base}${f}: HTTP ${r.status}`); return r; };
  const list = await (await get("songs.json")).json();
  return Promise.all(list.map(async (e) => ({ ...e, bytes: new Uint8Array(await (await get(e.file)).arrayBuffer()) })));
}

// the bundled tunes into the store, those not there yet (same name, author and size): the count added
export async function addMissing(songs, tunes, onEach = null) {
  const have = (await songs.list()).filter(Boolean);
  let added = 0;
  for (const t of tunes) {
    if (have.some((e) => e.name === t.name && e.author === t.author && e.size === t.bytes.length)) continue;
    if (onEach) onEach(t);
    await songs.put(t.bytes, t.lens);
    added++;
  }
  return added;
}
