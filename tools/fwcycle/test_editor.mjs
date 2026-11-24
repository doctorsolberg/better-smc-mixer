import test from 'node:test';
import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import vm from 'node:vm';
const url = new URL('./editor.mjs', import.meta.url);
const manifest = { product: 'better-smc-mixer', install: { app: 'app.bin', cfg: 'cfg.bin' }, files: { 'app.bin': { size: 2, sha256: 'a'.repeat(64) }, 'cfg.bin': { size: 3, sha256: 'b'.repeat(64) } } };

async function load() { assert.ok(existsSync(url), 'real UI adapter must exist'); return import(url); }
function fixture({ initialized = true, observer = true, origin = 'http://localhost:8000', buttons = [] } = {}) {
  const state = { version: 1, initialized, identity: { real: true, connected: true, kind: 'ours', transport: 'BLE' }, config: { clean: true, editorMatchesRunning: true }, latest: {}, events: [] };
  const controls = buttons.map(b => ({ textContent: b.label, disabled: false, getClientRects: () => [1], closest: () => null, getAttribute: () => null, click() { this.clicks = (this.clicks || 0) + 1; }, ...b }));
  const document = { readyState: 'complete', querySelector(s) { if (s === '#dlg') return { open: false }; return null; }, querySelectorAll(s) { return s === 'button' ? controls : []; } };
  const context = vm.createContext({ document, location: { origin, href: origin + '/app/editor/' }, getComputedStyle: () => ({ visibility: 'visible', display: 'block' }), URL, navigator: {} });
  context.window = context;
  if (observer) context.__smcCycleObserver = { snapshot: () => structuredClone(state) };
  const calls = [];
  const client = { async call(method, params, session) { calls.push({ method, session }); try { return { result: { value: await vm.runInContext(params.expression, context) } }; } catch(e) { return { exceptionDetails: { text: e.message } }; } } };
  return { state, controls, client, calls, context };
}

test('old completed job cannot certify install after a newer rollback',async()=>{
 const {EditorAdapter}=await load();const f=fixture();f.state.pageId='page-a';
 f.state.job={id:1,completed:true,stage:'complete',plan:{failed:0}};
 f.state.latest={complete:{jobId:1,sequence:10},backup:{jobId:1,sequence:4},rollback:{sequence:20}};
 const a=new EditorAdapter(f.client,'s',{origin:'http://localhost:8000'});
 assert.equal((await a.snapshot()).install.completed,false);
 f.state.latest.backup={jobId:1,sequence:21};f.state.latest.complete={jobId:1,sequence:22};
 assert.equal((await a.snapshot()).install.completed,true);
 f.state.latest.complete.jobId=2;assert.equal((await a.snapshot()).install.completed,false);
});

test('rollback readiness ignores checks from before the new wizard opens',async()=>{
 const {EditorAdapter}=await load();const f=fixture({buttons:[{label:'Go back'}]});
 f.context.document.querySelector=s=>s==='#dlg'?{open:true}:s==='#dlg h2'?{textContent:'Go back to M-Vave’s firmware'}:s==='#dlg .step.now b'?{textContent:'Go back'}:null;
 f.state.latest={rollback_prepare:{sequence:20},rollback_check:{sequence:10,status:'ok'}};
 const a=new EditorAdapter(f.client,'s',{origin:'http://localhost:8000'});
 assert.equal((await a.snapshot()).rollback.ready,false);
 f.state.latest.rollback_check.sequence=21;
 assert.equal((await a.snapshot()).rollback.ready,true);
});

test('snapshot requires initialized observer, not just readyState complete, and enforces exact origin', async () => {
  const { EditorAdapter } = await load();
  for (const options of [{ observer: false }, { initialized: false }, { origin: 'http://evil.test' }]) {
    const f = fixture(options); const adapter = new EditorAdapter(f.client, 's', { origin: 'http://localhost:8000' });
    await assert.rejects(adapter.snapshot(), /observer|initialized|origin/);
  }
  const f = fixture(); const result = await new EditorAdapter(f.client, 's', { origin: 'http://localhost:8000' }).snapshot();
  assert.equal(result.identity.transport, 'BLE'); assert.equal(f.calls[0].session, 's');
  assert.equal(result.config.saved, true); assert.equal(result.config.dirty, false); assert.equal(result.config.pulled, false);
  assert.equal(result.stock.verified, false); assert.equal(result.install.boot_selection_verified, false);
  assert.equal(result.install.active, false); assert.equal(result.rollback.active, false);
});

