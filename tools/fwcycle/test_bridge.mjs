import test from 'node:test';
import assert from 'node:assert/strict';
import {spawnSync} from 'node:child_process';
import {mkdtemp, chmod, rm, symlink, realpath} from 'node:fs/promises';
import {join} from 'node:path';
import {tmpdir} from 'node:os';
import {Readable, Writable} from 'node:stream';
import {Bridge, run} from './bridge.mjs';

class Protocol {
  calls = [];
  listeners = new Map();
  targets = [{targetId: 'editor', type: 'page', url: 'http://127.0.0.1:8876/app/editor/'}];
  async call(method, params = {}, sessionId) {
    this.calls.push({method, params, sessionId});
    if (method === 'Target.getTargets') return {targetInfos: this.targets};
    if (method === 'Target.getTargetInfo') return {targetInfo: this.targets.find(t => t.targetId === params.targetId)};
    if (method === 'Page.getFrameTree') return {frameTree: {frame: {id: 'frame', url: attachParams.origin + '/app/editor/'}}};
    if (method === 'Target.attachToTarget') return {sessionId: 'session'};
    return {};
  }
  on(method, fn, sessionId) {
    const entries = this.listeners.get(method) ?? new Set(); this.listeners.set(method, entries);
    const entry = {fn, sessionId}; entries.add(entry); return () => entries.delete(entry);
  }
  emit(method, params, sessionId = 'session') {
    for (const e of this.listeners.get(method) ?? []) if (!e.sessionId || e.sessionId === sessionId) e.fn(params, sessionId);
  }
  close() {this.closed = true;}
}
const attachParams = {endpoint: 'http://127.0.0.1:9222', origin: 'http://127.0.0.1:8876'};
function harness(options = {}) {
  const protocol = new Protocol();
  const fetches = [];
  const fetchImpl = async (url, init) => {
    fetches.push({url: String(url), init});
    return new Response(JSON.stringify({webSocketDebuggerUrl: 'ws://127.0.0.1:9222/devtools/browser/owned'}));
  };
  class Editor {
    constructor(client, sessionId, {origin}) {assert.equal(client, protocol); assert.equal(sessionId, 'session'); assert.equal(origin, attachParams.origin);}
    async snapshot() {return {ready: true};}
    async action(params) {await options.onAction?.(protocol, params); return {action: params.name};}
    async release(params) {return {manifest: params.manifest};}
    async exportConfig(params) {return {name: params.name};}
  }
  return {protocol, fetches, bridge: new Bridge({fetchImpl, connect: async () => protocol, loadEditor: async () => ({EditorAdapter: Editor}), ...options})};
}

test('attaches uniquely to exact local editor without activating or navigating', async () => {
  const {bridge, protocol, fetches} = harness();
  const attached = await bridge.dispatch('attach', attachParams);
  assert.equal(attached.targetId, 'editor');
  assert.equal(fetches[0].init.redirect, 'error');
  assert.deepEqual(await bridge.dispatch('snapshot'), {ready: true});
  assert.deepEqual(await bridge.dispatch('action', {name: 'own_connect', transport: 'usb'}), {action: 'own_connect'});
  assert.deepEqual(await bridge.dispatch('release', {manifest: 'pinned.json'}), {manifest: 'pinned.json'});
  assert.deepEqual(await bridge.dispatch('exportConfig', {name: 'before.json'}), {name: 'before.json'});
  await bridge.dispatch('disconnectBridge');
  assert.ok(protocol.calls.some(c => c.method === 'Target.detachFromTarget' && c.params.sessionId === 'session'));
  assert.equal(protocol.closed, true);
  assert.ok(!protocol.calls.some(c => /navigate|activateTarget|bringToFront|Browser.close|closeTarget|setPermission/i.test(c.method)));
});

