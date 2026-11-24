// The configuration being edited: the file's own JSON (docs/config.md, "Files"), filled out so
// every preset, page, strip and control exists, plus the edits the editor makes on it and the
// canonical form it writes back (as tools/smcconf/codec.py: defaults left out, fixed key order,
// a control on one line).

export const SHIFT_STATES = ['fine', 'inherit', 'own', 'block'];

export class Config {
  constructor(schema, file) {
    this.S = schema;
    this.file = this.fill(structuredClone(file));
  }

  fill(f) {
    const S = this.S;
    f.schemaVersion = S.version;
    f.global ??= {};
    f.presets ??= [];
    for (let p = 0; p < S.device.presets; p++) {
      const pr = (f.presets[p] ??= {});
      pr.row ??= {};
      for (const b of S.rowButtons) pr.row[b.json] ??= {};
      pr.pages ??= [];
      for (let g = 0; g < S.device.pages; g++) {
        const pg = (pr.pages[g] ??= {});
        pg.strips ??= [];
        for (let s = 0; s < S.device.strips; s++) {
          const st = (pg.strips[s] ??= {});
          for (const c of S.stripControls) st[c.json] ??= {};
        }
      }
    }
    return f;
  }

  // ---- addressing --------------------------------------------------------------------------
  // A control's id within a page: "s3.fader" (strips 0-7) or "row.play".

  static ids(S) {
    const out = [];
    for (let s = 0; s < S.device.strips; s++) for (const c of S.stripControls) out.push(`s${s}.${c.json}`);
    for (const b of S.rowButtons) out.push(`row.${b.json}`);
    return out;
  }

  kind(id) { return id.startsWith('row.') ? 'button' : this.S.kindOfStrip(id.split('.')[1]); }
  isNav(id) { return id.startsWith('row.') && this.S.isNav(id.slice(4)); }
  strip(id) { return id.startsWith('row.') ? -1 : Number(id.slice(1, id.indexOf('.'))); }
  key(id) { return id.split('.')[1]; }

  control(p, g, id) {
    const pr = this.file.presets[p];
    if (id.startsWith('row.')) return pr.row[id.slice(4)];
    return pr.pages[g].strips[this.strip(id)][this.key(id)];
  }
  setControl(p, g, id, obj) {
    const pr = this.file.presets[p];
    if (id.startsWith('row.')) pr.row[id.slice(4)] = obj;
    else pr.pages[g].strips[this.strip(id)][this.key(id)] = obj;
  }

  presetChannel(p) { return this.file.presets[p].channel ?? 1; }
  presetSetting(p, k) { const f = this.S.presetFields.find((x) => x.json === k); return this.file.presets[p][k] ?? f.default; }
  globalSetting(k) { const f = this.S.globalFields.find((x) => x.json === k); return this.file.global[k] ?? f.default; }

  // ---- the Shift layer ---------------------------------------------------------------------

  shiftState(ctl, kind) {
    const sh = ctl.shift;
    if (sh === 'inherit') return 'inherit';
    if (sh === 'block') return 'block';
    if (sh && typeof sh === 'object') return 'own';
    if (kind === 'encoder' && (ctl.type ?? 'none') !== 'none') return 'fine';
    return 'inherit';
  }

  // The layer the core runs under Shift, or null for nothing.
  shiftLayer(ctl, kind) {
    switch (this.shiftState(ctl, kind)) {
      case 'inherit': return this.normal(ctl);
      case 'block': return null;
      case 'fine': { const l = this.normal(ctl); l.mode = 'fine'; delete l.n; return l; }
      default: { const { name, ...l } = ctl.shift; return structuredClone(l); }
    }
  }

  normal(ctl) { const { name, shift, ...l } = ctl; return structuredClone(l); }

  setShiftState(ctl, kind, state) {
    if (state === 'own') {
      if (!(ctl.shift && typeof ctl.shift === 'object')) {
        // a name of the Shift layer lives on it: it starts without one (the control's name shows)
        const l = this.shiftLayer(ctl, kind) ?? this.normal(ctl);
        if (!l.type || l.type === 'none') { l.type = this.S.types(kind)[1]; }
        ctl.shift = this.cleanLayer(l, kind);
      }
    } else if (state === 'block') ctl.shift = 'block';
    else if (state === 'fine') delete ctl.shift;
    else if (state === 'inherit') { if (kind === 'encoder') ctl.shift = 'inherit'; else delete ctl.shift; }
  }

  // ---- names (they live only in the file, D-8) ----------------------------------------------

