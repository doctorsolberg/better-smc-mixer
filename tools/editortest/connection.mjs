// Connection failure/race tests for MixerDevice. Deferred fake clients, no MIDI/device APIs.
import { MixerDevice } from '../../app/editor/js/mixer.js';
import { LinkError, CMD, KIND_REPLY, record, frame, crc32 } from '../../app/editor/js/link.js';

const deferred = () => { let resolve, reject; const promise = new Promise((a, b) => { resolve = a; reject = b; }); return { promise, resolve, reject }; };
const polyfill = () => { globalThis.CustomEvent ??= class extends Event { constructor(t, o) { super(t); this.detail = o?.detail; } }; };
const baseFile = (codec) => codec.binaryToFile(codec.fileToBinary({ schemaVersion: codec.version }));
const info = (revision = 80, store = 'clean') => ({ revision, store, source: 'received', generation: 4, chunk: 128 });
function activate(m, client, doc) {
  m.client = client; m.connected = true; m.running = doc; m.saved = doc;
  m.revision = 80; m.storage = 'clean'; m.mixerId = null;
}

function refusedConfiguration(codec, version) {
  const input = { name: 'SMC-Mixer Config', state: 'connected', onmidimessage: null };
  const blob = new Uint8Array(36); // a complete transfer with a valid transport CRC, invalid magic
  const cfg = new Uint8Array(37), view = new DataView(cfg.buffer);
  cfg[1] = version; cfg[2] = codec.binaryVersion;
  view.setUint16(3, codec.version, true); view.setUint32(5, 16384, true);
  cfg[9] = 128; cfg[32] = 1;
  const le = (n) => [n & 255, (n >>> 8) & 255, (n >>> 16) & 255, (n >>> 24) & 255];
  const output = { name: input.name, send(bytes) {
    const request = record(Uint8Array.from(bytes)), cmd = request[2];
    let payload;
    if (cmd === CMD['cfg-info']) payload = cfg;
    else if (cmd === CMD['cfg-get-begin']) payload = [0, ...le(blob.length), ...le(crc32(blob))];
    else if (cmd === CMD['cfg-get-chunk']) {
      const offset = new DataView(Uint8Array.from(request.slice(3)).buffer).getUint32(0, true);
      payload = [0, ...le(offset), ...blob.slice(offset, offset + request[7])];
    } else throw new Error(`unexpected test command ${cmd}`);
    queueMicrotask(() => input.onmidimessage?.({ data: frame([KIND_REPLY, request[1], cmd, 0, ...payload]) }));
  } };
  return { input, output, access: { inputs: new Map([['c', input]]), outputs: new Map([['c', output]]) } };
}

export async function failurePaths(codec) {
  polyfill();
  const problems = [];
  const original = Object.getOwnPropertyDescriptor(globalThis, 'navigator');
  const use = (value) => Object.defineProperty(globalThis, 'navigator', { configurable: true, value });
  try {
    use({ requestMIDIAccess: async () => { throw Object.assign(new Error('permission denied'), { name: 'NotAllowedError' }); } });
    let m = new MixerDevice(codec);
    try { await m.connect(null); problems.push('permission refusal was treated as a connection'); }
    catch (e) { if (!(e instanceof LinkError) || !/no MIDI access with SysEx/.test(e.message)) problems.push(`permission error: ${e.message}`); }
    if (m.connected || m.timer || m.ports.length) problems.push('permission refusal left an active connection');
    m.close();
    use({ requestMIDIAccess: async () => ({ inputs: new Map(), outputs: new Map() }) });
    m = new MixerDevice(codec);
    try { await m.connect(null); problems.push('no ports was treated as a connection'); }
    catch (e) { if (!(e instanceof LinkError) || !/no mixer answered/.test(e.message)) problems.push(`no ports: ${e.message}`); }
    if (m.connected || m.timer || m.ports.length) problems.push('no ports left an active connection');
    m.close();

    for (const version of [255, 4]) {
      const fake = refusedConfiguration(codec, version);
      use({ requestMIDIAccess: async () => fake.access });
      m = new MixerDevice(codec);
      m.find = async () => ({ input: fake.input, output: fake.output,
        linkInfo: { caps: ['config'], fwVersion: '0.1.0', buildId: 'test' } });
      try { await m.connect(null); problems.push(`invalid configuration/protocol ${version} was connected`); }
      catch (e) {
        if (!/protocol|configuration|magic/i.test(e.message)) problems.push(`unexpected refusal ${version}: ${e.message}`);
      }
      if (m.connected || m.client || m.timer || fake.input.onmidimessage) problems.push(`refusal ${version} leaked a connection`);
      m.close();
    }

    const access = deferred();
    use({ requestMIDIAccess: () => access.promise });
    m = new MixerDevice(codec);
    const attempt = m.connect(null).catch((e) => e);
    m.close();
    const replacementAccess = {};
    activate(m, {}, '{}');
    m.access = replacementAccess;
    access.resolve({ inputs: new Map(), outputs: new Map() });
    await attempt;
    if (m.access !== replacementAccess || !m.connected) problems.push('cancelled MIDI permission request changed the replacement connection');
    m.close();

    const choice = deferred();
    use({ bluetooth: { requestDevice: () => choice.promise } });
    m = new MixerDevice(codec);
    const bleAttempt = m.connectBluetooth().catch((e) => e);
    m.close();
    activate(m, {}, '{}');
    let gattConnects = 0;
    choice.resolve({ name: 'SMC-Mixer', gatt: { async connect() { gattConnects++; throw new Error('cancelled choice'); } } });
    await bleAttempt;
    if (gattConnects || !m.connected) problems.push('cancelled Bluetooth choice opened a stale device connection');
    m.close();
  } finally {
    delete globalThis.navigator;
    if (original) Object.defineProperty(globalThis, 'navigator', original);
  }
  return problems;
}

