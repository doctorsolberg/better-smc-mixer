// The editor's contract tests (docs/integration.md, T-1), in Node without npm: the editor's own
// modules against the configuration files and the vectors the firmware's side wrote
// (tools/editortest/vectors.py). Run from the repository's root, in the build image:
//
//   tools/env/test_editor.sh                 (make quick and make test run it)
//   node tools/editortest/run.mjs [-v]       (any Node 18 or later, no packages)
//   node tools/editortest/run.mjs --write-generated
//                                            write vectors/gen-N.json from the editor's code
//                                            (then vectors.py --write for their sends)
//
// Prints a line per check and exits 1 if any failed.

import { readFileSync, readdirSync, writeFileSync } from 'node:fs';
import { spawnSync } from 'node:child_process';
import vm from 'node:vm';
import { Schema } from '../../app/editor/js/schema.js';
import { roundtrip, faults, generate, sends, binaryForm, binaryRefusals, edits } from './contract.mjs';
import { Codec } from '../../app/editor/js/cfgbin.js';
import * as link from './link.mjs';
import { failurePaths, connectionRaces, transferStates } from './connection.mjs';
import { criticalWords } from './words.mjs';
import { LinkClient } from '../../app/editor/js/link.js';
import { VERSION, compareVersions } from '../../app/editor/js/version.js';
import { createHash, webcrypto } from 'node:crypto';
// Node 18 (the build image's) has no global crypto; the page's js/release.js uses the browser's
if (!globalThis.crypto) globalThis.crypto = webcrypto;
import * as IP from '../../app/editor/js/installplan.js';
import * as F from '../../app/editor/js/flashimg.js';
import { StockLink, install as installStock, StockError } from '../../app/editor/js/stocklink.js';
import { StockDevice } from './stockdev.mjs';
import { MixerDevice } from '../../app/editor/js/mixer.js';
import { findRelease, releaseFiles } from '../../app/editor/js/release.js';
import { Reassembler, pieces } from '../../app/editor/js/blestock.js';

const VECTORS = 'tools/editortest/vectors';
const MODULES = 'app/editor/js';
const CONFIGS = ['config/test.json', 'config/factory.json'];
const verbose = process.argv.includes('-v');
const read = (p) => readFileSync(p, 'utf8');
const S = new Schema(JSON.parse(read('config/schema.json')));

let failed = 0;
function check(name, problems) {
  const list = problems ?? [];
  if (list.length) {
    failed++;
    console.log(`FAIL ${name}: ${list.length} problem(s); the first:`);
    for (const p of list.slice(0, 12)) console.log('  ' + (typeof p === 'string' ? p : JSON.stringify(p)));
  } else if (verbose) console.log(`ok   ${name}`);
}

const generated = generate(S);
if (process.argv.includes('--write-generated')) {
  for (const g of generated) writeFileSync(`${VECTORS}/${g.name}`, g.text);
  console.log(`wrote ${generated.map((g) => g.name).join(', ')}; now python3 -I tools/editortest/vectors.py --write`);
  process.exit(0);
}
const genFiles = readdirSync(VECTORS).filter((f) => /^gen-\d+\.json$/.test(f)).sort();

// Files: the configuration files come back byte for byte; the editor still writes the generated
// configurations the vectors were made from.
check('files: config/*.json and the generated ones through the reader and writer',
  [...CONFIGS, ...genFiles.map((f) => `${VECTORS}/${f}`)]
    .map((p) => [p, roundtrip(S, read(p))]).filter(([, l]) => l !== null).map(([p, l]) => `${p}: differs from line ${l}`));
check('files: the editor writes the generated configurations as vectors/ has them', [
  ...generated.filter((g) => !genFiles.includes(g.name) || read(`${VECTORS}/${g.name}`) !== g.text)
    .map((g) => `${g.name}: the editor writes it otherwise (node tools/editortest/run.mjs --write-generated, then vectors.py --write)`),
  ...genFiles.filter((f) => !generated.some((g) => g.name === f)).map((f) => `${f}: the editor no longer writes it`),
]);