  // The name of a layer: a Shift layer of its own has its own (none until given; the control's
  // would name something it does not do); any other Shift layer plays the Normal one, named so.
  nameOn(ctl, kind, layer) {
    if (layer === 'shift' && this.canRename(ctl, kind, 'shift')) return ctl.shift.name;
    return ctl.name;
  }
  // Under Shift, only a layer of its own does something else, so only it takes another name.
  canRename(ctl, kind, layer) { return layer === 'normal' || this.shiftState(ctl, kind) === 'own'; }
  rename(ctl, kind, layer, name) {
    const target = layer === 'shift' && this.canRename(ctl, kind, 'shift') ? ctl.shift : ctl;
    if (name) target.name = name; else delete target.name;
    return ctl;
  }

  // ---- one layer at a time (copy, paste, clear: the panel shows one layer) -------------------

  // What a layer of a control plays: its fields only (no name, no Shift), or null for nothing.
  layerOf(ctl, kind, layer) {
    if (layer === 'normal') { const l = this.normal(ctl); return (l.type ?? 'none') === 'none' ? null : l; }
    return this.shiftLayer(ctl, kind);
  }

  // Puts a layer (from layerOf, maybe of another control or kind) onto one layer of ctl. The other
  // layer and the name stay; fields this kind cannot take go. null: that layer sends nothing.
  putLayer(ctl, kind, layer, src) {
    const fields = src && (src.type ?? 'none') !== 'none' && this.S.types(kind).includes(src.type) ? structuredClone(src) : null;
    if (fields) {
      // One field, mode, means a different thing on each kind: keep it only if this kind has it.
      if (fields.mode !== undefined && !this.S.modeEnum(kind).values.some((v) => v.json === fields.mode)) delete fields.mode;
      this.cleanLayer(fields, kind);
    }
    if (layer === 'shift') {
      // the Shift layer's own name stays with it, as the control's name stays with the control
      const keep = ctl.shift && typeof ctl.shift === 'object' ? ctl.shift.name : undefined;
      ctl.shift = fields ? { ...(keep !== undefined && { name: keep }), ...fields } : 'block';
      return ctl;
    }
    for (const k of Object.keys(ctl)) if (k !== 'name' && k !== 'shift') delete ctl[k];
    if (fields) Object.assign(ctl, fields);
    // An encoder's default Shift (fine) needs a type; one that sends nothing has nothing to refine.
    if (!fields && ctl.shift === undefined && kind === 'encoder') delete ctl.shift;
    return ctl;
  }

  // ---- edits -------------------------------------------------------------------------------

  // Sets one field of a layer and keeps the layer valid: fields that stop applying go, numbers
  // are clamped into the new type's range, a required n gets a value.
  setField(layer, kind, json, value) {
    const S = this.S;
    if (json === 'type') {
      const oldType = layer.type ?? 'none';
      if (value === 'none') {
        for (const k of Object.keys(layer)) if (k !== 'name' && k !== 'shift') delete layer[k];
        return layer;
      }
      layer.type = value;
      if (S.typeMax(oldType) !== S.typeMax(value)) { delete layer.lo; delete layer.hi; delete layer.start; }
      if (S.hasNumber(value)) {
        const max = S.numberMax(value);
        if ((layer.number ?? 0) > max) layer.number = max;
      }
    } else if (value === undefined || value === null || value === '') {
      delete layer[json];
    } else {
      layer[json] = value;
    }
    return this.cleanLayer(layer, kind);
  }

  cleanLayer(layer, kind) {
    const S = this.S;
    const type = layer.type ?? 'none';
    const mode = layer.mode ?? S.modeDefault(kind);
    for (const f of S.layerFields) {
      if (layer[f.json] !== undefined && !S.applies(f.json, kind, type, mode)) delete layer[f.json];
    }
    if (S.applies('n', kind, type, mode) && layer.n === undefined) layer.n = 2;
    for (const k of ['number', 'lo', 'hi', 'start']) {
      if (typeof layer[k] === 'number') {
        const [min, max] = S.range(k, type);
        layer[k] = Math.max(min, Math.min(max, Math.round(layer[k])));
      }
    }
    return layer;
  }

  // ---- canonical form ----------------------------------------------------------------------

  canonLayer(layer, kind) {
    const S = this.S;
    const out = {};
    const type = layer.type ?? 'none';
    if (type === 'none') return out;
    out.type = type;
    for (const f of S.layerFields) {
      if (f.json === 'type') continue;
      const v = layer[f.json];
      if (v === undefined) continue;
      const def = S.value({ ...layer, [f.json]: undefined }, f.json, kind);
      if (f.json !== 'n' && v === def) continue;
      out[f.json] = v;
    }
    return out;
  }

