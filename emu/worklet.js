// SPDX-License-Identifier: GPL-3.0-only
// After X0X's worklet.js (charlesvestal/fm1-x0x, GPL-3.0-only).
// SiDJ in an AudioWorklet: sidj.wasm is the whole FM-1 app (web/emu/sidj_web.c).
// Each render quantum asks it for 128 frames, which runs the device's clock forward; between
// quanta the page's input goes in, the MIDI out stream (the ASID SysEx) goes out as soon as it
// exists, and about 30 times a second the screen, the lights and any saved settings go out.

const clock = globalThis.performance ? () => globalThis.performance.now() : () => Date.now();

class SIDJ extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ex = null;
    this.sentBlits = -1;
    this.sentWrites = -1;
    this.lastFrame = 0;
    this.busy = 0;
    this.frames = 0;
    this.port.onmessage = (e) => this.onMessage(e.data);
  }

  bytes(ptr, n) { return new Uint8Array(this.mem.buffer, ptr, n); }

  async onMessage(m) {
    if (m.type === "load") {
      const now = () => BigInt(Math.round(currentTime * 1e9));
      const wasi = {
        clock_time_get: (id, prec, out) => {
          new DataView(this.mem.buffer).setBigUint64(out, now(), true);
          return 0;
        },
      };
      const stub = new Proxy(wasi, { get: (t, k) => (k in t ? t[k] : () => 0) });
      const env = new Proxy({}, { get: () => () => 0 });
      const { instance } = await WebAssembly.instantiate(m.wasm, { wasi_snapshot_preview1: stub, env });
      this.ex = instance.exports;
      this.mem = this.ex.memory;
      if (this.ex._initialize) this.ex._initialize();
      const max = this.ex.web_store_max();
      for (const [obj, bytes] of Object.entries(m.store || {})) {
        const b = new Uint8Array(bytes).subarray(0, max);
        this.bytes(this.ex.web_store(+obj), b.length).set(b);
        this.ex.web_store_set_len(+obj, b.length);
      }
      this.sentWrites = this.ex.web_store_writes();
      this.ex.web_master(m.master ?? 2800);
      this.ex.web_boot();
      this.port.postMessage({ type: "ready", poolSize: this.ex.web_pool_size() });
    } else if (!this.ex) {
      return;
    } else if (m.type === "buttons") {
      this.ex.web_buttons(m.mask >>> 0);
    } else if (m.type === "keys") {
      this.ex.web_keys(m.mask >>> 0);
    } else if (m.type === "enc") {
      this.ex.web_enc(m.role, m.n | 0);
    } else if (m.type === "master") {
      this.ex.web_master(m.value | 0);
    } else if (m.type === "put") {                    // a .sid file into the store: {id, bytes, lens, md5, quiet}
      const ex = this.ex, b = new Uint8Array(m.bytes), n = Math.min(b.length, ex.web_song_max());
      this.bytes(ex.web_song_buf(), n).set(b.subarray(0, n));
      const lens = (m.lens || []).slice(0, ex.web_song_max_lens());
      new Uint16Array(this.mem.buffer, ex.web_song_lens(), lens.length).set(lens);
      this.bytes(ex.web_song_md5(), 16).set(m.md5 ? new Uint8Array(m.md5).subarray(0, 16) : new Uint8Array(16));
      const rc = b.length > ex.web_song_max() ? -1 : ex.web_song_put(n, lens.length, m.quiet ? 1 : 0);
      this.port.postMessage({ type: "put", id: m.id, rc, count: ex.web_song_count(), used: ex.web_pool_used() });
    } else if (m.type === "changed") {
      this.ex.web_songs_changed();
    } else if (m.type === "delete") {
      this.ex.web_song_delete(m.k | 0);
      this.port.postMessage({ type: "put", id: -1, rc: 0, count: this.ex.web_song_count(), used: this.ex.web_pool_used() });
    } else if (m.type === "clear") {
      this.ex.web_songs_clear();
      this.port.postMessage({ type: "put", id: -1, rc: 0, count: 0, used: this.ex.web_pool_used() });
    }
  }

  midi() {                                          // the MIDI out stream, as soon as there is some
    const ex = this.ex, n = ex.web_midi_out_len();
    if (!n) return;
    const bytes = this.bytes(ex.web_midi_out(), n).slice();
    ex.web_midi_out_clear();
    this.port.postMessage({ type: "midi", bytes }, [bytes.buffer]);
  }

  publish() {
    const ex = this.ex;
    const blits = ex.web_blits();
    const msg = { type: "frame", buttons: ex.web_lit_buttons() >>> 0, keys: ex.web_lit_keys() >>> 0 };
    if (this.frames >= 44100) {                       // the share of real time spent running the device
      msg.load = Math.round(100 * this.busy / (this.frames / 44.1));
      this.busy = this.frames = 0;
    }
    const transfer = [];
    if (blits !== this.sentBlits) {
      this.sentBlits = blits;
      msg.fb = new Uint16Array(this.mem.buffer, ex.web_fb(), 240 * 240).slice();
      transfer.push(msg.fb.buffer);
    }
    const writes = ex.web_store_writes();
    if (writes !== this.sentWrites) {
      this.sentWrites = writes;
      msg.store = {};
      for (let o = 0; o < ex.web_nobj(); o++) {
        const n = ex.web_store_len(o);
        if (n >= 0) msg.store[o] = this.bytes(ex.web_store(o), n).slice();
      }
    }
    this.port.postMessage(msg, transfer);
  }

  process(inputs, outputs) {
    if (!this.ex) return true;
    const out = outputs[0];
    const n = out[0].length;
    const t0 = clock();
    this.ex.web_render(n);
    this.busy += clock() - t0;
    this.frames += n;
    out[0].set(new Float32Array(this.mem.buffer, this.ex.web_out_l(), n));
    if (out[1]) out[1].set(new Float32Array(this.mem.buffer, this.ex.web_out_r(), n));
    this.midi();
    if (currentTime - this.lastFrame >= 1 / 30) {
      this.lastFrame = currentTime;
      this.publish();
    }
    return true;
  }
}

registerProcessor("sidj", SIDJ);