// Checks: the faults smcconf names, and none in the valid files.
check('checks: the editor names the field smcconf names (faults.json)', faults(S, JSON.parse(read(`${VECTORS}/faults.json`))));
check('checks: no fault in the valid files', [...CONFIGS, ...genFiles.map((f) => `${VECTORS}/${f}`)]
  .flatMap((p) => S.checkFile(JSON.parse(read(p))).map((e) => `${p}: ${e}`)));

// Sends: every sends vector against the editor's messages.
for (const f of readdirSync(VECTORS).filter((x) => /^sends-.*\.json$/.test(x)).sort()) {
  const vector = JSON.parse(read(`${VECTORS}/${f}`));
  const r = sends(S, read(vector.about.config), vector);
  const none = r.messages < 1000 ? [`only ${r.messages} messages: is the vector complete?`] : [];
  check(`sends: ${f} (${r.probes} probes, ${r.messages} messages)`, [...none, ...r.failures]);
}

// The binary form: the editor's codec (js/cfgbin.js) against smcconf's.
const codec = new Codec(JSON.parse(read('config/schema.json')));
const binCases = JSON.parse(read(`${VECTORS}/binary.json`));
check(`binary: cfgbin.js writes and reads the binary form as smcconf does (${binCases.length} cases)`, binaryForm(codec, binCases, read));
check('versions: a newer file or binary form says to update the editor (Q-29)', (() => {
  const out = [];
  const say = (fn) => { try { fn(); return ''; } catch (e) { return String(e.message ?? e); } };
  const file = say(() => codec.fromJson({ schemaVersion: S.version + 1 }));
  if (!/newer version.*update the editor/.test(file)) out.push(`a newer file: ${file}`);
  const blob = codec.fileToBinary({ schemaVersion: S.version });
  blob[4] = codec.binaryVersion + 1;
  const bin = say(() => codec.binaryToFile(blob));
  if (!/binary form .*newer version.*update the editor/.test(bin)) out.push(`a newer binary form: ${bin}`);
  const checked = S.checkFile({ schemaVersion: S.version + 1, presets: [] }).join(' | ');
  if (!/newer version.*update the editor/.test(checked)) out.push(`the file check: ${checked}`);
  return out;
})());
check('binary: cfgbin.js refuses every file of faults.json', binaryRefusals(codec, JSON.parse(read(`${VECTORS}/faults.json`))));
const editCases = JSON.parse(read(`${VECTORS}/edits.json`));
check(`edits: cfgbin.js makes the live edits (CFG_SET) smcconf makes (${editCases.length} cases)`, edits(codec, editCases, read));

// Narrow wording checks: the exact fine step and Program Change's press-only exception.
check('words: fader references, fine-step labels and program descriptions match core rules', criticalWords(S,
  readdirSync(VECTORS).filter((f) => /^sends-.*\.json$/.test(f)).map((f) => {
    const vector = JSON.parse(read(`${VECTORS}/${f}`));
    return { vector, text: read(vector.about.config) };
  })));

// The link client (T-2): the transcript of the device's own code, frame by frame and as a session.
const transcript = JSON.parse(read(`${VECTORS}/link-v1.json`));
check(`link: link.js makes and reads every frame of link-v1.json (${transcript.exchanges.length} exchanges)`, link.frames(transcript));
check('link: the session of link-v1.json through LinkClient (pull, push, save, set, revert, state, events, select, refusals, a lost reply, stock)',
  await link.session(transcript, link.pushedBlob(transcript)));