export async function connectionRaces(codec) {
  polyfill();
  const problems = [];
  const file = baseFile(codec), original = JSON.stringify(file);
  const replacement = JSON.stringify({ ...file, name: 'replacement connection' });
  const first = JSON.stringify({ ...file, global: { ...(file.global ?? {}), save: 'auto' } });
  const queued = JSON.stringify({ ...file, global: { ...(file.global ?? {}), pickup: 'jump' } });

  for (const reject of [false, true]) {
    const old = deferred(), m = new MixerDevice(codec);
    activate(m, { info: () => old.promise }, original);
    const task = m.poll();
    m.close();
    activate(m, { info: async () => info() }, replacement);
    if (reject) old.reject(new LinkError('no reply from the old connection'));
    else old.resolve(info(1, 'unsaved'));
    await task;
    if (!m.connected || m.storage !== 'clean') problems.push(`stale poll ${reject ? 'failure' : 'reply'} changed the replacement connection`);
    m.close();
  }

  {
    const old = deferred(), m = new MixerDevice(codec);
    activate(m, { edit: () => old.promise }, original);
    m.pending = { doc: first, what: 'old edit' };
    const task = m.pushNext();
    m.apply(queued, 'old queued edit');
    m.close();
    let sent = 0;
    activate(m, { edit: async () => { sent++; return info(); }, push: async () => { sent++; return info(); } }, replacement);
    old.resolve(info(1, 'unsaved'));
    await task;
    if (sent || m.running !== replacement || m.revision !== 80 || m.storage !== 'clean' || m.pending) {
      problems.push(`old edit or its queue crossed reconnection (${sent} new-connection writes)`);
    }
    m.close();
  }

  {
    const old = deferred(), next = deferred(), m = new MixerDevice(codec);
    activate(m, { edit: () => old.promise }, original);
    m.pending = { doc: first, what: 'old edit' };
    const oldTask = m.pushNext();
    m.close();
    activate(m, { edit: () => next.promise }, original);
    m.pending = { doc: first, what: 'replacement edit' };
    const newTask = m.pushNext();
    old.resolve(info(1, 'unsaved'));
    await oldTask;
    if (!m.pushing || !m.sending) problems.push('old edit completion unlocked the replacement operation');
    next.resolve(info(81, 'unsaved'));
    await newTask;
    if (m.pushing || m.sending || m.running !== first) problems.push('replacement edit did not finish normally');
    m.close();
  }

  {
    const m = new MixerDevice(codec);
    activate(m, {}, original);
    m.pushing = true;
    const task = m.saveNow().catch((e) => e); // A lost Save rejects; it cannot save a new connection.
    m.close();
    let saved = 0;
    activate(m, { save: async () => { saved++; }, info: async () => info() }, replacement);
    m.pushing = false;
    await task;
    if (saved) problems.push('Save waiting on the old connection saved the replacement connection');
    m.close();
  }

  {
    const old = deferred(), m = new MixerDevice(codec);
    activate(m, { save: () => old.promise }, original);
    const task = m.saveNow().catch((e) => e);
    m.close();
    let reads = 0;
    activate(m, { info: async () => { reads++; return info(); } }, replacement);
    old.resolve();
    await task;
    if (reads || m.running !== replacement) problems.push('old Save continued on the replacement client');
    m.close();
  }

  {
    const old = deferred(), m = new MixerDevice(codec);
    activate(m, { revert: () => old.promise }, original);
    const task = m.revert();
    m.close();
    let reads = 0;
    activate(m, { pull: async () => { reads++; return codec.fileToBinary(file); } }, replacement);
    old.resolve(info(1));
    const value = await task;
    if (reads || value != null || m.running !== replacement || m.revision !== 80) problems.push('old Revert changed the replacement connection');
    m.close();
  }

  {
    const old = deferred(), m = new MixerDevice(codec);
    activate(m, { info: () => old.promise }, original);
    const task = m.pullFile().catch((e) => e);
    m.close();
    let reads = 0;
    activate(m, { pull: async () => { reads++; return codec.fileToBinary(file); } }, replacement);
    old.resolve(info());
    const value = await task;
    if (reads || !(value instanceof LinkError) || m.running !== replacement) problems.push('old Load read the replacement client');
    m.close();
  }

  {
    const old = deferred(), m = new MixerDevice(codec);
    activate(m, { stock: () => old.promise }, original);
    const task = m.backToStock();
    m.close();
    activate(m, {}, replacement);
    old.resolve({ status: 'ok', pointer: 0x1000, tries: 1 });
    await task;
    if (!m.connected) problems.push('old STOCK completion disconnected the replacement connection');
    m.close();
  }

  {
    const old = deferred(), m = new MixerDevice(codec);
    activate(m, { stock: () => old.promise }, original);
    m.linkCaps = ['stock'];
    const task = m.stockCheck().catch((e) => e);
    m.close();
    activate(m, {}, replacement);
    old.resolve({ status: 'ok', pointer: 0x41000, tries: 0 });
    if (!((await task) instanceof LinkError)) problems.push('old STOCK check was accepted for the replacement connection');
    m.close();
  }

  {
    const old = deferred(), m = new MixerDevice(codec);
    activate(m, { select: () => old.promise }, original);
    m.select(1, 2);
    m.close();
    activate(m, {}, replacement);
    const selection = { preset: 3, page: 1, until: performance.now() + 500 };
    m.selecting = selection;
    old.reject(new LinkError('old selection failed'));
    await Promise.resolve(); await Promise.resolve();
    if (m.selecting !== selection) problems.push('old selection failure cleared the replacement selection');
    m.close();
  }
  return problems;
}

