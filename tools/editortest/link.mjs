// The editor's link client (app/editor/js/link.js) against the transcript of the device's own
// link and configuration protocol code (tools/editortest/transcripts.py wrote
// vectors/link-v1.json from build/host/devhost; docs/integration.md, T-2). run.mjs calls these;
// each returns a list of problems (empty: passed).

import * as L from '../../app/editor/js/link.js';

const hex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
const bytes = (h) => Uint8Array.from(h.match(/../g) ?? [], (x) => parseInt(x, 16));

// The frames: the fixed vector of docs/link.md, then every request and reply of the transcript.
export function frames(vector) {
  const out = [];
  const ping = hex(L.command(7, L.CMD.ping, [0x80, 0xFF, 0x00]));
  if (ping !== 'f07d534d18100701007f0051003df7') out.push(`the worked frame: ${ping}`);
  if (L.crc32(new TextEncoder().encode('123456789')) !== 0xCBF43926) out.push('crc32 of "123456789"');
  for (const [i, e] of vector.exchanges.entries()) {
    const cmd = e.cmd.startsWith('0x') ? parseInt(e.cmd, 16) : L.CMD[e.cmd];
    const req = hex(L.command(e.seq, cmd, bytes(e.payload)));
    if (req !== e.request) out.push(`exchange ${i} (${e.cmd}): request ${req}, the device's client ${e.request}`);
    const back = L.record(bytes(e.request));
    if (!back || hex(back) !== hex([L.KIND_CMD, e.seq, cmd, ...bytes(e.payload)])) out.push(`exchange ${i}: the request does not read back`);
    const r = L.decodeReply(bytes(e.reply));
    if (!r || r.seq !== e.reply_seq || r.cmd !== e.reply_cmd || r.status !== e.status || hex(r.payload) !== e.reply_payload) {
      out.push(`exchange ${i} (${e.cmd}): reply read as ${JSON.stringify(r && { ...r, payload: hex(r.payload) })}`);
    }
    if (L.decodeEvent(bytes(e.reply)) !== null) out.push(`exchange ${i}: a reply read as an EVENT`);
    for (const [k, ev] of (e.events ?? []).entries()) {
      const d = L.decodeEvent(bytes(ev.message));
      const rec = L.record(bytes(ev.message));
      if (!d || d.count !== ev.count || hex(rec.slice(2)) !== ev.state) out.push(`exchange ${i} (${e.cmd}): EVENT ${k} read as ${JSON.stringify(d)}`);
      if (L.decodeReply(bytes(ev.message)) !== null) out.push(`exchange ${i}: an EVENT read as a reply`);
    }
    for (const [k, sn] of (e.sent ?? []).entries()) {
      const d = L.decodeSent(bytes(sn.message));
      const raw = (d?.entries ?? []).map((x) => {
        const src = x.source ? [x.source.preset, x.source.page, x.source.control, x.source.layer] : [255, 255, 255, 255];
        return hex([...x.msg, ...Array(3 - x.msg.length).fill(0), ...src]);
      });
      if (!d || d.first !== sn.first || d.lost !== sn.lost || JSON.stringify(raw) !== JSON.stringify(sn.entries)) {
        out.push(`exchange ${i} (${e.cmd}): SENT ${k} read as ${JSON.stringify(d)}`);
      }
      if (L.decodeEvent(bytes(sn.message)) !== null || L.decodeReply(bytes(sn.message)) !== null) out.push(`exchange ${i}: a SENT read as another record`);
    }
  }
  const broken = bytes(vector.exchanges[0].reply);
  broken[6] ^= 1;
  if (L.decodeReply(broken) !== null) out.push('a reply with a wrong CRC is taken');
  if (L.decodeReply(bytes('f07d0102f7')) !== null) out.push('a foreign SysEx is taken');
  return out;
}

// A transport that answers each request with the transcript's reply, in its order.
class Replay {
  constructor(exchanges) { this.ex = exchanges; this.at = 0; this.problems = []; this.drop = 0; this.sent = 0; }
  listen(fn) { this.fn = fn; }
  send(msg) {
    this.sent++;
    const e = this.ex[this.at];
    if (!e) { this.problems.push(`request ${hex(msg)} after the transcript's end`); return; }
    if (hex(msg) !== e.request) { this.problems.push(`request ${this.at} (${e.cmd}): ${hex(msg)}, the transcript ${e.request}`); return; }
    if (this.drop) { this.drop--; return; } // lost on the way: the client must send it again
    this.at++;
    setTimeout(() => {
      this.fn(bytes('f07d0102f7')); // another device's SysEx on the same port: ignored
      this.fn(bytes(e.reply));
      for (const ev of e.events ?? []) this.fn(bytes(ev.message)); // the EVENTs that followed it
      for (const sn of e.sent ?? []) this.fn(bytes(sn.message)); // and the SENT records
    }, 0);
  }
}

