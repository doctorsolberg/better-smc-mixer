import test from 'node:test';
import assert from 'node:assert/strict';
import {CDPClient} from './cdp.mjs';

class Socket extends EventTarget {
  readyState = 1;
  sent = [];
  send(text) { this.sent.push(JSON.parse(text)); }
  receive(message) { this.dispatchEvent(new MessageEvent('message', {data: JSON.stringify(message)})); }
  close() { this.readyState = 3; this.dispatchEvent(new Event('close')); }
}

test('multiplexes out-of-order responses and session-scoped events', async () => {
  const socket = new Socket();
  const client = new CDPClient(socket);
  const events = [];
  const off = client.on('Page.event', p => events.push(p), 'one');
  const first = client.call('First', {}, 'one');
  const second = client.call('Second');
  socket.receive({method: 'Page.event', params: {n: 0}, sessionId: 'other'});
  socket.receive({method: 'Page.event', params: {n: 1}, sessionId: 'one'});
  socket.receive({id: socket.sent[1].id, result: {second: true}});
  socket.receive({id: socket.sent[0].id, result: {first: true}});
  assert.deepEqual(await first, {first: true});
  assert.deepEqual(await second, {second: true});
  assert.equal(socket.sent[0].sessionId, 'one');
  assert.deepEqual(events, [{n: 1}]);
  off();
  socket.receive({method: 'Page.event', params: {n: 2}, sessionId: 'one'});
  assert.equal(events.length, 1);
  client.close();
});

test('rejects protocol and Runtime evaluation exceptions', async () => {
  const socket = new Socket();
  const client = new CDPClient(socket);
  const protocol = client.call('Bad');
  socket.receive({id: socket.sent.at(-1).id, error: {message: 'denied'}});
  await assert.rejects(protocol, /denied/);
  const evaluation = client.call('Runtime.evaluate', {expression: 'bad()'});
  socket.receive({id: socket.sent.at(-1).id, result: {exceptionDetails: {text: 'Uncaught', exception: {description: 'boom'}}}});
  await assert.rejects(evaluation, /boom/);
  client.close();
});

test('bounds calls, timeouts, cancellation and closed transport', async () => {
  const socket = new Socket();
  const client = new CDPClient(socket, {timeoutMs: 15, maxPending: 1});
  const timed = client.call('Slow');
  await assert.rejects(client.call('Overflow'), /pending/i);
  await assert.rejects(timed, /timeout/i);
  const controller = new AbortController();
  const cancelled = client.call('Cancel', {}, undefined, {signal: controller.signal});
  controller.abort();
  await assert.rejects(cancelled, /abort/i);
  const closed = client.call('Closing');
  socket.close();
  await assert.rejects(closed, /closed/i);
  await assert.rejects(client.call('TooLate'), /closed/i);
});

test('fails closed on malformed or oversized protocol frames', async () => {
  for (const data of ['not JSON', JSON.stringify({method: 'x', params: {text: 'x'.repeat(200)}})]) {
    const socket = new Socket();
    const client = new CDPClient(socket, {maxMessageBytes: 128});
    const pending = client.call('Wait');
    socket.dispatchEvent(new MessageEvent('message', {data}));
    await assert.rejects(pending, /frame|message/i);
    client.close();
  }
});

test('connect waits for native-style socket open and bounds handshake', async () => {
  class Opening extends Socket {
    readyState = 0;
    constructor(url) { super(); this.url = url; queueMicrotask(() => {this.readyState = 1; this.dispatchEvent(new Event('open'));}); }
  }
  const client = await CDPClient.connect('ws://127.0.0.1:9222/devtools/browser/x', {WebSocketImpl: Opening});
  assert.equal(client.socket.url, 'ws://127.0.0.1:9222/devtools/browser/x');
  client.close();
  class Never extends Socket { readyState = 0; }
  await assert.rejects(CDPClient.connect('ws://127.0.0.1:9222/devtools/browser/x', {WebSocketImpl: Never, timeoutMs: 5}), /timeout/i);
});

test('tracks only scoped real download events with pending and interrupted states', async () => {
  const {DownloadTracker} = await import('./cdp.mjs');
  const socket = new Socket(); const client = new CDPClient(socket);
  const tracker = new DownloadTracker(client, {directory: '/private/run', frameIds: ['editor-frame'], limit: 2});
  socket.receive({method: 'Browser.downloadProgress', params: {guid: 'not-started', state: 'completed'}});
  socket.receive({method: 'Browser.downloadWillBegin', params: {guid: 'other', frameId: 'other-frame', suggestedFilename: 'private.bin'}});
  assert.deepEqual(tracker.snapshot(), []);
  socket.receive({method: 'Browser.downloadWillBegin', params: {guid: 'backup', frameId: 'editor-frame', suggestedFilename: 'full.bin'}});
  assert.equal(tracker.snapshot()[0].state, 'pending');
  socket.receive({method: 'Browser.downloadProgress', params: {guid: 'backup', state: 'inProgress', receivedBytes: 10, totalBytes: 20}});
  assert.equal(tracker.snapshot()[0].state, 'inProgress');
  socket.receive({method: 'Browser.downloadProgress', params: {guid: 'backup', state: 'completed', receivedBytes: 20, totalBytes: 20}});
  assert.equal(tracker.snapshot()[0].state, 'completed');
  assert.equal(tracker.snapshot()[0].path, '/private/run/backup');
  socket.receive({method: 'Browser.downloadWillBegin', params: {guid: 'export', frameId: 'editor-frame', suggestedFilename: '../../escape'}});
  socket.receive({method: 'Browser.downloadProgress', params: {guid: 'export', state: 'canceled'}});
  assert.equal(tracker.snapshot()[1].state, 'canceled');
  tracker.close(); client.close();
});

