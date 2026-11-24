// Host-only regressions: no native device APIs, packages or browser sessions.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { openTransport } from '../../app/editor/js/blestock.js';
import { MixerDevice } from '../../app/editor/js/mixer.js';
import { Codec } from '../../app/editor/js/cfgbin.js';
import { LinkError } from '../../app/editor/js/link.js';
import { StockLink } from '../../app/editor/js/stocklink.js';
import { StockDevice } from './stockdev.mjs';
import vm from 'node:vm';

const codec = new Codec(JSON.parse(readFileSync('config/schema.json', 'utf8')));
globalThis.CustomEvent ??= class extends Event { constructor(t, o) { super(t); this.detail = o?.detail; } };
const deferred = () => { let resolve, reject; const promise = new Promise((a, b) => { resolve = a; reject = b; }); return { promise, resolve, reject }; };
const tick = () => new Promise((r) => setImmediate(r));
function connected(client = {}) {
  const m = new MixerDevice(codec);
  Object.assign(m, { connected: true, client, running: '{"working":true}', saved: null, storage: 'unsaved', revision: 7, mixerId: new Uint8Array(16).fill(1) });
  return m;
}

test('GATT read-only DOMException retains cause, name and stage without mutation', async () => {
  const original = new DOMException('GATT connection failed', 'NetworkError');
  const device = { gatt: { connected: false, connect: async () => { throw original; } } };
  await assert.rejects(openTransport(device), (e) => {
    assert.equal(e.cause, original);
    assert.equal(e.name, 'NetworkError');
    assert.match(e.message, /^connect: GATT connection failed$/);
    assert.equal(original.message, 'GATT connection failed');
    return true;
  });
});

test('Save resolves only after request acknowledgement and confirmed clean storage', async () => {
  const request = deferred(), clean = deferred();
  let reads = 0, settled = false;
  const m = connected({ save: () => request.promise, info: async () => ++reads === 1 ? { store: 'writing' } : clean.promise });
  const task = m.saveNow().then(() => { settled = true; });
  await tick();
  assert.equal(settled, false);
  assert.equal(m.saved, null);
  request.resolve();
  await new Promise((r) => setTimeout(r, 150));
  assert.equal(settled, false, 'writing storage must not resolve Save');
  clean.resolve({ store: 'clean', generation: 5 });
  await task;
  assert.equal(m.storage, 'clean');
  assert.equal(m.saved, m.running);
  m.close();
});

test('Save refusal and failed storage reject instead of reporting success', async () => {
  for (const failedStore of [false, true]) {
    const m = connected({ save: async () => { if (!failedStore) throw new LinkError('save unavailable'); }, info: async () => ({ store: 'failed' }) });
    await assert.rejects(m.saveNow(), /save|unavailable/i);
    assert.equal(m.saved, null);
    m.close();
  }
});

test('Save on a closed connection rejects and cannot certify a replacement connection', async () => {
  const wait = deferred(), m = connected({ save: () => wait.promise });
  const task = m.saveNow();
  const rejected = assert.rejects(task, /connection|connected/);
  m.close();
  wait.resolve();
  await rejected;
  await assert.rejects(m.saveNow(), /connected/);
});

test('one firmware owner excludes a second install/back and disconnect during backup through completion', async () => {
  const v = JSON.parse(readFileSync('tools/editortest/vectors/install.json', 'utf8'));
  const bytes = (b) => new Uint8Array(Buffer.from(b, 'base64'));
  const flash = new Uint8Array(0x80000).fill(255);
  for (const [at, b] of v.flashes.stored) flash.set(bytes(b), at);
  const stock = new StockDevice(flash), m = connected();
  m.stock = new StockLink(stock, { timeout: 50, ackTimeout: 50 });
  const backup = deferred(), ready = deferred();
  const opts = { app: bytes(v.inputs.app), cfg: bytes(v.inputs.cfg), hooks: { backup: async () => { ready.resolve(); await backup.promise; } } };
  const task = m.installFirmware(opts);
  await ready.promise;
  const count = stock.n;
  await assert.rejects(m.installFirmware({ ...opts, hooks: {} }), /firmware.*(running|progress|active)/i);
  await assert.rejects(m.backToStock(), /firmware.*(running|progress|active)/i);
  assert.equal(stock.n, count, 'competing operations must not touch the transport');
  m.setConnected(false);
  assert.equal(m.connected, true, 'disconnect must not destroy an active job');
  backup.resolve();
  await task;
  assert.equal(m.firmwareJob.active, false);
  m.close();
});

