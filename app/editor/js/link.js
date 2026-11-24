// The editor's link client (docs/link.md, "Over SysEx" and "Configuration"; docs/integration.md,
// T-2, I-3): the command link's records in SysEx frames, and the configuration protocol's calls
// (CFG_INFO, pull, push, abort; SET, SAVE, REVERT since protocol 2; STATE, SELECT and the link's
// EVENTs since 3) over a transport it is given. No browser API in here: a transport
// is { send(bytes), listen(fn) } where fn gets each whole SysEx that arrives (F0 to F7, a
// Uint8Array or an array of numbers); webMidiTransport() makes one from Web MIDI's ports.
// tools/editortest/run.mjs checks it in Node against the transcript of the device's own code
// (tools/editortest/vectors/link-v1.json): the same request bytes, the same replies read.
//
// The same rules as the host's client (tools/smcconf/devlink.py, tools/smclog/smclog.py's
// SysexLink): a call is sent again with the same seq when no reply comes (the device answers a
// repeat without running it again), a busy device is asked again with a new seq for up to 2 s.

export const LINK_VERSION = 1;
export const CFG_VERSION = 4; // the configuration protocol this client speaks (CFGLINK_VERSION)
export const SYSEX_HEADER = [0xF0, 0x7D, 0x53, 0x4D]; // F0, the non-commercial ID 7D, the tag "SM"
export const KIND_CMD = 0x10;
export const KIND_REPLY = 0x11;
export const KIND_EVENT = 0x12; // after EVENTS on: count u8, the panel's state
export const KIND_SENT = 0x13; // after EVENTS' bit 1: first u32, lost u8, count u8, entries (P-5)
export const CMD = {
  ping: 0x01, info: 0x02, subscribe: 0x20, unsubscribe: 0x21, level: 0x22, levels: 0x23, raw: 0x24,
  metrics: 0x25, boot: 0x26, reset: 0x27, events: 0x28, stock: 0x29, inject: 0x30, setup: 0x31,
  'cfg-info': 0x40, 'cfg-get-begin': 0x41, 'cfg-get-chunk': 0x42, 'cfg-put-begin': 0x43,
  'cfg-put-chunk': 0x44, 'cfg-put-commit': 0x45, 'cfg-abort': 0x46, 'cfg-set': 0x47, 'cfg-save': 0x48,
  'cfg-revert': 0x49, 'cfg-state': 0x4A, 'cfg-select': 0x4B, 'cfg-sent': 0x4C,
};
export const LINK_STATUS = ['ok', 'unknown command', 'bad length', 'bad argument', 'unavailable'];
export const CFG_STATUS = ['ok', 'busy', 'no transfer', 'bad offset', 'size', 'incomplete', 'bad CRC', 'invalid', 'unavailable'];
const STATE = ['idle', 'get', 'put', 'applying', 'saving'];
const SOURCE = ['factory', 'test', 'received', 'stored'];
const STORE = ['none', 'clean', 'waiting', 'writing', 'failed', 'unsaved']; // enum cfglink_store
const CFGBIN_ERRORS = ['ok', 'short', 'magic', 'version', 'length', 'crc', 'record', 'tag', 'twice', 'order', 'where',
  'field', 'type', 'value', 'shift', 'space'];
const CAPS = ['log', 'raw', 'inject', 'metrics', 'setup', 'boot', 'reset', 'config', 'events', 'sent', 'stock']; // GET_INFO's bits (link.h, LINK_CAP_*)
const BT = ['off', 'on', 'none'];
// STOCK's status (store/bootsel.h, enum bootsel_status)
export const STOCK_STATUS = ['ok', 'sector 0 does not parse', 'sector 0 does not point at ours', "M-Vave's area does not check",
  'flash', 'busy', 'not offered'];
export const STATE_LEN = 30; // the panel's state (docs/link.md, "The panel's state"; CFGLINK_STATE_LEN)
export const ROW_PAGE = 255; // the binary form's page of the bottom row
export const SENT_LEN = 7; // an entry of what the mixer sent (docs/link.md, "What the mixer sent"; CFGLINK_SENT_LEN)
export const SENT_NOW = 0xFFFFFFFF; // CFG_SENT's from: none, and the next number
export const CHUNK = 128;
const BUSY_WAIT_MS = 2000;

