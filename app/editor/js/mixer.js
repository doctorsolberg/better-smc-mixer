// The mixer itself, behind the interface the page already uses for the mocked one (js/mock.js;
// docs/integration.md, I-5): the same fields and events, filled from the real link (js/link.js over
// Web MIDI SysEx) and the configuration's binary form (js/cfgbin.js).
//
// What the firmware has today, and so what this device does:
// - connect: Web MIDI with SysEx, the mixer's port found by asking it (GET_INFO), its versions
//   checked (FR-CFG-07: a mixer whose protocol, binary form or schema this editor does not know is
//   refused, saying which side to update), its configuration read;
// - apply: a small change as live edits (CFG_SET, P-1: the mixer keeps playing, its values, page
//   and Shift stay), a large one as the whole configuration (CFG_PUT: the core restarts on it); the
//   latest change wins while one is under way;
// - saving (Q-26): the configuration's own setting "save" says how the mixer saves what it plays:
//   on Save (CFG_SAVE, the default) or by itself 3 s after the last change (Q-22); Go back to the
//   last save is CFG_REVERT;
// - the MIDI the mixer sends shows in the monitor (its performance port, or the Bluetooth port),
//   each message with the control that sent it (P-5: the mixer's SENT records, joined to the port's
//   messages by order and bytes; a message stays "the mixer" when no record names it in time);
// - the panel (P-2, P-3, P-6): the mixer's EVENTs (EVENTS on) say where it stands: its preset,
//   page and Shift (the tabs follow while Linked), each control touched (it is selected, as a
//   click selects it), the storage state, and the running configuration's ID and revision (a
//   change made by another client is noticed); the tabs switch the mixer (CFG_SELECT);
// - the panel's Play mode works on the editor's side as with the mock;
// - firmware (docs/app.md, "Firmware"): a mixer that runs M-Vave's firmware is found by its service
//   SysEx (its name, js/stocklink.js) when no port answers the link, and the editor can install
//   ours over it (installFirmware); ours can point the mixer back at M-Vave's firmware (STOCK:
//   stockCheck, backToStock), which is also the first half of an update.

import { LinkClient, LinkError, ROW_PAGE, webMidiTransport } from './link.js';
import { StockLink, MIXER_NAME, install } from './stocklink.js';
import { requestMixer, openTransport } from './blestock.js';
import { dumps } from './config.js';

const POLL_MS = 1500; // CFG_INFO while connected: the storage state, and that the mixer is there
const QUIET_MS = 3000; // the mixer's pause before it saves (CFGLINK_QUIET_US)
const PROBE_MS = 400;
const SELECT_HOLD_MS = 500; // after a tab switched the mixer, older EVENTs do not switch the tabs back
const MAX_SETS = 8; // a change of more CFG_SETs than this goes as the whole configuration
const SENT_WAIT_MS = 250; // a message from the port waits this long for its SENT entry (P-5)
const SENT_KEEP_MS = 1000; // a SENT entry waits this long for its message (BLE may merge it away)

function withoutNames(v) {
  if (Array.isArray(v)) return v.map(withoutNames);
  if (v && typeof v === 'object') return Object.fromEntries(Object.entries(v).filter(([k]) => k !== 'name').map(([k, x]) => [k, withoutNames(x)]));
  return v;
}
// The same configuration, names, ID and revision aside.
function sameContent(a, b) {
  const strip = (f) => { const { id, revision, ...rest } = withoutNames(f); return rest; };
  return dumps(strip(a)) === dumps(strip(b));
}

export class MixerDevice extends EventTarget {
  static supported() { return typeof navigator !== 'undefined' && 'requestMIDIAccess' in navigator; }
  // Web Bluetooth: M-Vave's firmware over Bluetooth (connectBluetooth()); Chrome on macOS, Windows, Android.
  static bluetooth() { return typeof navigator !== 'undefined' && !!navigator.bluetooth; }

  constructor(codec) {
    super();
    this.real = true;
    this.codec = codec;
    this.connected = false;
    this.transport = 'USB';
    this.info = { firmware: '', buildId: '', protocol: 1, schema: codec.version, binary: codec.binaryVersion,
      presets: codec.device.presets, pages: codec.device.pages, strips: codec.device.strips, buffer: 0, battery: 'USB powered' };
    this.state = { preset: 0, page: 0, shift: false };
    this.fw = { kind: 'ours', version: '' };
    this.stock = null; // M-Vave's firmware's service packets (js/stocklink.js) when it is what runs
    this.bleClose = null; // closes the Bluetooth connection to M-Vave's AE40 service (js/blestock.js)
    this.withSent = false; // the mixer names the control of each message it sends (P-5)
    this.outRows = [];
    this.sentQ = [];
    this.joinTimer = null;
    this.storage = 'clean';
    this.running = null;  // what the mixer plays, as the editor's text
    this.saved = null;    // what it starts with, as far as the editor knows
    this.revision = 0;
    this.lastApply = 0;
    this.logLines = [];
    this.client = null;
    this.ports = [];
    this.pending = null;
    this.pushing = false;
    this.sending = false; // a configuration transfer is not yet confirmed by the mixer
    this.timer = null;
    this.connectionEpoch = 0; // old replies and queued operations never belong to a reconnection
  }

