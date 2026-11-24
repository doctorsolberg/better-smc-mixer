// Installing over M-Vave's firmware from the browser (SPEC Q-18: the editor installs, the owner,
// 2026-11-14). It speaks the stock firmware's service SysEx (docs/hardware.md, "Stock service
// protocol"): 0x11 its name, 0x23 read, 0x21 erase a 4 KB sector, 0x22 program up to 256 bytes,
// as tools/smcmidi/main.swift does on the bench (docs/bringup.md, "Step 2b" and "Step 3"), with the
// same guards:
// - nothing is written before the plan (js/installplan.js) holds on a backup the user has saved;
// - erase and write reach only our sectors and, once, sector 0; never the stock area and its VM,
//   the update loader, BTIF, EXIF or USRFLASH;
// - the stock firmware acknowledges every erase and write alike, so every one is checked by a read;
// - our area goes first and is checked whole; sector 0 (about 0.1 s without a uboot) last.
// ⚠️ Not run on the mixer from a browser yet; the same steps ran through smcmidi (2026-11-10).
import * as F from './flashimg.js';
import * as IP from './installplan.js';

export const CMD = { name: 0x11, erase: 0x21, write: 0x22, read: 0x23 };
const TYPE_FLASH = 0;
const BLOCK = 256; // a read or a write: one 256-byte page
// No list of M-Vave's versions: any firmware that answers as an SMC-Mixer is taken, and the plan
// on its backup decides (sector 0 and M-Vave's area parse and check, our sectors free or ours,
// the regions it keeps outside ours): M-Vave updates its firmware (docs/app.md, "Firmware").
export const MIXER_NAME = /^SMC-Mixer/;
// Never erased or written here: the stock area and its VM, the update loader, BTIF, EXIF, USRFLASH.
// Sector 0 only through writeSector0().
const FORBIDDEN = [[0x00000, 0x01000], [0x01000, 0x25000], [0x6E000, 0x7B000], [0x7B000, 0x7F000]];

export class StockError extends Error {}

// --- packets ---------------------------------------------------------------------------------------

export function packet(cmd, payload) {
  const n = payload.length;
  let sum = 0;
  for (const b of payload) sum = (sum + b) & 0xFF;
  return Uint8Array.from([0x00, 0x59, cmd, n & 0xFF, (n >> 8) & 0xFF, (n >> 16) & 0xFF, ...payload, ~sum & 0xFF]);
}

// 8-bit bytes bit-packed LSB first into 7-bit bytes, in F0 ... F7.
export function pack7(data) {
  const out = [0xF0];
  let acc = 0, bits = 0;
  for (const b of data) {
    acc |= b << bits; bits += 8;
    while (bits >= 7) { out.push(acc & 0x7F); acc >>= 7; bits -= 7; }
  }
  if (bits > 0) out.push(acc & 0x7F);
  out.push(0xF7);
  return Uint8Array.from(out);
}

export function unpack7(sysex) {
  const body = sysex[0] === 0xF0 ? sysex.subarray(1, sysex.length - 1) : sysex;
  const out = [];
  let acc = 0, bits = 0;
  for (const b of body) {
    acc |= (b & 0x7F) << bits; bits += 7;
    while (bits >= 8) { out.push(acc & 0xFF); acc >>= 8; bits -= 8; }
  }
  return Uint8Array.from(out);
}

// { cmd, payload, ok } of a packet, or null when it is not one.
export function parse(pkt) {
  if (pkt.length < 7 || pkt[0] !== 0x00 || pkt[1] !== 0x59) return null;
  const n = pkt[3] | (pkt[4] << 8) | (pkt[5] << 16);
  const payload = pkt.subarray(6, Math.min(6 + n, pkt.length));
  let sum = 0;
  for (const b of payload) sum = (sum + b) & 0xFF;
  return { cmd: pkt[2], payload: Uint8Array.from(payload), ok: pkt.length > 6 + n && pkt[6 + n] === (~sum & 0xFF) };
}

const le = (v, n) => Array.from({ length: n }, (_, i) => (v >>> (8 * i)) & 0xFF);
const isAck = (r) => r && r.cmd === 0 && r.payload.length === 1 && r.payload[0] === 0 && r.ok;
const forbidden = (lo, hi) => FORBIDDEN.some(([a, b]) => lo < b && a < hi);
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// --- the link --------------------------------------------------------------------------------------

// transport: { send(Uint8Array), onmessage: (Uint8Array) => void, raw? }: a SysEx message each
// (USB MIDI), or with raw set whole packets as they are (BLE: M-Vave's AE40 service, js/blestock.js;
// docs/hardware.md, "Two transports").
export class StockLink {
  constructor(transport, { timeout = 1000, ackTimeout = 3000, tries = 3 } = {}) {
    Object.assign(this, { transport, timeout, ackTimeout, tries, waiting: null, sent: 0 });
    transport.onmessage = (m) => {
      if (!this.waiting || (!transport.raw && m[0] !== 0xF0)) return;
      const r = parse(transport.raw ? m : unpack7(m));
      if (r) { const w = this.waiting; this.waiting = null; w(r); }
    };
  }

