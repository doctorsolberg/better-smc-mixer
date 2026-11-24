// The installation plan: tools/image/installplan.py's plan() in JavaScript, its report word for word
// (docs/image.md, "Installing: the dry run"). From an image of the mixer's 512 KB flash (what the
// stock firmware's read returns, or a raw read) and our app.bin, it works out the writes, applies
// them to the image as NOR flash takes them, follows the flash after the way the ROM and the uboot
// do, and checks that the rollback gives the flash before. It writes nothing and talks to no device:
// js/stocklink.js does the writing, and only after this plan holds. run.mjs holds it to the Python
// tool through tools/editortest/vectors/install.json.
import * as F from './flashimg.js';

export const FLASH_SIZE = 0x80000;
export const SLOT = 0x41000; // our area: 0x1000 + 0x40000, keys as at 0x1000
export const LOADER_AT = 0x6E000; // the stock update loader: kept
export const CFG_AT = 0x25000; // the configuration's slots and the panel's state (cfgstore.h, statestore.h)
export const CFG_SIZE = 0x12000;
const VM_END = LOADER_AT;
const VM_LEAST = 0x2000;
const CHUNK = 0x100;
const KEEP = ['BTIF', 'EXIF', 'USRFLASH'];
const BTIF_ITEMS = [[102, 6, 'CFG_BT_MAC_ADDR'], [110, 6, 'CFG_BT_FRE_OFFSET']];
const KEY_MODELS = [['keyed on the CPU address', (at) => at], ['keyed on the flash offset', () => F.AREA_AT]];

const sfcFrom = (data, flashOff, base) => F.sfc(data, flashOff - base + F.AREA_AT);
const align = (x, a = F.SECTOR) => Math.ceil(x / a) * a;
const sectors = (lo, hi) => { const out = []; for (let s = lo; s < hi; s += F.SECTOR) out.push(s); return out; };
const rng = (lo, hi) => `${F.hx(lo)}..${F.hx(hi - 1)}`;
const overlaps = (a, b) => a[0] < b[1] && b[0] < a[1];
const floorDiv = (a, b) => Math.floor(a / b);

// --- reading flash -----------------------------------------------------------------------------------

function sector0Ok(s) {
  try { F.sector0Entries(s); return true; } catch { return false; }
}

// [flash as stored, view]. A raw read is that already. A stock read shows sector 0 through the
// 256-byte keystream and the stock area decrypted up to the stock VM start.
export function flashView(data, view = 'auto') {
  if (data.length !== FLASH_SIZE) throw new Error(`${F.hx(data.length)} bytes: not a ${FLASH_SIZE / 1024} KB flash image`);
  if (view === 'auto') {
    view = sector0Ok(data.subarray(0, F.SECTOR)) ? 'raw' : sector0Ok(F.sector0View(data.subarray(0, F.SECTOR))) ? 'stock' : null;
    if (!view) throw new Error('sector 0 parses neither as stored nor as the stock firmware reads it');
  }
  if (view === 'raw') return [Uint8Array.from(data), view];
  const vm = region(F.parseArea(data.subarray(F.AREA_AT)).regions, 'VM');
  return [F.concat(F.sector0View(data.subarray(0, F.SECTOR)), sfcFrom(data.subarray(F.AREA_AT, vm.offset), F.AREA_AT, F.AREA_AT),
    data.subarray(vm.offset)), view];
}

function region(regions, name) {
  const r = regions.find((x) => x.name === name);
  if (!r) throw new Error(`the directory declares no ${name}`);
  return r;
}

