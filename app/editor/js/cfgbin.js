// The configuration's binary form in the editor (docs/config.md, "Binary form"; the owner's
// decision of 2026-11-13, option (a)): what the mixer stores and the configuration protocol
// carries, made from the file and read back into one. The third implementation of the same rules,
// after firmware/config/cfgbin.c (the mixer) and tools/smcconf/codec.py (the tools), and a port of
// the latter: the same model, the same checks, the same canonical bytes. tools/editortest/run.mjs
// holds it to codec.py's output byte for byte (vectors/binary.json, written by vectors.py).
//
//   const codec = new Codec(schemaJson);            // config/schema.json as parsed
//   codec.fileToBinary(fileObject)  -> Uint8Array   // checks first; CodecError(path, msg)
//   codec.binaryToFile(bytes)       -> file object  // canonical, without names (the binary has none)
//
// The model is codec.py's: the device's values (channel 0-15, enums as numbers) plus names.

import { crc32 } from './link.js';
import { versionText } from './schema.js';

export class CodecError extends Error {
  constructor(path, msg) {
    super(`${path || '(top)'}: ${msg}`);
    this.path = path;
    this.msg = msg;
  }
}

const need = (cond, path, msg) => { if (!cond) throw new CodecError(path, msg); };
const isInt = (v) => Number.isInteger(v);
const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/;
const FIELD_ORDER = ['msg', 'channel', 'number', 'mode', 'param', 'flags', 'lo', 'hi', 'start'];

class Enum {
  constructor(d) {
    this.name = d.name;
    this.values = d.values;
    this.json = d.values.map((v) => v.json);
  }
  get length() { return this.values.length; }
  index(name) { return this.json.indexOf(name); }
}

function field(d) {
  return {
    c: d.c, json: d.json, size: d.size, enum: d.enum, min: d.min ?? 0, max: d.max, offset: d.offset ?? 0,
    required: !!d.required, default: d.required ? null : (d.default ?? 0), named: d.named ?? [],
    applies: d.applies ?? {}, exclude: d.exclude ?? [], members: d.members ?? null,
  };
}

export class Codec {
  constructor(raw) {
    this.version = raw.schemaVersion;
    this.binaryVersion = raw.binaryVersion;
    this.device = raw.device;
    this.enums = {};
    for (const e of raw.enums) this.enums[e.name] = new Enum(e);
    this.msg = this.enums.msg;
    this.kinds = {};
    for (const k of raw.kinds) {
      const modeEnum = this.enums[k.modeEnum];
      this.kinds[k.name] = { name: k.name, modeEnum, modeDefault: modeEnum.index(k.modeDefault), shiftDefault: k.shiftDefault,
        types: k.types.map((t) => this.msg.index(t)) };
    }
    this.assignFields = raw.assignment.fields.map(field);
    this.presetFields = raw.preset.fields.map(field);
    this.globalFields = raw.global.fields.map(field);
    if (this.assignFields.map((f) => f.c).join() !== FIELD_ORDER.join()) throw new Error('schema: the assignment fields are not struct core_assign\'s');
    this.FIELD = Object.fromEntries(this.assignFields.map((f) => [f.c, f]));
    this.stripControls = raw.strip.controls;
    this.row = raw.row.buttons;
    this.fine = raw.control.shift.fine;
    this.tags = Object.fromEntries(raw.binary.records.map((r) => [r.name, r.tag]));
    this.rowPage = raw.binary.rowPage;
    this.optional = raw.binary.optionalTagBit;
    this.headerSize = raw.binary.headerSize;
    this.magic = [...raw.binary.magic].map((ch) => ch.charCodeAt(0));
    this.nameMax = raw.file.nameMax;
    this.NONE = this.msg.index('none');
    this.SHIFT = this.enums.shiftKind;
    this.INHERIT = this.SHIFT.index('inherit');
    this.OVERRIDE = this.SHIFT.index('override');
    this.BLOCK = this.SHIFT.index('block');
    this.BUTTON = this.kinds.button;
    this.JSON_KEYS = [];
    for (const f of this.assignFields) this.JSON_KEYS.push(...(f.members ? f.members.map((m) => m.json) : [f.json]));
    this.KIND_OF = Object.fromEntries(this.stripControls.map((c) => [c.json, this.kinds[c.kind]]));
    this.STRIP_KEYS = this.stripControls.map((c) => c.json);
    this.ROW_KEYS = this.row.map((b) => b.json);
  }

  // --- the schema's rules (schema.py) ----------------------------------------------------------

