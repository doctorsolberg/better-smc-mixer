// A mocked device link. It stands in for what the editor will get from the firmware (SPEC §7,
// docs/link.md, "Configuration"): an applied change, the saving model of docs/app.md, "Saving and
// trying" (A-12: what the mixer plays in RAM, what it starts with in flash, Save, Go back, and the
// autosave 3 s after the last change and touch, SPEC Q-22, when the configuration's setting "save"
// is auto (Q-26; manual by default)), the "control touched"
// event and the page / preset / Shift state (FR-CFG-06), the MIDI the device sends, and its log.
// Nothing here talks to hardware.

const QUIET_MS = 3000;

export class MockDevice extends EventTarget {
  constructor() {
    super();
    this.connected = true;
    this.transport = 'USB';
    this.info = {
      firmware: 'controller 20 (mock)', buildId: '5e1f0c2a', protocol: 4, schema: 1, binary: 1,
      presets: 4, pages: 4, strips: 8, buffer: 16384, battery: 'USB powered',
    };
    this.state = { preset: 0, page: 0, shift: false };
    // What runs on the mixer (docs/app.md, "Firmware"): ours with its version, M-Vave's own
    // ("stock", any version, which answers its service SysEx with its name), or the bootloader
    // waiting for an image after an interrupted update.
    this.fw = { kind: 'ours', version: '0.0.1' }; // an older one than the editor's: the update card shows
    this.storage = 'clean';      // clean | unsaved | waiting | writing | failed
    this.running = null;         // the configuration the mixer plays (RAM), as canonical text
    this.saved = null;           // the configuration it starts with (flash)
    this.generation = 5;
    this.revision = 12;
    this.lastActivity = 0;
    this.saveTimer = null;
    this.logLines = [];
    this.log('INFO', 'cfg', 'configuration loaded: flash slot 1, generation 5');
    this.log('INFO', 'core', 'core start: preset 1 page 1');
  }

  emit(type, detail) { this.dispatchEvent(new CustomEvent(type, { detail })); }

  // The mixer saves by itself: the setting "save" of the configuration it plays (Q-26).
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

  // The editor applied a change: the device takes it into RAM at once ("change it, try it"). The
  // flash changes only on Save, or by itself when the autosave is on.
  apply(doc, what) {
    if (!this.connected || this.fw.kind !== 'ours') return;
    this.running = doc;
    this.revision++;
    this.log('INFO', 'cfg', `${what}: applied, revision ${this.revision} (${doc.length} bytes)`);
    if (this.autosave) { if (this.storage !== 'writing') this.storage = this.running === this.saved ? 'clean' : 'waiting'; this.markActivity(); return; }
    if (this.storage === 'waiting') clearTimeout(this.saveTimer); // the setting turned to manual
    if (this.storage !== 'writing') this.storage = this.running === this.saved ? 'clean' : 'unsaved';
    this.emit('storage');
  }

  // SAVE: what the mixer plays becomes what it starts with.
  saveNow() {
    if (this.running === this.saved) return;
    clearTimeout(this.saveTimer);
    this.storage = 'waiting';
    this.save();
  }

  // REVERT: the mixer plays what it starts with again. The editor takes the same copy.
  revert() {
    if (this.running === this.saved) return null;
    clearTimeout(this.saveTimer);
    this.running = this.saved;
    this.revision++;
    this.storage = 'clean';
    this.log('INFO', 'cfg', `reverted to the saved configuration, revision ${this.revision}`);
    this.emit('storage');
    return this.saved;
  }

  // A change or a touch restarts the quiet pause; the save runs once it has passed.
  markActivity() {
    this.lastActivity = performance.now();
    clearTimeout(this.saveTimer);
    if (this.storage === 'writing') { this.storage = 'waiting'; this.log('INFO', 'cfg', 'save paused: the panel is in use'); }
    if (this.storage === 'waiting' || this.storage === 'writing') this.saveTimer = setTimeout(() => this.save(), QUIET_MS);
    this.emit('storage');
  }

  save() {
    if (this.storage !== 'waiting') return;
    this.storage = 'writing';
    this.log('INFO', 'cfg', 'save begun');
    this.emit('storage');
    this.saveTimer = setTimeout(() => {
      this.saved = this.running;
      this.generation++;
      this.storage = 'clean';
      this.log('INFO', 'cfg', `configuration saved: flash slot ${this.generation % 2 + 1}, generation ${this.generation}`);
      this.emit('storage');
    }, 700);
  }

  secondsToSave() {
    if (this.storage !== 'waiting') return 0;
    return Math.max(0, Math.ceil((QUIET_MS - (performance.now() - this.lastActivity)) / 1000));
  }

  // The panel was used (the simulated panel): the device reports the touch and the MIDI it sent.
  touch(id, msgs) {
    this.emit('touch', { id });
    for (const m of msgs) this.emit('midi', { dir: 'out', bytes: m, id, transport: this.transport });
    this.markActivity();
  }

  select(preset, page) {
    this.state.preset = preset;
    this.state.page = page;
    this.log('INFO', 'core', `preset ${preset + 1} page ${page + 1}`);
    this.emit('state');
  }

  setShift(on) {
    this.state.shift = on;
    this.log('DEBUG', 'core', `shift ${on ? 1 : 0}`);
    this.emit('state');
  }

  setFirmware(fw) {
    this.fw = fw;
    this.log('INFO', 'link', `firmware: ${fw.kind}${fw.version ? ` ${fw.version}` : ''}`);
    this.emit('connection');
  }

  setConnected(on, transport) {
    this.connected = on;
    if (transport) this.transport = transport;
    this.emit('connection');
  }
}