check('link: a call with no reply is sent again with the same seq, then given up', await link.noReply());
check('link: closing stops retries, queued calls and model uploads, and keeps accepted replies', await link.closed(codec));
check('link: a mixer with another protocol, binary form or schema is refused (FR-CFG-07)', link.versions());
check('link: the monitor names the control of each message the mixer sent (P-5, js/mixer.js)', await link.sentJoin(codec));
check('connection: permission refusal, missing ports and cancelled choices', await failurePaths(codec));
check('connection: old replies and queued operations cannot cross reconnection', await connectionRaces(codec));
check('connection: unconfirmed transfer states and partial failures do not claim success', await transferStates(codec));
// Node's built-in runner keeps the page/model defect regressions package-free (Node 18+).
const defects = spawnSync(process.execPath, ['--test', 'tools/editortest/editor-defects.test.mjs'], { encoding: 'utf8' });
check('editor: firmware ownership, Save, identity, monitor and offline regressions', defects.status === 0 ? [] : [defects.stdout, defects.stderr]);

// The product's version (docs/build.md, "The version"): the order of versions, and GET_INFO's text.
check('version: version.js is VERSION, and orders versions as semver does', (() => {
  const out = [];
  if (VERSION !== read('VERSION').trim()) out.push(`version.js ${VERSION}, VERSION ${read('VERSION').trim()}`);
  for (const [a, b, want] of [['0.1.0', '0.1.0', 0], ['0.1.1', '0.1.0', 1], ['0.2.0', '0.1.9', 1], ['0.10.0', '0.9.0', 1],
    ['1.0.0', '0.99.99', 1], ['0.2.0-rc.1', '0.2.0', -1], ['0.2.0-rc.2', '0.2.0-rc.1', 1], ['0.0.1', '0.1.0', -1]]) {
    const got = Math.sign(compareVersions(a, b));
    if (got !== want) out.push(`compare(${a}, ${b}) = ${got}, not ${want}`);
  }
  return out;
})());
check('link: GET_INFO gives the firmware\'s version after the build ID, and none from an older firmware', await (async () => {
  const out = [];
  const head = [1, 1, 0, 0x81, 0, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 2, 0xab, 0xcd]; // ..., build ID of 2 bytes
  const ask = (payload) => LinkClient.prototype.linkInfo.call({ call: async () => ({ status: 'ok', payload: Uint8Array.from(payload) }) });
  const a = await ask([...head, 5, ...Buffer.from('0.1.0')]);
  if (a.fwVersion !== '0.1.0' || a.buildId !== 'abcd') out.push(`with a version: ${JSON.stringify(a)}`);
  const b = await ask(head);
  if (b.fwVersion !== '' || b.buildId !== 'abcd') out.push(`an older firmware: ${JSON.stringify(b)}`);
  return out;
})());

// The installer's plan (js/installplan.js) against tools/image/installplan.py, case by case.
const install = JSON.parse(read(`${VECTORS}/install.json`));
check(`install: installplan.js gives installplan.py's plan, writes and images (${install.cases.length} cases)`, (() => {
  const out = [];
  const b64 = (s) => new Uint8Array(Buffer.from(s, 'base64'));
  const sha = (d) => (d && d.length ? createHash('sha256').update(d).digest('hex') : null);
  const flash = (runs) => { const f = new Uint8Array(IP.FLASH_SIZE).fill(0xFF); for (const [at, d] of runs) f.set(b64(d), at); return f; };
  const images = Object.fromEntries(Object.entries(install.flashes).map(([k, v]) => [k, flash(v)]));
  const app = b64(install.inputs.app), cfg = b64(install.inputs.cfg), tail = b64(install.inputs.tail);
  for (const c of install.cases) {
    const o = c.options;
    const dump = images[c.flash].subarray(0, o.size ?? IP.FLASH_SIZE);
    let p;
    try { p = IP.plan(dump, app, o.cfg ? cfg : null, o.tail ? tail : new Uint8Array(0), o.at ?? IP.SLOT, o.vm ?? null); } catch (e) { out.push(`${c.name}: threw ${e.stack}`); continue; }
    const got = IP.asJson(p);
    for (const k of Object.keys(c.plan)) {
      if (JSON.stringify(got[k]) !== JSON.stringify(c.plan[k])) {
        const a = got[k], b = c.plan[k];
        const i = Array.isArray(a) && Array.isArray(b) ? a.findIndex((x, n) => JSON.stringify(x) !== JSON.stringify(b[n])) : -1;
        out.push(`${c.name}: ${k}${i >= 0 ? `[${i}]` : ''}: ${JSON.stringify(i >= 0 ? a[i] : a)} instead of ${JSON.stringify(i >= 0 ? b[i] : b)}`);
      }
    }
    const shas = { after: sha(p.after), phys: sha(p.phys), writes: p.writes.map((w) => sha(w.data)) };
    if (JSON.stringify(shas) !== JSON.stringify(c.sha256)) out.push(`${c.name}: the images differ: ${JSON.stringify(shas)}`);
  }
  return out;
})());