export class LinkError extends Error {}

// --- frames ------------------------------------------------------------------------------------

export function crc16(bytes) { // CRC-16/CCITT-FALSE
  let c = 0xFFFF;
  for (const b of bytes) {
    c ^= b << 8;
    for (let i = 0; i < 8; i++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF;
  }
  return c;
}

let crcTable = null;
export function crc32(bytes) { // zlib's
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let n = 0; n < 256; n++) {
      let c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >>> 1) : c >>> 1;
      crcTable[n] = c >>> 0;
    }
  }
  let c = 0xFFFFFFFF;
  for (const b of bytes) c = crcTable[(c ^ b) & 0xFF] ^ (c >>> 8);
  return (c ^ 0xFFFFFFFF) >>> 0;
}

// Each group of up to 7 bytes: a byte of their top bits (bit i: byte i's), then the bytes with
// their top bits cleared.
export function pack7(bytes) {
  const out = [];
  for (let i = 0; i < bytes.length; i += 7) {
    const g = bytes.slice(i, i + 7);
    let top = 0;
    g.forEach((b, j) => { top |= ((b >> 7) & 1) << j; });
    out.push(top, ...Array.from(g, (b) => b & 0x7F));
  }
  return out;
}

// The reverse; null when it is not valid packing.
export function unpack7(bytes) {
  const out = [];
  for (let i = 0; i < bytes.length; i += 8) {
    const g = bytes.slice(i, i + 8);
    if (g.length < 2 || Array.from(g).some((b) => b > 0x7F)) return null;
    for (let j = 1; j < g.length; j++) out.push(g[j] | (((g[0] >> (j - 1)) & 1) << 7));
  }
  return out;
}

const le16 = (v) => [v & 0xFF, (v >> 8) & 0xFF];
const le32 = (v) => [v & 0xFF, (v >>> 8) & 0xFF, (v >>> 16) & 0xFF, (v >>> 24) & 0xFF];
const u16 = (p, at) => p[at] | (p[at + 1] << 8);
const u32 = (p, at) => (p[at] | (p[at + 1] << 8) | (p[at + 2] << 16) | (p[at + 3] << 24)) >>> 0;

// A record (kind to the end of its payload) as a whole SysEx.
export function frame(record) {
  const r = Array.from(record);
  return Uint8Array.from([...SYSEX_HEADER, ...pack7([...r, ...le16(crc16(r))]), 0xF7]);
}

export function command(seq, cmd, payload = []) {
  return frame([KIND_CMD, seq & 0xFF, cmd, ...payload]);
}

// The record a whole SysEx carries (CRC checked and removed); null when it is not ours or broken.
export function record(msg) {
  const m = Array.from(msg);
  if (m.length < SYSEX_HEADER.length + 1 || SYSEX_HEADER.some((b, i) => m[i] !== b) || m[m.length - 1] !== 0xF7) return null;
  const rec = unpack7(m.slice(SYSEX_HEADER.length, -1));
  if (!rec || rec.length < 5 || crc16(rec.slice(0, -2)) !== u16(rec, rec.length - 2)) return null;
  return rec.slice(0, -2);
}

// A REPLY: { seq, cmd, status (the link's), payload }; null for anything else.
export function decodeReply(msg) {
  const rec = record(msg);
  if (!rec || rec[0] !== KIND_REPLY || rec.length < 4) return null;
  return { seq: rec[1], cmd: rec[2], status: rec[3], payload: rec.slice(4) };
}

// An EVENT: { count, state (panelState()'s) }; null for anything else.
export function decodeEvent(msg) {
  const rec = record(msg);
  if (!rec || rec[0] !== KIND_EVENT || rec.length < 2 + STATE_LEN) return null;
  return { count: rec[1], state: panelState(rec.slice(2)) };
}

