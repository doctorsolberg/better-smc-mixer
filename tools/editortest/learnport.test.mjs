// Host-only Web MIDI Learn regressions. All access and ports are fakes.
import test from 'node:test';
import assert from 'node:assert/strict';
import { getEventListeners } from 'node:events';
import { createLearnPort } from '../../app/editor/js/learnport.js';

class Input extends EventTarget {
  constructor(id, name = id, state = 'connected', connection = 'closed') {
    super();
    Object.assign(this, { id, name, manufacturer: 'Fake', state, connection, type: 'input', opens: 0, closes: 0 });
  }
  async open() { this.opens++; this.connection = 'open'; }
  async close() { this.closes++; this.connection = 'closed'; }
  send(...bytes) {
    const event = Object.assign(new Event('midimessage'), { data: Uint8Array.from(bytes) });
    this.onmidimessage?.(event);
    this.dispatchEvent(event);
  }
}

class Access extends EventTarget {
  constructor(...ports) {
    super();
    this.inputs = new Map(ports.map((p) => [p.id, p]));
  }
  get outputs() { assert.fail('Learn must not inspect or write outputs'); }
  change(port, state) {
    port.state = state;
    this.onstatechange?.({ port });
    this.dispatchEvent(Object.assign(new Event('statechange'), { port }));
  }
}

function fixture(ports, options = {}) {
  const access = new Access(...ports), requests = [], states = [], messages = [];
  const learn = createLearnPort({
    requestAccess: async (opts) => { requests.push(opts); return access; },
    onChange: (state) => states.push(state), onMessage: (bytes) => messages.push(bytes), ...options,
  });
  return { access, learn, requests, states, messages };
}

test('Learn start requests non-SysEx access without opening any inputs', async () => {
  const synth = new Input('synth', 'SMC-Mixer Editor Loop');
  const mixerNames = ['SMC-Mixer', 'SMC-Mixer Config', 'Better-SMC-Mixer Bluetooth', 'SMC-Mixer Bluetooth',
    'Better-SMC-Mixer', 'SMC-Mixer-Private', 'SMC-Mixer-Master', 'SMC-Mixer -Private', 'SMC-Mixer -Master',
    'Better-SMC-Mixer SMC-Mixer', 'Better-SMC-Mixer SMC-Mixer Config', 'SMC-Mixer hardware test SMC-Mixer Config',
    'MIDIIN2 (SMC-Mixer)', 'MIDIIN3 (Better-SMC-Mixer)', 'SMC-Mixer:SMC-Mixer Config 20:1', 'Better-SMC-Mixer:Better-SMC-Mixer 24:0'];
  const ports = [synth, new Input('offline', 'Offline synth', 'disconnected'), new Input('owned', 'Renamed mixer'),
    ...mixerNames.map((name, i) => new Input(`mixer-${i}`, name))];
  const { learn, requests, states, messages } = fixture(ports, { excludeInput: (port) => port.id === 'owned' });
  assert.deepEqual(requests, []);
  assert.equal(await learn.start(), true);
  assert.deepEqual(requests, [{ sysex: false }]);
  assert.deepEqual(states.at(-1), { inputs: [{ id: 'synth', name: synth.name, manufacturer: 'Fake' }], selectedId: null });
  synth.send(0xB0, 74, 32);
  assert.deepEqual(messages, []);
  assert.ok(ports.every((p) => p.opens === 0));
});

test('only the explicitly selected input is opened and heard', async () => {
  const a = new Input('a'), b = new Input('b'), mixer = new Input('mix', 'SMC-Mixer');
  const { learn, states, messages } = fixture([a, b, mixer]);
  await learn.start();
  assert.equal(await learn.select(mixer.id), false);
  assert.equal(await learn.select(b.id), true);
  assert.equal(states.at(-1).selectedId, b.id);
  a.send(0xB0, 30, 12);
  mixer.send(0xB0, 30, 13);
  b.send(0xB1, 75, 14);
  assert.deepEqual(messages, [[0xB1, 75, 14]]);
  assert.deepEqual([a.opens, b.opens, mixer.opens], [0, 1, 0]);
});

test('channel messages retain running status across chunks and interleaved realtime', async () => {
  const in1 = new Input('instrument');
  const { learn, messages } = fixture([in1]);
  await learn.start();
  await learn.select(in1.id);
  in1.send(0xF8, 1, 2, 0xB3, 74, 0xF8);
  in1.send(12, 75, 13, 0xFE, 0xC4, 51, 52, 0xD5, 42);
  assert.deepEqual(messages, [[0xB3, 74, 12], [0xB3, 75, 13], [0xC4, 51], [0xC4, 52], [0xD5, 42]]);
});