  typeMax(msg) { return this.msg.values[msg].max ?? 0; }
  numberMax(msg) { return this.msg.values[msg].numberMax; }
  inTypeSet(name, msg) {
    if (name === 'notNone') return msg !== this.NONE;
    if (name === 'withNumber') return this.msg.values[msg].numberMax !== undefined;
    if (name === 'withValue') return this.msg.values[msg].max !== undefined;
    throw new Error(`type set ${name}`);
  }
  applies(f, kind, msg, mode) {
    const a = f.applies;
    if (a.types && !this.inTypeSet(a.types, msg)) return false;
    if (a.kinds && !a.kinds.includes(kind.name)) return false;
    if (a.modes && !a.modes.includes(kind.modeEnum.json[mode])) return false;
    return true;
  }
  fieldMax(f, msg) {
    if (f.max === 'numberMax') return this.numberMax(msg);
    if (f.max === 'typeMax') return this.typeMax(msg);
    return f.max;
  }
  fieldEnum(f, kind) {
    if (f.enum === 'kindMode') return kind.modeEnum;
    return f.enum ? this.enums[f.enum] : null;
  }
  def(f, kind = null, msg = 0, mode = 0, layer = null) {
    if (kind && !this.applies(f, kind, msg, mode)) return 0;
    const d = f.default;
    if (d === null) return 0;
    if (d === 'middle') return Math.floor((layer.lo + layer.hi + 1) / 2);
    for (const n of f.named) if (d === n.json) return n.value;
    if (d === 'kindMode') return kind.modeDefault;
    if (d === 'typeMax') return this.typeMax(msg);
    if (f.enum && typeof d === 'string') return this.fieldEnum(f, kind).index(d);
    if (f.members) return 0;
    return f.offset && isInt(d) ? d - f.offset : d;
  }
  rowNav(i) { return !!this.row[i].nav; }
  where(kind) { return ('aeiou'.includes(kind.name[0]) ? 'an ' : 'a ') + kind.name; }

  // --- the model --------------------------------------------------------------------------------

  emptyAssign() { return Object.fromEntries(FIELD_ORDER.map((c) => [c, 0])); }
  emptyControl() { return { name: null, normal: this.emptyAssign(), shift: this.emptyAssign(), shiftKind: this.INHERIT, shiftName: null }; }
  emptyConfig() {
    const dev = this.device;
    const g = Object.fromEntries(this.globalFields.map((f) => [f.c, this.def(f)]));
    const presets = [];
    for (let p = 0; p < dev.presets; p++) {
      const preset = { name: null, row: this.ROW_KEYS.map(() => this.emptyControl()), pages: [] };
      for (let pg = 0; pg < dev.pages; pg++) {
        const strips = [];
        for (let n = 0; n < dev.strips; n++) strips.push(Object.fromEntries(this.STRIP_KEYS.map((k) => [k, this.emptyControl()])));
        preset.pages.push({ name: null, strips });
      }
      for (const f of this.presetFields) preset[f.c] = this.def(f);
      presets.push(preset);
    }
    return { name: null, id: new Uint8Array(16), revision: 0, global: g, presets };
  }

  // [page, control id, kind, nav, ctl] of a preset, in the binary order; page rowPage: the row.
  * controls(preset) {
    for (const [pg, page] of preset.pages.entries()) {
      for (const c of this.stripControls) {
        for (const [n, strip] of page.strips.entries()) yield [pg, c.id + n, this.kinds[c.kind], false, strip[c.json]];
      }
    }
    for (const [i, ctl] of preset.row.entries()) yield [this.rowPage, i, this.BUTTON, this.rowNav(i), ctl];
  }

  controlAt(preset, page, cid) {
    if (page === this.rowPage) return cid < this.ROW_KEYS.length ? [this.BUTTON, this.rowNav(cid), preset.row[cid]] : null;
    const group = Math.floor(cid / 8), n = cid % 8;
    if (page >= this.device.pages || group >= this.stripControls.length || n >= this.device.strips) return null;
    const c = this.stripControls[group];
    return [this.kinds[c.kind], false, preset.pages[page].strips[n][c.json]];
  }

  fineOf(normal) {
    return { ...normal, mode: this.kinds.encoder.modeEnum.index(this.fine.mode), param: 0 };
  }

  defaultShift(kind, normal) {
    if (kind.shiftDefault === 'fine' && normal.msg !== this.NONE) return [this.OVERRIDE, this.fineOf(normal)];
    return [this.INHERIT, this.emptyAssign()];
  }

  sameShift(ctl, kind) {
    const [k, s] = this.defaultShift(kind, ctl.normal);
    return ctl.shiftKind === k && FIELD_ORDER.every((c) => ctl.shift[c] === s[c]);
  }

  // --- checks on the device's values ------------------------------------------------------------