// Exercise the page's real dialog functions without importing/booting the whole application.
const app = readFileSync('app/editor/js/app.js', 'utf8');
function pageDialog() {
  const node = (tag, attrs = {}, ...children) => ({ tag, ...attrs, children, replaceChildren(...kids) { this.children = kids; } });
  const dlg = { open: false, showModal() { this.open = true; }, close() { this.open = false; } }, body = node('div');
  const m = connected();
  const ctx = vm.createContext({ h: node, put: (n, ...kids) => n.replaceChildren(...kids), icon: () => null, dev: m,
    $: (s) => s === '#dlg' ? dlg : body, renderAll() {}, toast() {}, cycle: { record() {} } });
  vm.runInContext(app.slice(app.indexOf('function stepsDialog('), app.indexOf('// A pretend transfer:')) +
    app.slice(app.indexOf('function dialog(content)'), app.indexOf('function download(obj')), ctx);
  return { ctx, dlg, m, body };
}

test('closing/reopening a firmware dialog preserves its live DOM, step and pending confirmation', () => {
  const { ctx, dlg, m } = pageDialog();
  const owner = m.beginFirmwareJob('install');
  let builds = 0, next;
  ctx.steps = [{ title: 'Backup', body: (go) => { builds++; next = go; return 'backup'; } },
    { title: 'Confirm', body: () => { builds++; return 'confirm'; } }];
  ctx.owner = owner;
  vm.runInContext("stepsDialog('Install', steps, '', owner)", ctx);
  next();
  const view = owner.view;
  assert.ok(view, 'firmware job must retain its view');
  vm.runInContext('closeDialog()', ctx);
  assert.equal(dlg.open, false);
  vm.runInContext("stepsDialog('Competing update', steps, '', owner)", ctx);
  assert.equal(owner.view, view);
  assert.equal(builds, 2, 'reopening must not recreate hooks or restart the step');
  assert.equal(dlg.open, true);
});

test('firmware view cancellation settles a pending backup confirmation and refuses mid-write cancellation', async () => {
  const { ctx, m } = pageDialog();
  Object.assign(ctx, { cycleJobId: 0, cycleJob: null, saveBytes() {}, REAL_RELEASE: {},
    releaseFiles: async () => ({ app: new Uint8Array(), cfg: new Uint8Array() }),
    previewPlan: async () => ({ valid: true, files: { backup: { sha256: 'host-only' } } }),
    STEP_TEXT: {}, Date });
  const begin = app.indexOf('function installJob(');
  vm.runInContext(app.slice(begin, app.indexOf('// The installer\'s two working steps', begin)), ctx);
  const backup = deferred();
  m.installFirmware = async ({ hooks }) => { await hooks.backup(new Uint8Array(8)); backup.resolve(); return {}; };
  const job = ctx.installJob();
  job.start();
  await tick(); await tick();
  assert.equal(typeof job.go, 'function');
  assert.equal(typeof job.cancel, 'function');
  job.written = true;
  assert.equal(job.cancel(), false);
  assert.equal(job.active, true);
  job.written = false;
  assert.equal(job.cancel(), true);
  await tick();
  assert.equal(job.active, false);
  assert.match(job.error.message, /cancel/i);
  assert.equal(job.go, null, 'no detached confirmation may remain');
});

test('Save first keeps Update blocked through delayed clean and on refusal', async () => {
  for (const fails of [false, true]) {
    const pending = deferred(), go = { disabled: false }, button = { disabled: false, textContent: 'Save first' };
    const ctx = vm.createContext({ h: (_tag, props, text) => Object.assign({ textContent: text }, props),
      saveOnMixer: () => pending.promise });
    const start = app.indexOf('function saveFirstButton(');
    vm.runInContext(start < 0 ? '' : app.slice(start, app.indexOf('\nfunction ', start + 10)), ctx);
    assert.equal(typeof ctx.saveFirstButton, 'function', 'Save first must own the pending precondition');
    const save = ctx.saveFirstButton(go);
    const task = save.onclick({ currentTarget: button });
    assert.equal(go.disabled, true);
    assert.match(button.textContent, /Saving/);
    pending.resolve(!fails);
    await task;
    assert.equal(go.disabled, fails);
    assert.match(button.textContent, fails ? /Retry/ : /Saved/);
  }
});