  sameConnection(client, epoch) {
    return !!client && this.connected && this.client === client && this.connectionEpoch === epoch;
  }

  emit(type, detail) { this.dispatchEvent(new CustomEvent(type, { detail })); }

  // The mixer saves by itself: the setting of the configuration it plays (Q-26).
  get autosave() {
    if (this.running !== this.autosaveOf?.[0]) this.autosaveOf = [this.running, this.running !== null && JSON.parse(this.running).global?.save === 'auto'];
    return this.autosaveOf[1];
  }

  log(level, mod, text) {
    const t = (performance.now() / 1000).toFixed(3).padStart(9);
    const line = { t, level, mod, text };
    this.logLines.push(line);
    if (this.logLines.length > 500) this.logLines.shift();
    this.emit('log', line);
  }

  fail(text) {
    this.log('ERROR', 'link', text);
    this.emit('error', { text });
  }

  // Finds the mixer, checks it, reads its configuration. editorText: the editor's configuration
  // (canonical text), to tell whether the mixer plays the same.
  async connect(editorText) {
    if (this.connected) return;
    const epoch = this.connectionEpoch;
    const current = () => this.connectionEpoch === epoch;
    const ensureCurrent = () => { if (!current()) throw new LinkError('the connection attempt was cancelled'); };
    let access;
    try {
      access = await navigator.requestMIDIAccess({ sysex: true });
    } catch (e) {
      throw new LinkError(`the browser gave no MIDI access with SysEx (${e.message ?? e})`);
    }
    ensureCurrent();
    this.access = access;
    const found = await this.find(access, current);
    ensureCurrent();
    if (!found) {
      const stock = await this.findStock(access, current);
      ensureCurrent();
      if (stock) { this.connectStock(access, stock); return; }
      throw new LinkError('no mixer answered on any MIDI port: is it connected?');
    }
    const { input, output, linkInfo } = found;
    this.transport = /config/i.test(output.name) ? 'USB' : 'BLE';
    this.client = new LinkClient(webMidiTransport(input, output), { timeout: this.transport === 'USB' ? 500 : 1500 });
    this.ports = [input];
    try {
      if (!linkInfo.caps.includes('config')) throw new LinkError('this firmware has no configuration protocol');
      const info = await this.client.info();
      ensureCurrent();
      LinkClient.checkCompatible(info, this.codec.binaryVersion, this.codec.version);
      this.info.buildId = linkInfo.buildId;
      this.info.firmware = `better-smc-mixer ${linkInfo.fwVersion || '(no version)'}, build ${linkInfo.buildId}`;
      this.fw = { kind: 'ours', version: linkInfo.fwVersion };
      this.info.buffer = info.maxLength;
      this.info.protocol = info.version;
      this.cfgInfo = info;
      const blob = await this.client.pull(info.chunk);
      ensureCurrent();
      const file = this.codec.binaryToFile(blob);
      this.mixerFile = file;
      this.revision = info.revision;
      this.mixerId = info.id;
      const same = editorText && sameContent(JSON.parse(editorText), file);
      this.running = same ? editorText : dumps(file);
      this.storage = this.storeOf(info);
      this.saved = this.storage === 'clean' ? this.running : null;
      this.log('INFO', 'link', `connected over ${this.transport === 'USB' ? 'USB' : 'Bluetooth'} (${output.name}): firmware ${linkInfo.fwVersion || '(no version)'}, build ${linkInfo.buildId}, configuration protocol ${info.version}, binary form ${info.binaryVersion}, schema ${info.schemaVersion}`);
      this.log('INFO', 'cfg', `the mixer plays: ${info.source}, revision ${info.revision}, ${blob.length} bytes${same ? ', the same as the editor' : ', not the editor\'s'}`);
    } catch (e) {
      if (current()) this.close();
      throw e;
    }
    this.withSent = linkInfo.caps.includes('sent');
    this.linkCaps = linkInfo.caps;
    this.outRows = []; // the port's messages waiting for their SENT entry
    this.sentQ = []; // SENT entries waiting for their message
    if (linkInfo.caps.includes('events')) {
      this.client.onEvent = (e) => { if (current()) this.panel(e.state); };
      this.client.onSent = (s) => { if (current()) this.sentIn(s); };
      try { await this.client.watch(true, this.withSent); ensureCurrent(); }
      catch (e) { if (current()) this.close(); throw e; }
    }
    // EVENTS acknowledges subscription before its first state arrives. Read explicitly so
    // connection readiness never certifies the constructor's default preset/page.
    try {
      const state = await this.client.state();
      ensureCurrent();
      this.panel(state);
      this.panelObservation = { fresh: true, readSequence: (this.panelObservation?.readSequence ?? 0) + 1, connectionEpoch: epoch };
    } catch (e) { if (current()) this.close(); throw e; }
    ensureCurrent();
    this.listenMidi(access, input);
    access.onstatechange = (e) => { if (current() && e.port.state === 'disconnected' && this.ports.includes(e.port)) this.lost('the MIDI port went away'); };
    this.connected = true;
    this.timer = setInterval(() => this.poll(), POLL_MS);
    this.emit('connection');
    this.emit('storage');
  }