  checkAssign(kind, a, path) {
    const msg = a.msg;
    need(kind.types.includes(msg), path, `type ${this.msg.json[msg] ?? msg} is not for ${this.where(kind)} (${kind.types.map((t) => this.msg.json[t]).join(', ')})`);
    if (msg === this.NONE) {
      const bad = FIELD_ORDER.filter((c) => a[c]);
      need(!bad.length, path, `type none with ${bad.join(', ')} set`);
      return;
    }
    const mode = a.mode;
    need(mode < kind.modeEnum.length, path, `mode ${mode} is not one of ${kind.name}'s`);
    for (const f of this.assignFields) {
      const v = a[f.c];
      if (f.c === 'msg' || f.c === 'mode') continue;
      if (!this.applies(f, kind, msg, mode)) {
        need(!v, path, `${f.json ?? f.c} does not apply here (it is ${v})`);
        continue;
      }
      if (f.members) {
        let known = 0;
        for (const m of f.members) {
          known |= m.mask;
          need(Object.values(m.values).includes(v & m.mask), path, `${m.json}: bits 0x${(v & m.mask).toString(16)}`);
        }
        need(!(v & ~known), path, `unknown flags 0x${(v & ~known).toString(16)}`);
        continue;
      }
      const lo = f.min - f.offset, hi = this.fieldMax(f, msg) - f.offset;
      need((lo <= v && v <= hi) || f.named.some((n) => n.value === v), path, `${f.json} ${v + f.offset} is outside ${lo + f.offset}-${hi + f.offset}`);
    }
  }

  checkControl(kind, nav, ctl, path) {
    this.checkAssign(kind, ctl.normal, path);
    const k = ctl.shiftKind;
    need([this.INHERIT, this.OVERRIDE, this.BLOCK].includes(k), path, `shift kind ${k}`);
    need(!nav || k === this.INHERIT, path, 'the Shift layer of a navigation button is the firmware\'s (FR-SH-02)');
    if (k !== this.OVERRIDE) {
      need(!FIELD_ORDER.some((c) => ctl.shift[c]), path + '.shift', `set although the Shift layer is ${this.SHIFT.json[k]}`);
      return;
    }
    need(ctl.shift.msg !== this.NONE, path + '.shift', 'an override that sends nothing: use "block"');
    this.checkAssign(kind, ctl.shift, path + '.shift');
  }

  checkSetting(f, v, path, e) {
    if (e) {
      const allowed = e.json.map((_, i) => i).filter((i) => !f.exclude.includes(e.json[i]));
      need(allowed.includes(v), path, `${f.json} ${v} is not one of ${allowed.map((i) => e.json[i]).join(', ')}`);
    } else {
      need(f.min - f.offset <= v && v <= f.max - f.offset, path, `${f.json} ${v + f.offset} is outside ${f.min}-${f.max}`);
    }
  }

  checkModel(m) {
    for (const f of this.globalFields) this.checkSetting(f, m.global[f.c], 'global.' + f.json, this.enums[f.enum]);
    for (const [p, preset] of m.presets.entries()) {
      for (const f of this.presetFields) this.checkSetting(f, preset[f.c], `presets[${p}].${f.json}`, this.enums[f.enum]);
      for (const [pg, cid, kind, nav, ctl] of this.controls(preset)) this.checkControl(kind, nav, ctl, this.pathOf(p, pg, cid));
    }
  }

  pathOf(p, page, cid) {
    if (page === this.rowPage) return `presets[${p}].row.${this.ROW_KEYS[cid]}`;
    return `presets[${p}].pages[${page}].strips[${cid % 8}].${this.STRIP_KEYS[Math.floor(cid / 8)]}`;
  }

  // --- files ------------------------------------------------------------------------------------

  keysOk(obj, allowed, path) {
    need(obj !== null && typeof obj === 'object' && !Array.isArray(obj), path, 'an object is needed');
    const extra = Object.keys(obj).filter((k) => !allowed.includes(k));
    need(!extra.length, path, `unknown key${extra.length > 1 ? 's' : ''} ${extra.map((k) => `'${k}'`).join(', ')} (known: ${allowed.join(', ')})`);
  }

  nameOf(obj, path) {
    const v = obj.name;
    if (v !== undefined && v !== null) need(typeof v === 'string' && [...v].length <= this.nameMax, path + '.name', `a string of at most ${this.nameMax} characters`);
    return v ?? null;
  }

  jsonValue(f, v, path, e = null, lo = null, hi = null) {
    if (e) {
      const allowed = e.json.filter((n) => !f.exclude.includes(n));
      need(allowed.includes(v), path, `${JSON.stringify(v)} is not one of ${allowed.join(', ')}`);
      return e.index(v);
    }
    lo = lo ?? f.min;
    hi = hi ?? f.max;
    const words = f.named.map((n) => ` or ${n.json}`).join('');
    for (const n of f.named) if (v === n.json) return n.value;
    need(isInt(v), path, `a whole number${words} is needed, not ${JSON.stringify(v)}`);
    need(lo <= v && v <= hi, path, `${v} is outside ${lo}-${hi}${words}`);
    return v - f.offset;
  }