test('changed ID at equal revision invalidates the delta baseline before adopting metadata', async () => {
  for (const changedId of [true, false]) {
    let sent = 0, notices = 0;
    const m = connected({ edit: async () => { sent++; }, push: async () => { sent++; } });
    m.addEventListener('changed', () => { notices++; });
    const working = m.running, id = changedId ? new Uint8Array(16).fill(2) : new Uint8Array(m.mixerId);
    m.panel({ preset: 0, page: 0, shift: false, touches: 0, id, revision: changedId ? 7 : 8, store: 'clean' });
    assert.equal(notices, 1);
    assert.equal(m.running, null, 'old bytes must not be used as an incremental baseline');
    assert.equal(m.saved, null, 'external clean must not certify the stale working copy');
    assert.equal(m.diverged, true);
    m.apply(working, 'old edit');
    await tick();
    assert.equal(sent, 0, 'ordinary edits cannot overwrite the outside configuration');
    m.close();
  }
});

test('100-message burst keeps order but schedules one monitor redraw per frame', () => {
  const frames = [], st = { monitor: [], monitorOpen: true, monitorFilter: 'all', paused: false };
  let bars = 0, lists = 0;
  const ctx = vm.createContext({ st, dev: { state: {} }, performance, decode: (b) => String(b[1]),
    requestAnimationFrame: (f) => { frames.push(f); return frames.length; }, cancelAnimationFrame() {},
    renderMonitorBar: () => bars++, renderMonitorList: () => lists++ });
  vm.runInContext(app.slice(app.indexOf('function addMonitor('), app.indexOf('function renderMonitorBar(')), ctx);
  for (let i = 0; i < 100; i++) ctx.addMonitor({ dir: 'out', bytes: [0xb0, i, 1] });
  assert.equal(st.monitor.length, 100);
  assert.deepEqual(st.monitor.map((m) => m.bytes[1]), Array.from({ length: 100 }, (_, i) => i));
  assert.equal(bars, 0, 'burst must not synchronously rebuild the DOM');
  assert.equal(frames.length, 1);
  frames.shift()();
  assert.equal(bars, 1); assert.equal(lists, 1);
  st.monitorOpen = false;
  for (let i = 0; i < 400; i++) ctx.addMonitor({ dir: 'in', bytes: [0xb0, i, 1] });
  assert.equal(st.monitor.length, 400);
  frames.shift()();
  assert.equal(bars, 2); assert.equal(lists, 1, 'hidden list must not rebuild');
  st.paused = true;
  ctx.addMonitor({ bytes: [0xb0, 1, 1] });
  assert.equal(frames.length, 0);
});

test('unsupported browsers select offline real-device model unless mock was explicitly requested', () => {
  for (const supported of [false, true]) for (const search of ['', '?mock', '?devhost']) {
    const ctx = vm.createContext({ URLSearchParams, location: { search }, MixerDevice: { supported: () => supported } });
    const declaration = app.match(/^const MOCK = .*;$/m)[0];
    vm.runInContext(declaration + '; globalThis.selectedMock = MOCK;', ctx);
    assert.equal(ctx.selectedMock, search === '?mock', `${search || 'default'}, MIDI supported ${supported}`);
    if (!ctx.selectedMock) assert.equal(new MixerDevice(codec).connected, false);
  }
});

test('firmware ownership blocks configuration mutations and tab writes but not local editing', async () => {
  let writes = 0;
  const m = connected({ edit: async () => { writes++; }, select: async () => { writes++; } });
  m.beginFirmwareJob('update');
  m.apply('{"edit":true}', 'local edit');
  m.select(1, 2);
  await tick();
  assert.equal(writes, 0, 'firmware status owner excludes device navigation and edits');
  assert.equal(m.pending, null);
  m.close();
});