test('real chooser selects exact SMC-Mixer only during armed stock BLE action', async () => {
  const {PromptPolicy} = await import('./cdp.mjs');
  const socket = new Socket(); const client = new CDPClient(socket);
  const policy = new PromptPolicy(client, 'session', {timeoutMs: 100});
  policy.arm();
  socket.receive({method: 'DeviceAccess.deviceRequestPrompted', sessionId: 'other', params: {id: 'foreign', devices: [{id: 'a', name: 'SMC-Mixer'}]}});
  assert.equal(socket.sent.length, 0);
  socket.receive({method: 'DeviceAccess.deviceRequestPrompted', sessionId: 'session', params: {id: 'prompt', devices: []}});
  assert.equal(socket.sent.length, 0);
  socket.receive({method: 'DeviceAccess.deviceRequestPrompted', sessionId: 'session', params: {id: 'prompt', devices: [{id: 'wrong', name: 'SMC-Mixer_013'}, {id: 'right', name: 'SMC-Mixer'}]}});
  assert.equal(socket.sent[0].method, 'DeviceAccess.selectPrompt');
  assert.deepEqual(socket.sent[0].params, {id: 'prompt', deviceId: 'right'});
  socket.receive({id: socket.sent[0].id, result: {}});
  await policy.flush();
  assert.equal(policy.snapshot().chooser.state, 'selected');
  socket.receive({method: 'DeviceAccess.deviceRequestPrompted', sessionId: 'session', params: {id: 'prompt', devices: [{id: 'right', name: 'SMC-Mixer'}]}});
  assert.equal(socket.sent.length, 1, 'stale chooser update cannot select twice');
  policy.close(); client.close();
});

test('chooser refuses ambiguity, stale prompt replacement and unarmed requests', async () => {
  const {PromptPolicy} = await import('./cdp.mjs');
  for (const kind of ['unarmed', 'ambiguous', 'stale']) {
    const socket = new Socket(); const client = new CDPClient(socket); const policy = new PromptPolicy(client, 'session');
    if (kind !== 'unarmed') policy.arm();
    if (kind === 'stale') socket.receive({method: 'DeviceAccess.deviceRequestPrompted', sessionId: 'session', params: {id: 'first', devices: []}});
    socket.receive({method: 'DeviceAccess.deviceRequestPrompted', sessionId: 'session', params: {id: 'second', devices: kind === 'ambiguous' ? [{id: 'a', name: 'SMC-Mixer'}, {id: 'b', name: 'SMC-Mixer'}] : [{id: 'a', name: 'SMC-Mixer'}]}});
    assert.equal(socket.sent.at(-1).method, 'DeviceAccess.cancelPrompt');
    for (const sent of socket.sent) socket.receive({id: sent.id, result: {}});
    await policy.flush();
    assert.ok(['unexpected', 'ambiguous', 'stale'].includes(policy.snapshot().chooser.state));
    policy.close(); client.close();
  }
});

test('unexpected JavaScript dialogs are rejected, never globally accepted', async () => {
  const {PromptPolicy} = await import('./cdp.mjs');
  const socket = new Socket(); const client = new CDPClient(socket); const policy = new PromptPolicy(client, 'session');
  socket.receive({method: 'Page.javascriptDialogOpening', sessionId: 'session', params: {type: 'confirm', message: 'Unexpected destructive operation'}});
  assert.equal(socket.sent[0].method, 'Page.handleJavaScriptDialog');
  assert.deepEqual(socket.sent[0].params, {accept: false});
  socket.receive({id: socket.sent[0].id, result: {}});
  await policy.flush();
  assert.equal(policy.snapshot().dialog.state, 'rejected');
  policy.close(); client.close();
});

test('expired chooser cancels its real pending prompt and cannot select late devices', async () => {
  const {PromptPolicy} = await import('./cdp.mjs');
  const socket = new Socket(); const client = new CDPClient(socket); const policy = new PromptPolicy(client, 'session', {timeoutMs: 5});
  policy.arm();
  socket.receive({method: 'DeviceAccess.deviceRequestPrompted', sessionId: 'session', params: {id: 'expired', devices: []}});
  await new Promise(resolve => setTimeout(resolve, 15));
  assert.equal(socket.sent[0]?.method, 'DeviceAccess.cancelPrompt');
  socket.receive({id: socket.sent[0].id, result: {}});
  await policy.flush();
  assert.equal(policy.snapshot().chooser.state, 'timeout');
  socket.receive({method: 'DeviceAccess.deviceRequestPrompted', sessionId: 'session', params: {id: 'expired', devices: [{id: 'late', name: 'SMC-Mixer'}]}});
  assert.equal(socket.sent.length, 1);
  policy.close(); client.close();
});

test('rejects oversized outbound CDP calls without writing socket bytes', async () => {
  const socket = new Socket(); const client = new CDPClient(socket, {maxMessageBytes: 128});
  await assert.rejects(client.call('Runtime.evaluate', {expression: 'x'.repeat(200)}), /large/i);
  assert.equal(socket.sent.length, 0);
  client.close();
});

test('event listener failure rejects pending work instead of crashing process', async () => {
  const socket = new Socket(); const client = new CDPClient(socket, {timeoutMs: 20});
  client.on('Broken.event', () => {throw new Error('bad listener');});
  const pending = client.call('Wait');
  socket.receive({method: 'Broken.event', params: {}});
  await assert.rejects(pending, /listener/i);
  client.close();
});

export {Socket};