  // Asks every input/output pair with the same name, the configuration port first.
  async find(access, current = () => true) {
    const ins = [...access.inputs.values()];
    const pairs = [...access.outputs.values()].map((o) => [ins.find((i) => i.name === o.name), o]).filter(([i]) => i);
    const rank = (n) => (/smc-mixer config/i.test(n) ? 0 : /smc-mixer$/i.test(n) ? 2 : 1);
    pairs.sort((a, b) => rank(a[1].name) - rank(b[1].name));
    for (const [input, output] of pairs) {
      if (!current()) return null;
      if (rank(output.name) === 2) continue; // the performance port does not answer the link
      const client = new LinkClient(webMidiTransport(input, output), { timeout: PROBE_MS, tries: 1 });
      const handler = input.onmidimessage;
      try {
        const linkInfo = await client.linkInfo();
        if (!current()) {
          if (input.onmidimessage === handler) input.onmidimessage = null;
          return null;
        }
        this.log('INFO', 'link', `the mixer answered on ${output.name}`);
        return { input, output, linkInfo };
      } catch {
        if (input.onmidimessage === handler) input.onmidimessage = null;
      } finally {
        client.close();
      }
    }
    return null;
  }

  // M-Vave's firmware: a port pair whose output answers the service SysEx's name request with an
  // SMC-Mixer's ("SMC-Mixer-Private" on the versions seen, so that one is asked first).
  async findStock(access, current = () => true) {
    const ins = [...access.inputs.values()];
    const pairs = [...access.outputs.values()].map((o) => [ins.find((i) => i.name === o.name), o]).filter(([i]) => i);
    pairs.sort((a, b) => Number(!/private/i.test(a[1].name)) - Number(!/private/i.test(b[1].name)));
    for (const [input, output] of pairs) {
      if (!current()) return null;
      const link = new StockLink(stockTransport(input, output), { timeout: PROBE_MS, tries: 1 });
      const handler = input.onmidimessage;
      const name = await link.name();
      if (input.onmidimessage === handler) input.onmidimessage = null;
      if (!current()) return null;
      if (name && MIXER_NAME.test(name)) return { input, output, name };
    }
    return null;
  }

  connectStock(access, { input, output, name }) {
    const epoch = this.connectionEpoch;
    this.ports = [input];
    access.onstatechange = (e) => { if (this.connectionEpoch === epoch && e.port.state === 'disconnected' && this.ports.includes(e.port)) this.lost('the MIDI port went away'); };
    this.useStock(new StockLink(stockTransport(input, output)), name, 'USB', output.name);
  }

  useStock(link, name, transport, where) {
    this.stock = link;
    this.transport = transport;
    this.fw = { kind: 'stock', version: name };
    this.info.firmware = `M-Vave ${name}`;
    this.info.buildId = '';
    this.running = this.saved = null;
    this.log('INFO', 'link', `M-Vave's firmware answered on ${where}: ${name}`);
    this.connected = true;
    this.emit('connection');
  }