// A SENT record (P-5): { first, lost, entries (sentEntries()') }; null for anything else.
export function decodeSent(msg) {
  const rec = record(msg);
  if (!rec || rec[0] !== KIND_SENT || rec.length < 7 || rec.length !== 7 + SENT_LEN * rec[6]) return null;
  const first = u32(rec, 1);
  return { first, lost: rec[5], entries: sentEntries(rec.slice(7), first) };
}

// What the mixer sent: entries of SENT_LEN bytes as { n, msg (its bytes), source } where source is
// the control that sent it in the binary form's address ({ preset, page, control, layer }; page
// ROW_PAGE: the bottom row), null when no control did (MIDI in, the time, a selection).
export function sentEntries(p, first) {
  const out = [];
  for (let i = 0; i + SENT_LEN <= p.length; i += SENT_LEN) {
    const e = p.slice(i, i + SENT_LEN);
    const len = (e[0] & 0xF0) === 0xC0 || (e[0] & 0xF0) === 0xD0 ? 2 : 3;
    out.push({ n: (first + i / SENT_LEN) >>> 0, msg: Array.from(e.slice(0, len)),
      source: e[5] === 255 ? null : { preset: e[3], page: e[4], control: e[5], layer: e[6] } });
  }
  return out;
}

// --- the configuration protocol's replies ------------------------------------------------------

// The panel's state (CFG_STATE's reply after its status, an EVENT's after its count): the preset and
// the page 0-based; the last touch in the binary form's address (page ROW_PAGE: the bottom row),
// null before the first.
export function panelState(p) {
  if (p.length < STATE_LEN) throw new LinkError(`the panel's state: ${p.length} bytes`);
  const id = Array.from(p.slice(14, 30));
  return {
    preset: p[0], page: p[1], shift: p[2] !== 0, bluetooth: BT[p[3] & 0x7F] ?? String(p[3] & 0x7F), central: (p[3] & 0x80) !== 0,
    touches: p[4], touched: p[5] === 255 ? null : { preset: p[5], page: p[6], control: p[7], layer: p[8] },
    store: STORE[p[9]] ?? String(p[9]), revision: u32(p, 10), id: id.every((b) => b === 0) ? null : id,
  };
}

function sourceName(v) {
  return (SOURCE[v & 0x7F] ?? String(v & 0x7F)) + (v & 0x80 ? ' with settings' : '');
}

// CFG_INFO's fields (after the configuration status byte).
export function cfgInfo(p) {
  if (p.length < 31) throw new LinkError(`cfg-info: ${p.length} bytes`);
  const id = Array.from(p.slice(15, 31));
  return {
    version: p[0], binaryVersion: p[1], schemaVersion: u16(p, 2), maxLength: u32(p, 4), chunk: p[8],
    state: STATE[p[9]] ?? String(p[9]), source: sourceName(p[10]), revision: u32(p, 11),
    id: id.every((b) => b === 0) ? null : id,
    store: p.length >= 36 ? (STORE[p[31]] ?? String(p[31])) : 'none', generation: p.length >= 36 ? u32(p, 32) : 0,
  };
}

// cfgbin's error after an INVALID status.
export function errorText(p) {
  if (p.length < 9) return 'not valid';
  const [code, preset, page, control, field] = p;
  const where = [`byte ${u32(p, 5)}`];
  if (preset !== 255) where.push(`preset ${preset + 1}`);
  if (page !== 255) where.push(`page ${page + 1}`);
  if (control !== 255) where.push(`control ${control}`);
  if (field !== 255) where.push(`field ${field}`);
  return `${CFGBIN_ERRORS[code] ?? code} (${where.join(', ')})`;
}

// --- the client --------------------------------------------------------------------------------

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

export class LinkClient {
  // transport: { send(bytes), listen(fn), close?() }. close detaches only this transport's listener.
  // seq: the last seq used (the next call takes seq + 1).
  constructor(transport, { timeout = 1000, tries = 3, seq = Math.floor(Math.random() * 256) } = {}) {
    this.transport = transport;
    this.timeout = timeout;
    this.tries = tries;
    this.seq = seq & 0xFF;
    this.foreign = 0; // SysEx that was not a reply of ours
    this.onEvent = null; // fn({ count, state }) for each EVENT (EVENTS on: watch())
    this.onSent = null; // fn({ first, lost, entries }) for each SENT record (watch(on, true))
    this.waiting = null;
    this.closed = false;
    this.queue = Promise.resolve(); // one call at a time
    transport.listen((msg) => this.receive(msg));
  }