// The session of transcripts.py made with the client's own calls: the same bytes in the same
// order, the replies read as the device meant them.
export async function session(vector, testBlobHex) {
  const ex = vector.exchanges;
  const t = new Replay(ex);
  const c = new L.LinkClient(t, { seq: ex[0].seq - 1, timeout: 50 });
  const out = [];
  const expect = (what, got, want) => { if (JSON.stringify(got) !== JSON.stringify(want)) out.push(`${what}: ${JSON.stringify(got)}, want ${JSON.stringify(want)}`); };
  try {
    const before = await c.info();
    expect('cfg-info', [before.version, before.chunk, before.state, before.source, before.id, before.store],
      [L.CFG_VERSION, 128, 'idle', 'factory', null, 'clean']);
    const begin = ex.find((e) => e.cmd === 'cfg-get-begin');
    const pulled = await c.pull();
    const p = bytes(begin.reply_payload);
    expect('pull: length', pulled.length, p[1] | (p[2] << 8) | (p[3] << 16) | (p[4] << 24));
    await c.abort();
    t.drop = 1; // the first put-begin goes unanswered: sent again with the same seq
    const after = await c.push(bytes(testBlobHex));
    expect('push: the state after it', [after.state, after.source, after.revision, after.id && hex(after.id)],
      ['idle', 'received', 1, '0f1e2d3c4b5a49788695a4b3c2d1e0f9']);
    expect('push: received, not saved (manual)', after.store, 'unsaved');
    await c.save();
    const saved = await c.info();
    expect('save: the state after it', [saved.source, saved.revision, saved.store], ['received', 1, 'clean']);
    // CFG_SET with the records transcripts.py sent (Codec.edits() makes the same: edits.json).
    const sets = ex.filter((e) => e.cmd === 'cfg-set').map((e) => bytes(e.payload).slice(5));
    const edited = await c.edit([[0, sets[0]]], 6);
    expect('set: the state after it', [edited.state, edited.source, edited.revision, edited.store], ['idle', 'received', 2, 'unsaved']);
    try {
      await c.set(7, 4, sets[1]);
      out.push('a set naming another preset than its records was taken');
    } catch (e) {
      if (!(e instanceof L.LinkError) || !/cfg-set \(preset 5/.test(e.message)) out.push(`the refused set: ${e.message}`);
    }
    const reverted = await c.revert();
    expect('revert: the state after it', [reverted.state, reverted.source, reverted.revision, reverted.store], ['idle', 'stored', 3, 'clean']);
    // the panel (P-2, P-3, P-6): its state, the events, a selection, touches
    const id = '0f1e2d3c4b5a49788695a4b3c2d1e0f9';
    const s0 = await c.state();
    expect('cfg-state', [s0.preset, s0.page, s0.shift, s0.bluetooth, s0.central, s0.touches, s0.touched, s0.store, s0.revision, s0.id && hex(s0.id)],
      [0, 0, false, 'on', false, 0, null, 'clean', 3, id]);
    const next = await c.sentSince();
    expect('cfg-sent now: none, the next number', [next.first, next.entries.length], [0, 0]);
    const events = [], sent = [];
    c.onEvent = (e) => events.push(e);
    c.onSent = (s) => sent.push(s);
    await c.watch(true, true);
    await c.select(2, 1);
    await c.select(null, 3);
    await c.call(L.CMD.inject, [1]);
    await c.call(L.CMD.inject, [2, 2, 5, 0xFF, 0xFF, 0]); // encoder 6, a click back
    await c.call(L.CMD.inject, [2, 1, 35, 1, 0, 0]); // the row's Rew pressed
    await c.call(L.CMD.inject, [2, 1, 35, 0, 0, 0]);
    const since = await c.sentSince(0);
    await c.call(L.CMD.inject, [0]);
    try {
      await c.select(4, 0);
      out.push('a selection out of range was taken');
    } catch (e) {
      if (!(e instanceof L.LinkError) || !/cfg-select: bad argument/.test(e.message)) out.push(`the refused selection: ${e.message}`);
    }
    await c.watch(false);
    const s1 = await c.state();
    await new Promise((r) => setTimeout(r, 0));
    expect('the EVENTs: counts', events.map((e) => e.count), [0, 1, 2, 3, 4]);
    expect('the EVENTs: preset, page, touches', events.map((e) => [e.state.preset, e.state.page, e.state.touches]),
      [[0, 0, 0], [2, 1, 0], [2, 3, 0], [2, 3, 1], [2, 3, 2]]);
    expect('the EVENTs: the touches', events.slice(3).map((e) => e.state.touched),
      [{ preset: 2, page: 3, control: 13, layer: 0 }, { preset: 2, page: L.ROW_PAGE, control: 3, layer: 0 }]);
    expect('the EVENTs: the rest of the state', events.map((e) => [e.state.store, e.state.revision, hex(e.state.id)]),
      Array(5).fill(['clean', 3, id]));
    expect('cfg-state after them', [s1.preset, s1.page, s1.touches, s1.touched], [2, 3, 2, { preset: 2, page: L.ROW_PAGE, control: 3, layer: 0 }]);
    // P-5: what each one sent, with the control; the row's release sends too, but is no touch
    const enc = { preset: 2, page: 3, control: 13, layer: 0 }, rew = { preset: 2, page: L.ROW_PAGE, control: 3, layer: 0 };
    expect('the SENT records', sent.map((s) => [s.first, s.lost, s.entries.length]), [[0, 0, 1], [1, 0, 1], [2, 0, 1]]);
    expect('the SENT entries: the controls', sent.flatMap((s) => s.entries.map((e) => [e.n, e.source])), [[0, enc], [1, rew], [2, rew]]);
    expect('the SENT entries: the messages', sent.flatMap((s) => s.entries.map((e) => e.msg.length)), [3, 3, 3]);
    expect('cfg-sent from 0: the same', JSON.stringify(since), JSON.stringify({ first: 0, entries: sent.flatMap((s) => s.entries) }));
    const [st] = await c.cfg('cfg-put-chunk', [0, 0, 0, 0, 0]);
    expect('a chunk with no transfer', st, 'no transfer');
    expect('an unknown command', (await c.call(0x7F)).status, 'unknown command');
    expect('the log stream over SysEx', (await c.call(L.CMD.subscribe)).status, 'unavailable');
    // going back to M-Vave's firmware: the check, a wrong magic refused, then back
    expect('stock: the check', await c.stock(), { status: 'ok', pointer: 0x41000, tries: 0 });
    expect('stock: a wrong magic', (await c.call(L.CMD.stock, [1, 0x42, 0x41, 0x43, 0x58])).status, 'bad argument');
    expect('stock: back', await c.stock(true), { status: 'ok', pointer: 0x1000, tries: 1 });
  } catch (e) {
    out.push(`the session stopped: ${e.message}`);
  }
  out.push(...t.problems);
  if (t.at !== ex.length) out.push(`${ex.length - t.at} exchanges of the transcript not made`);
  if (c.foreign < ex.length) out.push(`foreign SysEx counted ${c.foreign}, want at least ${ex.length}`);
  return out;
}

// The blob transcripts.py pushed: its PUT_CHUNK payloads after their offsets.
export function pushedBlob(vector) {
  return vector.exchanges.filter((e) => e.cmd === 'cfg-put-chunk' && e.status === 0 && bytes(e.reply_payload)[0] === 0)
    .map((e) => e.payload.slice(8)).join('');
}

// No reply at all: the client gives up after its tries, each the same frame.
export async function noReply() {
  const sent = [];
  const c = new L.LinkClient({ listen() {}, send: (m) => sent.push(hex(m)) }, { seq: 9, timeout: 10, tries: 3 });
  try {
    await c.call(L.CMD.ping);
    return ['a call with no reply returned'];
  } catch (e) {
    const out = [];
    if (!(e instanceof L.LinkError)) out.push(`threw ${e}`);
    if (sent.length !== 3 || new Set(sent).size !== 1 || L.record(Uint8Array.from(sent[0].match(/../g), (x) => parseInt(x, 16)))[1] !== 10) {
      out.push(`sent ${JSON.stringify(sent)}: want the same seq-10 frame three times`);
    }
    return out;
  }
}

// Closing the UI must stop unsent frames, not only ignore their eventual replies. All transports
// here are in-memory fakes; even the queued Save is a model request, never a physical operation.
export async function closed(codec) {
  globalThis.CustomEvent ??= class extends Event { constructor(t, o) { super(t); this.detail = o?.detail; } };
  const { MixerDevice } = await import('../../app/editor/js/mixer.js');
  const problems = [];
  {
    const sent = [];
    const c = new L.LinkClient({ listen() {}, send: (m) => sent.push(L.record(m)[2]) }, { timeout: 10, tries: 3 });
    const m = new MixerDevice(codec); m.client = c; m.connected = true;
    const first = c.call(L.CMD.ping).catch((e) => e);
    const queued = c.call(L.CMD['cfg-save']).catch((e) => e);
    await Promise.resolve();
    m.close();
    const replies = await Promise.all([first, queued]);
    const afterClose = await c.call(L.CMD.info).catch((e) => e);
    if (sent.length !== 1) problems.push(`closed client sent ${sent.length} frames, want only the first before close`);
    if ([...replies, afterClose].some((e) => !(e instanceof L.LinkError) || !/closed/i.test(e.message))) problems.push('closed calls did not reject as closed');
  }
  {
    let receive;
    const c = new L.LinkClient({ listen(fn) { receive = fn; }, send(msg) {
      const r = L.record(msg);
      receive(L.frame([L.KIND_REPLY, r[1], r[2], 0])); // acknowledged before close
    } });
    const m = new MixerDevice(codec); m.client = c; m.connected = true;
    const task = c.call(L.CMD.ping).catch((e) => e);
    await Promise.resolve();
    m.close();
    if ((await task).status !== 'ok') problems.push('a reply already accepted before close was discarded');
  }
  {
    let receive, held;
    const sent = [];
    const le = (n) => [n & 255, (n >>> 8) & 255, (n >>> 16) & 255, (n >>> 24) & 255];
    const ack = (request, payload) => receive(L.frame([L.KIND_REPLY, request[1], request[2], 0, ...payload]));
    const c = new L.LinkClient({ listen(fn) { receive = fn; }, send(message) {
      const r = L.record(message), cmd = r[2]; sent.push(cmd);
      if (cmd === L.CMD['cfg-put-begin']) queueMicrotask(() => ack(r, [0]));
      else if (cmd === L.CMD['cfg-put-chunk']) {
        const p = Uint8Array.from(r.slice(3));
        const at = new DataView(p.buffer).getUint32(0, true) + p.length - 4;
        if (!held) held = () => ack(r, [0, ...le(at)]);
        else queueMicrotask(() => ack(r, [0, ...le(at)]));
      } else if (cmd === L.CMD['cfg-put-commit']) queueMicrotask(() => ack(r, [0]));
      else if (cmd === L.CMD['cfg-info']) {
        const p = new Uint8Array(37); p[1] = L.CFG_VERSION; p[2] = 1; p[3] = 1; p[6] = 64; p[9] = 128; p[11] = 2; p[32] = 5;
        queueMicrotask(() => ack(r, p));
      } else throw new Error(`unexpected model command ${cmd}`);
    } }, { timeout: 30, tries: 1 });
    const m = new MixerDevice(codec); m.client = c; m.connected = true;
    const task = c.push(codec.fileToBinary({ schemaVersion: codec.version }), 8).catch((e) => e);
    for (let i = 0; i < 64 && !held; i++) await Promise.resolve();
    if (!held) problems.push('model upload never reached its first chunk');
    m.close();
    held?.(); // A native callback could already be queued when the port was detached.
    const result = await task;
    if (!(result instanceof L.LinkError) || !/closed/i.test(result.message)) problems.push('a closed upload continued after its first chunk');
    if (sent.filter((x) => x === L.CMD['cfg-put-chunk']).length !== 1 || sent.includes(L.CMD['cfg-put-commit'])) problems.push('closed upload sent more chunks or a commit');
  }
  {
    let receive, refuse = true;
    const c = new L.LinkClient({ listen(fn) { receive = fn; }, send(msg) {
      if (refuse) { refuse = false; throw new Error('model port refused the write'); }
      const r = L.record(msg);
      queueMicrotask(() => receive(L.frame([L.KIND_REPLY, r[1], r[2], 0])));
    } }, { timeout: 10, tries: 1 });
    const error = await c.call(L.CMD.ping).catch((e) => e);
    if (!(error instanceof L.LinkError)) problems.push('transport send error was not a LinkError');
    if (c.waiting) {
      problems.push('transport send error left a waiting request/timer');
      c.waiting.done(null); c.waiting = null; // legacy-client cleanup for the negative test
    }
    const next = await c.call(L.CMD.info);
    if (next.status !== 'ok') problems.push('send error broke the next queued call');
    c.close?.();
  }
  {
    const input = {}, output = { send() {} };
    const a = new L.LinkClient(L.webMidiTransport(input, output));
    const b = new L.LinkClient(L.webMidiTransport(input, output));
    const replacement = input.onmidimessage;
    a.close?.();
    if (input.onmidimessage !== replacement) problems.push('old transport close detached a replacement handler');
    b.close?.();
    if (input.onmidimessage !== null) problems.push('current transport close did not detach its handler');
  }
  return problems;
}

// FR-CFG-07: a mixer whose protocol, binary form or schema the editor does not know is refused,
// saying which side to update.
export function versions() {
  const out = [];
  const info = { version: L.CFG_VERSION, binaryVersion: 1, schemaVersion: 1 };
  try { L.LinkClient.checkCompatible(info, 1, 1); } catch (e) { out.push(`the same versions refused: ${e.message}`); }
  for (const [what, bad] of [['protocol', { ...info, version: L.CFG_VERSION + 1 }], ['binary form', { ...info, binaryVersion: 2 }], ['schema', { ...info, schemaVersion: 2 }]]) {
    try {
      L.LinkClient.checkCompatible(bad, 1, 1);
      out.push(`another ${what} taken`);
    } catch (e) {
      if (!(e instanceof L.LinkError) || !/update the firmware or the editor|configuration protocol/.test(e.message)) out.push(`another ${what}: ${e.message}`);
    }
  }
  return out;
}

// P-5 in js/mixer.js: the MIDI from the port joined to the mixer's SENT entries by order and bytes,
// whichever comes first; an entry the port never shows (BLE merged it) is passed over; a message no
// entry names goes to the monitor as "the mixer" after the wait, and the ones behind it wait for it.
export async function sentJoin(codec) {
  globalThis.CustomEvent ??= class extends Event { constructor(t, o) { super(t); this.detail = o?.detail; } };
  const { MixerDevice } = await import('../../app/editor/js/mixer.js');
  const m = new MixerDevice(codec);
  const got = [];
  m.addEventListener('midi', (e) => got.push(e.detail));
  m.log = () => {};
  m.withSent = true;
  const port = (b) => { m.outRows.push({ bytes: b, t: performance.now(), source: undefined }); m.joinSent(); };
  const sent = (...entries) => m.sentIn({ lost: 0, entries: entries.map(([msg, preset, page, control, layer]) => ({ msg, source: { preset, page, control, layer } })) });
  port([0xB0, 14, 64]);
  sent([[0xB0, 14, 64], 1, 2, 13, 1]);
  sent([[0x90, 60, 127], 0, 255, 3, 0]);
  port([0x90, 60, 127]);
  sent([[0xB0, 1, 1], 0, 0, 0, 0], [[0xB0, 1, 2], 0, 0, 0, 0]);
  port([0xB0, 1, 2]);
  port([0xC0, 5]);
  sent([[0xB0, 7, 7], 0, 0, 1, 0]);
  port([0xB0, 7, 7]);
  const early = got.length;
  await new Promise((r) => setTimeout(r, 400));
  m.close();
  const out = [];
  if (early !== 3) out.push(`${early} messages went before the unnamed one's wait ended, want 3`);
  const want = [['b0 0e 40', 's5.encoder', 1, 2, 'shift'], ['90 3c 7f', 'row.rew', 0, 0, 'normal'], ['b0 01 02', 's0.fader', 0, 0, 'normal'],
    ['c0 05', 'the mixer', null, null, null], ['b0 07 07', 's1.fader', 0, 0, 'normal']];
  const seen = got.map((d) => [d.bytes.map((x) => x.toString(16).padStart(2, '0')).join(' '), d.id ?? d.who, d.preset ?? null, d.page ?? null, d.layer ?? null]);
  if (JSON.stringify(seen) !== JSON.stringify(want)) out.push(`the monitor got ${JSON.stringify(seen)}, want ${JSON.stringify(want)}`);
  return out;
}