test('typed actions refuse absent, duplicate, disabled or invisible intended controls without clicking', async () => {
  const { EditorAdapter } = await load();
  for (const buttons of [[], [{ label: 'Connect', disabled: true }], [{ label: 'Connect', getClientRects: () => [] }], [{ label: 'Connect' }, { label: 'Connect' }]]) {
    const f = fixture({ buttons }); f.state.identity.connected = false;
    const adapter = new EditorAdapter(f.client, 's', { origin: 'http://localhost:8000' });
    await assert.rejects(adapter.action({ name: 'ours_connect', transport: 'BLE', endpoint: 'Better-SMC-Mixer' }), /control|unique|enabled|visible/);
    assert.ok(f.controls.every(b => !b.clicks));
  }
  const f = fixture(); await assert.rejects(new EditorAdapter(f.client, 's', { origin: 'http://localhost:8000' }).action({ name: 'eval', expression: 'evil()' }), /action/);
});

test('release pin ignores JSON key order but rejects a different manifest and install requires bound preview', async () => {
  const { EditorAdapter } = await load();
  const f = fixture(); const adapter = new EditorAdapter(f.client, 's', { origin: 'http://localhost:8000' });
  f.state.release = { manifest, url: 'http://localhost:8000/release/manifest.json' };
  await adapter.release({ manifest: { files: manifest.files, install: manifest.install, product: manifest.product } });
  await assert.rejects(adapter.release({ manifest: { ...manifest, version: 'other' } }), /different release/);
  f.state.release.manifestSha256 = 'a'.repeat(64);
  await assert.rejects(adapter.release({ manifest, manifest_sha256: 'b'.repeat(64) }), /hash/);
  await assert.rejects(adapter.action({ name: 'install',binding:{pageId:'test-page',jobId:1} }), /pin|plan|preview|binding/);
});

test('a connect control that does not initiate MIDI access cannot leave a global proxy installed', async () => {
  const { EditorAdapter } = await load();
  const f = fixture({ buttons: [{ label: 'Connect', id: 'pill-act' }] }); f.state.identity.connected = false;
  const original = async () => ({}); f.context.navigator.requestMIDIAccess = original;
  await assert.rejects(new EditorAdapter(f.client, 's', { origin: 'http://localhost:8000' }).action({ name: 'ours_connect', transport: 'USB', endpoint: 'SMC-Mixer Config' }), /initiate/);
  assert.equal(f.context.navigator.requestMIDIAccess, original);
});

test('Install clicks only with a pinned matching preview at the exact wizard stage', async () => {
  const { EditorAdapter } = await load();
  const f = fixture({ buttons: [{ label: 'Install', closest: s => s === '#dlg .step.now' ? {} : null }] });
  let title = 'Install better-smc-mixer', stage = 'Install 0.1.0';
  f.context.document.querySelector = s => s === '#dlg' ? { open: true } : s === '#dlg h2' ? { textContent: title } : s === '#dlg .step.now b' ? { textContent: stage } : null;
  f.state.release = { manifest, url: 'http://localhost:8000/release/manifest.json' };
  f.state.pageId='test-page';f.state.job = { id:1,stage: 'backup_waiting', preview: { valid: true, files: { app: manifest.files['app.bin'], cfg: manifest.files['cfg.bin'] } } };
  const adapter = new EditorAdapter(f.client, 's', { origin: 'http://localhost:8000' });
  await adapter.release({ manifest });
  f.state.job.preview.files.app = { size: 9, sha256: 'c'.repeat(64) };
  await assert.rejects(adapter.action({ name: 'install',binding:{pageId:'test-page',jobId:1} }), /bound/); assert.ok(!f.controls[0].clicks);
  f.state.job.preview.files.app = manifest.files['app.bin']; stage = 'Save a backup of the mixer';
  await assert.rejects(adapter.action({ name: 'install',binding:{pageId:'test-page',jobId:1} }), /stage/); assert.ok(!f.controls[0].clicks);
  stage = 'Install 0.1.0'; await adapter.action({ name: 'install',binding:{pageId:'test-page',jobId:1} }); assert.equal(f.controls[0].clicks, 1);
});