  close() {
    if (this.closed) return;
    this.closed = true;
    this.onEvent = this.onSent = null;
    const waiting = this.waiting;
    this.waiting = null;
    if (waiting) waiting.done(null); // clear its timer and let the caller reject as closed
    try { this.transport.close?.(); } catch { /* the port may already be gone */ }
  }

  receive(msg) {
    if (this.closed) return;
    const e = decodeEvent(msg);
    if (e) { if (this.onEvent) this.onEvent(e); return; }
    const s = decodeSent(msg);
    if (s) { if (this.onSent) this.onSent(s); return; }
    const r = decodeReply(msg);
    const w = this.waiting;
    if (!r || !w || r.seq !== w.seq || r.cmd !== w.cmd) { this.foreign++; return; }
    this.waiting = null;
    w.done(r);
  }

  // One command: { status (the link's name), payload }. The same frame again after each timeout.
  call(cmd, payload = []) {
    const run = async () => {
      if (this.closed) throw new LinkError('the link connection is closed');
      this.seq = (this.seq + 1) & 0xFF;
      const seq = this.seq;
      const msg = command(seq, cmd, payload);
      for (let t = 0; t < this.tries; t++) {
        if (this.closed) throw new LinkError('the link connection is closed');
        const r = await new Promise((resolve, reject) => {
          let timer;
          const waiting = { seq, cmd, done: (x) => { clearTimeout(timer); resolve(x); } };
          timer = setTimeout(() => { if (this.waiting === waiting) this.waiting = null; resolve(null); }, this.timeout);
          this.waiting = waiting;
          try { this.transport.send(msg); }
          catch (e) {
            clearTimeout(timer);
            if (this.waiting === waiting) this.waiting = null;
            reject(e instanceof LinkError ? e : new LinkError(`could not send command: ${e.message ?? e}`));
          }
        });
        if (r) return { status: LINK_STATUS[r.status] ?? String(r.status), payload: r.payload };
        if (this.closed) throw new LinkError('the link connection is closed');
      }
      const name = Object.keys(CMD).find((k) => CMD[k] === cmd) ?? cmd;
      throw new LinkError(`no reply to ${name} after ${this.tries} tries`);
    };
    const p = this.queue.then(run, run);
    this.queue = p.catch(() => {});
    return p;
  }

  // A configuration command: [the configuration status's name, the payload after it]. A busy
  // device is asked again (a new seq) for up to 2 s.
  async cfg(name, payload = []) {
    const giveUp = Date.now() + BUSY_WAIT_MS;
    for (;;) {
      const r = await this.call(CMD[name], payload);
      if (r.status !== 'ok') throw new LinkError(`${name}: ${r.status}`);
      if (!r.payload.length) throw new LinkError(`${name}: an empty reply`);
      const status = CFG_STATUS[r.payload[0]] ?? String(r.payload[0]);
      if (status !== 'busy' || Date.now() > giveUp) return [status, r.payload.slice(1)];
      await sleep(20);
    }
  }

  // GET_INFO: { version, variant (0 the application, 1 the hardware test), caps: [...], buildId (hex),
  // fwVersion (the product's version after the build ID; '' from a firmware before it) }.
  async linkInfo() {
    const r = await this.call(CMD.info);
    const p = r.payload;
    if (r.status !== 'ok' || p.length < 16) throw new LinkError(`info: ${r.status}`);
    const caps = u32(p, 3);
    const at = 16 + p[15];
    const fwVersion = p.length > at ? String.fromCharCode(...p.slice(at + 1, at + 1 + p[at])) : '';
    return { version: p[0], variant: p[2], caps: CAPS.filter((_, i) => caps & (1 << i)),
      buildId: Array.from(p.slice(16, at), (b) => b.toString(16).padStart(2, '0')).join(''), fwVersion };
  }