test('rejects nonliteral loopback endpoints and nonexact origins before I/O', async () => {
  for (const endpoint of ['http://evil.test:9222', 'http://localhost:9222', 'http://127.1:9222', 'http://2130706433:9222', 'http://127.0.0.1:9222/other', 'http://user@127.0.0.1:9222', 'https://127.0.0.1:9222', 'http://127.0.0.1:9222?x']) {
    const {bridge, fetches} = harness();
    await assert.rejects(bridge.dispatch('attach', {...attachParams, endpoint}), /endpoint/i);
    assert.equal(fetches.length, 0);
  }
  for (const origin of ['http://127.0.0.1:8876/path', 'http://localhost:8876', 'http://127.0.0.1:8876.evil.test', 'http://127.0.0.1:8876/']) {
    const {bridge, fetches} = harness();
    await assert.rejects(bridge.dispatch('attach', {...attachParams, origin}), /origin/i);
    assert.equal(fetches.length, 0);
  }
});

test('refuses redirected metadata and remote or different-port websocket discovery', async () => {
  for (const webSocketDebuggerUrl of ['ws://evil.test:9222/devtools/browser/x', 'ws://127.0.0.1:9333/devtools/browser/x', 'ws://127.1:9222/devtools/browser/x', 'ws://user@127.0.0.1:9222/devtools/browser/x', 'ws://127.0.0.1:9222/devtools/page/x']) {
    let connected = false;
    const {bridge} = harness({fetchImpl: async () => new Response(JSON.stringify({webSocketDebuggerUrl})), connect: async () => {connected = true; throw new Error('should not connect');}});
    await assert.rejects(bridge.dispatch('attach', attachParams), /websocket/i);
    assert.equal(connected, false);
  }
  for (const response of [new Response('{}', {status: 302}), new Response('{}', {status: 500}), new Response('x'.repeat(70_000))]) {
    const {bridge} = harness({fetchImpl: async () => response});
    await assert.rejects(bridge.dispatch('attach', attachParams), /metadata/i);
  }
});

test('refuses ambiguous, wrong-origin, changed and explicitly mismatched targets and cleans up', async () => {
  for (const kind of ['ambiguous', 'wrong', 'explicit', 'changed', 'double']) {
    const {bridge, protocol} = harness();
    if (kind === 'ambiguous') protocol.targets.push({...protocol.targets[0], targetId: 'second'});
    if (kind === 'wrong') protocol.targets[0].url = 'http://127.0.0.1:8876.evil.test/';
    if (kind === 'changed') {
      const call = protocol.call.bind(protocol);
      protocol.call = async (method, params, sessionId) => method === 'Target.getTargetInfo' ? {targetInfo: {targetId: 'editor', type: 'page', url: 'http://evil.test/'}} : call(method, params, sessionId);
    }
    if (kind === 'double') {
      await bridge.dispatch('attach', attachParams);
      await assert.rejects(bridge.dispatch('attach', attachParams), /already/i);
      await bridge.dispatch('disconnectBridge');
    } else {
      await assert.rejects(bridge.dispatch('attach', {...attachParams, ...(kind === 'explicit' ? {targetId: 'other'} : {})}), /target/i);
      assert.equal(protocol.closed, true);
    }
  }
});

test('download behavior requires explicit private nonsymlink directory and scoped events', async () => {
  const directory = await realpath(await mkdtemp(join(tmpdir(), 'fwcycle-download-')));
  try {
    await chmod(directory, 0o700);
    const {bridge, protocol} = harness();
    await bridge.dispatch('attach', {...attachParams, downloads: directory});
    const setup = protocol.calls.find(c => c.method === 'Browser.setDownloadBehavior');
    assert.equal(setup.params.downloadPath, directory);
    assert.equal(setup.params.behavior, 'allowAndName');
    assert.equal(setup.params.eventsEnabled, true);
    protocol.emit('Browser.downloadWillBegin', {guid: 'download', frameId: 'frame', suggestedFilename: 'before.json'}, undefined);
    assert.equal((await bridge.dispatch('downloads')).downloads[0].state, 'pending');
    protocol.emit('Browser.downloadProgress', {guid: 'download', state: 'completed', receivedBytes: 123, totalBytes: 123}, undefined);
    assert.equal((await bridge.dispatch('downloads')).downloads[0].state, 'completed');
    await bridge.dispatch('disconnectBridge');
    await chmod(directory, 0o755);
    const bad = harness();
    await assert.rejects(bad.bridge.dispatch('attach', {...attachParams, downloads: directory}), /private/i);
    assert.equal(bad.fetches.length, 0);
    await chmod(directory, 0o700);
    const link = directory + '-link';
    await symlink(directory, link);
    try {await assert.rejects(harness().bridge.dispatch('attach', {...attachParams, downloads: link}), /symlink|canonical/i);}
    finally {await rm(link);}
  } finally {await rm(directory, {recursive: true, force: true});}
});