test('cancelled firmware wizard cannot advance a detached step or resume its confirmation', () => {
  const { ctx, m } = pageDialog();
  const job = m.beginFirmwareJob('install');
  let next, advanced = 0;
  ctx.owner = job;
  ctx.steps = [{ title: 'Before', body: (go) => { next = go; return 'before'; } },
    { title: 'Write', body: () => { advanced++; return 'write'; } }];
  vm.runInContext("stepsDialog('Install', steps, '', owner)", ctx);
  job.cancelled = true;
  job.active = false;
  next();
  assert.equal(advanced, 0);
});

test('connection loss settles a firmware backup confirmation with an honest error', async () => {
  const { ctx, m } = pageDialog();
  Object.assign(ctx, { cycleJobId: 0, cycleJob: null, saveBytes() {}, REAL_RELEASE: {},
    releaseFiles: async () => ({ app: new Uint8Array(), cfg: new Uint8Array() }),
    previewPlan: async () => ({ valid: true, files: { backup: { sha256: 'host-only' } } }), STEP_TEXT: {}, Date });
  const begin = app.indexOf('function installJob(');
  vm.runInContext(app.slice(begin, app.indexOf('// The installer\'s two working steps', begin)), ctx);
  m.installFirmware = async ({ hooks }) => { await hooks.backup(new Uint8Array(8)); return {}; };
  const job = ctx.installJob();job.start();
  await tick();await tick();
  assert.equal(typeof job.go, 'function');
  m.lost('host-only simulated port loss');
  await tick();
  assert.equal(job.active, false, 'lost connection must not leave a confirmation promise stranded');
  assert.match(job.error.message, /connection/);
  assert.equal(job.go, null);
});

test('Go back job can be explicitly cancelled before any write', () => {
  const m = connected();
  const job = m.beginFirmwareJob('back');
  assert.equal(typeof job.cancel, 'function');
  assert.equal(job.cancel(), true);
  assert.equal(job.active, false);
  assert.equal(job.cancelled, true);
});

test('cancelling during backup preview cannot strand a newly-created confirmation hook', async () => {
  const { ctx, m } = pageDialog();
  const preview = deferred();
  Object.assign(ctx, { cycleJobId: 0, cycleJob: null, saveBytes() {}, REAL_RELEASE: {},
    releaseFiles: async () => ({ app: new Uint8Array(), cfg: new Uint8Array() }),
    previewPlan: () => preview.promise, STEP_TEXT: {}, Date });
  const begin = app.indexOf('function installJob(');
  vm.runInContext(app.slice(begin, app.indexOf('// The installer\'s two working steps', begin)), ctx);
  m.installFirmware = async ({ hooks }) => { await hooks.backup(new Uint8Array(8)); return {}; };
  const job = ctx.installJob();job.start();await tick();
  job.cancel();
  preview.resolve({ valid: true, files: { backup: { sha256: 'host-only' } } });
  await tick();await tick();
  assert.equal(job.active, false);
  assert.equal(job.go, null);
  assert.match(job.error.message, /cancel/i);
});

test('Save rejects an outside identity change instead of certifying stale editor bytes', async () => {
  const m = connected({ save: async () => {}, info: async () => ({ store: 'clean', revision: 7, id: new Uint8Array(16).fill(2) }) });
  await assert.rejects(m.saveNow(), /configuration.*changed/);
  assert.equal(m.saved, null);
  assert.equal(m.diverged, true);
  m.close();
});

test('same ID and revision do not diverge, and reconnect can establish a fresh baseline', () => {
  const m = connected();
  m.panel({ preset: 0, page: 0, shift: false, touches: 0, id: new Uint8Array(m.mixerId), revision: 7, store: 'unsaved' });
  assert.equal(!!m.diverged, false);
  m.diverged = true;
  m.close();
  assert.equal(!!m.diverged, false, 'reconnect must not retain an obsolete divergence latch');
});

test('cancelled or stale firmware owner cannot reuse the device after its result', async () => {
  const m = connected({ stock: async () => { throw Error('must not reach transport'); } });
  const owner = m.beginFirmwareJob('back');
  owner.cancel();
  await assert.rejects(m.backToStock(owner), /firmware.*(active|cancel|owner|ended)/i);
  m.close();
});