test('SysEx and system common drop any partial message and clear running status', async () => {
  const in1 = new Input('instrument');
  const { learn, messages } = fixture([in1]);
  await learn.start();
  await learn.select(in1.id);
  in1.send(0xB3, 74, 0xF0, 0x7D, 1, 0xF8);
  in1.send(2, 0xF7, 13, 14, 0xE2, 9, 7, 0xF2, 0, 1, 15, 16, 0xE2, 10, 8);
  assert.deepEqual(messages, [[0xE2, 9, 7], [0xE2, 10, 8]]);
});

test('zero-velocity Note On is delivered as Note Off without changing running status', async () => {
  const in1 = new Input('instrument');
  const { learn, messages } = fixture([in1]);
  await learn.start();
  await learn.select(in1.id);
  in1.send(0x95, 60, 0, 61, 100, 0x85, 62, 23, 0xA5, 63, 45);
  assert.deepEqual(messages, [[0x85, 60, 0], [0x95, 61, 100], [0x85, 62, 23], [0xA5, 63, 45]]);
});

test('input statechange updates choices and a replug requires new selection', async () => {
  const a = new Input('a'), b = new Input('b', 'B', 'disconnected');
  const { access, learn, states, messages } = fixture([a, b]);
  let otherChanges = 0;
  access.onstatechange = () => { otherChanges++; };
  await learn.start();
  await learn.select(a.id);
  a.send(0xB0, 74);
  access.change(a, 'disconnected');
  assert.deepEqual(states.at(-1), { inputs: [], selectedId: null });
  assert.deepEqual(getEventListeners(a, 'midimessage'), []);
  access.change(b, 'connected');
  access.change(a, 'connected');
  a.send(12, 0xB0, 75, 13);
  assert.deepEqual(messages, []);
  assert.deepEqual(states.at(-1).inputs.map(({ id }) => id), ['a', 'b']);
  assert.equal(states.at(-1).selectedId, null);
  await learn.select(a.id);
  a.send(12, 0xB0, 75, 13);
  assert.deepEqual(messages, [[0xB0, 75, 13]], 'old partial messages were cleared');
  await learn.stop();
  assert.equal(a.closes, 2);
  assert.deepEqual(getEventListeners(access, 'statechange'), []);
  const updates = states.length;
  access.change(a, 'disconnected');
  assert.equal(states.length, updates);
  assert.equal(otherChanges, 4, 'the other access handler was left alone');
});

test('switching inputs removes the old listener and closes only Learn-opened ports', async () => {
  const a = new Input('a'), shared = new Input('shared', 'Shared input', 'connected', 'open');
  const { learn, messages } = fixture([a, shared]);
  const other = [];
  const otherListener = (e) => other.push(Array.from(e.data));
  shared.addEventListener('midimessage', otherListener);
  await learn.start();
  await learn.select(a.id);
  await learn.select(shared.id);
  a.send(0x90, 60, 100);
  shared.send(0x90, 61, 100);
  await learn.stop();
  assert.deepEqual(getEventListeners(a, 'midimessage'), []);
  assert.deepEqual(getEventListeners(shared, 'midimessage'), [otherListener]);
  shared.send(0x90, 62, 100);
  assert.deepEqual(messages, [[0x90, 61, 100]]);
  assert.deepEqual(other, [[0x90, 61, 100], [0x90, 62, 100]]);
  assert.deepEqual([a.opens, a.closes, shared.opens, shared.closes], [1, 1, 0, 0]);
});

test('selecting no input stops listening without ending discovery', async () => {
  const a = new Input('a');
  const { learn, states, messages } = fixture([a]);
  await learn.start();
  await learn.select(a.id);
  assert.equal(await learn.select(null), true);
  assert.equal(states.at(-1).selectedId, null);
  a.send(0xB0, 75, 13);
  assert.deepEqual(messages, []);
  assert.equal(a.closes, 1);
  assert.equal(await learn.select(a.id), true);
});

const deferred = () => {
  let resolve;
  const promise = new Promise((r) => { resolve = r; });
  return { promise, resolve };
};

test('cancelling a permission prompt ignores late access', async () => {
  const a = new Input('a'), wait = deferred();
  const { access, learn, states } = fixture([a], { requestAccess: () => wait.promise });
  const start = learn.start();
  learn.stop();
  wait.resolve(access);
  assert.equal(await start, false);
  access.change(a, 'disconnected');
  assert.deepEqual(states, []);
  assert.equal(await learn.select(a.id), false);
});

test('permission denial reaches the caller and can be retried from a later Learn click', async () => {
  const a = new Input('a');
  const denied = new DOMException('permission denied', 'NotAllowedError');
  let refuse = true;
  const { learn, access, states } = fixture([a], { requestAccess: async () => {
    if (refuse) throw denied;
    return access;
  } });
  await assert.rejects(learn.start(), (error) => error === denied);
  assert.equal(await learn.select(a.id), false);
  assert.deepEqual(states, []);
  refuse = false;
  assert.equal(await learn.start(), true);
  assert.equal(states.at(-1).selectedId, null);
});

