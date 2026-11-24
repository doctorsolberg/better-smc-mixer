// The editor's own device code (app/editor/js/mixer.js, link.js, cfgbin.js) live against the
// firmware's own code (build/host/devhost: the link, the configuration protocol, the control path,
// the core and the flash stores, built for the host): the layer between T-2 (a fixed transcript)
// and T-3 (the mixer). docs/integration.md, "T-2L".
//
// Run by tools/editortest/live.py, which starts devhost with --sysex-fd and --flash and joins this
// process's stdin and stdout to its config port: MIDI bytes both ways, as Web MIDI would carry
// them. Here Web MIDI is a stand-in (navigator.requestMIDIAccess) with one port pair named
// "SMC-Mixer Config" on stdin/stdout; the page's MixerDevice does the rest as in the browser.
// Reports go to stderr; exit 1 if a check failed.

import { readFileSync } from 'node:fs';
import { Codec } from '../../app/editor/js/cfgbin.js';
import { dumps } from '../../app/editor/js/config.js';
import { CMD } from '../../app/editor/js/link.js';
import { MixerDevice } from '../../app/editor/js/mixer.js';

const say = (s) => process.stderr.write(s + '\n');
if (typeof globalThis.CustomEvent === 'undefined') {
  globalThis.CustomEvent = class extends Event { constructor(type, o = {}) { super(type); this.detail = o.detail; } };
}

// --- Web MIDI on stdin/stdout ------------------------------------------------------------------

class Port {
  constructor(name, type) { this.name = name; this.type = type; this.state = 'connected'; this.onmidimessage = null; }
  send(bytes) { process.stdout.write(Uint8Array.from(bytes)); }
}
const input = new Port('SMC-Mixer Config', 'input');
const output = new Port('SMC-Mixer Config', 'output');
let sx = null;
process.stdin.on('data', (chunk) => {
  for (const b of chunk) {
    if (b === 0xF0) sx = [b];
    else if (sx && b === 0xF7) {
      sx.push(b);
      const data = Uint8Array.from(sx);
      sx = null;
      if (input.onmidimessage) input.onmidimessage({ data });
    } else if (sx && b < 0x80) sx.push(b);
    else if (b >= 0xF8) continue; // real-time: anywhere
    else if (b >= 0x80) {
      sx = null;
      if (input.onmidimessage) input.onmidimessage({ data: Uint8Array.from([b]) }); // not SysEx: shown, not parsed
    }
  }
});
// defined, not assigned: newer Node has a navigator of its own, a getter
Object.defineProperty(globalThis, 'navigator', { configurable: true, writable: true, value: {
  requestMIDIAccess: async () => ({
    inputs: new Map([['in', input]]), outputs: new Map([['out', output]]), onstatechange: null,
  }),
} });

// --- checks ------------------------------------------------------------------------------------

let failed = 0;
const ok = (name) => say(`ok   ${name}`);
function check(name, cond, detail = '') {
  if (cond) ok(name);
  else { failed++; say(`FAIL ${name}${detail ? ': ' + detail : ''}`); }
}
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function until(what, cond, ms = 3000) {
  const end = Date.now() + ms;
  while (Date.now() < end) { if (cond()) return true; await sleep(20); }
  return false;
}
const settled = (dev) => until('the push', () => !dev.pushing && !dev.pending, 8000);

const raw = JSON.parse(readFileSync('config/schema.json', 'utf8'));
const codec = new Codec(raw);
const factoryText = readFileSync('config/factory.json', 'utf8');
const testText = readFileSync('config/test.json', 'utf8');
// The mixer plays a file's configuration: the file it gives back, read by the editor, is the file,
// names (the binary form has none), ID and revision (P-6) aside. The expected side does not go
// through the editor's encoder: a fault there would cancel out.
const noNames = (v) => (Array.isArray(v) ? v.map(noNames) : v && typeof v === 'object'
  ? Object.fromEntries(Object.entries(v).filter(([k]) => k !== 'name').map(([k, x]) => [k, noNames(x)])) : v);