  // One packet out, the first reply back, or null after ms.
  request(pkt, ms) {
    return new Promise((resolve) => {
      const t = setTimeout(() => { this.waiting = null; resolve(null); }, ms);
      this.waiting = (r) => { clearTimeout(t); resolve(r); };
      this.sent++;
      this.transport.send(this.transport.raw ? pkt : pack7(pkt));
    });
  }

  async name() {
    const r = await this.request(packet(CMD.name, []), this.timeout);
    if (!r || !r.ok) return null;
    return String.fromCharCode(...r.payload.filter((c) => c >= 0x20 && c < 0x7F));
  }

  async read(addr, len = BLOCK) {
    const payload = [TYPE_FLASH, ...le(addr, 4), ...le(len, 3)];
    for (let i = 0; i < this.tries; i++) {
      const r = await this.request(packet(CMD.read, payload), this.timeout);
      if (!r) continue;
      if (r.cmd !== CMD.read) throw new StockError(`read ${F.hx(addr)}: refused (command ${F.hx(r.cmd)})`);
      const p = r.payload;
      if (r.ok && p.length === 8 + len && p[0] === TYPE_FLASH && F.u32(p, 1) === addr && (p[5] | (p[6] << 8) | (p[7] << 16)) === len) return p.subarray(8);
    }
    throw new StockError(`read ${F.hx(addr)}: no good reply after ${this.tries} tries`);
  }

  async readRange(addr, size, progress = null) {
    const out = new Uint8Array(size);
    for (let at = 0; at < size; at += BLOCK) {
      out.set(await this.read(addr + at, Math.min(BLOCK, size - at)), at);
      if (progress) progress(at + BLOCK, size);
    }
    return out;
  }

  // An erase or a write, sent again when no reply comes (both are harmless to repeat).
  async acked(pkt, what) {
    for (let i = 0; i < this.tries; i++) {
      const r = await this.request(pkt, this.ackTimeout);
      if (!r) continue;
      if (!isAck(r)) throw new StockError(`${what}: refused (command ${F.hx(r.cmd)})`);
      return;
    }
    throw new StockError(`${what}: no reply after ${this.tries} tries`);
  }

  async erase(sector, { boot = false } = {}) {
    if (sector % F.SECTOR || sector < 0 || sector + F.SECTOR > IP.FLASH_SIZE) throw new StockError(`${F.hx(sector)} is not a sector`);
    if (!(boot && sector === 0) && forbidden(sector, sector + F.SECTOR)) throw new StockError(`sector ${F.hx(sector)} is never erased here`);
    await this.acked(packet(CMD.erase, [TYPE_FLASH, ...le(sector, 4)]), `erase ${F.hx(sector)}`);
  }

  // Programs one page (at most 256 bytes, not across a page).
  async program(addr, data, { boot = false } = {}) {
    if (!data.length || data.length > BLOCK || Math.floor(addr / BLOCK) !== Math.floor((addr + data.length - 1) / BLOCK)) throw new StockError(`write ${F.hx(addr)}: not inside one page`);
    if (!(boot && addr + data.length <= F.SECTOR) && forbidden(addr, addr + data.length)) throw new StockError(`write ${F.hx(addr)}: never written here`);
    await this.acked(packet(CMD.write, [TYPE_FLASH, ...le(addr, 4), ...le(data.length, 3), ...data]), `write ${F.hx(addr)}`);
  }
}

// --- the installation ------------------------------------------------------------------------------

const erasedView = (n) => F.bytes(n, 0xFF);