// The installer (js/stocklink.js) on a stand-in for M-Vave's firmware (stockdev.mjs).
check('install: the installer writes the plan\'s flash through M-Vave\'s SysEx, and refuses what it must', await (async () => {
  const out = [];
  const b64 = (s) => new Uint8Array(Buffer.from(s, 'base64'));
  const sha = (d) => createHash('sha256').update(d).digest('hex');
  const flash = (runs) => { const f = new Uint8Array(IP.FLASH_SIZE).fill(0xFF); for (const [at, d] of runs) f.set(b64(d), at); return f; };
  const stored = flash(install.flashes.stored);
  const app = b64(install.inputs.app), cfg = b64(install.inputs.cfg);
  const want = install.cases.find((c) => c.name === 'stored, with cfg_tool.bin').sha256.after;
  const run = async (dev, opts = {}) => {
    const link = new StockLink(dev, { timeout: 50, ackTimeout: 50 });
    let backupSeen = null;
    try {
      const r = await installStock(link, { app, cfg, hooks: { backup: async (b) => { backupSeen = { size: b.length, flashThen: sha(dev.flash) }; } }, ...opts });
      return { r, backupSeen };
    } catch (e) {
      if (!(e instanceof StockError)) out.push(`threw ${e.stack}`);
      return { error: e.message, backupSeen };
    }
  };
  // installed: the device's flash is the plan's flash after, byte for byte
  let dev = new StockDevice(stored);
  let res = await run(dev);
  if (res.error || sha(dev.flash) !== want) out.push(`a plain installation: ${res.error || 'the flash differs from the plan'}`);
  if (!res.backupSeen || res.backupSeen.size !== IP.FLASH_SIZE || res.backupSeen.flashThen !== sha(stored)) out.push('the backup is not the whole flash before any write');
  if (dev.erases.some((s) => s !== 0 && (s < 0x41000 || s >= 0x6E000))) out.push(`erased outside our sectors: ${dev.erases.map(F.hx)}`);
  // replies lost now and then: sent again, the same result
  dev = new StockDevice(stored, { loseEvery: 97 });
  res = await run(dev);
  if (res.error || sha(dev.flash) !== want) out.push(`with lost replies: ${res.error || 'the flash differs'}`);
  // no list of versions: another M-Vave version installs on the plan's checks alone
  dev = new StockDevice(stored, { name: 'SMC-Mixer_015' });
  res = await run(dev);
  if (res.error || sha(dev.flash) !== want) out.push(`another version: ${res.error}`);
  // over BLE (M-Vave's AE40 service, js/blestock.js): raw packets, each request cut into 20-byte
  // writes the device puts together, each reply into 20-byte notifications the transport does
  dev = new StockDevice(stored, { raw: true });
  const ble = (() => {
    const atDev = new Reassembler(), atHost = new Reassembler();
    let handler = () => {};
    dev.onmessage = (reply) => { for (const n of pieces(reply, 20)) for (const p of atHost.feed(n)) handler(p); };
    return {
      raw: true,
      send: (pkt) => { for (const w of pieces(pkt, 20)) for (const p of atDev.feed(w)) dev.send(p); },
      set onmessage(fn) { handler = fn; },
    };
  })();
  {
    const link = new StockLink(ble, { timeout: 50, ackTimeout: 50 });
    try {
      await installStock(link, { app, cfg, hooks: { backup: async () => {} } });
      if (sha(dev.flash) !== want) out.push('over BLE: the flash differs from the plan');
    } catch (e) { out.push(`over BLE: ${e.message}`); }
  }
  // the reassembler: noise before a packet skipped, a packet across pieces, two in one piece
  {
    const r = new Reassembler(), a = Uint8Array.from([0, 0x59, 0x11, 1, 0, 0, 7, 0xF8]), b = Uint8Array.from([0, 0x59, 0, 0, 0, 0, 0xFF]);
    const got = [...r.feed([0x80, 0x12, 0, 0x59, 0x11]), ...r.feed([1, 0, 0, 7, 0xF8, 0, 0x59, 0]), ...r.feed([0, 0, 0, 0xFF])];
    if (got.length !== 2 || !F.equal(got[0], a) || !F.equal(got[1], b)) out.push(`reassembler: ${got.map((g) => Buffer.from(g).toString('hex'))}`);
  }
  // another device's firmware: nothing but the name asked
  dev = new StockDevice(stored, { name: 'SMC-PAD_010' });
  res = await run(dev);
  if (!/not an SMC-Mixer/.test(res.error || '') || dev.n !== 1) out.push(`another device: ${res.error}, ${dev.n} requests`);
  // sector 0 write-protected: our area written (harmless), sector 0 as before, a clear message
  dev = new StockDevice(stored, { protect: [[0, 0x1000]] });
  res = await run(dev);
  if (!/did not take/.test(res.error || '') || !F.equal(dev.flash.subarray(0, 0x1000), stored.subarray(0, 0x1000))) out.push(`sector 0 protected: ${res.error}`);
  // our area write-protected: stops before sector 0
  dev = new StockDevice(stored, { protect: [[0x42000, 0x43000]] });
  res = await run(dev);
  if (!/does not read back/.test(res.error || '') || !F.equal(dev.flash.subarray(0, 0x1000), stored.subarray(0, 0x1000))) out.push(`area protected: ${res.error}`);
  // already ours: the plan refuses, nothing erased or written
  const installed = flash(install.flashes.installed);
  dev = new StockDevice(installed);
  res = await run(dev);
  if (!/plan does not hold/.test(res.error || '') || dev.erases.length || dev.writes || sha(dev.flash) !== sha(installed)) out.push(`already installed: ${res.error}`);
  // an earlier installation of ours (the update) and a run that stopped: the plan's flash after
  for (const [key, name] of [['earlier', 'an earlier installation of ours (the update)'], ['stopped', 'a run of this installation that stopped']]) {
    dev = new StockDevice(flash(install.flashes[key]));
    res = await run(dev);
    if (res.error || sha(dev.flash) !== install.cases.find((c) => c.name === name).sha256.after || res.r.found !== key) out.push(`${key}: ${res.error || 'the flash differs from the plan'}`);
  }
  // the cable pulled at any point before sector 0: the mixer starts M-Vave's firmware, and the
  // installer run again finishes the job (a first installation and an update)
  let cuts = 0;
  for (const [key, name] of [['stored', 'stored, with cfg_tool.bin'], ['earlier', 'an earlier installation of ours (the update)']]) {
    const start = flash(install.flashes[key]);
    const done = install.cases.find((c) => c.name === name).sha256.after;
    for (const n of [5, 400, 2100, 2160, 2200, 2240, 2300, 2400, 2500]) {
      dev = new StockDevice(start, { dieAfter: n });
      res = await run(dev);
      if (!res.error) continue; // done before the cut
      if (!F.equal(dev.flash.subarray(0, 0x1000), start.subarray(0, 0x1000))) continue; // inside the sector 0 step
      cuts++;
      const again = new StockDevice(dev.flash);
      const r2 = await run(again);
      if (r2.error || sha(again.flash) !== done) out.push(`${key}, cut after ${n} requests: ${r2.error || 'the flash differs from the plan'}`);
    }
  }
  if (cuts < 8) out.push(`only ${cuts} cuts fell before sector 0`);
  // the guards: the stock area, its VM, the loader, BTIF..USRFLASH are never erased or written
  const link = new StockLink(new StockDevice(stored), { timeout: 50, ackTimeout: 50 });
  for (const a of [0x0, 0x1000, 0x24000, 0x6E000, 0x7B000, 0x7D000]) {
    try { await link.erase(a); out.push(`erase ${F.hx(a)} allowed`); } catch (e) { if (!(e instanceof StockError)) out.push(String(e)); }
    try { await link.program(a, new Uint8Array(4)); out.push(`write ${F.hx(a)} allowed`); } catch (e) { if (!(e instanceof StockError)) out.push(String(e)); }
  }
  try { await link.program(0x410F0, new Uint8Array(32)); out.push('a write across a page allowed'); } catch { /* refused */ }
  return out;
})());