  fileValue(f, v, e = null) {
    if (e) return e.json[v];
    for (const n of f.named) if (v === n.value) return n.json;
    return v + f.offset;
  }

  resolve(kind, written, path) {
    need('type' in written || !Object.keys(written).length, path, 'no type');
    const t = written.type ?? 'none';
    const msg = this.jsonValue(this.FIELD.msg, t, path + '.type', this.msg);
    need(kind.types.includes(msg), path + '.type', `${t} is not for ${this.where(kind)} (${kind.types.map((i) => this.msg.json[i]).join(', ')})`);
    if (msg === this.NONE) {
      const extra = Object.keys(written).filter((k) => k !== 'type');
      need(!extra.length, path, `type none takes no other field (${extra.join(', ')})`);
      return this.emptyAssign();
    }
    let mode = kind.modeDefault;
    if ('mode' in written) mode = this.jsonValue(this.FIELD.mode, written.mode, path + '.mode', kind.modeEnum);
    const a = { msg, mode };
    for (const f of this.assignFields) {
      if (f.c === 'msg' || f.c === 'mode') continue;
      const keys = f.members ? f.members.map((m) => m.json) : [f.json];
      const applies = this.applies(f, kind, msg, mode);
      for (const k of keys) {
        need(!(k in written) || applies, path + '.' + k, `does not apply to ${this.msg.json[msg]} ${kind.name} in mode ${kind.modeEnum.json[mode]}`);
      }
      if (!applies) { a[f.c] = 0; continue; }
      if (f.members) {
        let v = 0;
        for (const m of f.members) {
          if (!(m.json in written)) continue;
          const w = written[m.json];
          const key = typeof w === 'boolean' ? JSON.stringify(w) : w;
          need(typeof key === 'string' && key in m.values, path + '.' + m.json, `${JSON.stringify(w)} is not one of ${Object.keys(m.values).join(', ')}`);
          v |= m.values[key];
        }
        a[f.c] = v;
      } else if (f.json in written) {
        a[f.c] = this.jsonValue(f, written[f.json], path + '.' + f.json, null, null, this.fieldMax(f, msg));
      } else {
        need(!f.required, path + '.' + f.json, `needed in mode ${kind.modeEnum.json[mode]}`);
        a[f.c] = this.def(f, kind, msg, mode, a);
      }
    }
    return Object.fromEntries(FIELD_ORDER.map((c) => [c, a[c]]));
  }

  controlFromJson(obj, kind, nav, path) {
    const ctl = this.emptyControl();
    if (obj === null || obj === undefined) return ctl;
    this.keysOk(obj, ['name', 'shift', ...this.JSON_KEYS], path);
    ctl.name = this.nameOf(obj, path);
    const written = Object.fromEntries(Object.entries(obj).filter(([k]) => this.JSON_KEYS.includes(k)));
    ctl.normal = this.resolve(kind, written, path);
    if (!('shift' in obj)) {
      [ctl.shiftKind, ctl.shift] = this.defaultShift(kind, ctl.normal);
      return ctl;
    }
    need(!nav, path + '.shift', 'the Shift layer of a navigation button is the firmware\'s (FR-SH-02)');
    const s = obj.shift;
    if (s === 'inherit' || s === 'block') {
      ctl.shiftKind = this.SHIFT.index(s);
      return ctl;
    }
    need(s !== null && typeof s === 'object' && !Array.isArray(s), path + '.shift', '"inherit", "block" or a layer of its own');
    this.keysOk(s, ['name', ...this.JSON_KEYS], path + '.shift');
    need('type' in s, path + '.shift', 'a layer of its own is written in full: its type at least');
    // its own name (only a layer of its own has one; names live only in the file, D-8)
    ctl.shiftName = this.nameOf(s, path + '.shift');
    ctl.shift = this.resolve(kind, Object.fromEntries(Object.entries(s).filter(([k]) => this.JSON_KEYS.includes(k))), path + '.shift');
    need(ctl.shift.msg !== this.NONE, path + '.shift', 'a layer that sends nothing: use "block"');
    ctl.shiftKind = this.OVERRIDE;
    return ctl;
  }