  // M-Vave's firmware over Bluetooth: the browser's chooser (call it from a click), then the
  // firmware's own service, AE40 (js/blestock.js; docs/hardware.md, "Two transports"). Ours is
  // reached over Bluetooth MIDI by connect(), as on USB.
  async connectBluetooth() {
    if (this.connected) return;
    const epoch = this.connectionEpoch;
    const current = () => this.connectionEpoch === epoch;
    const device = await requestMixer();
    if (!current()) throw new LinkError('the Bluetooth connection attempt was cancelled');
    let t;
    try {
      t = await openTransport(device, { log: (level, mod, text) => { if (current()) this.log(level, mod, text); } });
    } catch (e) {
      this.emit('source-error', { source: 'bluetooth', name: e.name, message: e.message });
      if (device.gatt?.connected) device.gatt.disconnect();
      // The mixer takes one Bluetooth connection at a time and does not advertise while it has one:
      // connected as a Bluetooth MIDI device (Audio MIDI Setup, an app), it is missing from the
      // browser's list or listed as paired and refused ("Unsupported device", 2026-11-14)
      if (/unsupported device/i.test(e.message)) throw new LinkError(`${device.name || 'this device'} cannot be reached (${e.message}). If the mixer is connected as a Bluetooth MIDI device (Audio MIDI Setup on a Mac, a music app on a phone or tablet), disconnect it there first: it takes one connection at a time. Then Bluetooth again`);
      throw new LinkError(`${device.name || 'this device'} does not offer M-Vave’s service (${e.message}); a mixer with better-smc-mixer connects through Bluetooth MIDI: Connect`);
    }
    if (!current()) { await t.close(); throw new LinkError('the Bluetooth connection attempt was cancelled'); }
    const link = new StockLink(t, { timeout: 3000, ackTimeout: 3000 });
    let name;
    try { name = await link.name(); }
    catch (e) { await t.close(); throw e; }
    if (!current()) { await t.close(); throw new LinkError('the Bluetooth connection attempt was cancelled'); }
    if (!name || !MIXER_NAME.test(name)) {
      await t.close();
      throw new LinkError(name ? `${name}: not an SMC-Mixer` : `${device.name || 'this device'} did not answer`);
    }
    const gone = () => { if (current()) this.lost('the Bluetooth connection ended'); };
    device.addEventListener('gattserverdisconnected', gone, { once: true });
    this.bleClose = () => { device.removeEventListener('gattserverdisconnected', gone); t.close(); };
    this.useStock(link, name, 'BLE', `Bluetooth (${device.name || 'the mixer'}, AE40)`);
  }

  // Ours over M-Vave's (js/stocklink.js install()): app, cfg: the release's app.bin and
  // cfg_tool.bin; hooks as install() takes them. The mixer then needs switching off and on.
  beginFirmwareJob(kind) {
    if (this.firmwareJob?.active) throw new LinkError('a firmware operation is already in progress');
    const job = this.firmwareJob = { kind, active: true, stage: 'preparing', error: null };
    job.cancel = () => {
      if (job.written || job.switching || job.saving || job.installing) return false;
      job.cancelled = true; job.active = false;
      job.error = new LinkError('Cancelled before writing. Nothing was written.');
      return true;
    };
    return job;
  }

  firmwareOwner(owner) {
    if (owner && (owner !== this.firmwareJob || !owner.active || owner.cancelled)) throw new LinkError('the firmware job owner is no longer active');
    if (this.firmwareJob?.active && owner !== this.firmwareJob) throw new LinkError('a firmware operation is already in progress');
  }

  async installFirmware(opts) {
    this.firmwareOwner(opts.job);
    if (!this.connected || !this.stock) throw new LinkError('the mixer does not run M-Vave\'s firmware');
    const job = opts.job || this.beginFirmwareJob('install');
    if (job.installing) throw new LinkError('a firmware installation is already in progress');
    job.installing = true;
    try {
      this.log('INFO', 'link', 'installing better-smc-mixer');
      const r = await install(this.stock, { ...opts, hooks: { ...opts.hooks, log: (t) => { this.log('INFO', 'install', t); opts.hooks?.log?.(t); } } });
      this.log('INFO', 'install', `done: ${r.found === 'earlier' ? 'an earlier installation replaced' : 'installed'}, sector 0 in ${r.sector0Ms} ms`);
      return r;
    } finally { job.installing = false; if (!opts.job) job.active = false; }
  }

  // STOCK (docs/link.md): whether ours can point the mixer back at M-Vave's firmware; nothing is
  // written. { status, pointer, tries }.
  async stockCheck(job) {
    this.firmwareOwner(job);
    const client = this.client, epoch = this.connectionEpoch;
    if (!this.sameConnection(client, epoch) || !this.linkCaps?.includes('stock')) return { status: 'not offered', pointer: 0, tries: 0 };
    const result = await client.stock(false);
    if (!this.sameConnection(client, epoch)) throw new LinkError('the connection changed while checking the firmware');
    return result;
  }