// The page's device (js/mixer.js) finds M-Vave's firmware on its service port when nothing answers
// the link, and installs through it: Web MIDI here is a stand-in with the stock firmware's two port
// pairs, "SMC-Mixer-Private" (stockdev.mjs) and "SMC-Mixer-Master" (silent).
check('install: MixerDevice finds M-Vave\'s firmware on its service port and installs over it', await (async () => {
  const out = [];
  if (typeof globalThis.CustomEvent === 'undefined') {
    globalThis.CustomEvent = class extends Event { constructor(type, o = {}) { super(type); this.detail = o.detail; } };
  }
  const b64 = (x) => new Uint8Array(Buffer.from(x, 'base64'));
  const flash = (runs) => { const f = new Uint8Array(IP.FLASH_SIZE).fill(0xFF); for (const [at, d] of runs) f.set(b64(d), at); return f; };
  const sha = (d) => createHash('sha256').update(d).digest('hex');
  const stockDev = new StockDevice(flash(install.flashes.stored));
  const port = (name, type, send) => ({ name, type, state: 'connected', onmidimessage: null, send });
  const privIn = port('SMC-Mixer-Private', 'input'), privOut = port('SMC-Mixer-Private', 'output', (b) => stockDev.send(Uint8Array.from(b)));
  stockDev.onmessage = (m) => privIn.onmidimessage && privIn.onmidimessage({ data: m });
  const mastIn = port('SMC-Mixer-Master', 'input'), mastOut = port('SMC-Mixer-Master', 'output', () => {});
  // defined, not assigned: newer Node has a navigator of its own, a getter
  const before = Object.getOwnPropertyDescriptor(globalThis, 'navigator');
  Object.defineProperty(globalThis, 'navigator', { configurable: true, writable: true, value: { requestMIDIAccess: async () => ({ inputs: new Map([['a', mastIn], ['b', privIn]]), outputs: new Map([['a', mastOut], ['b', privOut]]), onstatechange: null }) } });
  const dev = new MixerDevice(new Codec(JSON.parse(read('config/schema.json'))));
  let connections = 0;
  dev.addEventListener('connection', () => connections++);
  await dev.connect(null);
  if (!dev.connected || dev.fw.kind !== 'stock' || dev.fw.version !== 'SMC-Mixer_012' || connections !== 1) out.push(`connect: ${JSON.stringify(dev.fw)}, connected ${dev.connected}`);
  const steps = new Set();
  let backup = null;
  const r = await dev.installFirmware({ app: b64(install.inputs.app), cfg: b64(install.inputs.cfg),
    hooks: { progress: (step) => steps.add(step), backup: async (b) => { backup = b; } } });
  const want = install.cases.find((c) => c.name === 'stored, with cfg_tool.bin').sha256.after;
  if (sha(stockDev.flash) !== want) out.push('the flash after differs from the plan');
  if (!backup || backup.length !== IP.FLASH_SIZE) out.push('no backup handed to the page');
  if (!steps.has('backup') || !steps.has('area') || !steps.has('check') || !steps.has('sector 0')) out.push(`progress steps: ${[...steps]}`);
  if (r.found !== '') out.push(`found: ${r.found}`);
  if (!dev.logLines.some((l) => /M-Vave's firmware answered on SMC-Mixer-Private: SMC-Mixer_012/.test(l.text))) out.push('no log line for the stock firmware');
  // ours is not offered STOCK without a link: the check says so, nothing sent
  const c = await dev.stockCheck();
  if (c.status !== 'not offered') out.push(`stockCheck on M-Vave's firmware: ${c.status}`);
  dev.close();
  delete globalThis.navigator;
  if (before) Object.defineProperty(globalThis, 'navigator', before);
  return out;
})());

// Over Bluetooth (MixerDevice.connectBluetooth, js/blestock.js): a stand-in for Web Bluetooth whose
// GATT device carries M-Vave's AE40 service to the stand-in firmware, raw packets in 20-byte pieces.
check('install: MixerDevice installs over M-Vave\'s Bluetooth service (AE40)', await (async () => {
  const out = [];
  const b64 = (x) => new Uint8Array(Buffer.from(x, 'base64'));
  const flash = (runs) => { const f = new Uint8Array(IP.FLASH_SIZE).fill(0xFF); for (const [at, d] of runs) f.set(b64(d), at); return f; };
  const sha = (d) => createHash('sha256').update(d).digest('hex');
  const stockDev = new StockDevice(flash(install.flashes.stored), { raw: true, name: 'SMC-Mixer_013' });
  const atDev = new Reassembler();
  const notify = new EventTarget();
  stockDev.onmessage = (reply) => {
    for (const piece of pieces(reply, 20)) {
      const e = new Event('characteristicvaluechanged');
      Object.defineProperty(e, 'target', { value: { value: new DataView(Uint8Array.from(piece).buffer) } });
      notify.dispatchEvent(e);
    }
  };
  let maxWrite = 0;
  const device = Object.assign(new EventTarget(), { name: 'SMC-Mixer', gatt: {
    connected: false,
    async connect() { this.connected = true; return this; },
    disconnect() { this.connected = false; device.dispatchEvent(new Event('gattserverdisconnected')); },
    async getPrimaryService(uuid) {
      if (uuid !== 0xae40) throw new Error('no such service');
      return { async getCharacteristic(c) {
        if (c === 0xae41) return { async writeValueWithoutResponse(b) { maxWrite = Math.max(maxWrite, b.length); for (const p of atDev.feed(b)) stockDev.send(p); } };
        if (c === 0xae42) return Object.assign(notify, { async startNotifications() {}, async stopNotifications() {} });
        throw new Error('no such characteristic');
      } };
    },
  } });
  let asked = null;
  const before = Object.getOwnPropertyDescriptor(globalThis, 'navigator');
  Object.defineProperty(globalThis, 'navigator', { configurable: true, writable: true, value: { bluetooth: { requestDevice: async (o) => { asked = o; return device; } } } });
  const dev = new MixerDevice(new Codec(JSON.parse(read('config/schema.json'))));
  try {
    await dev.connectBluetooth();
    if (!dev.connected || dev.fw.kind !== 'stock' || dev.fw.version !== 'SMC-Mixer_013' || dev.transport !== 'BLE') out.push(`connect: ${JSON.stringify(dev.fw)}, ${dev.transport}`);
    if (!asked?.optionalServices?.includes(0xae40)) out.push('the chooser was not asked for AE40');
    let backup = null;
    await dev.installFirmware({ app: b64(install.inputs.app), cfg: b64(install.inputs.cfg), hooks: { backup: async (b) => { backup = b; } } });
    const want = install.cases.find((c) => c.name === 'stored, with cfg_tool.bin').sha256.after;
    if (sha(stockDev.flash) !== want) out.push('the flash after differs from the plan');
    if (!backup || backup.length !== IP.FLASH_SIZE) out.push('no backup');
    if (maxWrite > 20) out.push(`a write of ${maxWrite} bytes`);
    // the link going away ends the connection
    device.gatt.disconnect();
    if (dev.connected) out.push('still connected after the Bluetooth link ended');
  } catch (e) { out.push(`threw ${e.stack}`); }
  dev.close();
  delete globalThis.navigator;
  if (before) Object.defineProperty(globalThis, 'navigator', before);
  return out;
})());

// The release the editor installs (js/release.js): the manifest found beside the editor or in
// build/release/, the install files checked against its sizes and SHA-256 (a fetch stand-in here).
check('release: the editor finds the package and checks what it installs', await (async () => {
  const out = [];
  const app = new Uint8Array([1, 2, 3, 4]), cfgTool = new Uint8Array([5, 6]);
  const sum = (b) => createHash('sha256').update(b).digest('hex');
  const manifest = { product: 'better-smc-mixer', version: null, install: { app: 'firmware/controller-ble.bin', cfg: 'firmware/cfg_tool.bin' },
    files: { 'firmware/controller-ble.bin': { size: 4, sha256: sum(app) }, 'firmware/cfg_tool.bin': { size: 2, sha256: sum(cfgTool) } } };
  const site = (files) => async (url) => {
    const b = files[new URL(url).pathname];
    if (b === undefined) return { ok: false, status: 404 };
    return { ok: true, status: 200, json: async () => b, arrayBuffer: async () => b.buffer.slice(b.byteOffset, b.byteOffset + b.byteLength) };
  };
  const files = { '/build/release/manifest.json': manifest, '/build/release/firmware/controller-ble.bin': app, '/build/release/firmware/cfg_tool.bin': cfgTool };
  const rel = await findRelease('http://x/app/editor/index.html', site(files));
  if (rel?.url !== 'http://x/build/release/manifest.json') out.push(`the editor's release: ${rel?.url}`);
  const got = await releaseFiles(rel, site(files));
  if (sum(got.app) !== sum(app) || sum(got.cfg) !== sum(cfgTool)) out.push('the install files differ');
  // the package's own editor/ looks beside itself first
  const pkg = await findRelease('http://x/p/editor/index.html', site({ '/p/manifest.json': manifest }));
  if (pkg?.url !== 'http://x/p/manifest.json') out.push(`the package's release: ${pkg?.url}`);
  // a file that does not match is refused, and so is a package that installs nothing
  const bad = { ...files, '/build/release/firmware/cfg_tool.bin': new Uint8Array([5, 7]) };
  if (!(await releaseFiles(rel, site(bad)).then(() => false, (e) => /does not match/.test(e.message)))) out.push('a changed file was taken');
  const none = await findRelease('http://x/app/editor/', site({ '/build/release/manifest.json': { ...manifest, install: null } }));
  if (none?.install !== null || !(await releaseFiles(none, site({})).then(() => false, () => true))) out.push('a package without install files');
  if ((await findRelease('http://x/app/editor/', site({}))) !== null) out.push('no package: not null');
  return out;
})());

// Modules: every module of the editor parses (app.js needs a browser to run). In this process
// where Node runs with --experimental-vm-modules (test_editor.sh), else a node --check each.
check('modules: every module of the editor parses', readdirSync(MODULES).filter((f) => f.endsWith('.js')).flatMap((f) => {
  const src = read(`${MODULES}/${f}`);
  if (vm.SourceTextModule) {
    try { new vm.SourceTextModule(src, { identifier: f }); return []; } catch (e) { return [`${f}: ${e}`]; }
  }
  const p = spawnSync(process.execPath, ['--input-type=module', '--check'], { input: src, encoding: 'utf8' });
  return p.status === 0 ? [] : [`${f}: ${p.stderr.split('\n').slice(0, 4).join(' ')}`];
}));

console.log(failed ? `editor contract tests: ${failed} failed` : 'editor contract tests: ok');
process.exit(failed ? 1 : 0);