  // A configuration file's object to the model; CodecError on any fault.
  fromJson(obj) {
    const dev = this.device;
    const m = this.emptyConfig();
    this.keysOk(obj, ['schemaVersion', 'id', 'revision', 'name', 'global', 'presets'], '');
    need('schemaVersion' in obj, 'schemaVersion', 'missing');
    need(obj.schemaVersion === this.version, 'schemaVersion', versionText(obj.schemaVersion, this.version));
    m.name = this.nameOf(obj, '');
    if ('id' in obj) {
      need(typeof obj.id === 'string' && UUID_RE.test(obj.id), 'id', 'a UUID in lower case, 8-4-4-4-12 hex digits');
      m.id = Uint8Array.from(obj.id.replace(/-/g, '').match(/../g), (h) => parseInt(h, 16));
    }
    if ('revision' in obj) {
      need(isInt(obj.revision) && obj.revision >= 0 && obj.revision <= 0xFFFFFFFF, 'revision', 'a whole number 0-4294967295');
      m.revision = obj.revision;
    }
    const g = obj.global ?? {};
    this.keysOk(g, this.globalFields.map((f) => f.json), 'global');
    for (const f of this.globalFields) if (f.json in g) m.global[f.c] = this.jsonValue(f, g[f.json], 'global.' + f.json, this.enums[f.enum]);
    const presets = obj.presets ?? [];
    need(Array.isArray(presets), 'presets', 'a list is needed');
    need(presets.length <= dev.presets, 'presets', `${presets.length} presets; the device has ${dev.presets}`);
    for (const [p, po] of presets.entries()) {
      const path = `presets[${p}]`;
      const preset = m.presets[p];
      if (po === null) continue;
      this.keysOk(po, ['name', ...this.presetFields.map((f) => f.json), 'row', 'pages'], path);
      preset.name = this.nameOf(po, path);
      for (const f of this.presetFields) if (f.json in po) preset[f.c] = this.jsonValue(f, po[f.json], path + '.' + f.json, this.enums[f.enum]);
      const row = po.row ?? {};
      this.keysOk(row, this.ROW_KEYS, path + '.row');
      this.ROW_KEYS.forEach((k, i) => { preset.row[i] = this.controlFromJson(row[k], this.BUTTON, this.rowNav(i), path + '.row.' + k); });
      const pages = po.pages ?? [];
      need(Array.isArray(pages), path + '.pages', 'a list is needed');
      need(pages.length <= dev.pages, path + '.pages', `${pages.length} pages; the device has ${dev.pages}`);
      for (const [pg, pgo] of pages.entries()) {
        const ppath = `${path}.pages[${pg}]`;
        if (pgo === null) continue;
        this.keysOk(pgo, ['name', 'strips'], ppath);
        const page = preset.pages[pg];
        page.name = this.nameOf(pgo, ppath);
        const strips = pgo.strips ?? [];
        need(Array.isArray(strips) && strips.length <= dev.strips, ppath + '.strips', `a list of at most ${dev.strips} strips`);
        for (const [n, so] of strips.entries()) {
          const spath = `${ppath}.strips[${n}]`;
          if (so === null) continue;
          this.keysOk(so, this.STRIP_KEYS, spath);
          for (const k of this.STRIP_KEYS) page.strips[n][k] = this.controlFromJson(so[k], this.KIND_OF[k], false, spath + '.' + k);
        }
      }
    }
    this.checkModel(m);
    return m;
  }

  assignToJson(kind, a) {
    const out = {};
    const { msg, mode } = a;
    if (msg === this.NONE) return out;
    for (const f of this.assignFields) {
      if (!this.applies(f, kind, msg, mode) && f.c !== 'msg') continue;
      if (f.members) {
        for (const m of f.members) {
          const v = a.flags & m.mask;
          if (v !== 0) {
            const k = Object.keys(m.values).find((x) => m.values[x] === v);
            out[m.json] = k === 'true' ? true : k === 'false' ? false : k;
          }
        }
        continue;
      }
      const v = a[f.c];
      if (v === this.def(f, kind, msg, mode, a)) continue;
      out[f.json] = this.fileValue(f, v, this.fieldEnum(f, kind));
    }
    return out;
  }

  controlToJson(kind, nav, ctl) {
    const out = {};
    if (ctl.name !== null) out.name = ctl.name;
    Object.assign(out, this.assignToJson(kind, ctl.normal));
    const named = ctl.shiftKind === this.OVERRIDE && ctl.shiftName !== null && ctl.shiftName !== undefined;
    if (named || !this.sameShift(ctl, kind)) {
      out.shift = ctl.shiftKind === this.OVERRIDE
        ? { ...(named && { name: ctl.shiftName }), ...this.assignToJson(kind, ctl.shift) } : this.SHIFT.json[ctl.shiftKind];
    }
    return out;
  }