test('action arms chooser only for stock_connect BLE and exposes typed prompt status', async () => {
  for (const params of [{name: 'stock_connect', transport: 'BLE'}, {name: 'stock_connect', transport: 'usb'}, {name: 'own_connect', transport: 'BLE'}]) {
    const {bridge, protocol} = harness({onAction: async protocol => protocol.emit('DeviceAccess.deviceRequestPrompted', {id: 'chooser', devices: [{id: 'mixer', name: 'SMC-Mixer'}]})});
    await bridge.dispatch('attach', attachParams);
    assert.ok(protocol.calls.some(c => c.method === 'DeviceAccess.enable' && c.sessionId === 'session'));
    assert.ok(protocol.calls.some(c => c.method === 'Page.enable'));
    await bridge.dispatch('action', params);
    const expected = params.name === 'stock_connect' && params.transport === 'BLE' ? 'DeviceAccess.selectPrompt' : 'DeviceAccess.cancelPrompt';
    assert.ok(protocol.calls.some(c => c.method === expected));
    assert.equal((await bridge.dispatch('nativePrompt')).chooser.state, expected.endsWith('selectPrompt') ? 'selected' : 'unexpected');
    await assert.rejects(bridge.dispatch('nativePrompt', {accept: true}), /read.only/i);
    await bridge.dispatch('disconnectBridge');
  }
});

test('bounds JSON-line input/output and validates request envelopes without leaking exceptions', () => {
  const input = [
    'not JSON',
    JSON.stringify({method: 'hello'}),
    JSON.stringify({id: 3, method: 'hello', params: []}),
    JSON.stringify({id: 4, method: 'hello', params: {text: 'x'.repeat(70_000)}}),
    JSON.stringify({id: 5, method: 'hello'}),
  ].join('\n') + '\n';
  const child = spawnSync(process.execPath, [new URL('./bridge.mjs', import.meta.url).pathname], {input, encoding: 'utf8', timeout: 2000});
  assert.equal(child.status, 0, child.stderr);
  const replies = child.stdout.trim().split('\n').map(JSON.parse);
  assert.equal(replies.length, 5);
  for (const reply of replies.slice(0, 4)) assert.equal(typeof reply.error.message, 'string');
  assert.match(replies[3].error.message, /large/i);
  assert.equal(replies[4].result.hostOnly, true);
  assert.equal(child.stderr, '');
});

test('refuses actions after target origin drift and disarms a failed BLE action', async () => {
  const changed = harness();
  await changed.bridge.dispatch('attach', attachParams);
  changed.protocol.targets[0].url = 'http://127.0.0.1:8877/';
  await assert.rejects(changed.bridge.dispatch('action', {name: 'own_connect', transport: 'usb'}), /target|origin/i);
  await changed.bridge.dispatch('disconnectBridge');
  const failed = harness({onAction: async () => {throw new Error('button missing');}});
  await failed.bridge.dispatch('attach', attachParams);
  await assert.rejects(failed.bridge.dispatch('action', {name: 'stock_connect', transport: 'BLE'}), /button missing/);
  failed.protocol.emit('DeviceAccess.deviceRequestPrompted', {id: 'late', devices: [{id: 'device', name: 'SMC-Mixer'}]});
  assert.ok(!failed.protocol.calls.some(c => c.method === 'DeviceAccess.selectPrompt'));
  await failed.bridge.dispatch('disconnectBridge');
});

