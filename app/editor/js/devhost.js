// ?devhost: the page against the firmware built for the host (docs/integration.md, T-4, "The page
// against devhost"). serve.py starts build/host/devhost for each WebSocket on /devhost and joins
// its config port and its performance port to it; here a stand-in for Web MIDI
// (navigator.requestMIDIAccess) offers them as the mixer's two USB ports, "SMC-Mixer Config" and
// "SMC-Mixer", so the page's own MixerDevice (js/mixer.js) connects, edits, saves and shows what
// the mixer sends as it does on the mixer. Each WebSocket message: the port (0 config,
// 1 performance), then MIDI bytes. Without ?devhost this module does nothing.

class Port extends EventTarget {
  constructor(id, name, type, send) {
    super();
    Object.assign(this, { id, name, type, manufacturer: 'devhost', state: 'connected', connection: 'open', onmidimessage: null });
    if (send) this.send = send;
  }
  deliver(data) { if (this.onmidimessage) this.onmidimessage({ data, target: this }); }
}

// MIDI bytes to messages: SysEx whole, channel messages by their status, real-time alone.
function splitter(deliver) {
  let sx = null, msg = null, need = 0;
  const LEN = { 0x8: 3, 0x9: 3, 0xA: 3, 0xB: 3, 0xC: 2, 0xD: 2, 0xE: 3 };
  return (bytes) => {
    for (const b of bytes) {
      if (b >= 0xF8) { deliver(Uint8Array.of(b)); continue; }
      if (b === 0xF0) { sx = [b]; msg = null; continue; }
      if (b === 0xF7) { if (sx) { sx.push(b); deliver(Uint8Array.from(sx)); } sx = null; continue; }
      if (sx) { if (b < 0x80) { sx.push(b); continue; } sx = null; }
      if (b >= 0x80) {
        need = LEN[b >> 4] ?? 1;
        msg = [b];
      } else if (msg) {
        msg.push(b);
      } else continue; // a data byte with no status: dropped
      if (msg && msg.length === need) { deliver(Uint8Array.from(msg)); msg = need > 1 ? [msg[0]] : null; } // running status
    }
  };
}

export function installDevhost() {
  if (typeof location === 'undefined' || !new URLSearchParams(location.search).has('devhost')) return false;
  const ws = new WebSocket(`${location.protocol === 'https:' ? 'wss' : 'ws'}://${location.host}/devhost`);
  ws.binaryType = 'arraybuffer';
  const send = (bytes) => { if (ws.readyState === WebSocket.OPEN) ws.send(Uint8Array.of(0, ...bytes)); };
  const cfgIn = new Port('cfg-in', 'SMC-Mixer Config', 'input');
  const cfgOut = new Port('cfg-out', 'SMC-Mixer Config', 'output', send);
  const perfIn = new Port('perf-in', 'SMC-Mixer', 'input');
  const perfOut = new Port('perf-out', 'SMC-Mixer', 'output', () => {}); // the mixer takes no MIDI from the editor here
  const toCfg = splitter((m) => cfgIn.deliver(m));
  const toPerf = splitter((m) => perfIn.deliver(m));
  ws.onmessage = (e) => {
    const d = new Uint8Array(e.data);
    if (d[0] === 0) toCfg(d.subarray(1)); else if (d[0] === 1) toPerf(d.subarray(1));
  };
  const opened = new Promise((resolve, reject) => {
    ws.onopen = resolve;
    ws.onerror = () => reject(new Error('no devhost: is serve.py running, and build/host/devhost built (make -C firmware sim)?'));
  });
  const access = {
    inputs: new Map([[cfgIn.id, cfgIn], [perfIn.id, perfIn]]),
    outputs: new Map([[cfgOut.id, cfgOut], [perfOut.id, perfOut]]),
    sysexEnabled: true,
    onstatechange: null,
  };
  ws.onclose = () => {
    for (const p of [cfgIn, cfgOut, perfIn, perfOut]) p.state = 'disconnected';
    if (access.onstatechange) access.onstatechange({ port: cfgIn });
  };
  Object.defineProperty(navigator, 'requestMIDIAccess', {
    configurable: true,
    value: async () => { await opened; return access; },
  });
  return true;
}
