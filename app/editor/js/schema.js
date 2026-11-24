// The configuration schema, read from config/schema.json at run time (SPEC D-4): our own
// description, not a JSON Schema. This module answers what the editor asks of it: which types a
// kind of control takes, which fields apply to a layer, their ranges and defaults, and whether a
// file is valid. The rules follow tools/smcconf (schema.py, codec.py); where the editor checks
// less than the tools, the tools stay the reference.

// A version this editor does not read, said so that a musician knows what to update (Q-29; the same
// words as tools/smcconf/codec.py's version_text). who: 'editor' or 'tool'.
export function versionText(found, reads, who = 'editor') {
  if (Number.isInteger(found) && found > reads) return `${found}: made by a newer version of better-smc-mixer; this ${who} reads version ${reads}: update the ${who}`;
  return `${JSON.stringify(found)}: this ${who} reads version ${reads}`;
}

export class Schema {
  constructor(raw) {
    this.raw = raw;
    this.version = raw.schemaVersion;
    this.device = raw.device;
    this.enums = {};
    for (const e of raw.enums) this.enums[e.name] = e;
    this.kinds = {};
    for (const k of raw.kinds) this.kinds[k.name] = k;
    this.stripControls = raw.strip.controls;     // [{json, kind}]
    this.rowButtons = raw.row.buttons;           // [{json, nav?}]
    this.presetFields = raw.preset.fields;
    this.globalFields = raw.global.fields;
    this.nameMax = raw.file.nameMax;
    // An assignment's fields, with the button flags' members flattened: [{json, def, field}].
    this.layerFields = [];
    for (const f of raw.assignment.fields) {
      if (f.members) {
        for (const m of f.members) this.layerFields.push({ json: m.json, member: m, field: f });
      } else {
        this.layerFields.push({ json: f.json, field: f });
      }
    }
  }

  msg(type) { return this.enums.msg.values.find((v) => v.json === type); }
  msgDoc(type) { return this.msg(type)?.doc ?? ''; }
  typeMax(type) { return this.msg(type)?.max ?? 0; }
  numberMax(type) { return this.msg(type)?.numberMax; }
  hasNumber(type) { return this.numberMax(type) !== undefined; }
  hasValue(type) { return this.msg(type)?.max !== undefined; }
  is14(type) { return this.typeMax(type) > 127; }

  kindOfStrip(key) { return this.stripControls.find((c) => c.json === key)?.kind; }
  isNav(rowKey) { return !!this.rowButtons.find((b) => b.json === rowKey)?.nav; }
  modeEnum(kind) { return this.enums[this.kinds[kind].modeEnum]; }
  modeDefault(kind) { return this.kinds[kind].modeDefault; }
  types(kind) { return this.kinds[kind].types; }

  fieldDef(json) { return this.layerFields.find((f) => f.json === json); }

  applies(json, kind, type, mode) {
    const a = this.fieldDef(json)?.field.applies;
    if (!a) return true;
    if (a.types === 'notNone' && type === 'none') return false;
    if (a.types === 'withNumber' && !this.hasNumber(type)) return false;
    if (a.types === 'withValue' && !this.hasValue(type)) return false;
    if (a.kinds && !a.kinds.includes(kind)) return false;
    if (a.modes && !a.modes.includes(mode)) return false;
    return true;
  }

  // The value a layer has for a field: its own, else the default. Undefined for a field that
  // does not apply, or for a required one that is missing (n).
  value(layer, json, kind) {
    const type = layer.type ?? 'none';
    const mode = layer.mode ?? this.modeDefault(kind);
    if (json === 'type') return type;
    if (!this.applies(json, kind, type, mode)) return undefined;
    if (layer[json] !== undefined) return layer[json];
    const d = this.fieldDef(json);
    if (d.member) return d.member.default;
    const def = d.field.default;
    if (def === 'kindMode') return this.modeDefault(kind);
    if (def === 'typeMax') return this.typeMax(type);
    if (def === 'middle') {
      const lo = this.value(layer, 'lo', kind), hi = this.value(layer, 'hi', kind);
      return Math.floor((lo + hi + 1) / 2);
    }
    return def;
  }

  // Range of a numeric field for a type: [min, max].
  range(json, type) {
    const f = this.fieldDef(json).field;
    const max = f.max === 'numberMax' ? this.numberMax(type) : f.max === 'typeMax' ? this.typeMax(type) : f.max;
    return [f.min ?? 0, max];
  }

  // ---- checks (FR-APP-22: every fault names its path) ----------------------------------------

  checkLayer(layer, kind, path, errs, isShift) {
    if (typeof layer !== 'object' || layer === null || Array.isArray(layer)) {
      errs.push(`${path}: not an object`);
      return;
    }
    const allowed = new Set(['type', 'name', ...this.layerFields.map((f) => f.json)]);
    if (!isShift) allowed.add('shift');
    for (const k of Object.keys(layer)) if (!allowed.has(k)) errs.push(`${path}.${k}: unknown field`);
    const fieldKeys = Object.keys(layer).filter((k) => k !== 'name' && k !== 'shift');
    if (fieldKeys.length && layer.type === undefined) {
      errs.push(`${path}: a control with fields must have a type`);
      return;
    }
    const type = layer.type ?? 'none';
    if (!this.types(kind).includes(type)) {
      errs.push(`${path}.type: ${JSON.stringify(type)} is not a type a ${kind} takes`);
      return;
    }
    if (isShift && type === 'none') errs.push(`${path}: to send nothing under Shift, write "block"`);
    const modes = this.modeEnum(kind).values.map((v) => v.json);
    const mode = layer.mode ?? this.modeDefault(kind);
    if (layer.mode !== undefined && !modes.includes(layer.mode)) errs.push(`${path}.mode: ${JSON.stringify(layer.mode)} is not a ${kind} mode`);
    for (const k of fieldKeys) {
      if (k === 'type' || !allowed.has(k)) continue;
      if (!this.applies(k, kind, type, mode)) { errs.push(`${path}.${k}: does not apply here`); continue; }
      const v = layer[k];
      const d = this.fieldDef(k);
      if (d.member) {
        const ok = Object.keys(d.member.values).map((x) => (x === 'true' ? true : x === 'false' ? false : x));
        if (!ok.includes(v)) errs.push(`${path}.${k}: ${JSON.stringify(v)} is not one of ${ok.join(', ')}`);
        continue;
      }
      if (k === 'mode') continue;
      if (k === 'channel' && v === 'preset') continue;
      const [min, max] = this.range(k, type);
      if (!Number.isInteger(v) || v < min || v > max) errs.push(`${path}.${k}: ${JSON.stringify(v)} is outside ${min}-${max}`);
    }
    if (this.applies('n', kind, type, mode) && layer.n === undefined) errs.push(`${path}.n: needed in mode ${mode}`);
  }