  // The canonical file object of a model.
  toJson(m) {
    const empty = (o) => !Object.keys(o).length;
    const trim = (items) => { while (items.length && empty(items[items.length - 1])) items.pop(); return items; };
    const out = { schemaVersion: this.version };
    if (m.id.some((b) => b)) {
      const h = Array.from(m.id, (b) => b.toString(16).padStart(2, '0')).join('');
      out.id = `${h.slice(0, 8)}-${h.slice(8, 12)}-${h.slice(12, 16)}-${h.slice(16, 20)}-${h.slice(20)}`;
    }
    if (m.revision) out.revision = m.revision;
    if (m.name !== null) out.name = m.name;
    const g = {};
    for (const f of this.globalFields) {
      const v = m.global[f.c];
      if (v !== this.def(f)) g[f.json] = this.fileValue(f, v, this.enums[f.enum]);
    }
    if (!empty(g)) out.global = g;
    const presets = [];
    for (const preset of m.presets) {
      const po = {};
      if (preset.name !== null) po.name = preset.name;
      for (const f of this.presetFields) {
        const v = preset[f.c];
        if (v !== this.def(f)) po[f.json] = this.fileValue(f, v, this.enums[f.enum]);
      }
      const row = {};
      this.ROW_KEYS.forEach((k, i) => {
        const c = this.controlToJson(this.BUTTON, this.rowNav(i), preset.row[i]);
        if (!empty(c)) row[k] = c;
      });
      if (!empty(row)) po.row = row;
      const pages = [];
      for (const page of preset.pages) {
        const pgo = {};
        if (page.name !== null) pgo.name = page.name;
        const strips = page.strips.map((strip) => {
          const so = {};
          for (const k of this.STRIP_KEYS) {
            const c = this.controlToJson(this.KIND_OF[k], false, strip[k]);
            if (!empty(c)) so[k] = c;
          }
          return so;
        });
        if (trim(strips).length) pgo.strips = strips;
        pages.push(pgo);
      }
      if (trim(pages).length) po.pages = pages;
      presets.push(po);
    }
    if (trim(presets).length) out.presets = presets;
    return out;
  }

  // --- the binary form --------------------------------------------------------------------------

  packFields(fields, values, mask, out) {
    fields.forEach((f, i) => {
      if (!((mask >> i) & 1)) return;
      const v = values[i];
      out.push(v & 0xFF);
      if (f.size === 2) out.push((v >> 8) & 0xFF);
    });
  }

  // The global record and its mask (0 when every setting is the default).
  globalRecord(m) {
    const gv = this.globalFields.map((f) => m.global[f.c]);
    let gmask = 0;
    this.globalFields.forEach((f, i) => { if (gv[i] !== this.def(f)) gmask |= 1 << i; });
    const payload = [gmask];
    this.packFields(this.globalFields, gv, gmask, payload);
    return [[this.tags.global, payload.length, ...payload], gmask];
  }

  // Preset p's record and its mask (0 when every setting is the default).
  presetRecord(p, preset) {
    const pv = this.presetFields.map((f) => preset[f.c]);
    let pmask = 0;
    this.presetFields.forEach((f, i) => { if (pv[i] !== this.def(f)) pmask |= 1 << i; });
    const payload = [p, pmask];
    this.packFields(this.presetFields, pv, pmask, payload);
    return [[this.tags.preset, payload.length, ...payload], pmask];
  }

  // A control's records in the canonical form: its Normal layer unless none, its Shift layer
  // unless the default; empty when both are.
  controlRecords(pg, cid, kind, ctl) {
    const vals = (a) => FIELD_ORDER.map((c) => a[c]);
    const out = [];
    const a = ctl.normal;
    if (a.msg !== this.NONE) {
      const v = vals(a);
      let mask = 0;
      this.assignFields.forEach((f, i) => { if (v[i] !== this.def(f, kind, a.msg, a.mode, a)) mask |= 1 << i; });
      const payload = [pg, cid, mask & 0xFF, mask >> 8];
      this.packFields(this.assignFields, v, mask, payload);
      out.push(this.tags.assign, payload.length, ...payload);
    }
    if (!this.sameShift(ctl, kind)) {
      const payload = [pg, cid, ctl.shiftKind];
      if (ctl.shiftKind === this.OVERRIDE) {
        const v = vals(ctl.shift), base = vals(a);
        let mask = 0;
        v.forEach((x, i) => { if (x !== base[i]) mask |= 1 << i; });
        payload.push(mask & 0xFF, mask >> 8);
        this.packFields(this.assignFields, v, mask, payload);
      }
      out.push(this.tags.shift, payload.length, ...payload);
    }
    return out;
  }

  // The canonical binary form of a model (checked first).
  encode(m) {
    this.checkModel(m);
    const body = [];
    const [g, gmask] = this.globalRecord(m);
    if (gmask) body.push(...g);
    for (const [p, preset] of m.presets.entries()) {
      const recs = [];
      for (const [pg, cid, kind, , ctl] of this.controls(preset)) recs.push(...this.controlRecords(pg, cid, kind, ctl));
      const [prec, pmask] = this.presetRecord(p, preset);
      if (pmask || recs.length) body.push(...prec, ...recs);
    }
    const rest = [...le32(m.revision), ...m.id, ...body];
    return Uint8Array.from([...this.magic, this.binaryVersion, this.headerSize, 0, 0, ...le32(body.length), ...le32(crc32(rest)), ...rest]);
  }