test('explicit pull establishes validated bytes without reopening divergence UI', async () => {
  const blob = codec.fileToBinary({ schemaVersion: codec.version, revision: 8 });
  const st = { preset: 0, page: 0, shift: false, touches: 0, revision: 8, id: null, store: 'clean' };
  const m = connected({ info: async () => ({ ...st, chunk: 128 }), pull: async () => blob, state: async () => st });
  let changes = 0;m.addEventListener('changed', () => changes++);
  await m.pullFile();
  assert.equal(changes, 0);
  assert.equal(m.diverged, false);
  assert.equal(m.saved, m.running);
  m.close();
});

test('cancelled installation confirmation button is inert, not a call to a removed hook', () => {
  let installButton;
  const ctx = vm.createContext({ RELEASE: {version:'test'}, dev: { transport:'USB' }, cycleJob:{},
    h: (tag, attrs, ...kids) => { const n={tag,...attrs,kids};if(tag==='button'&&kids.includes('Install'))installButton=n;return n; } });
  const start=app.indexOf('function installSteps(');
  vm.runInContext(app.slice(start,app.indexOf('function releaseNote(',start)),ctx);
  const job={cancelled:true,active:false,go:null,bar:{},text:{}};
  ctx.installSteps(job,'done')[1].body(()=>{});
  assert.doesNotThrow(()=>installButton.onclick({currentTarget:{}}));
  assert.equal(job.written, undefined);
});

test('offline no-MIDI state hides Bluetooth install too, even if Web Bluetooth exists', () => {
  const nodes = { '#pill-act': {dataset:{}}, '#pill-bt': {dataset:{}} };
  const ctx = vm.createContext({ MOCK:false, dev:{real:true,connected:false}, MixerDevice:{supported:()=>false,bluetooth:()=>true}, $:s=>nodes[s],
    put(){},icon(){},h(){},connectBluetooth(){},connectMixer(){} });
  const start=app.indexOf('function renderPillAction(');
  vm.runInContext(app.slice(start,app.indexOf('// Bluetooth: where each firmware',start)),ctx);
  ctx.renderPillAction('Offline');
  assert.equal(nodes['#pill-bt'].hidden,true);
  assert.equal(nodes['#pill-act'].hidden,true);
});

test('late Update stock check cannot re-enable Save first after cancellation', async () => {
  const { ctx, m } = pageDialog();
  const check = deferred(); let steps, writes = 0;
  const node = (tag, attrs = {}, ...kids) => ({ tag, ...attrs, kids });
  Object.assign(ctx, { h: node, reopenFirmware: () => false, installJob: () => m.beginFirmwareJob('update'),
    RELEASE: { version: 'test' }, REAL_RELEASE: null, unsaved: () => true, releaseNote: () => null, installSteps: () => [],
    STOCK_TEXT: {}, BLE_AGAIN: '', stepsDialog: (_title, all) => { steps = all; },
    saveOnMixer: async () => { writes++; return true; } });
  m.stockCheck = () => check.promise;
  const start = app.indexOf('function updateReal(');
  vm.runInContext(app.slice(start, app.indexOf('// After a restart, ours over Bluetooth:', start)), ctx);
  ctx.updateReal();
  const before = steps[0].body(() => {}), save = before.kids.at(-1).kids[0];
  m.firmwareJob.cancel();
  check.resolve({ status: 'ok' }); await tick();
  assert.equal(save.disabled, true, 'cancelled owner must ignore a late successful preflight');
  await save.onclick({ currentTarget: save });
  assert.equal(writes, 0, 'even a retained callback must not save for a cancelled job');
  m.close();
});

test('connection loss during backup preview cannot create a stranded confirmation', async () => {
  const { ctx, m } = pageDialog();
  const preview = deferred();
  Object.assign(ctx, { cycleJobId: 0, cycleJob: null, saveBytes() {}, REAL_RELEASE: {},
    releaseFiles: async () => ({ app: new Uint8Array(), cfg: new Uint8Array() }),
    previewPlan: () => preview.promise, STEP_TEXT: {}, Date });
  const begin = app.indexOf('function installJob(');
  vm.runInContext(app.slice(begin, app.indexOf('// The installer\'s two working steps', begin)), ctx);
  m.installFirmware = async ({ hooks }) => { await hooks.backup(new Uint8Array(8)); return {}; };
  const job = ctx.installJob(); job.start(); await tick();
  m.lost('host-only loss during preview');
  preview.resolve({ valid: true, files: { backup: { sha256: 'host-only' } } });
  await tick(); await tick();
  assert.equal(job.active, false, 'lost connection before confirmation registration must settle the job');
  assert.equal(job.go, null);
  assert.match(job.error.message, /connection/);
});