  async info() {
    const [status, p] = await this.cfg('cfg-info');
    if (status !== 'ok') throw new LinkError(`cfg-info: ${status}`);
    return cfgInfo(p);
  }

  // FR-CFG-07: a device that speaks another protocol, binary form or schema is refused.
  static checkCompatible(info, binaryVersion, schemaVersion) {
    if (info.version !== CFG_VERSION) throw new LinkError(`the mixer speaks configuration protocol ${info.version}, the editor ${CFG_VERSION}`);
    if (info.binaryVersion !== binaryVersion || info.schemaVersion !== schemaVersion) {
      throw new LinkError(`the mixer takes binary form ${info.binaryVersion}, schema ${info.schemaVersion}; the editor writes ${binaryVersion}, ${schemaVersion}: update the firmware or the editor`);
    }
  }

  // The running configuration's binary form (a snapshot), checked by its CRC-32.
  async pull(chunk = CHUNK) {
    const [status, p] = await this.cfg('cfg-get-begin');
    if (status !== 'ok') throw new LinkError(`cfg-get-begin: ${status === 'invalid' ? errorText(p) : status}`);
    const length = u32(p, 0); const crc = u32(p, 4);
    const out = [];
    while (out.length < length) {
      const [st, q] = await this.cfg('cfg-get-chunk', [...le32(out.length), Math.min(chunk, 255)]);
      if (st !== 'ok') throw new LinkError(`cfg-get-chunk at ${out.length}: ${st}`);
      const data = q.slice(4);
      if (u32(q, 0) !== out.length || !data.length) throw new LinkError(`cfg-get-chunk at ${out.length}: ${data.length} bytes at ${u32(q, 0)}`);
      out.push(...data);
    }
    if (out.length !== length || crc32(out) !== crc) throw new LinkError(`the configuration read does not check (${out.length} bytes, want ${length})`);
    return Uint8Array.from(out);
  }

  // Uploads blob and commits it; returns CFG_INFO once the mixer has applied it. Nothing changes
  // on the mixer unless every check passes there.
  async push(blob, chunk = CHUNK) {
    const b = Array.from(blob);
    let [status] = await this.cfg('cfg-put-begin', [...le32(b.length), ...le32(crc32(b))]);
    if (status !== 'ok') throw new LinkError(`cfg-put-begin (${b.length} bytes): ${status}`);
    let at = 0;
    while (at < b.length) {
      const [st, p] = await this.cfg('cfg-put-chunk', [...le32(at), ...b.slice(at, at + chunk)]);
      const got = p.length >= 4 ? u32(p, 0) : null;
      if (st === 'bad offset' && got !== null && got <= b.length) { at = got; continue; } // a reply lost on the way
      if (st !== 'ok') { await this.abort(); throw new LinkError(`cfg-put-chunk at ${at}: ${st}`); }
      at = got;
    }
    let p;
    [status, p] = await this.cfg('cfg-put-commit');
    if (status !== 'ok') throw new LinkError(`cfg-put-commit: ${status === 'invalid' ? errorText(p) : status}`);
    return this.applied('the configuration');
  }

  async abort() {
    try { await this.cfg('cfg-abort'); } catch (e) { if (!(e instanceof LinkError)) throw e; }
  }

  // CFG_INFO until the mixer has taken a change (the state is no longer "applying").
  async applied(what, ready = () => true) {
    const giveUp = Date.now() + BUSY_WAIT_MS;
    for (;;) {
      const info = await this.info();
      if (info.state !== 'applying' && ready(info)) return info;
      if (Date.now() > giveUp) throw new LinkError(`the mixer has not applied ${what} within 2 s`);
      await sleep(20);
    }
  }

  // CFG_SET (P-1): records of the binary form for one preset (Codec.edits()), applied by the mixer
  // without a restart. The revision field is kept for older firmware. Resolves once checked;
  // the mixer applies it in its next round.
  async set(revision, preset, records) {
    const [status, p] = await this.cfg('cfg-set', [...le32(revision), preset, ...records]);
    if (status !== 'ok') throw new LinkError(`cfg-set (preset ${preset + 1}, ${records.length} bytes): ${status === 'invalid' ? errorText(p) : status}`);
  }