  // STOCK back: sector 0 at M-Vave's area; the mixer restarts on M-Vave's firmware about 100 ms
  // after the reply, so the connection ends here.
  async backToStock(owner) {
    this.firmwareOwner(owner);
    const client = this.client, epoch = this.connectionEpoch;
    if (!this.sameConnection(client, epoch)) throw new LinkError('the mixer is not connected');
    const job = owner || this.beginFirmwareJob('back');
    job.switching = true;
    let r;
    try { r = await client.stock(true); }
    finally { if (!owner) job.active = false; }
    if (!this.sameConnection(client, epoch)) return r;
    this.log(r.status === 'ok' ? 'WARN' : 'ERROR', 'link', `going back to M-Vave's firmware: ${r.status}, sector 0 points at 0x${r.pointer.toString(16)}${r.tries ? `, ${r.tries} write(s)` : ''}`);
    if (r.status === 'ok') this.lost('the mixer restarts with M-Vave\'s firmware');
    return r;
  }

  // The MIDI the mixer sends, for the monitor: its performance port on USB, the same port on BLE.
  listenMidi(access, configInput) {
    const perf = this.transport === 'USB'
      ? [...access.inputs.values()].find((i) => i !== configInput && /smc-mixer$/i.test(i.name)) : null;
    const show = (e) => {
      const d = e.data;
      if (!d.length || d[0] >= 0xF0) return;
      this.outRows.push({ bytes: Array.from(d), t: performance.now(), source: undefined });
      this.joinSent();
    };
    if (perf) {
      perf.onmidimessage = show;
      this.ports.push(perf);
    } else {
      const sysex = configInput.onmidimessage;
      configInput.onmidimessage = (e) => { if (e.data[0] === 0xF0) sysex(e); else show(e); };
    }
  }

  // A SENT record (P-5): its entries wait to be joined to the messages from the port.
  sentIn(s) {
    if (s.lost) this.log('DEBUG', 'link', `${s.lost} sent messages not named (the mixer keeps the last 63)`);
    const now = performance.now();
    for (const e of s.entries) this.sentQ.push({ msg: e.msg, source: e.source, t: now });
    this.joinSent();
  }

  // Each message from the port takes the first SENT entry with the same bytes, in order; a message
  // goes to the monitor once it has its entry or has waited SENT_WAIT_MS (then "the mixer").
  joinSent() {
    const now = performance.now();
    const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
    for (const r of this.outRows) {
      if (r.source !== undefined) continue;
      const i = this.sentQ.findIndex((e) => same(e.msg, r.bytes));
      if (i < 0) continue;
      r.source = this.sentQ[i].source;
      this.sentQ.splice(0, i + 1); // the ones before it went unmatched (merged away)
    }
    this.sentQ = this.sentQ.filter((e) => now - e.t < SENT_KEEP_MS);
    while (this.outRows.length) {
      const r = this.outRows[0];
      if (r.source === undefined && this.withSent && now - r.t < SENT_WAIT_MS) break;
      this.outRows.shift();
      const d = { dir: 'out', bytes: r.bytes, transport: this.transport, who: 'the mixer' };
      const id = r.source ? this.controlId(r.source) : null;
      if (id) Object.assign(d, { id, preset: r.source.preset, page: r.source.page === ROW_PAGE ? 0 : r.source.page, layer: r.source.layer ? 'shift' : 'normal' });
      this.emit('midi', d);
    }
    clearTimeout(this.joinTimer);
    this.joinTimer = this.outRows.length ? setTimeout(() => this.joinSent(), 50) : null;
  }

  storeOf(info) { return info.store === 'none' ? 'clean' : info.store; }

  identityIs(info) {
    const sameId = (a, b) => (a ?? []).length === (b ?? []).length && (a ?? []).every((v, i) => v === b[i]);
    const changed = info.revision !== this.revision || (info.id !== undefined && !sameId(this.mixerId, info.id));
    if (changed) {
      this.running = this.saved = null;
      this.pending = null;
      this.diverged = true;
      this.log('WARN', 'cfg', 'the mixer’s configuration identity changed elsewhere; read it or explicitly send the editor’s copy');
    }
    this.revision = info.revision;
    if (info.id !== undefined) this.mixerId = info.id;
    if (changed) this.emit('changed', { id: this.mixerId, revision: this.revision });
  }

  // The storage state the mixer reports (CFG_INFO, CFG_STATE or an EVENT).
  storageIs(store, generation) {
    store = store === 'none' ? 'clean' : store;
    if (store === this.storage) return;
    if (store === 'clean' && this.storage !== 'clean') {
      this.saved = this.running;
      this.log('INFO', 'cfg', `configuration saved${generation ? `: generation ${generation}` : ''}`);
    }
    if (store === 'failed') this.log('ERROR', 'cfg', 'the mixer could not save (the configuration saved before stays)');
    this.storage = store;
    this.emit('storage');
  }

