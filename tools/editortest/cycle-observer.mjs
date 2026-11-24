// Host-only diagnostic seam tests; no browser, ports or hardware.
import test from 'node:test';
import assert from 'node:assert/strict';
import { existsSync, readFileSync } from 'node:fs';
import { StockLink, install } from '../../app/editor/js/stocklink.js';
import { StockDevice } from './stockdev.mjs';
import { createHash, webcrypto } from 'node:crypto';
if (!globalThis.crypto) globalThis.crypto = webcrypto;
// Node 18 host image has Event/EventTarget but no CustomEvent (browser does).
globalThis.CustomEvent ??= class extends Event {
  constructor(type, options) { super(type); this.detail = options?.detail; }
};
const moduleUrl = new URL('../../app/editor/js/cycle-observer.js', import.meta.url);
const load = async () => { assert.ok(existsSync(moduleUrl), 'cycle observer module must exist'); return import(moduleUrl); };

test('page identity is stable per observer and different across lifetimes',async()=>{
 const {createCycleObserver}=await load();const a={},b={};
 createCycleObserver({target:a});createCycleObserver({target:b});
 assert.equal(a.__smcCycleObserver.snapshot().pageId,a.__smcCycleObserver.snapshot().pageId);
 assert.notEqual(a.__smcCycleObserver.snapshot().pageId,b.__smcCycleObserver.snapshot().pageId);
});