  // Every edit in order; CFG_INFO once the last is applied.
  async edit(edits, revision) {
    if (edits.length === 0) return this.info();
    for (const [preset, records] of edits) await this.set(revision, preset, records);
    // Older firmware could report idle with zero metadata while SET was still pending.
    return this.applied('the edit', ({ source }) => ['received', 'test', 'test with settings'].includes(source));
  }

  // CFG_SAVE (Q-26): the running configuration saved now; resolves when the mixer has taken the
  // command (the save itself follows: CFG_INFO's storage). LinkError "unavailable": no flash, or a
  // setup's configuration.
  async save() {
    const [status] = await this.cfg('cfg-save');
    if (status !== 'ok') throw new LinkError(`cfg-save: ${status}`);
  }

  // CFG_REVERT (Q-26): what the mixer starts with replaces what it plays; CFG_INFO once applied.
  async revert() {
    const [status] = await this.cfg('cfg-revert');
    if (status !== 'ok') throw new LinkError(`cfg-revert: ${status}`);
    return this.applied('the revert');
  }

  // CFG_STATE (P-2, P-6): the panel's state (panelState()).
  async state() {
    const [status, p] = await this.cfg('cfg-state');
    if (status !== 'ok') throw new LinkError(`cfg-state: ${status}`);
    return panelState(p);
  }

  // CFG_SELECT (P-3): the mixer switches to preset and page (0-based; null keeps one, a preset
  // alone goes to its last page) in its next round; its EVENT then shows it.
  async select(preset, page) {
    const [status] = await this.cfg('cfg-select', [preset ?? 0xFF, page ?? 0xFF]);
    if (status !== 'ok') throw new LinkError(`cfg-select: ${status}`);
  }

  // EVENTS (P-2, P-5): the panel's EVENTs on or off (each to onEvent, the first at once), and what
  // the mixer sent (each SENT record to onSent).
  async watch(on = true, sent = false) {
    const what = (on ? 1 : 0) | (sent ? 2 : 0);
    const r = await this.call(CMD.events, [what]);
    if (r.status !== 'ok') throw new LinkError(`events ${what}: ${r.status}`);
  }

  // STOCK (docs/link.md): going back to M-Vave's firmware. back false: the check alone, nothing
  // written; true: sector 0 pointed at M-Vave's area, and the mixer restarts on it about 100 ms
  // after the reply when that held. { status (STOCK_STATUS's name), pointer, tries }.
  async stock(back = false) {
    const r = await this.call(CMD.stock, back ? [1, 0x42, 0x41, 0x43, 0x4B] : [0]); // "BACK"
    if (r.status !== 'ok') throw new LinkError(`stock: ${r.status}`);
    const p = r.payload;
    if (p.length !== 6) throw new LinkError(`stock: a reply of ${p.length} bytes`);
    return { status: STOCK_STATUS[p[0]] ?? String(p[0]), pointer: u32(p, 1), tries: p[5] };
  }

  // CFG_SENT (P-5): what the mixer sent from that number on, at most 16: { first, entries }; first
  // is the next number when there is none.
  async sentSince(number = SENT_NOW) {
    const [status, p] = await this.cfg('cfg-sent', [number & 0xFF, (number >>> 8) & 0xFF, (number >>> 16) & 0xFF, (number >>> 24) & 0xFF]);
    if (status !== 'ok') throw new LinkError(`cfg-sent: ${status}`);
    if (p.length < 5 || p.length !== 5 + SENT_LEN * p[4]) throw new LinkError(`cfg-sent: a reply of ${p.length} bytes`);
    const first = u32(p, 0);
    return { first, entries: sentEntries(p.slice(5), first) };
  }
}

// A transport over Web MIDI's ports (the "SMC-Mixer Config" input and output).
export function webMidiTransport(input, output) {
  let handler;
  return {
    send: (bytes) => output.send(Array.from(bytes)),
    listen: (fn) => { handler = (e) => { if (e.data[0] === 0xF0) fn(e.data); }; input.onmidimessage = handler; },
    close: () => { if (input.onmidimessage === handler) input.onmidimessage = null; },
  };
}