  // The panel's state (an EVENT, P-2): where the mixer is, what was touched, the storage, and
  // whether its configuration is still the one the editor gave it (P-6).
  panel(st, acceptIdentity = false) {
    const was = this.panelState;
    this.panelState = st;
    const held = this.selecting && performance.now() < this.selecting.until &&
      (st.preset !== this.selecting.preset || st.page !== this.selecting.page);
    if (!held) this.selecting = null;
    if (!held && (st.preset !== this.state.preset || st.page !== this.state.page || st.shift !== this.state.shift)) {
      this.state = { preset: st.preset, page: st.page, shift: st.shift };
      this.emit('state');
    }
    if (was && st.touches !== was.touches && st.touched) {
      const id = this.controlId(st.touched);
      if (id) this.emit('touch', { id, layer: st.touched.layer ? 'shift' : 'normal', from: 'mixer' });
    }
    if (!this.pushing && !acceptIdentity) {
      this.identityIs(st);
      this.storageIs(st.store);
    }
  }

  // The editor's ID of a control by the binary form's address (docs/link.md, "The panel's state").
  controlId({ page, control }) {
    if (page === ROW_PAGE) return this.codec.ROW_KEYS[control] ? `row.${this.codec.ROW_KEYS[control]}` : null;
    const key = this.codec.STRIP_KEYS[Math.floor(control / 8)];
    return key && control % 8 < this.codec.device.strips ? `s${control % 8}.${key}` : null;
  }

  async poll() {
    const client = this.client, epoch = this.connectionEpoch;
    if (!this.sameConnection(client, epoch) || this.pushing) return;
    try {
      const info = await client.info();
      if (this.sameConnection(client, epoch) && !this.pushing && info.state !== 'applying') {
        this.identityIs(info);
        this.storageIs(info.store, info.generation);
      }
    } catch (e) {
      if (this.sameConnection(client, epoch) && e instanceof LinkError) this.lost(e.message);
    }
  }

  lost(why) {
    if (!this.connected) return;
    this.log('WARN', 'link', `connection lost: ${why}`);
    this.close();
    this.emit('connection');
  }

  close() {
    if (this.firmwareJob?.rejectGo) {
      const job = this.firmwareJob;
      job.rejectGo(new LinkError('the connection ended before installation was confirmed'));
      job.go = job.rejectGo = null;
    }
    this.connectionEpoch++;
    this.connected = false;
    this.diverged = false;
    this.pending = null;
    this.pushing = false;
    this.sending = false;
    this.selecting = null;
    if (this.client) { this.client.onEvent = null; this.client.onSent = null; this.client.close?.(); }
    clearTimeout(this.joinTimer);
    this.outRows = [];
    this.sentQ = [];
    this.panelState = null;
    clearInterval(this.timer);
    this.timer = null;
    for (const p of this.ports) p.onmidimessage = null;
    this.ports = [];
    if (this.access) this.access.onstatechange = null;
    this.stock = null;
    this.client = null;
    this.access = null;
    const closeBle = this.bleClose;
    this.bleClose = null;
    if (closeBle) closeBle();
  }

  // The editor applied a change: it goes to the mixer as live edits when they are few, else the whole
  // configuration. While one is under way the newest change waits, and only it.
  apply(doc, what, resolveDivergence = false) {
    if (!this.connected || this.firmwareJob?.active) return;
    if (this.diverged && !resolveDivergence) return;
    if (resolveDivergence) this.diverged = false;
    this.pending = { doc, what };
    if (!this.pushing) this.pushNext();
  }