// Follow the boot as the ROM and the uboot do; throws where the chain breaks.
export function bootChain(image, baseOf = KEY_MODELS[0][1]) {
  const s0 = F.sector0Entries(image.subarray(0, F.SECTOR));
  const names = s0.slice(1).map((e) => e.name);
  if (!names.includes('uboot.boot')) throw new Error('sector 0 lists no uboot.boot');
  if (!names.includes('app_dir_head')) throw new Error('sector 0 lists no app_dir_head');
  const ptr = s0.slice(1).find((e) => e.name === 'app_dir_head').offset;
  if (ptr % F.SECTOR || !(F.SECTOR <= ptr && ptr < image.length)) throw new Error(`the boot pointer ${F.hx(ptr)} is not a sector inside flash`);
  const base = baseOf(ptr);
  const head = F.Entry.parse(sfcFrom(image.subarray(ptr, ptr + F.ENTRY_SIZE), ptr, base));
  if (!head || head.name !== 'app_area_head') throw new Error(`no app_area_head at ${F.hx(ptr)}`);
  if (ptr + head.size > image.length) throw new Error(`the area at ${F.hx(ptr)} would end past the flash`);
  const area = F.parseArea(sfcFrom(image.subarray(ptr, ptr + head.size), ptr, base));
  if (Math.floor(area.entry / 0x10000) * 0x10000 !== F.CPU_BASE) throw new Error(`the entry point ${F.hx(area.entry)} is outside the window the uboot maps`);
  const rel = area.entry - F.CPU_BASE;
  const hit = area.entries.slice(1).find((e) => e.attr === F.ATTR_FILE && e.offset <= rel && rel < e.offset + e.size);
  if (!hit) throw new Error(`the entry point ${F.hx(area.entry)} lands in no file`);
  return { s0, pointer: ptr, area, entryFlash: ptr + rel, entryFile: hit.name, entryAt: rel - hit.offset };
}

// [[start, end]] of the runs of sectors that are not erased.
export function used(image, lo = 0, hi = null) {
  const out = [];
  for (const s of sectors(lo, hi || image.length)) {
    if (image.subarray(s, s + F.SECTOR).some((b) => b !== 0xFF)) {
      if (out.length && out[out.length - 1][1] === s) out[out.length - 1][1] = s + F.SECTOR;
      else out.push([s, s + F.SECTOR]);
    }
  }
  return out;
}

// The runs [[start, end]] without the range cut.
function minus(runs, cut) {
  const out = [];
  for (const [a, b] of runs) for (const [lo, hi] of [[a, Math.min(b, cut[0])], [Math.max(a, cut[1]), b]]) if (lo < hi) out.push([lo, hi]);
  return out;
}

const isErased = (image, s) => image.subarray(s, s + F.SECTOR).every((b) => b === 0xFF);

// Whether the directory of an earlier installation of ours is at `at`: its first entry, through
// the SFC, an app_area_head whose header CRC checks (installplan.py, earlier_area).
export function earlierArea(image, at) {
  const head = F.Entry.parse(sfcFrom(image.subarray(at, at + F.ENTRY_SIZE), at, at));
  return !!head && head.name === 'app_area_head';
}

function btifItem(btif, offset, length) {
  const h = F.u32(btif, offset);
  if (h === 0xFFFFFFFF) return 'empty';
  const data = btif.subarray(offset + 4, offset + 4 + length);
  return (h >>> 20) === length && (F.crc16(data) & 0xFF) === (h & 0xFF) ? 'valid' : 'other';
}

// --- writes as flash takes them ----------------------------------------------------------------------

export const write = (what, offset, data, erase = [], file = '') => ({ what, offset, data, erase, file });

// The image after the writes: an erase sets its sector to 0xFF, programming ANDs (NOR flash).
export function apply(image, writes) {
  const out = Uint8Array.from(image);
  for (const w of writes) {
    for (const s of w.erase) out.fill(0xFF, s, s + F.SECTOR);
    for (let i = 0; i < w.data.length; i++) out[w.offset + i] &= w.data[i];
  }
  return out;
}

function needsErase(old, now) {
  let n = 0;
  for (let i = 0; i < Math.min(old.length, now.length); i++) {
    let v = now[i] & ~old[i] & 0xFF;
    while (v) { n += v & 1; v >>= 1; }
  }
  return n;
}

// --- the plan ----------------------------------------------------------------------------------------

