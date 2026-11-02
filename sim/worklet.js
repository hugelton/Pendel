// SPDX-License-Identifier: GPL-3.0-only
// Copyright (C) 2026 Leo Kuroshita (@kurogedelic), Hügelton Instruments
// The Pocket AMP in an AudioWorklet: the whole app (engine, buttons, screen) runs here as WebAssembly, as it runs
// on the device between its audio interrupt and its main loop. The page sends buttons, MIDI and the input choice;
// this sends back the screen about 30 times a second.

const FRAME_MS = 33;
const PENTA = [40, 43, 45, 47, 50, 52, 55, 57, 59, 62, 64, 67];   // E minor pentatonic, guitar range

class PendelProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.ready = false;
    this.keys = 0;
    this.input = 'demo';
    this.nextFrame = 0;
    this.ks = { line: new Float32Array(2048), n: 0, p: 0, next: 0 };
    this.port.onmessage = (e) => {
      try {
        this.message(e.data);
      } catch (err) {
        this.port.postMessage({ type: 'error', text: String(err && err.stack || err) });
      }
    };
    this.port.onmessageerror = () => this.port.postMessage({ type: 'error', text: 'a message could not be read' });
  }

  message(d) {
    if (d.type === 'init') {
      const inst = new WebAssembly.Instance(new WebAssembly.Module(d.bytes), {});   // (a Module does not cross into a worklet)
      this.x = inst.exports;
      this.x._initialize();
      this.x.sim_init(d.palette | 0);
      this.mem = this.x.memory.buffer;
      this.inP = this.x.sim_in();
      this.outP = this.x.sim_out();
      this.scrP = this.x.sim_screen();
      this.ready = true;
      this.port.postMessage({ type: 'ready', rate: sampleRate });
    } else if (!this.ready) {
      return;
    } else if (d.type === 'keys') {
      this.keys = d.down;
    } else if (d.type === 'volume') {
      this.x && this.x.sim_volume(d.dir);
    } else if (d.type === 'midi') {
      this.x.sim_midi(d.data[0], d.data[1] | 0, d.data[2] | 0);
    } else if (d.type === 'input') {
      this.input = d.input;
    } else if (d.type === 'palette') {
      this.x.sim_init(d.palette | 0);          // a fresh start in the new colours (as a power cycle)
    }
  }

  // a plucked string now and then: something to loop without a guitar
  demo(out, n) {
    const ks = this.ks;
    for (let i = 0; i < n; i++) {
      if (ks.next-- <= 0) {
        const note = PENTA[(Math.random() * PENTA.length) | 0];
        ks.n = Math.max(8, Math.round(sampleRate / (440 * Math.pow(2, (note - 69) / 12))));
        for (let k = 0; k < ks.n; k++) ks.line[k] = (Math.random() * 2 - 1) * 0.45;
        ks.p = 0;
        ks.next = (sampleRate * (Math.random() < 0.3 ? 0.25 : 0.5)) | 0;
      }
      if (!ks.n) { out[i] = 0; continue; }
      const q = ks.p + 1 === ks.n ? 0 : ks.p + 1;
      out[i] = ks.line[ks.p];
      ks.line[ks.p] = (ks.line[ks.p] + ks.line[q]) * 0.498;
      ks.p = q;
    }
  }

  process(inputs, outputs) {
    if (!this.ready) return true;
    const out = outputs[0];
    const n = out[0].length;
    const inp = new Int16Array(this.mem, this.inP, n);
    if (this.input === 'mic' && inputs[0] && inputs[0][0]) {
      const m = inputs[0][0];
      for (let i = 0; i < n; i++) inp[i] = Math.max(-32768, Math.min(32767, Math.round(m[i] * 32767)));
    } else if (this.input === 'demo') {
      const f = this.demoBuf || (this.demoBuf = new Float32Array(1024));
      this.demo(f, n);
      for (let i = 0; i < n; i++) inp[i] = Math.round(f[i] * 32767);
    } else {
      inp.fill(0);
    }
    const now = Math.round((currentFrame / sampleRate) * 1000);
    this.x.sim_keys(this.keys, now);
    this.x.sim_audio(n);
    const o = new Int16Array(this.mem, this.outP, 2 * n);
    for (let i = 0; i < n; i++) {
      out[0][i] = o[2 * i] / 32768;
      if (out[1]) out[1][i] = o[2 * i + 1] / 32768;
    }
    if (now >= this.nextFrame) {
      this.nextFrame = now + FRAME_MS;
      if (this.x.sim_frame(now)) {
        const px = new Uint8ClampedArray(this.mem, this.scrP, 240 * 240 * 4).slice();
        this.port.postMessage({ type: 'screen', px }, [px.buffer]);
      }
    }
    return true;
  }
}

registerProcessor('pendel', PendelProcessor);