  // The live edits that turn model old into new (docs/config.md, "Edits"; codec.py's edits(), byte
  // for byte): a list of [preset, records], each at most `limit` bytes, for CFG_SET in this order.
  // Every setting record or control that differs is written whole, as the canonical form writes it
  // (a control that has none: a Normal layer of no fields, none; settings at their defaults: a
  // record of mask 0); names do not count. The global record goes first, with the first preset
  // that changes (else preset 0). [] when they play the same.
  edits(old, nu, limit = EDIT_MAX) {
    this.checkModel(nu);
    const same = (a, b) => a.length === b.length && a.every((x, i) => x === b[i]);
    const sameAssign = (a, b) => FIELD_ORDER.every((c) => a[c] === b[c]);
    const per = [];
    for (const [p, pn] of nu.presets.entries()) {
      const po = old.presets[p];
      const recs = [];
      const [prec] = this.presetRecord(p, pn);
      if (!same(prec, this.presetRecord(p, po)[0])) recs.push(prec);
      const co = [...this.controls(po)];
      for (const [i, [pg, cid, kind, , cn]] of [...this.controls(pn)].entries()) {
        const o = co[i][4];
        if (sameAssign(cn.normal, o.normal) && cn.shiftKind === o.shiftKind && sameAssign(cn.shift, o.shift)) continue;
        const r = this.controlRecords(pg, cid, kind, cn);
        recs.push(r.length ? r : [this.tags.assign, 4, pg, cid, 0, 0]);
      }
      if (recs.length) per.push([p, recs]);
    }
    const [g] = this.globalRecord(nu);
    if (!same(g, this.globalRecord(old)[0])) {
      if (per.length) per[0][1].unshift(g);
      else per.push([0, [g]]);
    }
    const out = [];
    for (const [p, recs] of per) {
      for (const r of recs) {
        if (r.length > limit) throw new Error(`a record of ${r.length} bytes: longer than an edit`);
        const last = out[out.length - 1];
        if (last && last[0] === p && last[1].length + r.length <= limit) last[1].push(...r);
        else out.push([p, [...r]]);
      }
    }
    return out.map(([p, r]) => [p, Uint8Array.from(r)]);
  }

  unpackFields(fields, mask, data, off, path) {
    const out = {};
    fields.forEach((f, i) => {
      if (!((mask >> i) & 1)) return;
      need(off + f.size <= data.length, path, 'the record ends inside its fields');
      out[i] = f.size === 1 ? data[off] : data[off] | (data[off + 1] << 8);
      off += f.size;
    });
    need(off === data.length, path, `${data.length - off} bytes after the fields`);
    return out;
  }