test('baseline uses the actual pill independent of its status label and refuses dirty configuration', async () => {
  const { EditorAdapter } = await load();
  const f = fixture({ buttons: [{ label: 'Saved', id: 'device-pill' }] });
  f.state.config.clean = false;
  await assert.rejects(new EditorAdapter(f.client, 's', { origin: 'http://localhost:8000' }).action({ name: 'baseline' }), /clean/);
  assert.ok(!f.controls[0].clicks);
});

test('baseline finds unique pill by ID even with the update badge, then loads from device sheet', async () => {
  const { EditorAdapter } = await load();
  const f = fixture({ buttons: [{ label: '0.2.0Saved', id: 'device-pill' }] }); let open = false;
  f.context.document.querySelector = s => s === '#dlg' ? { open } : s === '#dlg h2' ? { textContent: 'SMC-Mixer' } : null;
  f.controls[0].click = () => { open = true; };
  let loaded = false; f.controls.push({ textContent: 'Load from the mixer', getAttribute: () => null, getClientRects: () => [1], closest: s => s === '#dlg' ? {} : null, click: () => { loaded = true; } });
  await new EditorAdapter(f.client, 's', { origin: 'http://localhost:8000' }).action({ name: 'baseline' });
  assert.equal(loaded, true);
});

test('USB stock and BLE stock use distinct exact UI controls', async () => {
  const { EditorAdapter } = await load();
  for (const [transport, label] of [['USB', 'Connect'], ['BLE', 'Bluetooth']]) {
    const f = fixture({ buttons: [{ label, id: transport === 'USB' ? 'pill-act' : 'pill-bt' }] }); f.state.identity.connected = false;
    f.context.navigator.requestMIDIAccess = async () => ({ inputs: new Map([['p', { name: 'SMC-Mixer-Private' }]]), outputs: new Map([['p', { name: 'SMC-Mixer-Private' }]]) });
    if (transport === 'USB') f.controls[0].click = () => { f.controls[0].clicks = 1; void f.context.navigator.requestMIDIAccess({ sysex: true }); };
    await new EditorAdapter(f.client, 's', { origin: 'http://localhost:8000' }).action({ name: 'stock_connect', transport, endpoint: 'SMC-Mixer-Private' });
    assert.equal(f.controls[0].clicks, 1);
  }
});

test('Everything export uses File menu, exact dialog and Download, not configuration telemetry', async () => {
  const { EditorAdapter } = await load();
  const f = fixture(); let title = '', open = false; const name = { value: '', getClientRects: () => [1], closest: () => null };
  f.context.document.querySelector = s => s === '#dlg' ? { open } : s === '#dlg h2' ? { textContent: title } : s === '#btn-file' ? { ...name, click: () => {} } : s === '#save-name' ? name : null;
  f.controls.push({ textContent: 'Everything', disabled: false, getClientRects: () => [1], getAttribute: () => null,
    closest: s => s === '#menu' ? {} : null, click() { title = 'Save a copy: Everything'; open = true; } });
  let downloaded = 0;
  f.controls.push({ textContent: 'Download', disabled: false, getClientRects: () => [1], getAttribute: () => null,
    closest: s => s === '#dlg' ? {} : null, click() { downloaded++; } });
  const result = await new EditorAdapter(f.client, 's', { origin: 'http://localhost:8000' }).exportConfig({ name: 'baseline' });
  assert.equal(result.filename, 'baseline.json'); assert.equal(name.value, 'baseline'); assert.equal(downloaded, 1);
  assert.equal(result.completion, 'Browser.downloadProgress required');
});

test('exact MIDI endpoint filtering excludes USB fallback and restores the browser method', async () => {
  const { EditorAdapter } = await load();
  const f = fixture({ buttons: [{ label: 'Connect', id: 'pill-act' }] }); f.state.identity.connected = false;
  const ports = new Map([['ble', { name: 'Better-SMC-Mixer' }], ['usb', { name: 'SMC-Mixer Config' }]]);
  const original = async () => ({ inputs: ports, outputs: ports }); f.context.navigator.requestMIDIAccess = original;
  let access;
  f.controls[0].click = () => { access = f.context.navigator.requestMIDIAccess({ sysex: true }); };
  await new EditorAdapter(f.client, 's', { origin: 'http://localhost:8000' }).action({ name: 'ours_connect', transport: 'BLE', endpoint: 'Better-SMC-Mixer' });
  assert.deepEqual([...(await access).inputs.values()].map(p => p.name), ['Better-SMC-Mixer']);
  assert.equal(f.context.navigator.requestMIDIAccess, original);
});