test('direct Go back retains its firmware owner while the sector-0 write is in flight', async () => {
  const reply = deferred();
  const m = connected({ stock: () => reply.promise });
  const task = m.backToStock();
  const owner = m.firmwareJob;
  const cancelled = owner.cancel();
  const keptOwner = owner.active;
  reply.resolve({ status: 'busy', pointer: 0, tries: 0 });
  await task;
  assert.equal(cancelled, false, 'a direct device call must protect uncertain sector-0 writes too');
  assert.equal(keptOwner, true);
  assert.equal(owner.active, false, 'standalone call releases its owner after the result');
  m.close();
});

test('firmware owner excludes Revert transport mutations, including a queued pre-wizard Revert', async () => {
  for (const queued of [false, true]) {
    let writes = 0;
    const m = connected({ revert: async () => { writes++; throw Error('unexpected Revert transport'); } });
    m.pushing = queued;
    if (!queued) m.beginFirmwareJob('update');
    const result = m.revert();
    if (queued) { m.beginFirmwareJob('update'); m.pushing = false; }
    assert.equal(await result, null);
    assert.equal(writes, 0, 'Revert must not reach the device while the firmware wizard owns it');
    m.close();
  }
});

test('mixer sheet disables Revert while a firmware job owns the device', () => {
  const { ctx, m } = pageDialog();
  const node = (tag, attrs = {}, ...kids) => ({ tag, ...attrs, kids });
  Object.assign(ctx, { h: node, btn: (_icon, text, attrs) => node('button', attrs, text), st: { follow: true },
    linkButtons: () => [], LINK_HELP: {}, savingExplainer() {}, unsaved: () => true, unsent: () => false,
    revertDialog() {}, stockDialog() {}, soon() {} });
  const start = app.indexOf('function deviceOurs(');
  vm.runInContext(app.slice(start, app.indexOf('function renderLogs(', start)), ctx);
  const find = (nodes) => nodes.flatMap((n) => !n ? [] : [n, ...find(n.kids || [])]);
  m.beginFirmwareJob('update');
  const button = find(ctx.deviceOurs(null, {})).find((n) => n.tag === 'button' && n.kids.includes('Go back to the last save…'));
  assert.equal(button.disabled, true);
  m.firmwareJob.active = false;
  assert.equal(find(ctx.deviceOurs(null, {})).find((n) => n.tag === 'button' && n.kids.includes('Go back to the last save…')).disabled, false);
  m.close();
});

test('Update backup confirmation becomes cancellable only after confirmed stock reconnect', async () => {
  for (const transport of ['USB', 'BLE']) {
    const { ctx, m } = pageDialog();
    const switching = deferred(), reconnect = deferred(), added = [];
    let steps, advanced = 0;
    const node = (tag, attrs = {}, ...kids) => ({ tag, ...attrs, kids, replaceChildren(...nodes) { this.kids = nodes; }, after(...nodes) { added.push(...nodes); } });
    Object.assign(ctx, { h: node, btn: (_icon, text, attrs) => node('button', attrs, text),
      cycleJobId: 0, cycleJob: null, saveBytes() {}, REAL_RELEASE: { install: true }, RELEASE: { version: 'test' },
      releaseFiles: async () => ({ app: new Uint8Array(), cfg: new Uint8Array() }),
      previewPlan: async () => ({ valid: true, files: { backup: { sha256: 'host-only' } } }),
      STEP_TEXT: {}, STOCK_TEXT: {}, BLE_AGAIN: '', Date, sleep: async () => {}, docText: () => '{}', bluetoothHelp: () => null,
      reopenFirmware: () => false, stepsDialog: (_title, all) => { steps = all; } });
    const begin = app.indexOf('function installJob(');
    vm.runInContext(app.slice(begin, app.indexOf('function releaseNote(', begin)) +
      app.slice(app.indexOf('function updateReal('), app.indexOf('function saveFirstButton(')), ctx);
    m.transport = transport;
    m.backToStock = () => switching.promise;
    m.connect = m.connectBluetooth = async () => { await reconnect.promise; m.connected = true; m.stock = {}; };
    m.installFirmware = async ({ hooks }) => { await hooks.backup(new Uint8Array(8)); return {}; };
    ctx.updateReal();
    const job = m.firmwareJob;
    steps[1].body(() => advanced++);
    assert.equal(job.cancel(), false, 'sector-0 switch must not be cancellable');
    switching.resolve({ status: 'ok' });
    await tick();
    assert.equal(job.cancel(), false, 'an unconfirmed reconnect must not unlock cancellation');
    if (transport === 'BLE') added[0].kids[0].onclick();
    reconnect.resolve();
    await tick();
    assert.equal(advanced, 1, `${transport}: stock reconnect advances to backup`);
    job.start(); await tick(); await tick();
    assert.equal(typeof job.go, 'function');
    assert.equal(job.cancel(), true, `${transport}: backup confirmation must be cancellable`);
    await tick();
    assert.equal(job.active, false);
    assert.equal(job.go, null);
    assert.match(job.error.message, /cancel/i);
    m.close();
  }
});