test('preview validates before UI approval and binds the exact release inputs without bytes', async () => {
  const { previewPlan, releaseSnapshot } = await load();
  const v = JSON.parse(readFileSync('tools/editortest/vectors/install.json'));
  const backup = new Uint8Array(524288).fill(255);
  for (const [at, bytes] of v.flashes.stored) backup.set(Buffer.from(bytes, 'base64'), at);
  const app = Buffer.from(v.inputs.app, 'base64'), cfg = Buffer.from(v.inputs.cfg, 'base64');
  const view = new StockDevice(backup).readView(0, 524288);
  // Stock service transforms the header and encrypted area independently.
  const device = new StockDevice(backup);
  for (let at = 0; at < view.length; at += 256) view.set(device.readView(at, 256), at);
  const result = await previewPlan(view, app, cfg);
  assert.equal(result.valid, true); assert.equal(result.plan.failed, 0);
  assert.equal(result.files.app.sha256, createHash('sha256').update(app).digest('hex'));
  assert.equal(result.files.backup.size, 524288);
  assert.doesNotMatch(JSON.stringify(result), /"data"|"before":\[/);
  assert.equal((await previewPlan(new Uint8Array(10), app, cfg)).valid, false);
  const manifest = { product: 'better-smc-mixer', install: { app: 'app.bin', cfg: 'cfg.bin' }, files: {} };
  const release = await releaseSnapshot({ url: 'http://localhost/release/manifest.json', manifest, install: manifest.install });
  assert.equal(release.files.app.url, 'http://localhost/release/app.bin');
  assert.equal(release.semanticManifestSha256, createHash('sha256').update(JSON.stringify(manifest)).digest('hex'));
  assert.equal(release.manifestSha256, null);
});

test('observer errors and hostile pre-existing publisher cannot disrupt device events', async () => {
  const { createCycleObserver, observeDevice } = await load();
  const target = {}; Object.defineProperty(target, '__smcCycleObserver', { value: 'untrusted' });
  const observer = createCycleObserver({ target, readState: () => { throw Error('broken'); } });
  const dev = new EventTarget(); observeDevice(dev, observer);
  const event = new Event('error'); event.detail = { text: 'actual source error' };
  assert.doesNotThrow(() => dev.dispatchEvent(event));
  assert.equal(target.__smcCycleObserver, 'untrusted');
  const healthy = {}; const second = createCycleObserver({ target: healthy }); observeDevice(dev, second);
  dev.dispatchEvent(event);
  assert.equal(healthy.__smcCycleObserver.snapshot().latest.error.message, 'actual source error');
});

test('the editor wires initialization, release, preapproval plan and terminal evidence', () => {
  const source = readFileSync('app/editor/js/app.js', 'utf8');
  for (const needed of ["from './cycle-observer.js'", 'observeDevice(dev, cycle)', "cycle.record('initialized'", 'releaseSnapshot(r,',
    'previewPlan(b, app, cfgTool)', "cycle.record('plan'", "cycle.record('complete'", "cycle.record('rollback'", "cycle.record('config_loaded'", "source: 'connect'", "source: 'bluetooth'", "source: 'pull'", "cycle.record('rollback_check'", "evidence.stage = 'release_loading'", "cycleJob.stage = 'installing'"]) assert.ok(source.includes(needed), needed);
  assert.match(source, /plan:\s*\(p\).*cycle\.record\('plan'/);
});

test('fresh pull records actual clean storage and saved configuration evidence', async () => {
  const { MixerDevice } = await import('../../app/editor/js/mixer.js');
  const { Codec } = await import('../../app/editor/js/cfgbin.js');
  const { deviceSnapshot } = await load();
  const codec = new Codec(JSON.parse(readFileSync('config/schema.json')));
  const device = new MixerDevice(codec); const blob = codec.fileToBinary({ schemaVersion: codec.version });
  device.connected = true;
  const calls = [];
  device.client = {
    info: async () => { calls.push('info'); return { version: 4, binaryVersion: 1, schemaVersion: 1, store: 'clean', chunk: 128, revision: 0, id: null }; },
    pull: async () => { calls.push('pull'); return blob; },
    state: async () => { calls.push('state'); return { preset: 0, page: 3, shift: false, bluetooth: 'on', store: 'clean', revision: 0 }; },
  };
  const file = await device.pullFile();
  const snapshot = deviceSnapshot(device, device.running);
  assert.deepEqual(snapshot.panel, { preset: 0, page: 3, shift: false, bluetooth: 'on' });
  assert.deepEqual(calls, ['info', 'pull', 'state']);
  assert.equal(snapshot.panelObservation.fresh, true);
  assert.equal(snapshot.panelObservation.readSequence, 1);
  assert.equal(snapshot.panelObservation.connectionEpoch, device.connectionEpoch);
  assert.equal(snapshot.config.savedMatchesRunning, true);
  assert.equal(snapshot.identity.protocol, 4);
  assert.ok(file);
});

// CFG_INFO and CFG_STATE use a 16-byte array (null for the all-zero ID), not the file's UUID.
async function pullFixture({ revision = 6, id = '12345678-1234-5678-9abc-123456789abc' } = {}) {
  const { MixerDevice } = await import('../../app/editor/js/mixer.js');
  const { Codec } = await import('../../app/editor/js/cfgbin.js');
  const codec = new Codec(JSON.parse(readFileSync('config/schema.json')));
  const file = { schemaVersion: codec.version, revision, ...(id ? { id } : {}) };
  const model = codec.fromJson(file);
  const identity = { revision, id: model.id.some(b => b) ? Array.from(model.id) : null };
  const device = new MixerDevice(codec), calls = [];
  device.connected = true;
  const info = { version: 4, binaryVersion: codec.binaryVersion, schemaVersion: codec.version,
    chunk: 128, store: 'clean', generation: 2, ...identity };
  const state = { preset: 0, page: 3, shift: false, bluetooth: 'on', store: 'waiting', ...identity };
  device.client = {
    info: async () => { calls.push('info'); return info; },
    pull: async () => { calls.push('pull'); return codec.fileToBinary(file); },
    state: async () => { calls.push('state'); return state; },
  };
  return { device, calls, file, info, state };
}

test('pull keeps newer CFG_STATE storage instead of certifying stale clean CFG_INFO', async () => {
  const { deviceSnapshot } = await load();
  const { device, calls, file } = await pullFixture();
  assert.equal((await device.pullFile()).revision, file.revision);
  const snapshot = deviceSnapshot(device, device.running);
  assert.equal(snapshot.identity.storage, 'waiting');
  assert.equal(snapshot.config.clean, false);
  assert.equal(snapshot.config.savedMatchesRunning, false);
  assert.equal(device.saved, null);
  assert.equal(snapshot.panelObservation.fresh, true);
  assert.deepEqual(calls, ['info', 'pull', 'state'], 'reading must not add a writer');
});

test('pull rejects revision changes between CFG_INFO and CFG_STATE without publishing stale clean evidence', async () => {
  const { deviceSnapshot } = await load();
  const { device, calls, state } = await pullFixture();
  device.running = device.saved = 'previous confirmed configuration';
  const previous = device.running;
  device.pending = { doc: previous, what: 'queued stale edit' };
  state.revision = 7;
  await assert.rejects(device.pullFile(), { name: 'Error', message: /configuration changed while reading/ });
  const snapshot = deviceSnapshot(device, previous);
  assert.equal(snapshot.identity.storage, 'waiting');
  assert.equal(snapshot.config.clean, false);
  assert.equal(snapshot.config.savedMatchesRunning, false);
  assert.equal(snapshot.panelObservation.fresh, false);
  assert.equal(snapshot.panelObservation.readSequence, 0);
  assert.equal(device.running, null, 'a rejected pull must invalidate the stale delta baseline');
  assert.equal(device.saved, null);
  assert.equal(device.diverged, true);
  assert.equal(device.pending, null);
  assert.deepEqual(calls, ['info', 'pull', 'state']);
});

test('pull rejects identity changes between CFG_INFO and CFG_STATE even at the same revision', async () => {
  const { deviceSnapshot } = await load();
  const { device, state, calls } = await pullFixture();
  state.id = [...state.id]; state.id[15] ^= 1;
  state.store = 'clean';
  await assert.rejects(device.pullFile(), /configuration changed while reading/);
  assert.equal(device.running, null);
  assert.equal(device.saved, null);
  assert.equal(deviceSnapshot(device).panelObservation.fresh, false);
  assert.deepEqual(calls, ['info', 'pull', 'state']);
});

test('pull rejects blob identity or revision that disagrees with otherwise matching replies', async (t) => {
  const { deviceSnapshot } = await load();
  for (const field of ['revision', 'id']) await t.test(field, async () => {
    const { device, info, state, calls } = await pullFixture();
    if (field === 'revision') info.revision = state.revision = 7;
    else { info.id = [...info.id]; info.id[0] ^= 1; state.id = [...info.id]; }
    state.store = 'clean';
    await assert.rejects(device.pullFile(), /configuration changed while reading/);
    assert.equal(device.running, null);
    assert.equal(device.saved, null);
    assert.equal(deviceSnapshot(device).panelObservation.fresh, false);
    assert.deepEqual(calls, ['info', 'pull', 'state']);
  });
});

test('consistent pull accepts equal ID bytes and uses latest state for every storage outcome', async (t) => {
  const { deviceSnapshot } = await load();
  for (const store of ['clean', 'none', 'waiting', 'writing', 'failed', 'unsaved']) await t.test(store, async () => {
    const { device, info, state } = await pullFixture();
    info.store = 'waiting';
    state.store = store;
    state.id = [...info.id]; // equal wire IDs, not the same array reference
    await device.pullFile();
    const snapshot = deviceSnapshot(device, device.running);
    const clean = store === 'clean' || store === 'none';
    assert.equal(snapshot.identity.storage, clean ? 'clean' : store);
    assert.equal(snapshot.config.clean, clean);
    assert.equal(snapshot.config.savedMatchesRunning, clean);
  });
  await t.test('null wire ID and omitted file revision mean zero', async () => {
    const { device, state } = await pullFixture({ id: null, revision: 0 });
    state.store = 'clean';
    const file = await device.pullFile();
    assert.equal(file.id, undefined);
    assert.equal(file.revision, undefined);
    assert.equal(device.saved, device.running);
  });
});

test('rejected pull can be retried with consistent current snapshots', async () => {
  const { deviceSnapshot } = await load();
  const { device, info, state, file } = await pullFixture();
  state.revision = 7;
  await assert.rejects(device.pullFile(), /configuration changed while reading/);
  info.revision = file.revision = 7;
  state.store = 'clean';
  assert.equal((await device.pullFile()).revision, 7);
  const snapshot = deviceSnapshot(device, device.running);
  assert.equal(snapshot.config.clean, true);
  assert.equal(snapshot.config.savedMatchesRunning, true);
  assert.equal(snapshot.panelObservation.fresh, true);
  assert.equal(snapshot.panelObservation.readSequence, 1);
});

test('a new pull invalidates prior panel freshness until its own state reply succeeds', async () => {
  const { MixerDevice } = await import('../../app/editor/js/mixer.js');
  const { Codec } = await import('../../app/editor/js/cfgbin.js');
  const { deviceSnapshot } = await load();
  const codec = new Codec(JSON.parse(readFileSync('config/schema.json')));
  const device = new MixerDevice(codec);
  let rejectState;
  device.connected = true;
  device.client = {
    info: async () => ({ store: 'clean', chunk: 128, revision: 0, id: null }),
    pull: async () => codec.fileToBinary({ schemaVersion: codec.version }),
    state: async () => ({ preset: 0, page: 3, shift: false, bluetooth: 'on', store: 'clean', revision: 0 }),
  };
  await device.pullFile();
  device.client.state = () => new Promise((_, reject) => { rejectState = reject; });
  const pending = device.pullFile();
  assert.equal(deviceSnapshot(device).panelObservation.fresh, false);
  while (!rejectState) await new Promise(resolve => setImmediate(resolve));
  rejectState(new Error('panel read failed'));
  await assert.rejects(pending, /panel read failed/);
  assert.equal(deviceSnapshot(device).panelObservation.fresh, false);
  assert.equal(deviceSnapshot(device).panelObservation.readSequence, 1);
});

test('connect awaits an explicit panel read even when EVENTS has not delivered its first state', async (t) => {
  const { MixerDevice } = await import('../../app/editor/js/mixer.js');
  const { LinkClient } = await import('../../app/editor/js/link.js');
  const { Codec } = await import('../../app/editor/js/cfgbin.js');
  const { deviceSnapshot } = await load();
  const codec = new Codec(JSON.parse(readFileSync('config/schema.json')));
  const device = new MixerDevice(codec);
  const original = Object.getOwnPropertyDescriptor(globalThis, 'navigator');
  Object.defineProperty(globalThis, 'navigator', { configurable: true, value: { requestMIDIAccess: async () => ({}) } });
  t.after(() => { device.close(); delete globalThis.navigator; if (original) Object.defineProperty(globalThis, 'navigator', original); });
  const input = { onmidimessage: null }, output = { name: 'test BLE endpoint', send() {} };
  device.find = async () => ({ input, output, linkInfo: { caps: ['config', 'events'], buildId: 'host-test', fwVersion: '0.1.0' } });
  device.listenMidi = () => {};
  t.mock.method(LinkClient.prototype, 'info', async () => ({ version: 4, binaryVersion: codec.binaryVersion, schemaVersion: codec.version, store: 'clean', revision: 0 }));
  t.mock.method(LinkClient.prototype, 'pull', async () => codec.fileToBinary({ schemaVersion: codec.version }));
  t.mock.method(LinkClient.prototype, 'watch', async () => {});
  let resolveState;
  t.mock.method(LinkClient.prototype, 'state', () => new Promise(resolve => { resolveState = resolve; }));
  let complete = false;
  const attempt = device.connect(null).then(() => { complete = true; });
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(complete, false, 'connection readiness must wait for CFG_STATE, not just EVENTS acknowledgement');
  assert.equal(typeof resolveState, 'function');
  resolveState({ preset: 0, page: 3, shift: false, bluetooth: 'on', store: 'clean', revision: 0 });
  await attempt;
  assert.deepEqual(deviceSnapshot(device).panel, { preset: 0, page: 3, shift: false, bluetooth: 'on' });
  assert.equal(deviceSnapshot(device).panelObservation.fresh, true);
});

test('default and optimistic editor state never become observed physical panel evidence', async () => {
  const { MixerDevice } = await import('../../app/editor/js/mixer.js');
  const { Codec } = await import('../../app/editor/js/cfgbin.js');
  const { deviceSnapshot } = await load();
  const device = new MixerDevice(new Codec(JSON.parse(readFileSync('config/schema.json'))));
  device.connected = true;
  assert.deepEqual(deviceSnapshot(device).panel, { preset: null, page: null, shift: null, bluetooth: null });
  assert.equal(deviceSnapshot(device).panelObservation.fresh, false);
  device.panel({ preset: 0, page: 3, shift: false, bluetooth: 'on', revision: 0, store: 'clean' });
  device.selecting = { preset: 1, page: 2, until: performance.now() + 1000 };
  device.state = { preset: 1, page: 2, shift: true };
  device.panel({ preset: 0, page: 3, shift: false, bluetooth: 'off', revision: 0, store: 'clean' });
  assert.deepEqual(deviceSnapshot(device).panel, { preset: 0, page: 3, shift: false, bluetooth: 'off' });
  assert.equal(deviceSnapshot(device).panelObservation.fresh, false, 'an EVENT alone does not prove an explicit state read');
  device.close();
  assert.deepEqual(deviceSnapshot(device).panel, { preset: null, page: null, shift: null, bluetooth: null });
});

test('a late panel read cannot publish freshness into a replacement connection', async () => {
  const { MixerDevice } = await import('../../app/editor/js/mixer.js');
  const { Codec } = await import('../../app/editor/js/cfgbin.js');
  const { deviceSnapshot } = await load();
  const codec = new Codec(JSON.parse(readFileSync('config/schema.json')));
  const device = new MixerDevice(codec);
  let resolveState;
  device.connected = true;
  device.client = {
    info: async () => ({ store: 'clean' }),
    pull: async () => codec.fileToBinary({ schemaVersion: codec.version }),
    state: () => new Promise(resolve => { resolveState = resolve; }),
  };
  const pending = device.pullFile();
  while (!resolveState) await new Promise(resolve => setImmediate(resolve));
  device.close();
  device.connected = true;
  device.client = {};
  resolveState({ preset: 0, page: 3, shift: false, bluetooth: 'on', revision: 0, store: 'clean' });
  await assert.rejects(pending, /connection changed while reading the panel/);
  assert.equal(deviceSnapshot(device).panelObservation.fresh, false);
  assert.equal(deviceSnapshot(device).panel.page, null);
  assert.equal(device.running, null);
  device.close();
});

test('source Bluetooth failures retain their actual name/message before user-facing wrapping', async () => {
  const source = readFileSync('app/editor/js/mixer.js', 'utf8');
  assert.ok(source.includes("this.emit('source-error', { source: 'bluetooth'"));
});

test('release metadata distinguishes exact response bytes from its semantic manifest hash', async () => {
  const { releaseSnapshot } = await load();
  const manifest = { product: 'better-smc-mixer', files: {} };
  const bytes = new TextEncoder().encode(JSON.stringify(manifest, null, 2) + '\n');
  const hash = createHash('sha256').update(bytes).digest('hex');
  const result = await releaseSnapshot({ url: 'http://localhost/release/manifest.json', manifest }, hash);
  assert.equal(result.manifestSha256, hash);
  assert.notEqual(result.manifestSha256, result.semanticManifestSha256);
});

test('public observer is read-only, bounded, detached and excludes private device bytes', async () => {
  const { createCycleObserver, deviceSnapshot } = await load();
  const target = {};
  const dev = { real: true, connected: true, transport: 'BLE', fw: { kind: 'ours', version: '0.1.0' },
    info: { buildId: 'abcd', protocol: 4, binary: 1, schema: 1 }, cfgInfo: { binaryVersion: 1, schemaVersion: 1 },
    storage: 'clean', running: 'config', saved: 'config', state: { preset: 2, page: 1, shift: true },
    panelState: { preset: 2, page: 1, shift: true }, stock: { secret: 'vendor' } };
  const observer = createCycleObserver({ target, limit: 3, readState: () => ({ initialized: true, ...deviceSnapshot(dev, 'config') }) });
  observer.record('initialized', { backup: [1, 2], secret: 'never' });
  for (let i = 0; i < 6; i++) observer.record('progress', { stage: 'backup', done: i, total: 6, bytes: [3] });
  const first = target.__smcCycleObserver.snapshot();
  assert.equal(first.version, 1); assert.equal(first.initialized, true);
  assert.equal(first.events.length, 3); assert.equal(first.sequence, 7);
  assert.equal(first.identity.build, 'abcd'); assert.equal(first.identity.binaryForm, 1);
  assert.equal(first.config.clean, true); assert.equal(first.config.editorMatchesRunning, true);
  assert.equal(first.panel.shift, true);
  assert.doesNotMatch(JSON.stringify(first), /vendor|never|"bytes"|"backup":\[/);
  first.events[0].stage = 'tampered'; first.identity.build = 'tampered';
  assert.equal(target.__smcCycleObserver.snapshot().identity.build, 'abcd');
  assert.notEqual(target.__smcCycleObserver.snapshot().events[0].stage, 'tampered');
  assert.deepEqual(Object.keys(target.__smcCycleObserver), ['snapshot']);
  assert.throws(() => { target.__smcCycleObserver = {}; }, TypeError);
  assert.throws(() => { target.__smcCycleObserver.record = () => {}; }, TypeError);
});

test('validated plan hook precedes any write and telemetry exceptions cannot alter install', async () => {
  const v = JSON.parse(readFileSync('tools/editortest/vectors/install.json'));
  const flash = new Uint8Array(524288).fill(255);
  for (const [at, bytes] of v.flashes.stored) flash.set(Buffer.from(bytes, 'base64'), at);
  const dev = new StockDevice(flash);
  const stages = [];
  let planned = false;
  const result = await install(new StockLink(dev), { app: Buffer.from(v.inputs.app, 'base64'), cfg: Buffer.from(v.inputs.cfg, 'base64'),
    hooks: { plan(p) { assert.equal(dev.erases.length, 0); assert.equal(p.failed, 0); assert.ok(!JSON.stringify(p).includes('data')); planned = true; throw Error('listener'); },
      observe(event) { stages.push(event.stage); throw Error('listener'); } } });
  assert.equal(planned, true);
  assert.ok(stages.includes('sector0_start')); assert.ok(stages.includes('sector0_verified'));
  assert.equal(result.plan.report.failed, 0);
  assert.equal(createHash('sha256').update(dev.flash).digest('hex'), v.cases.find(c => c.name === 'stored, with cfg_tool.bin').sha256.after);
});
