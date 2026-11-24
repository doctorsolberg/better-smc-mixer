// The SMC-Mixer's flash images (JieLi AC6966C, BR25): tools/image/flashimg.py in JavaScript, the
// parts the installer needs (docs/image.md). The two must agree byte for byte: run.mjs holds this
// module and installplan.js to the Python tools through tools/editortest/vectors/install.json.
// Nothing here talks to a device. Bytes are Uint8Arrays.

export const CHIPKEY = 0x980F; // the mixer's SFC chip key (docs/image.md, "SFC cipher")
export const AREA_AT = 0x1000; // flash offset of the application area; the CPU sees it at 0x1E00000
export const CPU_BASE = 0x1E00000;
export const ENTRY = 0x1E00120; // the entry point: the first file right after a 9-slot directory
export const SLOTS = 9;
export const ENTRY_SIZE = 32;
export const SECTOR = 0x1000;
const BLOCK = 32; // SFC block
const S0_RESTART = 256; // sector 0's keystream restarts every 256 bytes
const S0_KEY = 0xFFFF;

export const ATTR_HEAD = 0x83, ATTR_FILE = 0x82, ATTR_VM = 0x12, ATTR_REGION = 0x92;
export const RES_FILE = 0xFF, RES_VM = 0x80, RES_PRCT = 0x82, RES_REGION = 0x81;

// --- bytes ---------------------------------------------------------------------------------------

export const bytes = (n, fill = 0) => new Uint8Array(n).fill(fill);
export function concat(...parts) {
  const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
  let at = 0;
  for (const p of parts) { out.set(p, at); at += p.length; }
  return out;
}
export function equal(a, b) {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) if (a[i] !== b[i]) return false;
  return true;
}
export const u16 = (b, at) => b[at] | (b[at + 1] << 8);
export const u32 = (b, at) => (b[at] | (b[at + 1] << 8) | (b[at + 2] << 16) | (b[at + 3] << 24)) >>> 0;
const put16 = (b, at, v) => { b[at] = v & 0xFF; b[at + 1] = (v >>> 8) & 0xFF; };
const put32 = (b, at, v) => { for (let i = 0; i < 4; i++) b[at + i] = (v >>> (8 * i)) & 0xFF; };
// Python's f'{n:#x}' and f'{n:#06x}'
export const hx = (n) => (n < 0 ? '-0x' + (-n).toString(16) : '0x' + n.toString(16));
export const hx4 = (n) => '0x' + n.toString(16).padStart(4, '0');

// --- checksums and ciphers -------------------------------------------------------------------------

const CRC = (() => {
  const t = new Uint16Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i << 8;
    for (let k = 0; k < 8; k++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xFFFF : (c << 1) & 0xFFFF;
    t[i] = c;
  }
  return t;
})();

// CRC-16/XMODEM: polynomial 0x1021, initial value 0, not reflected.
export function crc16(data, crc = 0) {
  for (let i = 0; i < data.length; i++) crc = ((crc << 8) & 0xFFFF) ^ CRC[(crc >> 8) ^ data[i]];
  return crc;
}

// JieLi's ENC keystream: the low byte of a 16-bit LFSR (polynomial 0x1021) per byte.
export function keystream(key, n, skip = 0) {
  key &= 0xFFFF;
  const out = new Uint8Array(n);
  for (let i = 0; i < skip + n; i++) {
    if (i >= skip) out[i - skip] = key & 0xFF;
    key = key & 0x8000 ? ((key << 1) ^ 0x1021) & 0xFFFF : (key << 1) & 0xFFFF;
  }
  return out;
}

// The ENC cipher (XOR with the keystream from its start); symmetric.
export function enc(data, key) {
  const ks = keystream(key, data.length);
  return data.map((b, i) => b ^ ks[i]);
}

// The key of the 32-byte SFC block holding flash address addr of the application area: the
// hardware keys on the CPU address (0x1E00000 + addr - 0x1000), bits 2..17.
const sfcKey = (addr, chipkey = CHIPKEY) => chipkey ^ (((CPU_BASE + addr - AREA_AT) >>> 2) & 0xFFFF);