  canonControl(ctl, kind, nav) {
    const out = {};
    if (ctl.name) out.name = ctl.name;
    Object.assign(out, this.canonLayer(ctl, kind));
    if (nav) return out;
    const st = this.shiftState(ctl, kind);
    const hasType = (ctl.type ?? 'none') !== 'none';
    const def = kind === 'encoder' && hasType ? 'fine' : 'inherit';
    if (st === 'own') {
      const own = this.canonLayer(ctl.shift, kind);
      const fine = this.canonLayer(this.shiftLayer({ ...ctl, shift: undefined }, kind) ?? {}, kind);
      if (ctl.shift.name) out.shift = { name: ctl.shift.name, ...own };
      else if (!(def === 'fine' && JSON.stringify(own) === JSON.stringify(fine))) out.shift = own;
    } else if (st !== def && st !== 'fine') {
      out.shift = st;
    }
    return out;
  }

  // The canonical file object. scope: {preset} or {preset, page} for a partial export, which
  // is still a whole configuration file with one preset (and one page).
  canonical(scope = {}) {
    const S = this.S, f = this.file;
    const out = { schemaVersion: S.version };
    if (f.id) out.id = f.id;
    if (f.revision) out.revision = f.revision;
    if (f.name) out.name = f.name;
    const g = {};
    for (const fl of S.globalFields) if (f.global[fl.json] !== undefined && f.global[fl.json] !== fl.default) g[fl.json] = f.global[fl.json];
    if (Object.keys(g).length) out.global = g;
    const presets = [];
    const pIdx = scope.preset !== undefined ? [scope.preset] : [...f.presets.keys()];
    for (const p of pIdx) {
      const pr = f.presets[p];
      const po = {};
      if (pr.name) po.name = pr.name;
      for (const fl of S.presetFields) if (pr[fl.json] !== undefined && pr[fl.json] !== fl.default) po[fl.json] = pr[fl.json];
      if (scope.page === undefined) {
        const row = {};
        for (const b of S.rowButtons) {
          const c = this.canonControl(pr.row[b.json], 'button', !!b.nav);
          if (Object.keys(c).length) row[b.json] = c;
        }
        if (Object.keys(row).length) po.row = row;
      }
      const pages = [];
      const gIdx = scope.page !== undefined ? [scope.page] : [...pr.pages.keys()];
      for (const gi of gIdx) {
        const pg = pr.pages[gi];
        const pgo = {};
        if (pg.name) pgo.name = pg.name;
        const strips = pg.strips.map((st) => {
          const so = {};
          for (const c of S.stripControls) {
            const cc = this.canonControl(st[c.json], c.kind, false);
            if (Object.keys(cc).length) so[c.json] = cc;
          }
          return so;
        });
        trim(strips);
        if (strips.length) pgo.strips = strips;
        pages.push(pgo);
      }
      trim(pages);
      if (pages.length) po.pages = pages;
      presets.push(po);
    }
    trim(presets);
    if (presets.length) out.presets = presets;
    return out;
  }
}

function trim(list) {
  while (list.length && !Object.keys(list[list.length - 1]).length) list.pop();
}

// Stable formatting (codec.py, dumps): an object or list of plain values on one line, others
// indented, so each control is one line in a diff.
export function dumps(obj) {
  const flat = (v) => typeof v !== 'object' || v === null || Object.values(v).every((x) => typeof x !== 'object' || x === null);
  const one = (v) => {
    if (typeof v !== 'object' || v === null) return JSON.stringify(v);
    if (Array.isArray(v)) return '[' + v.map((x) => JSON.stringify(x)).join(', ') + ']';
    return '{' + Object.entries(v).map(([k, x]) => `${JSON.stringify(k)}: ${JSON.stringify(x)}`).join(', ') + '}';
  };
  const fmt = (v, ind) => {
    if (flat(v)) return one(v);
    const pad = '  '.repeat(ind + 1);
    if (Array.isArray(v)) return '[\n' + v.map((x) => pad + fmt(x, ind + 1)).join(',\n') + '\n' + '  '.repeat(ind) + ']';
    return '{\n' + Object.entries(v).map(([k, x]) => `${pad}${JSON.stringify(k)}: ${fmt(x, ind + 1)}`).join(',\n') + '\n' + '  '.repeat(ind) + '}';
  };
  return fmt(obj, 0) + '\n';
}