test('a late open is closed before a new session selects the same port', async () => {
  const a = new Input('a'), wait = deferred();
  const { learn, states, messages } = fixture([a]);
  const open = a.open.bind(a);
  a.open = async () => { await wait.promise; await open(); };
  await learn.start();
  const first = learn.select(a.id);
  await Promise.resolve(); // let the first open begin before cancellation
  const stop = learn.stop();
  await learn.start();
  const second = learn.select(a.id);
  wait.resolve();
  assert.equal(await first, false);
  await stop;
  assert.equal(await second, true);
  assert.equal(states.at(-1).selectedId, a.id);
  assert.equal(a.closes, 1);
  assert.equal(a.connection, 'open');
  a.send(0xB0, 75, 13);
  assert.deepEqual(messages, [[0xB0, 75, 13]]);
  await learn.stop();
  assert.deepEqual([a.opens, a.closes, a.connection], [2, 2, 'closed']);
});

test('stopping during open releases the port after open actually finishes', async () => {
  const a = new Input('a'), wait = deferred();
  const { learn } = fixture([a]);
  const open = a.open.bind(a);
  a.open = async () => { await wait.promise; await open(); };
  await learn.start();
  const select = learn.select(a.id);
  await Promise.resolve(); // let open begin before cancellation
  const stop = learn.stop();
  wait.resolve();
  assert.equal(await select, false);
  await stop;
  assert.deepEqual([a.opens, a.closes, a.connection], [1, 1, 'closed']);
});

test('a new selection waits for a pending close of the same port', async () => {
  const a = new Input('a'), wait = deferred();
  const { learn } = fixture([a]);
  const close = a.close.bind(a);
  a.close = async () => { await wait.promise; await close(); };
  await learn.start();
  await learn.select(a.id);
  const stop = learn.stop();
  await learn.start();
  const second = learn.select(a.id);
  wait.resolve();
  await stop;
  assert.equal(await second, true);
  assert.equal(a.connection, 'open');
  await learn.stop();
  assert.deepEqual([a.opens, a.closes, a.connection], [2, 2, 'closed']);
});

test('stop waits for a close already started by unplugging the selected input', async () => {
  const a = new Input('a'), wait = deferred();
  const { access, learn } = fixture([a]);
  const close = a.close.bind(a);
  a.close = async () => { await wait.promise; await close(); };
  await learn.start();
  await learn.select(a.id);
  access.change(a, 'disconnected');
  let stopped = false;
  const stop = learn.stop().then(() => { stopped = true; });
  await new Promise((resolve) => setImmediate(resolve));
  assert.equal(stopped, false);
  wait.resolve();
  await stop;
  assert.equal(a.connection, 'closed');
});

test('closing Learn during message delivery drops the rest of that packet', async () => {
  const a = new Input('a'), messages = [];
  const { learn } = fixture([a], { onMessage: (bytes) => { messages.push(bytes); learn.stop(); } });
  await learn.start();
  await learn.select(a.id);
  a.send(0xB0, 75, 13, 76, 14);
  assert.deepEqual(messages, [[0xB0, 75, 13]]);
  assert.equal(a.closes, 1);
});

test('failed open leaves Learn available for choosing another input', async () => {
  const a = new Input('a'), b = new Input('b');
  a.open = async () => { throw new Error('cannot open port'); };
  const { learn, states, messages } = fixture([a, b]);
  await learn.start();
  await assert.rejects(learn.select(a.id), /cannot open port/);
  assert.equal(a.closes, 0);
  assert.equal(states.at(-1).selectedId, null);
  a.send(0xB0, 75, 13);
  assert.deepEqual(messages, []);
  assert.equal(await learn.select(b.id), true);
});

test('an external mixer claim refreshes the list without closing its input', async () => {
  const a = new Input('a');
  let claimed = false;
  const { learn, states, messages } = fixture([a], { excludeInput: (port) => claimed && port.id === a.id });
  await learn.start();
  await learn.select(a.id);
  claimed = true;
  learn.refresh();
  a.send(0xB0, 74, 20);
  assert.deepEqual(states.at(-1), { inputs: [], selectedId: null });
  assert.deepEqual(messages, []);
  assert.equal(a.closes, 0);
});

test('selecting an input that disappears during prior cleanup does not pin a stale selection', async () => {
  const a = new Input('a'), wait = deferred();
  const { learn, states } = fixture([a]);
  const close = a.close.bind(a);
  a.close = async () => { await wait.promise; await close(); };
  await learn.start();
  await learn.select(a.id);
  const stop = learn.stop();
  await learn.start();
  const stale = learn.select(a.id);
  // Web MIDI's map/state can change before the corresponding statechange callback arrives.
  a.state = 'disconnected';
  wait.resolve();
  await stop;
  assert.equal(await stale, false);
  a.state = 'connected';
  assert.equal(await learn.select(a.id), true);
  assert.equal(states.at(-1).selectedId, a.id);
});