// The SFC cipher of data that sits at flash offset flashOff (any alignment); symmetric.
export function sfc(data, flashOff, chipkey = CHIPKEY) {
  const out = Uint8Array.from(data);
  let pos = 0;
  while (pos < out.length) {
    const a = flashOff + pos;
    const start = ((a % BLOCK) + BLOCK) % BLOCK;
    const n = Math.min(BLOCK - start, out.length - pos);
    const ks = keystream(sfcKey(a - start, chipkey), n, start);
    for (let i = 0; i < n; i++) out[pos + i] ^= ks[i];
    pos += n;
  }
  return out;
}

// --- JLFS entries ----------------------------------------------------------------------------------

const latin1 = (b) => String.fromCharCode(...b);
const ascii = (s) => Uint8Array.from(s, (c) => c.charCodeAt(0) & 0xFF);

// One 32-byte JLFS entry: hdr_crc, data_crc, offset, size, attr, res, index, name[16]. The name field
// is the name, a NUL, 0xFF padding, and optionally a tag in its last bytes.
export class Entry {
  constructor(name, offset, size, attr, res = RES_FILE, index = 0, dataCrc = 0xFFFF, tag = new Uint8Array(0)) {
    Object.assign(this, { name, offset, size, attr, res, index, dataCrc, tag });
  }

  pack() {
    const field = concat(ascii(this.name), bytes(1));
    if (field.length + this.tag.length > 16) throw new Error(`name and tag too long: ${this.name}`);
    const body = new Uint8Array(30);
    put16(body, 0, this.dataCrc);
    put32(body, 2, this.offset);
    put32(body, 6, this.size);
    body[10] = this.attr;
    body[11] = this.res;
    put16(body, 12, this.index);
    body.set(concat(field, bytes(16 - field.length - this.tag.length, 0xFF), this.tag), 14);
    const out = new Uint8Array(32);
    put16(out, 0, crc16(body));
    out.set(body, 2);
    return out;
  }

  // The entry in 32 bytes, or null when its header CRC does not match.
  static parse(b) {
    if (b.length < ENTRY_SIZE || crc16(b.subarray(2, ENTRY_SIZE)) !== u16(b, 0)) return null;
    const nameField = b.subarray(16, 32);
    const nul = nameField.indexOf(0);
    const name = latin1(nul < 0 ? nameField : nameField.subarray(0, nul));
    let rest = nul < 0 ? new Uint8Array(0) : nameField.subarray(nul + 1);
    let k = 0;
    while (k < rest.length && rest[k] === 0xFF) k++;
    rest = Uint8Array.from(rest.subarray(k));
    return new Entry(name, u32(b, 4), u32(b, 8), b[12], b[13], u16(b, 14), u16(b, 2), rest);
  }
}

// Entries from buf[at] on, up to and including the one with index != 0; decode, if given, is applied
// to each 32-byte entry first (sector 0's entries are ENC-ciphered one by one).
export function readEntries(buf, at = 0, decode = null, limit = 64) {
  const out = [];
  for (let i = 0; i < limit; i++) {
    const raw = buf.subarray(at + i * ENTRY_SIZE, at + (i + 1) * ENTRY_SIZE);
    const e = Entry.parse(decode ? decode(raw) : raw);
    if (!e) throw new Error(`entry ${i} at ${hx(at + i * ENTRY_SIZE)}: bad header CRC`);
    out.push(e);
    if (e.index) return out;
  }
  throw new Error(`no last entry within ${limit}`);
}

// --- sector 0 --------------------------------------------------------------------------------------

// Physical sector 0 <-> the view the stock firmware's flash read (0x23) returns: XOR with the ENC
// keystream of 0xFFFF restarting every 256 bytes. Symmetric.
export function sector0View(sector) {
  const ks = keystream(S0_KEY, S0_RESTART);
  return sector.map((b, i) => b ^ ks[i % S0_RESTART]);
}

// The flash header and the file list of a physical sector 0 (each entry ENC 0xFFFF on its own).
export function sector0Entries(physical) {
  const head = Entry.parse(enc(physical.subarray(0, ENTRY_SIZE), S0_KEY));
  if (!head) throw new Error('sector 0: bad flash header CRC');
  return [head, ...readEntries(physical, ENTRY_SIZE, (r) => enc(r, S0_KEY), 16)];
}

// A copy of a physical sector 0 whose app_dir_head points at dirOffset: only that entry changes.
export function setBootPointer(physical, dirOffset, name = 'app_dir_head') {
  const out = Uint8Array.from(physical);
  const list = sector0Entries(physical);
  for (let i = 1; i < list.length; i++) {
    if (list[i].name === name) {
      list[i].offset = dirOffset;
      out.set(enc(list[i].pack(), S0_KEY), i * ENTRY_SIZE);
      return out;
    }
  }
  throw new Error(`no ${name} entry in sector 0`);
}