const content = (file) => { const { id, revision, ...rest } = noNames(file); return JSON.stringify(rest); };
const sameAs = (file, text) => content(file) === content(codec.toJson(codec.fromJson(JSON.parse(text))));
function edited(text, fn) {
  const doc = JSON.parse(text);
  fn(doc);
  return dumps(doc);
}

const dev = new MixerDevice(codec);
const errors = [];
dev.addEventListener('error', (e) => errors.push(e.detail.text));
const touches = [];
dev.addEventListener('touch', (e) => touches.push(e.detail));

async function main() {
  // Connect: the mixer found by asking it, its versions checked, its configuration read.
  await dev.connect(factoryText);
  check('connect: found, versions agree, the configuration read', dev.connected && dev.transport === 'USB');
  check('connect: the firmware reports the product\'s version (VERSION)', dev.fw.version === readFileSync('VERSION', 'utf8').trim(), dev.fw.version);
  check('connect: the mixer plays the factory configuration, as the editor', dev.running === factoryText);
  check('connect: nothing to save on a fresh mixer', dev.storage === 'clean', dev.storage);
  check('connect: the mixer offers EVENTS and SENT', dev.client.onEvent !== null && dev.withSent);

  // A live edit: one fader's CC (P-1), the mixer keeps running.
  const edit1 = edited(factoryText, (d) => { d.presets[0].pages[0].strips[0].fader.number = 1; });
  dev.apply(edit1, 'fader 1 to CC 1');
  await settled(dev);
  check('live edit: applied as SETs, not a restart', dev.logLines.some((l) => /live edit/.test(l.text) && /fader 1 to CC 1/.test(l.text)),
    dev.logLines.slice(-3).map((l) => l.text).join(' | '));
  check('live edit: the mixer now plays it', sameAs(await dev.pullFile(), edit1));
  check('live edit: not saved (the setting is manual)', dev.storage === 'unsaved', dev.storage);

  // Save (Q-26).
  const beforeSave = dev.revision;
  const named = edited(edit1, (d) => { d.name = 'Local name'; });
  dev.apply(named, 'local name');
  await settled(dev);
  check('names only: no device revision change', dev.revision === beforeSave && dev.running === named);
  await dev.saveNow();
  await until('clean', () => dev.storage === 'clean');
  check('save: the mixer starts with it now', dev.storage === 'clean' && dev.saved === dev.running, dev.storage);
  check('save: revision unchanged', dev.revision === beforeSave);

  // A two-SET edit, then Go back to the last save.
  const edit2 = edited(edit1, (d) => {
    d.presets[0].pages[0].strips[1].encoder.number = 2;
    d.presets[1].channel = (d.presets[1].channel ?? 1) + 1;
  });
  dev.apply(edit2, 'encoder 2 to CC 2 and preset 2 channel');
  await settled(dev);
  check('second edit: adopts the confirmed revision after two SETs', dev.revision === ((beforeSave + 2) >>> 0));
  check('second edit: unsaved', dev.storage === 'unsaved', dev.storage);
  const back = await dev.revert();
  check('go back: the saved configuration again', back !== null && sameAs(JSON.parse(back), edit1));
  const backDoc = back && JSON.parse(back);
  check('go back: canonical file metadata is the current revision and local name is kept',
    backDoc?.name === 'Local name' && backDoc.revision === dev.revision &&
    Object.keys(backDoc)[1] === 'revision');
  check('go back: the mixer plays it', sameAs(await dev.pullFile(), edit1));
  check('go back: clean', dev.storage === 'clean', dev.storage);

  // Act as a separate client: reuse the running ID and revision, but change the content.
  const beforeOutside = await dev.client.info();
  const outside = codec.decode(await dev.client.pull(beforeOutside.chunk));
  outside.presets[0].channel = (outside.presets[0].channel + 1) % 16;
  const changes = [];
  dev.addEventListener('changed', (e) => changes.push(e.detail));
  await dev.client.push(codec.encode(outside));
  check('outside upload: same input ID and revision still invalidate the editor baseline',
    await until('outside revision', () => changes.some((c) => c.revision === ((beforeOutside.revision + 1) >>> 0))) &&
    dev.diverged && dev.running === null);
  outside.id = new Uint8Array(16).fill(0x42);
  outside.revision = dev.revision;
  await dev.client.push(codec.encode(outside));
  await until('outside ID', () => dev.mixerId?.[0] === 0x42);
  await dev.revert();
  check('go back after outside upload: adopts the running identity and clears divergence',
    !dev.diverged && dev.mixerId === null && dev.revision === ((beforeOutside.revision + 3) >>> 0));

  // The tabs switch the mixer (P-3); its EVENT confirms it (P-2).
  dev.select(2, 3);
  check('select: the mixer goes to preset 3 page 4', await until('state', () => dev.panelState?.preset === 2 && dev.panelState?.page === 3),
    JSON.stringify(dev.panelState));
  await sleep(600); // past the editor's hold on older EVENTs
  check('select: the editor follows the mixer', dev.state.preset === 2 && dev.state.page === 3, JSON.stringify(dev.state));

  // A touch on the mixer selects the control in the editor (P-2), and what it sent is named (P-5).
  const inject = (p) => dev.client.call(CMD.inject, p);
  check('inject: on', (await inject([1])).status === 'ok');
  const sentBefore = dev.sentQ.length;
  await inject([2, 2, 0, 1, 0, 0]); // encoder 1, one click
  check('touch: the encoder is selected in the editor', await until('touch', () => touches.some((t) => t.from === 'mixer')),
    JSON.stringify(touches));
  check('touch: the control the mixer names is the first encoder', touches.some((t) => t.from === 'mixer' && t.id === 's0.encoder'),
    JSON.stringify(touches));
  // devhost's performance port is not on this pipe, so the SENT entries wait in sentQ for their
  // message: their source is what the monitor would show.
  await inject([2, 1, 32 + 3, 1, 0, 0]); // the row's Rew pressed
  await inject([2, 1, 32 + 3, 0, 0, 0]); // and released
  check('sent: the mixer names what it sent (P-5)',
    await until('sent', () => dev.sentQ.length > sentBefore && dev.sentQ.some((s) => dev.controlId(s.source) === 'row.rew')),
    JSON.stringify(dev.sentQ.map((s) => s.source)));
  await inject([0]);

  // A large change: the whole configuration (PUT, the core restarts).
  dev.apply(testText, 'open config/test.json');
  await settled(dev);
  check('open a file: sent whole', dev.logLines.some((l) => /open config\/test.json: applied/.test(l.text) && /core restarted/.test(l.text)),
    dev.logLines.slice(-2).map((l) => l.text).join(' | '));
  check('open a file: the mixer plays it', sameAs(await dev.pullFile(), testText));

  // The saving setting (Q-26): By itself, 3 s after the last change.
  const auto = edited(testText, (d) => { d.global = { ...(d.global ?? {}), save: 'auto' }; });
  dev.apply(auto, 'the mixer saves by itself');
  await settled(dev);
  check('autosave: waiting first', ['waiting', 'writing', 'clean'].includes(dev.storage), dev.storage);
  check('autosave: saved by itself after the quiet pause', await until('clean', () => dev.storage === 'clean', 7000), dev.storage);

  // Going back to M-Vave's firmware (STOCK, devhost --boot): the check, then back; the mixer
  // restarts on it, so the connection ends.
  const sc = await dev.stockCheck();
  check('stock: the check (sector 0 at ours, M-Vave\'s area checks)', sc.status === 'ok' && sc.pointer === 0x41000, JSON.stringify(sc));
  check('no error reported by the editor', errors.length === 0, errors.join(' | '));
  const toStock = await dev.backToStock();
  check('stock: back (sector 0 at M-Vave\'s area; the connection ends)', toStock.status === 'ok' && toStock.pointer === 0x1000 && toStock.tries === 1 && !dev.connected,
    JSON.stringify(toStock));
  dev.close();
}

main().catch((e) => { failed++; say(`FAIL ${e.stack ?? e}`); }).finally(() => {
  say(failed ? `live: ${failed} failed` : 'live: passed');
  process.exit(failed ? 1 : 0);
});