export async function transferStates(codec) {
  polyfill();
  const problems = [], file = baseFile(codec), original = JSON.stringify(file);
  const changed = JSON.stringify({ ...file, global: { ...(file.global ?? {}), pickup: 'jump' } });
  {
    const wait = deferred(), m = new MixerDevice(codec), states = [];
    activate(m, { edit: () => wait.promise }, original);
    m.addEventListener('storage', () => states.push(!!m.sending));
    m.pending = { doc: changed, what: 'delayed change' };
    const task = m.pushNext();
    if (!m.sending || !states.includes(true)) problems.push('pending transfer was not exposed before acknowledgement');
    if (m.running !== original) problems.push('unacknowledged configuration was marked applied');
    const acknowledged = { ...info(81, 'unsaved'), id: new Uint8Array(16).fill(3) };
    wait.resolve(acknowledged);
    await task;
    if (m.sending || m.pushing || m.running !== changed || m.storage !== 'unsaved' || states.at(-1) !== false) problems.push('completed transfer state did not clear');
    if (m.mixerId !== acknowledged.id) problems.push('confirmed configuration identity was not retained');
    m.close();
  }
  {
    const m = new MixerDevice(codec), states = [];
    activate(m, {}, original);
    m.addEventListener('storage', () => states.push(!!m.sending));
    m.pending = { doc: JSON.stringify({ ...file, name: 'local names only' }), what: 'rename' };
    await m.pushNext();
    if (states.some(Boolean) || m.sending) problems.push('names-only edit was described as a device transfer');
    m.close();
  }
  {
    const m = new MixerDevice(codec);
    activate(m, { edit: async () => { throw new LinkError('model second SET refused after a partial change'); },
                  push: async () => { throw new LinkError('model fallback refused'); } }, original);
    m.pending = { doc: changed, what: 'partial failure' };
    await m.pushNext();
    if (m.connected || m.sending || m.pushing || m.pending) problems.push('failed device transfer kept a stale editable device baseline');
    m.close();
  }
  {
    const m = new MixerDevice(codec);
    activate(m, {}, original);
    m.pending = { doc: '{invalid local JSON', what: 'local invalid file' };
    await m.pushNext();
    if (!m.connected || m.running !== original || m.sending) problems.push('local validation error discarded a known device connection');
    m.close();
  }
  return problems;
}