class Report {
  constructor() { this.items = []; }
  add(section, status, text) { this.items.push({ section, status, text }); }
  check(section, ok, text, bad = null) { this.add(section, ok ? 'pass' : 'fail', ok ? text : (bad || text)); return ok; }
  get failed() { return this.items.filter((i) => i.status === 'fail').length; }
}

// The dry run. Returns { report, view, found, before, after, phys, writes, rollback, steps, undo }
// (found: what of ours the flash holds, '' none, 'earlier' an installation, 'stopped' this one's pages);
// report.failed counts what broke, and the images are set only as far as the checks got.
export function plan(dump, app, cfg = null, tail = new Uint8Array(0), at = SLOT, vm = null, view = 'auto') {
  const rep = new Report();
  const p = { report: rep, view: '', found: '', before: null, after: null, phys: null, writes: [], rollback: [], steps: [], undo: [] };

  let sec = 'now';
  let before;
  try { [before, p.view] = flashView(dump, view); } catch (e) { rep.add(sec, 'fail', `the image: ${e.message}`); return p; }
  p.before = before;
  let stock;
  try { stock = bootChain(before); } catch (e) { rep.add(sec, 'fail', `the boot chain of the flash now: ${e.message}`); return p; }
  const s0names = stock.s0.slice(1).map((e) => e.name).join(', ');
  if (!rep.check(sec, stock.pointer === F.AREA_AT, `sector 0 (${s0names}): the boot pointer is the stock ${F.hx(F.AREA_AT)}`,
    `the boot pointer is ${F.hx(stock.pointer)}, not the stock ${F.hx(F.AREA_AT)}: not the stock flash (already installed?)`)) return p;
  const svm = region(stock.area.regions, 'VM');
  rep.add(sec, 'pass', `the stock area at ${F.hx(stock.pointer)}, as the uboot reads it: CRCs ok, entry ${F.hx(stock.area.entry)} = `
    + `${stock.entryFile}+${F.hx(stock.entryAt)} (flash ${F.hx(stock.entryFlash)}); `
    + stock.area.files.map(([n, d]) => `${n} ${F.hx(d.length)}`).join(', ') + `; VM ${F.hx(svm.offset)}+${F.hx(svm.size)}`);
  const inUse = used(before);
  rep.add(sec, 'info', 'sectors in use: ' + inUse.map(([a, b]) => rng(a, b)).join(', '));

  sec = 'ours';
  const files = [['app.bin', app], ...(cfg ? [['cfg_tool.bin', cfg]] : [])];
  const size = F.SLOTS * F.ENTRY_SIZE + files.reduce((n, [, d]) => n + d.length, 0) + tail.length;
  const end = at + size;
  const [vmStart, vmSize] = vm || [align(end), VM_END - align(end)];
  const cfgRng = [CFG_AT, CFG_AT + CFG_SIZE];
  const keep = KEEP.map((n) => region(stock.area.regions, n));
  const vmR = F.stockRegions(vmStart, vmSize)[0];
  const prct = new F.Region('PRCT', 0, vmStart, F.ATTR_REGION, F.RES_PRCT, Uint8Array.from('CODE', (c) => c.charCodeAt(0)));
  const area = new F.Area(F.ENTRY, files, [vmR, prct, ...keep], tail);
  const ours = [at, align(end)];
  const vmRng = [vmStart, vmStart + vmSize];
  let ok = rep.check(sec, at % F.SECTOR === 0 && F.AREA_AT < at && end <= FLASH_SIZE,
    `area ${rng(at, end)} (${F.hx(size)} bytes)`, `the area ${rng(at, end)} does not fit`);
  ok = rep.check(sec, vmStart >= align(end) && vmStart % F.SECTOR === 0 && vmSize % F.SECTOR === 0 && vmSize >= VM_LEAST && vmRng[1] <= VM_END,
    `our VM ${rng(...vmRng)} (${floorDiv(vmSize, 1024)} KB; the SDK needs ${floorDiv(VM_LEAST, 1024)} KB), `
    + `after the area, sector-aligned, below the stock update loader (${F.hx(VM_END)})`,
    `our VM ${F.hx(vmStart)}+${F.hx(vmSize)} must be sector-aligned, at least ${F.hx(VM_LEAST)} bytes, `
    + `start after the area's last sector (${F.hx(align(end))}) and end by the stock update loader (${F.hx(VM_END)})`) && ok;
  ok = rep.check(sec, svm.offset + F.SECTOR <= cfgRng[0] && cfgRng[1] <= at,
    `the configuration's slots ${rng(...cfgRng)} below the area, above the stock VM's first sector (${F.hx(svm.offset)})`,
    `the configuration's slots ${rng(...cfgRng)} must lie between the stock VM's first sector (${F.hx(svm.offset)}) and the area (${F.hx(at)})`) && ok;
  if (!ok) return p;
  let plain;
  try { plain = F.buildArea(area, at); } catch (e) { rep.add(sec, 'fail', `the directory: ${e.message}`); return p; }
  rep.add(sec, 'pass', 'directory: ' + F.parseArea(plain).entries.slice(1).map((e) => `${e.name} ${F.hx(e.offset)}+${F.hx(e.size)}`).join(', ')
    + `; entry ${F.hx(F.ENTRY)}`);
  const ciphers = KEY_MODELS.map(([, base]) => sfcFrom(plain, at, base(at)));
  if (!rep.check(sec, F.equal(ciphers[0], ciphers[1]), `both SFC key models encrypt it the same at ${F.hx(at)}`,
    `at ${F.hx(at)} the two SFC key models encrypt it differently: only ${F.hx(F.AREA_AT)} + k * 0x40000 is safe`)) return p;
  const phys = p.phys = ciphers[0];
  for (const r of keep) {
    const clash = [['the area', ours], ['our VM', vmRng], ["the configuration's slots", cfgRng]]
      .filter(([, x]) => overlaps(x, [r.offset, r.offset + r.size])).map(([n]) => n);
    rep.check(sec, !clash.length, `${r.name} ${F.hx(r.offset)}+${F.hx(r.size)} kept as the stock directory has it`,
      `${clash.join(' and ')} overlap${clash.length === 1 ? 's' : ''} ${r.name}`);
  }
  const loader = used(before, LOADER_AT, keep[0].offset);
  rep.check(sec, !loader.some((u) => overlaps(vmRng, u)),
    `the update loader's sectors (${loader.map((u) => rng(...u)).join(', ') || 'none in this flash'}) stay outside our VM`,
    'our VM overlaps the stock update loader');
  rep.check(sec, !loader.some((u) => overlaps(cfgRng, u)) && cfgRng[1] <= LOADER_AT,
    'and outside the configuration\'s slots', 'the configuration\'s slots overlap the stock update loader');
  // What of ours is there already (installplan.py): an earlier installation, or a run of this one
  // that stopped (each page this area's or erased).
  const want = F.bytes(ours[1] - at, 0xFF);
  want.set(phys);
  const pageOk = (q) => { const g = before.subarray(q, q + CHUNK); return g.every((b) => b === 0xFF) || F.equal(g, want.subarray(q - at, q - at + CHUNK)); };
  const mine = sectors(...ours).filter((s) => !isErased(before, s) && Array.from({ length: F.SECTOR / CHUNK }, (_, i) => s + i * CHUNK).every(pageOk));
  if (earlierArea(before, at)) {
    p.found = 'earlier';
    rep.add(sec, 'info', `our sectors ${rng(at, VM_END)} hold an earlier installation of ours (its directory `
      + `at ${F.hx(at)}): erased and written again; the configuration's slots and the panel's state `
      + `${rng(...cfgRng)} are kept`);
  } else {
    p.found = mine.length ? 'stopped' : '';
    const busy = [...sectors(at, vmRng[1]), ...(mine.length ? [] : sectors(...cfgRng))].filter((s) => !isErased(before, s) && !mine.includes(s));
    const runs = [];
    for (const s of busy) {
      if (runs.length && runs[runs.length - 1][1] === s) runs[runs.length - 1][1] = s + F.SECTOR;
      else runs.push([s, s + F.SECTOR]);
    }
    runs.sort((a, b) => a[0] - b[0]);
    rep.check(sec, !busy.length, `our sectors ${rng(...cfgRng)} and ${rng(at, vmRng[1])} are erased in this flash`
      + (mine.length ? `, but ${mine.length} of the area's that hold its bytes already: a run of this installation `
        + 'that stopped; the configuration\'s slots are kept' : ''),
    'our sectors are in use: ' + runs.map((u) => rng(...u)).join(', '));
  }
  if (rep.failed) return p;

  sec = 'writes';
  const s0Old = before.subarray(0, F.SECTOR);
  const s0New = F.setBootPointer(s0Old, at);
  const idx = stock.s0.findIndex((e, i) => i && e.name === 'app_dir_head');
  const ent = [idx * F.ENTRY_SIZE, (idx + 1) * F.ENTRY_SIZE];
  const bits = needsErase(s0Old.subarray(...ent), s0New.subarray(...ent));
  const erased = (lo, hi) => F.crc16(F.bytes(hi - lo, 0xFF));
  const stale = p.found === 'earlier' ? sectors(ours[1], VM_END).filter((s) => !isErased(before, s)) : [];
  p.writes = [...(stale.length ? [write('our earlier sectors', stale[0], new Uint8Array(0), stale)] : []),
    write('our area', at, phys, sectors(...ours), 'area-phys.bin'), write('the boot pointer', 0, s0New, [0], 'sector0-after.bin')];
  p.steps = [
    `1. Check that the device holds this flash: CRC-16 of sector 0 0x0+0x1000 = ${F.hx4(F.crc16(s0Old))}, `
    + `of the stock area ${F.hx(F.AREA_AT)}+${F.hx(svm.offset - F.AREA_AT)} = ${F.hx4(F.crc16(before.subarray(F.AREA_AT, svm.offset)))}, `
    + `of our sectors ${F.hx(at)}+${F.hx(vmRng[1] - at)} = ${F.hx4(F.crc16(before.subarray(at, vmRng[1])))} `
    + { '': '(erased).', earlier: '(an earlier installation of ours).', stopped: '(part of this area, the rest erased).' }[p.found],
    (stale.length ? `2. Our area: erase the earlier installation's ${stale.length} sectors after it (${stale.map((x) => F.hx(x)).join(', ')}), then `
      : '2. Our area: ')
    + `erase ${sectors(...ours).length} sectors ${rng(...ours)} (${!p.found ? 'already erased here' : 'where they hold anything'}; `
    + 'the first, with the directory, last), write '
    + `${F.hx(phys.length)} bytes (area-phys.bin; ${Math.ceil(phys.length / CHUNK)} writes of 256 bytes with 0x22, smcmidi sinco-write). `
    + `Then CRC-16 of ${F.hx(at)}+${F.hx(phys.length)} = ${F.hx4(F.crc16(phys))}; the stock read (0x23) returns `
    + 'these bytes as stored, being above the stock VM start.',
    `3. The boot pointer: sector 0's entry ${idx} (app_dir_head, at ${F.hx(ent[0])}) from ${F.hx(F.AREA_AT)} to `
    + `${F.hx(at)}, the other ${F.SECTOR - F.ENTRY_SIZE} bytes as they are: erase sector 0, write `
    + `sector0-after.bin. Then CRC-16 of 0x0+0x1000 = ${F.hx4(F.crc16(s0New))}.`,
    `4. Reset: the uboot reads the boot pointer ${F.hx(at)} and jumps to ${F.hx(F.ENTRY)} = flash `
    + `${F.hx(at + F.ENTRY - F.CPU_BASE)}, the first byte of our app.bin.`,
  ];
  rep.add(sec, 'info', bits === 0 ? 'the boot pointer entry alone could be changed by programming, without erasing sector 0 (its bits only go from 1 to 0)'
    : `the boot pointer entry needs sector 0 erased: ${bits} of its bits go from 0 to 1`);
  rep.add(sec, 'info', `the stock write (0x22) stores sector 0 as sent (bringup.md, Step 2b): CRC-16 ${F.hx4(F.crc16(s0New))}; `
    + 'its driver checks only the end of the flash, not PRCT (static analysis)');

  sec = 'after';
  const after = p.after = apply(before, p.writes);
  for (const [name, base] of KEY_MODELS) {
    let c;
    try { c = bootChain(after, base); } catch (e) { rep.add(sec, 'fail', `${name}: ${e.message}`); continue; }
    rep.check(sec, c.pointer === at && c.area.entry === F.ENTRY && c.entryFile === 'app.bin' && c.entryAt === 0 && F.equal(c.area.file('app.bin'), app),
      `${name}: sector 0 -> ${F.hx(c.pointer)}, directory CRCs ok, entry ${F.hx(c.area.entry)} = flash ${F.hx(c.entryFlash)}, `
      + 'the first byte of app.bin, which is ours byte for byte',
      `${name}: the chain leads to ${F.hx(c.pointer)}, entry ${F.hx(c.area.entry)} in ${c.entryFile}+${F.hx(c.entryAt)}, not to our app.bin's first byte`);
  }
  const changed = sectors(0, FLASH_SIZE).filter((s) => !F.equal(before.subarray(s, s + F.SECTOR), after.subarray(s, s + F.SECTOR)));
  const diff0 = [];
  for (let i = 0; i < F.SECTOR; i++) if (before[i] !== after[i]) diff0.push(i);
  const oursSet = new Set([0, ...sectors(...ours), ...stale]);
  rep.check(sec, changed.every((s) => oursSet.has(s)) && diff0.length > 0 && ent[0] <= diff0[0] && diff0[diff0.length - 1] < ent[1],
    diff0.length ? `what changes: sector 0 bytes ${F.hx(diff0[0])}..${F.hx(diff0[diff0.length - 1])} (app_dir_head) and `
      + `${changed.length - 1} of our sectors; nothing else` : '',
    'the writes change more than the boot pointer entry and our sectors: ' + changed.map((s) => F.hx(s)).join(', '));
  const kept = [['the stock area', F.AREA_AT, svm.offset], ...keep.map((r) => [r.name, r.offset, r.offset + r.size]),
    ...minus(inUse, [at, VM_END]).filter(([, b]) => b > F.SECTOR).map(([a, b]) => [`in use ${rng(Math.max(a, F.SECTOR), b)}`, Math.max(a, F.SECTOR), b])];
  const same = (a, b) => F.equal(before.subarray(a, b), after.subarray(a, b));
  rep.check(sec, kept.every(([, a, b]) => same(a, b)), 'unchanged byte for byte: the stock area, ' + KEEP.join(', ') + ', every other sector in use',
    'changed: ' + kept.filter(([, a, b]) => !same(a, b)).map(([n]) => n).join(', '));

  sec = 'first run';
  rep.add(sec, 'info', `the SDK's settings store (VM) is ours at ${rng(...vmRng)}; its writes stay there (⚠️ the SDK's VM code has not run on the device)`);
  rep.add(sec, 'info', `the configuration's two slots and the panel's state ${rng(...cfgRng)} (firmware/store/cfgstore.h, statestore.h): `
    + (p.found && !sectors(...cfgRng).every((s) => isErased(before, s))
      ? 'kept, so the first start runs the configuration saved there, if our firmware reads it; '
      : 'erased now, so the first start runs the factory configuration with Bluetooth on, preset 1 page 1; ')
    + 'our firmware writes one slot a few seconds '
    + 'after a configuration is received (✅ at this place 2026-11-12, controller 21). A later '
    + 'installation of our area leaves them; the rollback (R2) erases them');
  const btif = region(stock.area.regions, 'BTIF');
  const data = before.subarray(btif.offset, btif.offset + btif.size);
  let atItem = 0;
  for (const [ident, n, name] of BTIF_ITEMS) {
    const st = btifItem(data, atItem, n);
    if (ident === 102) {
      const text = { valid: 'valid: our firmware reads the stock MAC address and writes nothing to BTIF',
        empty: 'empty: our firmware writes a random MAC address into it (programming an erased slot, no erase)',
        other: 'holds something else: our firmware refuses to write there and makes a new random MAC address on every boot' }[st];
      rep.add(sec, st === 'valid' ? 'pass' : 'info', `BTIF item ${ident} (${name}) at ${F.hx(btif.offset + atItem)}: ${text}`);
    } else rep.add(sec, 'info', `BTIF item ${ident} (${name}) at ${F.hx(btif.offset + atItem)}: ${st}; our firmware only reads it`);
    atItem += 4 + n;
  }

  sec = 'rollback';
  p.rollback = [write('sector 0 back', 0, Uint8Array.from(s0Old), [0], 'sector0-before.bin'),
    write('our sectors erased', at, new Uint8Array(0), [...sectors(...cfgRng), ...sectors(at, vmRng[1])])];
  // the flash before, but for what of ours it held (nothing on a first installation)
  const bare = apply(before, [write('', 0, new Uint8Array(0), [...sectors(...cfgRng), ...sectors(at, VM_END)])]);
  const asBefore = 'the flash before' + (F.equal(bare, before) ? '' : ' with our sectors erased');
  p.undo = [
    `R1. Sector 0 back: erase it and write sector0-before.bin; CRC-16 of 0x0+0x1000 = ${F.hx4(F.crc16(s0Old))}. `
    + 'The stock firmware starts again: its area was never touched.',
    `R2. Erase our sectors ${rng(...cfgRng)} and ${rng(at, vmRng[1])} (the configuration's slots and the panel's state, `
    + 'the area and the VM our firmware wrote), so the stock VM range holds nothing of ours; CRC-16 = '
    + `${F.hx4(erased(...cfgRng))} and ${F.hx4(erased(at, vmRng[1]))}.`,
    `R3. CRC-16 of the whole flash 0x0+${F.hx(FLASH_SIZE)} = ${F.hx4(F.crc16(bare))}, ${asBefore}, `
    + "unless the stock firmware's own VM or BTIF were written in between (then compare range by range). "
    + 'The last line of defence stays the raw read of Rehearsal 1 (recovery.md).',
  ];
  rep.check(sec, F.equal(apply(after, p.rollback), bare), `applied to the flash after, the rollback gives ${asBefore} byte for byte`,
    'the rollback does NOT give the flash before');

  sec = 'open';
  for (const text of ["the uboot's way through the directory is open-smc-pad's reading on the SMC-PAD; not seen on the mixer",
    !cfg ? 'no cfg_tool.bin (--no-cfg): our SDK build resets in its first syscfg_read() without one (docs/bringup.md, "Step 4: boot marks", run 3)' : '',
    p.view === 'stock' ? 'this flash is a model until a raw ROM read confirms it (recovery.md, Rehearsal 1)' : '']) {
    if (text) rep.add(sec, 'open', text);
  }
  return p;
}

// The plan as installplan.py --json gives it.
export function asJson(p) {
  const w = (x) => ({ what: x.what, offset: x.offset, size: x.data.length, erase: [...x.erase],
    crc16: x.data.length ? F.crc16(x.data) : null, file: x.file || null });
  return { view: p.view, found: p.found, items: p.report.items, failed: p.report.failed, steps: p.steps,
    writes: p.writes.map(w), rollback_steps: p.undo, rollback: p.rollback.map(w),
    crc16: { before: p.before ? F.crc16(p.before) : null, after: p.after ? F.crc16(p.after) : null } };
}