// --- the application area --------------------------------------------------------------------------

export class Region {
  constructor(name, offset, size, attr = ATTR_REGION, res = RES_REGION, tag = new Uint8Array(0)) {
    Object.assign(this, { name, offset, size, attr, res, tag });
  }
}

// The regions of the stock directory; PRCT ends where the VM starts.
export function stockRegions(vmStart = 0x24000, vmSize = 0x57000) {
  return [new Region('VM', vmStart, vmSize, ATTR_VM, RES_VM, Uint8Array.of(0, 0x10, 0, 0)),
    new Region('PRCT', 0, vmStart, ATTR_REGION, RES_PRCT, ascii('CODE')),
    new Region('BTIF', 0x7B000, 0x1000, ATTR_REGION, RES_REGION, ascii('AUTO')),
    new Region('EXIF', 0x7C000, 0x1000, ATTR_REGION, RES_REGION, ascii('AUTO')),
    new Region('USRFLASH', 0x7D000, 0x2000)];
}

export class Area {
  constructor(entry, files, regions, tail = new Uint8Array(0), entries = []) {
    Object.assign(this, { entry, files, regions, tail, entries }); // files: [[name, bytes]]
  }

  get size() { return SLOTS * ENTRY_SIZE + this.files.reduce((n, [, d]) => n + d.length, 0); }

  file(name) { const f = this.files.find(([n]) => n === name); if (!f) throw new Error(`no ${name}`); return f[1]; }
}

// The application area as the CPU sees it, from its parts; at is the flash offset it goes to.
export function buildArea(area, at = AREA_AT) {
  const n = 1 + area.files.length + area.regions.length;
  if (n > SLOTS) throw new Error(`${n} entries do not fit the ${SLOTS}-slot directory`);
  const entries = [];
  let off = SLOTS * ENTRY_SIZE;
  for (const [name, data] of area.files) {
    entries.push(new Entry(name, off, data.length, ATTR_FILE, RES_FILE, 0, crc16(data)));
    off += data.length;
  }
  area.regions.forEach((r, i) => entries.push(new Entry(r.name, r.offset, r.size, r.attr, r.res,
    i === area.regions.length - 1 ? 1 : 0, 0xFFFF, r.tag)));
  const dir = concat(...entries.map((e) => e.pack()));
  const rest = concat(dir, bytes((SLOTS - 1) * ENTRY_SIZE - dir.length, 0xFF), ...area.files.map(([, d]) => d));
  const head = new Entry('app_area_head', area.entry, area.size, ATTR_HEAD, RES_FILE, 0, crc16(rest));
  const plain = concat(head.pack(), rest, area.tail);
  const vm = area.regions.find((r) => r.name === 'VM');
  if (vm && at + plain.length > vm.offset) throw new Error(`area ends at ${hx(at + plain.length)}, past the VM start ${hx(vm.offset)}`);
  return plain;
}

// A plaintext application area split into its parts; throws on any CRC mismatch.
export function parseArea(plain) {
  const ents = readEntries(plain, 0, null, SLOTS);
  const head = ents[0];
  if (head.name !== 'app_area_head') throw new Error(`first entry is '${head.name}', not app_area_head`);
  if (head.size > plain.length || crc16(plain.subarray(ENTRY_SIZE, head.size)) !== head.dataCrc) {
    throw new Error('app_area_head: area CRC does not match');
  }
  const files = [], regions = [];
  for (const e of ents.slice(1)) {
    if (e.attr === ATTR_FILE) {
      const data = Uint8Array.from(plain.subarray(e.offset, e.offset + e.size));
      if (crc16(data) !== e.dataCrc) throw new Error(`${e.name}: data CRC does not match`);
      files.push([e.name, data]);
    } else regions.push(new Region(e.name, e.offset, e.size, e.attr, e.res, e.tag));
  }
  return new Area(head.offset, files, regions, new Uint8Array(0), ents);
}

// The area as stored in flash: SFC-encrypted, then erased (0xFF) up to flash offset upto.
export function physical(plain, flashOff = AREA_AT, chipkey = CHIPKEY, upto = null) {
  const out = sfc(plain, flashOff, chipkey);
  return upto === null ? out : concat(out, bytes(upto - flashOff - plain.length, 0xFF));
}