  // The binary form to the model (names empty); CodecError with the offset on any fault.
  decode(bytes) {
    const blob = Uint8Array.from(bytes);
    const H = this.headerSize;
    need(blob.length >= H, 'header', `${blob.length} bytes: shorter than the header`);
    need(this.magic.every((b, i) => blob[i] === b), 'header', 'not a configuration (magic)');
    const version = blob[4], hsize = blob[5], bodyLen = u32(blob, 8), crc = u32(blob, 12);
    need(version === this.binaryVersion, 'header', `binary form ${versionText(version, this.binaryVersion)}`);
    need(hsize >= H, 'header', `header size ${hsize}`);
    need(hsize + bodyLen === blob.length, 'header', `length ${hsize} + ${bodyLen}, but ${blob.length} bytes`);
    need(crc32(blob.subarray(16)) === crc, 'header', 'CRC mismatch');
    const m = this.emptyConfig();
    m.revision = u32(blob, 16);
    m.id = blob.slice(20, 36);
    let off = hsize, preset = null, pidx = -1, seenGlobal = false, last = null;
    const seenPresets = new Set(), seen = new Set();
    while (off < blob.length) {
      const here = `offset ${off}`;
      need(off + 2 <= blob.length, here, 'a record header past the end');
      const tag = blob[off], ln = blob[off + 1];
      const data = blob.subarray(off + 2, off + 2 + ln);
      need(data.length === ln, here, `a record of ${ln} bytes past the end`);
      off += 2 + ln;
      const prev = last;
      last = null;
      if (tag === this.tags.global) {
        need(!seenGlobal, here, 'a second global record');
        seenGlobal = true;
        need(ln >= 1, here, 'an empty global record');
        const mask = data[0];
        need(!(mask >> this.globalFields.length), here, `unknown global fields 0x${mask.toString(16)}`);
        const v = this.unpackFields(this.globalFields, mask, data, 1, here);
        for (const [i, x] of Object.entries(v)) m.global[this.globalFields[i].c] = x;
        for (const f of this.globalFields) this.checkSetting(f, m.global[f.c], `${here} global.${f.json}`, this.enums[f.enum]);
      } else if (tag === this.tags.preset) {
        need(ln >= 2, here, `a preset record of ${ln} bytes`);
        const p = data[0], mask = data[1];
        need(p < this.device.presets, here, `preset ${p + 1}; the device has ${this.device.presets}`);
        need(!seenPresets.has(p), here, `a second record of preset ${p + 1}`);
        seenPresets.add(p);
        need(!(mask >> this.presetFields.length), here, `unknown preset fields 0x${mask.toString(16)}`);
        const v = this.unpackFields(this.presetFields, mask, data, 2, here);
        preset = m.presets[p];
        pidx = p;
        this.presetFields.forEach((f, i) => {
          const x = i in v ? v[i] : this.def(f);
          this.checkSetting(f, x, `${here} presets[${p}].${f.json}`, this.enums[f.enum]);
          preset[f.c] = x;
        });
      } else if (tag === this.tags.assign || tag === this.tags.shift) {
        need(preset !== null, here, 'a control before any preset record');
        need(ln >= 2, here, `a control record of ${ln} bytes`);
        const page = data[0], cid = data[1];
        const at = this.controlAt(preset, page, cid);
        need(at !== null, here, `no control ${cid} on page ${page}`);
        const [kind, nav, ctl] = at;
        const path = `${here} ${this.pathOf(pidx, page, cid)}`;
        const key = `${pidx},${page},${cid}`;
        if (tag === this.tags.assign) {
          need(!seen.has(key + ',0'), path, 'a second Normal layer');
          need(!seen.has(key + ',1'), path, 'the Normal layer after the Shift layer');
          seen.add(key + ',0');
          need(ln >= 4, path, 'no field mask');
          const mask = data[2] | (data[3] << 8);
          need(!(mask >> FIELD_ORDER.length), path, `unknown fields 0x${mask.toString(16)}`);
          const v = this.unpackFields(this.assignFields, mask, data, 4, path);
          const msg = 0 in v ? v[0] : this.NONE;
          const mode = 3 in v ? v[3] : msg !== this.NONE ? kind.modeDefault : 0;
          const a = this.emptyAssign();
          a.msg = msg;
          a.mode = mode;
          if (kind.types.includes(msg) && mode < kind.modeEnum.length) {
            this.assignFields.forEach((f, i) => { a[f.c] = i in v ? v[i] : f.c === 'mode' ? mode : this.def(f, kind, msg, mode, a); });
          }
          this.checkAssign(kind, a, path);
          ctl.normal = a;
          last = key;
        } else {
          need(!seen.has(key + ',1'), path, 'a second Shift layer');
          need(!seen.has(key + ',0') || prev === key, path, 'the Shift layer apart from its Normal layer');
          seen.add(key + ',1');
          need(ln >= 3, path, 'no Shift kind');
          const k = data[2];
          need([this.INHERIT, this.OVERRIDE, this.BLOCK].includes(k), path, `Shift kind ${k}`);
          need(!nav, path, 'the Shift layer of a navigation button is the firmware\'s (FR-SH-02)');
          ctl.shiftKind = k;
          ctl.shift = this.emptyAssign();
          if (k === this.OVERRIDE) {
            need(ln >= 5, path, 'no field mask');
            const mask = data[3] | (data[4] << 8);
            need(!(mask >> FIELD_ORDER.length), path, `unknown fields 0x${mask.toString(16)}`);
            const v = this.unpackFields(this.assignFields, mask, data, 5, path);
            const s = { ...ctl.normal };
            for (const [i, x] of Object.entries(v)) s[FIELD_ORDER[i]] = x;
            ctl.shift = s;
          } else {
            need(ln === 3, path, `${ln - 3} bytes after an ${this.SHIFT.json[k]}`);
          }
        }
      } else {
        need(tag & this.optional, here, `unknown record type ${tag}`);
      }
    }
    for (const [p, pr] of m.presets.entries()) {
      for (const [pg, cid, kind, nav, ctl] of this.controls(pr)) {
        if (!seen.has(`${p},${pg},${cid},1`)) [ctl.shiftKind, ctl.shift] = this.defaultShift(kind, ctl.normal);
        this.checkControl(kind, nav, ctl, this.pathOf(p, pg, cid));
      }
    }
    return m;
  }

  // --- the editor's two calls -------------------------------------------------------------------

  fileToBinary(file) { return this.encode(this.fromJson(file)); }
  binaryToFile(bytes) { return this.toJson(this.decode(bytes)); }
}

// A CFG_SET's records: the link's 192-byte command less kind, seq, cmd, revision, preset.
export const EDIT_MAX = 184;

const le32 = (v) => [v & 0xFF, (v >>> 8) & 0xFF, (v >>> 16) & 0xFF, (v >>> 24) & 0xFF];
const u32 = (p, at) => (p[at] | (p[at + 1] << 8) | (p[at + 2] << 16) | (p[at + 3] << 24)) >>> 0;