  async pushNext() {
    const client = this.client, epoch = this.connectionEpoch;
    const active = () => this.sameConnection(client, epoch);
    if (!active()) return;
    this.pushing = true;
    try {
      while (this.pending && active()) {
        const { doc, what } = this.pending;
        this.pending = null;
        let attempted = false;
        try {
          const t = performance.now();
          const next = this.codec.fromJson(JSON.parse(doc));
          let edits = null;
          try { edits = this.codec.edits(this.codec.fromJson(JSON.parse(this.running)), next); } catch { /* the whole configuration */ }
          let info, how;
          if (edits && edits.length === 0) {
            how = 'names only, kept in the editor';
          } else if (edits && edits.length <= MAX_SETS) {
            try {
              this.sending = true;
              this.emit('storage');
              if (!active()) return;
              attempted = true;
              info = await client.edit(edits, (this.revision + 1) >>> 0);
              if (!active()) return;
              how = `${edits.length} live edit${edits.length > 1 ? 's' : ''}, ${edits.reduce((n, [, r]) => n + r.length, 0)} bytes`;
            } catch (e) {
              if (!active()) return;
              if (!(e instanceof LinkError) || /no reply/.test(e.message)) throw e;
              this.log('WARN', 'cfg', `${what}: the live edit was refused (${e.message}); sending the whole configuration`);
            }
          }
          if (!how) {
            // For older firmware only: offer a revision. The running identity comes from CFG_INFO.
            // Keep the running ID when a whole file doesn't supply one.
            next.revision = (this.revision + 1) >>> 0;
            if (!next.id.some((b) => b) && this.mixerId) next.id = Uint8Array.from(this.mixerId);
            const blob = this.codec.encode(next);
            this.sending = true;
            this.emit('storage');
            if (!active()) return;
            attempted = true;
            info = await client.push(blob);
            if (!active()) return;
            how = `${blob.length} bytes, the core restarted`;
          }
          // Keep the exact editor draft for local comparisons, not the device's identity metadata.
          this.running = doc;
          if (info) {
            this.revision = info.revision;
            if (info.id !== undefined) this.mixerId = info.id;
            this.storage = this.storeOf(info);
          }
          this.lastApply = performance.now();
          this.log('INFO', 'cfg', `${what}: applied (${how}) in ${Math.round(performance.now() - t)} ms`);
        } catch (e) {
          if (!active()) return;
          this.fail(`${what}: ${attempted ? 'not confirmed on the mixer' : 'not applied'}: ${e.message}${attempted ? '; reconnect before retrying' : ''}`);
          // A multi-SET change may have applied partly. Do not compute another delta against
          // a stale baseline: reconnect reads what the mixer actually plays before retrying.
          if (attempted) this.lost('configuration transfer failed; reconnect to read the mixer');
        }
        if (active()) this.emit('storage');
      }
    } finally {
      if (active()) { this.pushing = false; this.sending = false; this.emit('storage'); }
    }
  }

  // The mixer's configuration read again (Load from the mixer): its file, without names.
  async pullFile() {
    const client = this.client, epoch = this.connectionEpoch;
    const active = () => this.sameConnection(client, epoch);
    if (!active()) throw new LinkError('the mixer is not connected');
    this.panelObservation = { fresh: false, readSequence: this.panelObservation?.readSequence ?? 0, connectionEpoch: epoch };
    const info = await client.info();
    if (!active()) throw new LinkError('the connection changed while reading the configuration');
    const blob = await client.pull(info.chunk);
    if (!active()) throw new LinkError('the connection changed while reading the configuration');
    const file = this.codec.binaryToFile(blob);
    // A configuration pull is the acceptance baseline/final read boundary. No panel writes:
    // a failed CFG_STATE keeps the prior observation stale and the pull unconfirmed.
    const state = await client.state();
    if (!active()) throw new LinkError('the connection changed while reading the panel');
    // CFG_INFO/CFG_STATE IDs are byte arrays (null for zero); file IDs are UUID strings.
    // Check all three snapshots before publishing the pulled bytes as current or saved.
    const model = this.codec.fromJson(file);
    const id = model.id.some((b) => b) ? model.id : null;
    const sameId = (a, b) => (a ?? []).length === (b ?? []).length && (a ?? []).every((v, i) => v === b[i]);
    if (info.revision !== state.revision || model.revision !== state.revision ||
        !sameId(info.id, state.id) || !sameId(id, state.id)) {
      // The panel reply is newer storage evidence, but none of these bytes is a safe delta baseline.
      this.running = this.saved = null;
      this.pending = null;
      this.diverged = true;
      this.panel(state);
      throw new LinkError('the configuration changed while reading; Load from the mixer again');
    }
    this.panelObservation = { fresh: true, readSequence: (this.panelObservation?.readSequence ?? 0) + 1, connectionEpoch: epoch };
    this.running = dumps(file);
    this.diverged = false;
    this.revision = state.revision;
    this.mixerId = state.id;
    this.panel(state, true);
    // Fresh read evidence, not a save operation: clean means these bytes are already persisted.
    this.cfgInfo = info;
    this.storage = this.storeOf(state);
    this.saved = this.storage === 'clean' ? this.running : null;
    return file;
  }

  // One link exchange at a time: Save and Go back wait for a push under way.
  async exclusive(fn) {
    const client = this.client, epoch = this.connectionEpoch;
    const active = () => this.sameConnection(client, epoch);
    while (this.pushing && active()) await new Promise((r) => setTimeout(r, 20));
    if (!active()) return;
    this.pushing = true;
    try { return await fn(client, active); }
    finally {
      if (active()) { this.pushing = false; if (this.pending) this.pushNext(); }
    }
  }