  checkControl(obj, kind, path, errs, nav) {
    if (obj === null) return;
    this.checkLayer(obj, kind, path, errs, false);
    if (obj.name !== undefined) this.checkName(obj.name, path + '.name', errs);
    if (obj.shift === undefined) return;
    if (nav) { errs.push(`${path}.shift: the Shift layer of a navigation button is the firmware's (FR-SH-02)`); return; }
    if (obj.shift === 'inherit' || obj.shift === 'block') return;
    if (typeof obj.shift === 'string') { errs.push(`${path}.shift: "inherit", "block" or a layer of its own`); return; }
    if (obj.shift && obj.shift.type === undefined) { errs.push(`${path}.shift: a layer of its own is written in full: its type at least`); return; }
    this.checkLayer(obj.shift, kind, path + '.shift', errs, true);
    if (obj.shift.name !== undefined) this.checkName(obj.shift.name, path + '.shift.name', errs);
  }

  checkName(v, path, errs) {
    if (typeof v !== 'string') errs.push(`${path}: not a string`);
    else if ([...v].length > this.nameMax) errs.push(`${path}: longer than ${this.nameMax} characters`);
  }

  checkSettings(obj, fields, path, errs) {
    for (const [k, v] of Object.entries(obj)) {
      const f = fields.find((x) => x.json === k);
      if (!f) { errs.push(`${path}.${k}: unknown field`); continue; }
      if (f.enum) {
        const ok = this.enums[f.enum].values.map((x) => x.json).filter((x) => !(f.exclude || []).includes(x));
        if (!ok.includes(v)) errs.push(`${path}.${k}: ${JSON.stringify(v)} is not one of ${ok.join(', ')}`);
      } else if (!Number.isInteger(v) || v < f.min || v > f.max) {
        errs.push(`${path}.${k}: ${JSON.stringify(v)} is outside ${f.min}-${f.max}`);
      }
    }
  }

  // A whole configuration file. Returns a list of faults; empty when valid.
  checkFile(file) {
    const errs = [];
    if (typeof file !== 'object' || file === null) return ['(file): not a JSON object'];
    const top = ['schemaVersion', 'id', 'revision', 'name', 'global', 'presets'];
    for (const k of Object.keys(file)) if (!top.includes(k)) errs.push(`${k}: unknown field`);
    if (file.schemaVersion !== this.version) errs.push(`schemaVersion: ${versionText(file.schemaVersion, this.version)}`);
    if (file.id !== undefined && !/^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/.test(file.id)) errs.push('id: not a UUID');
    if (file.revision !== undefined && !(Number.isInteger(file.revision) && file.revision >= 0 && file.revision <= 4294967295)) errs.push('revision: outside 0-4294967295');
    if (file.name !== undefined) this.checkName(file.name, 'name', errs);
    if (file.global !== undefined) this.checkSettings(file.global, this.globalFields, 'global', errs);
    const presets = file.presets ?? [];
    if (presets.length > this.device.presets) errs.push(`presets: ${presets.length}, the device has ${this.device.presets}`);
    presets.forEach((p, pi) => {
      const pp = `presets[${pi}]`;
      if (p === null) return;
      const { name, row, pages, ...settings } = p;
      if (name !== undefined) this.checkName(name, pp + '.name', errs);
      this.checkSettings(settings, this.presetFields, pp, errs);
      for (const [k, c] of Object.entries(row ?? {})) {
        if (!this.rowButtons.find((b) => b.json === k)) { errs.push(`${pp}.row.${k}: no such button`); continue; }
        this.checkControl(c, 'button', `${pp}.row.${k}`, errs, this.isNav(k));
      }
      const pg = pages ?? [];
      if (pg.length > this.device.pages) errs.push(`${pp}.pages: ${pg.length}, the device has ${this.device.pages}`);
      pg.forEach((page, gi) => {
        if (page === null) return;
        const gp = `${pp}.pages[${gi}]`;
        if (page.name !== undefined) this.checkName(page.name, gp + '.name', errs);
        const strips = page.strips ?? [];
        if (strips.length > this.device.strips) errs.push(`${gp}.strips: ${strips.length}, the device has ${this.device.strips}`);
        strips.forEach((s, si) => {
          if (s === null) return;
          for (const [k, c] of Object.entries(s)) {
            const kind = this.kindOfStrip(k);
            if (!kind) { errs.push(`${gp}.strips[${si}].${k}: no such control`); continue; }
            this.checkControl(c, kind, `${gp}.strips[${si}].${k}`, errs, false);
          }
        });
      });
    });
    return errs;
  }
}