// Our firmware over M-Vave's, step by step. hooks: progress(step, done, total), backup(bytes) (a
// promise resolved once the user has the file), log(text). Returns { plan, sector0Ms }.
export async function install(link, { app, cfg, hooks = {}, now = () => Date.now() }) {
  const say = hooks.log || (() => {});
  const progress = hooks.progress || (() => {});
  // Diagnostic hooks never participate in control flow. No firmware/backup bytes leave here.
  const observe = (stage) => { try { hooks.observe?.({ stage }); } catch { /* observer only */ } };

  // 1. Which firmware answers.
  const name = await link.name();
  if (!name) throw new StockError('M-Vave\'s firmware does not answer');
  if (!MIXER_NAME.test(name)) throw new StockError(`${name}: not an SMC-Mixer`);
  say(`the mixer runs ${name}`);

  // 2. The backup: the whole flash as the stock firmware reads it, kept by the user first.
  const backup = await link.readRange(0, IP.FLASH_SIZE, (d, t) => progress('backup', d, t));
  if (hooks.backup) await hooks.backup(backup);

  // 3. The plan on that backup; nothing is written unless every check holds.
  const p = IP.plan(backup, app, cfg);
  if (p.report.failed) {
    throw new StockError('the installation plan does not hold: ' + p.report.items.filter((i) => i.status === 'fail').map((i) => i.text).join('; '));
  }
  if (p.view !== 'stock') throw new StockError(`the backup reads as a ${p.view} image, not as M-Vave's firmware reads it`);
  try { hooks.plan?.(IP.asJson(p)); } catch { /* observer only */ }
  const area = p.writes.find((w) => w.what === 'our area');
  const boot = p.writes.find((w) => w.what === 'the boot pointer');
  const stale = p.writes.find((w) => w.what === 'our earlier sectors');
  say(`plan: ${p.report.items.filter((i) => i.status === 'pass').length} checks hold${p.found === 'earlier' ? '; an earlier installation of ours is replaced' : p.found === 'stopped' ? '; a run that stopped is finished' : ''}`);

  // 4. Our area. First the earlier installation's sectors past the new area (its VM); then the
  // area sector by sector, written unless it already holds ours, then read back, the first sector
  // (the directory) last: until it is written the flash holds the earlier directory or none, and a
  // stop anywhere leaves a flash the plan takes again. Nothing points at our area yet, so M-Vave's
  // firmware starts as it was.
  for (const s of stale ? stale.erase : []) {
    await link.erase(s);
    if (!F.equal(await link.readRange(s, F.SECTOR), erasedView(F.SECTOR))) throw new StockError(`sector ${F.hx(s)} does not read back erased`);
  }
  const order = [...area.erase.slice(1), area.erase[0]];
  const total = order.length;
  for (let i = 0; i < total; i++) {
    const s = order[i];
    const want = F.bytes(F.SECTOR, 0xFF);
    want.set(area.data.subarray(s - area.offset, Math.min(s - area.offset + F.SECTOR, area.data.length)));
    let got = await link.readRange(s, F.SECTOR);
    if (!F.equal(got, want)) {
      // The plan checked what it held: erased, ours from before, or this area's pages.
      if (!F.equal(got, erasedView(F.SECTOR))) await link.erase(s);
      for (let off = 0; off < F.SECTOR; off += BLOCK) {
        const page = want.subarray(off, off + BLOCK);
        if (page.some((b) => b !== 0xFF)) await link.program(s + off, page);
      }
      got = await link.readRange(s, F.SECTOR);
      if (!F.equal(got, want)) throw new StockError(`sector ${F.hx(s)} does not read back as written`);
    }
    progress('area', i + 1, total);
  }
  const check = await link.readRange(area.offset, area.data.length, (d, t) => progress('check', Math.min(d, t), t));
  if (!F.equal(check, area.data)) throw new StockError('our area changed after it was written');
  say(`our area ${F.hx(area.offset)}+${F.hx(area.data.length)} written and read back`);

  // 5. Sector 0: still the backup's, then erased (or write protection: nothing written), the new
  // one written page by page and read back; a write that fails is sent again.
  const s0Before = backup.subarray(0, F.SECTOR); // as the stock read shows it
  const s0After = F.sector0View(boot.data);
  if (!F.equal(await link.readRange(0, F.SECTOR), s0Before)) throw new StockError('sector 0 changed since the backup: nothing written');
  const t0 = now();
  observe('sector0_start');
  await link.erase(0, { boot: true });
  const erased = await link.readRange(0, F.SECTOR);
  if (F.equal(erased, s0Before)) throw new StockError('the erase of sector 0 did not take (write protection): nothing written');
  if (!F.equal(erased, F.sector0View(erasedView(F.SECTOR)))) throw new StockError('sector 0 is neither erased nor as before: install again at once, do not switch off');
  for (let attempt = 1; ; attempt++) {
    for (let off = 0; off < F.SECTOR; off += BLOCK) {
      const page = boot.data.subarray(off, off + BLOCK);
      if (page.some((b) => b !== 0xFF)) await link.program(off, page, { boot: true });
    }
    const got = await link.readRange(0, F.SECTOR);
    if (F.equal(got, s0After)) break;
    if (attempt === 3) throw new StockError('sector 0 does not read back as written: do not switch off; install again, or the recovery key');
    await sleep(50);
  }
  const sector0Ms = now() - t0;
  observe('sector0_verified');
  progress('sector 0', 1, 1);
  say(`sector 0 points at ${F.hx(area.offset)} (${sector0Ms} ms without a uboot); switch the mixer off and on`);
  return { plan: p, sector0Ms, found: p.found };
}