test('explicit profile is verified from browser command line or fails closed', async () => {
  const {bridge, protocol} = harness();
  const original = protocol.call.bind(protocol);
  protocol.call = async (method, params, sessionId) => method === 'Browser.getBrowserCommandLine' ? {arguments: ['chrome', '--user-data-dir=/private/owned-profile']} : original(method, params, sessionId);
  await assert.rejects(bridge.dispatch('attach', {...attachParams, profile: '/private/wrong-profile'}), /profile/i);
  assert.equal(protocol.closed, true);
  const good = harness();
  const goodCall = good.protocol.call.bind(good.protocol);
  good.protocol.call = async (method, params, sessionId) => method === 'Browser.getBrowserCommandLine' ? {arguments: ['chrome', '--user-data-dir=/private/owned-profile']} : goodCall(method, params, sessionId);
  const result = await good.bridge.dispatch('attach', {...attachParams, profile: '/private/owned-profile'});
  assert.equal(result.profileVerified, true);
  await good.bridge.dispatch('disconnectBridge');
});

test('editor contract uses exact uppercase BLE transport for chooser arming', async () => {
  const {bridge, protocol} = harness({onAction: async protocol => protocol.emit('DeviceAccess.deviceRequestPrompted', {id: 'upper', devices: [{id: 'mixer', name: 'SMC-Mixer'}]})});
  await bridge.dispatch('attach', attachParams);
  await bridge.dispatch('action', {name: 'stock_connect', transport: 'BLE'});
  assert.ok(protocol.calls.some(c => c.method === 'DeviceAccess.selectPrompt'));
  await bridge.dispatch('disconnectBridge');
});

test('JSON-lines writer bounds large editor output and processes requests sequentially', async () => {
  const replies = [];
  let active = 0;
  const bridge = {
    async dispatch(method) {
      assert.equal(active++, 0);
      await new Promise(resolve => setTimeout(resolve, 2));
      active--;
      return method === 'snapshot' ? {log: 'x'.repeat(100_000)} : {ok: true};
    },
    async detach() {this.detached = true;},
  };
  const output = new Writable({write(chunk, encoding, callback) {replies.push(JSON.parse(String(chunk))); callback();}});
  await run(Readable.from(['{"id":1,"method":"snapshot"}\n{"id":2,"method":"hello"}']), output, bridge);
  assert.match(replies[0].error.message, /Response too large/);
  assert.deepEqual(replies[1], {id: 2, result: {ok: true}});
  assert.equal(bridge.detached, true);
});

test('standalone hello is host-only JSON-lines and rejects arbitrary methods', () => {
  const child = spawnSync(process.execPath, [new URL('./bridge.mjs', import.meta.url).pathname], {
    input: '{"id":1,"method":"hello"}\n{"id":2,"method":"evaluate","params":{"expression":"42"}}\n', encoding: 'utf8', timeout: 2000,
  });
  assert.equal(child.status, 0, child.stderr);
  const lines = child.stdout.trim().split('\n').map(JSON.parse);
  assert.equal(lines.length, 2);
  assert.equal(lines[0].id, 1);
  assert.equal(lines[0].result.hostOnly, true);
  assert.equal(lines[0].result.nativeWebSocket, typeof WebSocket === 'function');
  assert.equal(lines[0].result.nativeFetch, typeof fetch === 'function');
  assert.match(lines[1].error.message, /unsupported/i);
});

test('unattached typed editor requests fail safely', async () => {
  const bridge = new Bridge();
  for (const method of ['snapshot', 'action', 'release', 'exportConfig']) {
    await assert.rejects(bridge.dispatch(method, {}), /attach/i);
  }
  assert.deepEqual(await bridge.dispatch('disconnectBridge'), {detached: true});
});