  // SAVE (Q-26): what the mixer plays becomes what it starts with; the storage state follows.
  async saveNow() {
    const client = this.client, epoch = this.connectionEpoch;
    const active = () => this.sameConnection(client, epoch);
    if (!active()) throw new LinkError('the mixer is not connected');
    if (this.diverged) throw new LinkError('the configuration changed elsewhere; read or send a copy before saving');
    try {
      await this.exclusive(async (owner, current) => {
        await owner.save();
        if (!current()) return;
        this.log('INFO', 'cfg', 'save asked');
        const deadline = performance.now() + 15000;
        while (current()) {
          const info = await owner.info();
          if (!current()) return;
          if (info.revision !== undefined) this.identityIs(info);
          if (this.diverged) throw new LinkError('the configuration changed while saving; read it before continuing');
          this.storageIs(this.storeOf(info), info.generation);
          if (this.storage === 'clean') { this.saved = this.running; break; }
          if (this.storage === 'failed') throw new LinkError('the mixer could not save; the previous saved configuration stays');
          if (performance.now() >= deadline) throw new LinkError('save not confirmed: storage is still ' + this.storage);
          await new Promise((r) => setTimeout(r, 100));
        }
      });
      if (!active()) throw new LinkError('the connection changed before Save was confirmed');
    } catch (e) {
      if (active()) this.fail(/unavailable/.test(e.message) ? 'the mixer cannot save this configuration (a setup\'s, or no flash)' : `save: ${e.message}`);
      throw e;
    }
    if (active()) this.emit('storage');
  }

  // REVERT (Q-26): the mixer plays what it starts with again; resolves with it as the editor's text
  // (the saved copy with its names when the mixer plays the same), null if it failed.
  async revert() {
    const client = this.client, epoch = this.connectionEpoch;
    const active = () => this.sameConnection(client, epoch);
    if (!active()) return null;
    try {
      return (await this.exclusive(async (owner, current) => {
        this.firmwareOwner();
        const info = await owner.revert();
        if (!current()) return null;
        const blob = await owner.pull(info.chunk);
        if (!current()) return null;
        const file = this.codec.binaryToFile(blob);
        const { id, revision } = this.codec.fromJson(file);
        const reportedId = info.id ?? [];
        if (revision !== info.revision || !id.every((v, i) => v === (reportedId[i] ?? 0))) {
          throw new LinkError('the configuration changed while going back; Load from the mixer again');
        }
        const local = this.saved && JSON.parse(this.saved);
        if (local && sameContent(local, file)) {
          // Keep local names and canonical key order, but take the returned identity.
          const { schemaVersion, id: localId, revision: localRevision, ...content } = local;
          this.running = dumps({ schemaVersion, ...(file.id && { id: file.id }),
            ...(file.revision && { revision: file.revision }), ...content });
        } else this.running = dumps(file);
        this.saved = this.running;
        this.revision = info.revision;
        this.mixerId = info.id;
        this.diverged = false;
        this.storage = this.storeOf(info);
        this.log('INFO', 'cfg', `went back to the saved configuration (${info.source}, revision ${info.revision})`);
        this.emit('storage');
        return this.running;
      })) ?? null;
    } catch (e) {
      if (active()) this.fail(`go back: ${e.message}`);
      return null;
    }
  }

  markActivity() {}
  secondsToSave() {
    if (this.storage !== 'waiting') return 0;
    return Math.max(1, Math.ceil((QUIET_MS - (performance.now() - this.lastApply)) / 1000));
  }

  // The panel's Play mode: on the editor's side, as with the mock.
  touch(id, msgs) {
    this.emit('touch', { id });
    for (const m of msgs) this.emit('midi', { dir: 'out', bytes: m, id, transport: 'editor' });
  }
  // A tab switched (Linked): the mixer goes there too (CFG_SELECT, P-3); its EVENT confirms it.
  select(preset, page) {
    if (this.firmwareJob?.active) return;
    this.state.preset = preset; this.state.page = page; this.emit('state');
    if (!this.connected) return;
    this.selecting = { preset, page, until: performance.now() + SELECT_HOLD_MS };
    const client = this.client, epoch = this.connectionEpoch;
    client.select(preset, page).catch((e) => {
      if (!this.sameConnection(client, epoch)) return;
      this.selecting = null;
      this.fail(`the mixer did not switch to preset ${preset + 1} page ${page + 1}: ${e.message}`);
    });
  }
  setShift(on) { this.state.shift = on; this.emit('state'); }
  setFirmware() {}
  setConnected(on, job) {
    if (!on && this.firmwareJob?.active && job !== this.firmwareJob) return;
    if (!on && this.connected) { this.log('INFO', 'link', 'disconnected'); this.close(); this.emit('connection'); }
  }
}

// The stock link's transport over a Web MIDI port pair (js/stocklink.js: send, onmessage).
function stockTransport(input, output) {
  return {
    send: (bytes) => output.send(Array.from(bytes)),
    set onmessage(fn) { input.onmidimessage = (e) => fn(e.data); },
  };
}