test('failed Update stock check leaves one dismissible error result, not an active lock', async () => {
  const {ctx,m}=pageDialog();
  Object.assign(ctx,{reopenFirmware:()=>false,installJob:()=>m.beginFirmwareJob('update'),RELEASE:{version:'test'},REAL_RELEASE:null,
    unsaved:()=>false,releaseNote:()=>null,installSteps:()=>[],STOCK_TEXT:{},BLE_AGAIN:'',
    h:(tag,attrs,...kids)=>({tag,...attrs,kids,textContent:'',replaceChildren(...c){this.kids=c}})});
  m.stockCheck=async()=>{throw Error('stock check refused')};
  const start=app.indexOf('function updateReal(');
  vm.runInContext(app.slice(start,app.indexOf('function saveFirstButton(',start)),ctx);
  ctx.updateReal();await tick();
  assert.equal(m.firmwareJob.active,false);
  assert.match(m.firmwareJob.error.message,/stock check refused/);
});

// The instrument card's source line, built by the page's real function with the page's h().
function sourceLine(list) {
  const node = (tag, attrs = {}, ...kids) => {
    const n = { tag, attrs, children: [] };
    for (const c of kids.flat()) if (c !== null && c !== undefined && c !== false) n.children.push(typeof c === 'object' && c.tag ? c : String(c));
    return n;
  };
  const start = app.indexOf('function instrumentSource(');
  assert.ok(start >= 0, 'app.js defines instrumentSource(list)');
  const ctx = vm.createContext({ h: node, icon: (name) => node('span', { class: 'ic', name }),
    plural: (n, one) => `${n} ${n === 1 ? one : one + 's'}` });
  vm.runInContext(app.slice(start, app.indexOf('\n}\n', start) + 3), ctx);
  return ctx.instrumentSource(list);
}
const texts = (n) => typeof n === 'string' ? [n] : n.children.flatMap(texts);
const links = (n) => typeof n === 'string' ? [] : [...(n.tag === 'a' ? [n] : []), ...n.children.flatMap(links)];

test('instrument card: a MIDI Guide list links to its own file, compactly, with no raw URL or licence', () => {
  const file = 'https://github.com/pencilresearch/midi/blob/main/Elektron/Digitone%20II.csv';
  const line = sourceLine({ name: 'Elektron Digitone II', params: new Array(341).fill({}),
    source: { name: 'MIDI Guide', url: 'https://midi.guide', data: file, updated: '2025-06-01T10:00:00Z' } });
  const text = texts(line).join('');
  assert.doesNotMatch(text, /https?:|,\s*$|CC BY-SA/, text);
  assert.match(text, /^341 parameters · MIDI Guide/);
  const a = links(line);
  assert.equal(a.length, 1, 'one link: the instrument in MIDI Guide');
  assert.equal(a[0].attrs.href, file);
  assert.equal(a[0].attrs.target, '_blank');
  assert.match(a[0].attrs.title, /MIDI Guide/);
});

test('instrument card: a built-in list keeps its own description', () => {
  const line = sourceLine({ name: 'Generic', params: [{}], about: 'Plain CCs.' });
  assert.deepEqual(texts(line), ['Plain CCs.']);
  assert.equal(links(line).length, 0);
});
