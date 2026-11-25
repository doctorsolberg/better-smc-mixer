// The editor's side of the contract tests (docs/integration.md, T-1): what the editor's own modules
// (app/editor/js) say, against the shared vectors the firmware's side writes
// (tools/editortest/vectors.py). Plain ES modules with no I/O of their own: run.mjs reads the files
// and runs these in Node (in the build image, tools/env/test_editor.sh).

import { Config, dumps } from '../../app/editor/js/config.js';
import {
  messages, buttonValue, targetKey, resolveChannel, ParamSelection, isParam, faderValue, FADER_MAX,
  encoderDelta, encoderValue, hexBytes,
} from '../../app/editor/js/midi.js';

// ---- files ------------------------------------------------------------------------------------

// A file through the editor's reader and writer: null when it comes back the same, else the
// first line (from 1) that differs.
export function roundtrip(S, text) {
  const out = dumps(new Config(S, JSON.parse(text)).canonical());
  if (out === text) return null;
  const a = text.split('\n'), b = out.split('\n');
  let line = 0;
  while (line < a.length && a[line] === b[line]) line++;
  return line + 1;
}

// faults.json's cases: those whose field the editor does not name, with what it said.
export function faults(S, cases) {
  const bad = [];
  for (const c of cases) {
    const errs = S.checkFile(c.file);
    if (!errs.some((e) => e.slice(0, e.indexOf(': ')) === c.path)) bad.push({ file: c.file, path: c.path, editor: errs });
  }
  return bad;
}

// ---- generated configurations -------------------------------------------------------------------

// Every type of every kind in every mode, on the preset's channel and a fixed one, with ranges
// whole, inverted and partial, and the Shift layer's forms; spread over as many files as needed.
function specs(S, kind) {
  const out = [];
  const modes = S.modeEnum(kind).values.map((v) => v.json);
  let i = 0;
  for (const type of S.types(kind)) {
    if (type === 'none') continue;
    for (const mode of modes) {
      for (const channel of ['preset', 3 + (i % 5)]) {
        out.push({ type, mode, channel, i: i++ });
        if (kind === 'button' && type !== 'program') out.push({ type, mode, channel, noRelease: true, i: i++ });
      }
    }
  }
  return out;
}

const NUMBERS = {
  cc7: [0, 7, 98, 99, 100, 101, 127, 74, 6, 38], cc14: [0, 1, 31, 7], note: [0, 60, 127, 36],
  nrpn: [0, 1, 127, 128, 0x1234, 16383, 99], rpn: [0, 1, 2, 16383, 5],
  relative2c: [10, 99], relativeBinOffset: [11, 101], relativeSignedBit: [12, 0],
};

function fill(S, cfg, kind, layer, sp, shiftSeed) {
  const set = (k, v) => cfg.setField(layer, kind, k, v);
  set('type', sp.type);
  set('mode', sp.mode);
  if (S.applies('n', kind, sp.type, sp.mode)) set('n', [2, 3, 7, 127][sp.i % 4]);
  set('channel', sp.channel);
  if (S.hasNumber(sp.type)) { const ns = NUMBERS[sp.type]; set('number', ns[sp.i % ns.length]); }
  if (S.hasValue(sp.type)) {
    const max = S.typeMax(sp.type);
    const r = [[undefined, undefined], [max, 0], [Math.round(max * 0.1), Math.round(max * 0.8)], [3, max]][(sp.i + shiftSeed) % 4];
    if (sp.type === 'note' && kind === 'button') r[0] = [0, 0, 0, 20][(sp.i + shiftSeed) % 4];
    set('lo', r[0]); set('hi', r[1]);
    if (kind === 'encoder' && sp.i % 3 === 1) set('start', max);
  }
  if (sp.noRelease) set('noRelease', true);
  if (kind === 'button') set('led', ['value', 'on', 'off'][sp.i % 3]);
}

// The configurations the editor's edit code writes: [{name, text}] (gen-1.json, …).
export function generate(S) {
  const ids = Config.ids(S);
  const slots = { fader: [], encoder: [], button: [] };
  for (let p = 0; p < S.device.presets; p++) {
    for (let g = 0; g < S.device.pages; g++) {
      for (const id of ids) {
        if (id.startsWith('row.') && g > 0) continue;
        slots[id.startsWith('row.') ? 'button' : S.kindOfStrip(id.split('.')[1])].push([p, g, id]);
      }
    }
  }
  const all = { fader: specs(S, 'fader'), encoder: specs(S, 'encoder'), button: specs(S, 'button') };
  const files = Math.max(...Object.keys(all).map((k) => Math.ceil(all[k].length / slots[k].length)));
  const out = [];
  for (let n = 0; n < Math.max(files, 2); n++) {
    const cfg = new Config(S, { schemaVersion: S.version, name: `generated ${n + 1}`, revision: 100 + n });
    // presets: the defaults; a channel, Note Off and the null parameter; latching; hybrid
    const ps = [{}, { channel: 5, noteOff: '8n', paramNull: 'gesture', shiftMode: 'latch' },
      { channel: 16, paramNull: 'gesture', shiftMode: 'hybrid' }, { channel: 2, shiftMode: 'hold' }];
    ps.forEach((o, p) => Object.assign(cfg.file.presets[p], o));
    if (n % 2) Object.assign(cfg.file.global, { pickup: 'jump', layerMode: 'immediate', fine14: 'lsb' });
    for (const kind of Object.keys(all)) {
      const list = all[kind];
      slots[kind].forEach(([p, g, id], k) => {
        const sp = list[(n * slots[kind].length + k) % list.length];
        const ctl = cfg.control(p, g, id);
        fill(S, cfg, kind, ctl, sp, n);
        if (cfg.isNav(id)) return;
        // the Shift layer: the default, its own (another spec), nothing, the Normal layer
        const sh = (sp.i + n) % 4;
        if (sh === 1) {
          cfg.setShiftState(ctl, kind, 'own');
          fill(S, cfg, kind, ctl.shift, list[(sp.i * 7 + 3) % list.length], n + 1);
        } else if (sh === 2) cfg.setShiftState(ctl, kind, 'block');
        else if (sh === 3) cfg.setShiftState(ctl, kind, 'inherit');
      });
    }
    out.push({ name: `gen-${n + 1}.json`, text: dumps(cfg.canonical()) });
  }
  return out;
}

// ---- sends: the core's messages against the editor's ---------------------------------------------

const hexes = (list) => (list ?? []).map((m) => m.split(' ').map((x) => parseInt(x, 16)));

// The values a layer's messages carry, in order (what the core chose to send; the editor then
// says how each is sent).
function valuesIn(layer, ch, msgs) {
  const t = layer.type, n = layer.number ?? 0, c = ch - 1, vals = [];
  for (let i = 0; i < msgs.length; i++) {
    const [s, a, b] = msgs[i];
    if (t === 'cc7' && s === 0xB0 + c && a === n) vals.push(b);
    else if (t === 'cc14' && s === 0xB0 + c && a === n) vals.push((b << 7) | (msgs[i + 1]?.[1] === n + 32 ? msgs[i + 1][2] : 0));
    else if ((t === 'nrpn' || t === 'rpn') && s === 0xB0 + c && a === 6) vals.push((b << 7) | (msgs[i + 1]?.[1] === 38 ? msgs[i + 1][2] : 0));
    else if (t === 'pitchbend' && s === 0xE0 + c) vals.push(a | (b << 7));
    else if (t === 'note' && (s === 0x90 + c || s === 0x80 + c) && a === n) vals.push(s === 0x80 + c ? 0 : b);
    else if (t === 'program' && s === 0xC0 + c) vals.push(a);
    else if (t === 'aftertouch' && s === 0xD0 + c) vals.push(a);
    else if (t.startsWith('relative') && s === 0xB0 + c && a === n) {
      vals.push(t === 'relative2c' ? (b < 64 ? b : b - 128) : t === 'relativeBinOffset' ? b - 64 : b < 64 ? b : 64 - b);
    }
  }
  return vals;
}

// The channels' selection after what the core sent (each phase starts from the truth, so one
// fault does not spread to the probes after it).
function resync(sel, msgs) {
  for (let i = 0; i < msgs.length; i++) {
    const [s, a, b] = msgs[i];
    if ((s & 0xF0) !== 0xB0 || a < 98 || a > 101) continue;
    const c = s & 15, next = msgs[i + 1];
    if ((a === 99 || a === 101) && next && next[0] === s && next[1] === a - 1) {
      sel.out[c] = `${a === 99 ? 'nrpn' : 'rpn'}:${(b << 7) | next[2]}`;
      i++;
    } else sel.out[c] = null;
  }
}

function layerOf(cfg, p, g, id, shift) {
  const ctl = cfg.control(p, g, id), kind = cfg.kind(id);
  return shift ? cfg.shiftLayer(ctl, kind) : cfg.normal(ctl);
}

// The phases each kind's probe has (vectors.py, plan), besides enter, exit and after.
const PHASES = { fader: ['up', 'down', 'end'], encoder: ['cw', 'cwEnd', 'ccw', 'ccwEnd'], button: ['press', 'release', 'end'] };

// One sends vector (vectors.py): every probe's messages, phase by phase, against what the editor
// says the control sends. Returns {probes, messages, failures}.
export function sends(S, configText, vector) {
  const cfg = new Config(S, JSON.parse(configText));
  const store = new Map(), sel = new ParamSelection(), failures = [];
  let count = 0;
  const same = (a, b) => JSON.stringify(a) === JSON.stringify(b);
  for (const pr of vector.probes) {
    const kind = cfg.kind(pr.id);
    const show = (x) => (Array.isArray(x) ? x.map((m) => (Array.isArray(m) ? hexBytes(m) : m)) : x);
    const fail = (phase, what, want, got) => failures.push({
      preset: pr.p + 1, page: pr.g + 1, control: pr.id, layer: pr.shift ? 'shift' : 'normal', phase, what,
      want: show(want), got: show(got),
    });
    const at = (name) => hexes(pr.midi[name]);
    const st = [pr.p, pr.g, pr.shift ? 1 : 0];
    if (!same(pr.state[0], st)) fail('enter', 'state (preset page shift, 0-based)', st, pr.state[0]);
    if (!same(pr.state[1], [pr.p, pr.g, 0])) fail('exit', 'state after Shift (preset page shift, 0-based)', [pr.p, pr.g, 0], pr.state[1]);
    for (const name of ['enter', 'exit', 'after']) if (pr.midi[name]) fail(name, 'nothing sent', [], at(name));
    const layer = layerOf(cfg, pr.p, pr.g, pr.id, pr.shift);
    if (!layer || (layer.type ?? 'none') === 'none') {
      for (const name of PHASES[kind]) if (pr.midi[name]) fail(name, 'nothing sent (no type)', [], at(name));
      continue;
    }
    const pc = cfg.presetChannel(pr.p), noteOff = cfg.presetSetting(pr.p, 'noteOff');
    const paramNull = cfg.presetSetting(pr.p, 'paramNull') === 'gesture';
    const ch = resolveChannel(layer, pc), key = targetKey(S, layer, pc);
    const send = (v) => messages(S, kind, layer, pc, v, noteOff, sel);
    // what the core sent in a phase against what the editor says
    const expect = (phase, want) => {
      const got = at(phase);
      count += got.length;
      if (!same(want, got)) fail(phase, 'messages', want, got);
      resync(sel, got);
      if (key) { const v = valuesIn(layer, ch, got); if (v.length) store.set(key, v[v.length - 1]); }
    };
    if (kind === 'fader') {
      sel.reselect(layer, pc);
      const mode = S.value(layer, 'mode', 'fader');
      const jump = (mode === 'global' ? cfg.globalSetting('pickup') : mode) === 'jump';
      // a sweep end to end reaches both ends of the range, unless the target holds a value the
      // fader cannot reach (another control's, outside this one's range): then it never picks up
      const lo = S.value(layer, 'lo', 'fader'), hi = S.value(layer, 'hi', 'fader'), tv0 = store.get(key);
      const reaches = jump || tv0 === undefined || (tv0 >= Math.min(lo, hi) && tv0 <= Math.max(lo, hi));
      let sent = false;
      for (const phase of ['up', 'down']) {
        let vals = valuesIn(layer, ch, at(phase));
        if (jump) {
          // jump: every position sends its value unless the target has it already
          vals = [];
          let tv = store.get(key);
          const ks = phase === 'up' ? [...Array(17).keys()] : [...Array(16).keys()].reverse();
          for (const k of ks) {
            const v = faderValue(S, layer, Math.round((k * FADER_MAX) / 16));
            if (tv === undefined || v !== tv) { vals.push(v); tv = v; }
          }
        }
        const want = vals.flatMap(send);
        sent ||= want.length > 0;
        expect(phase, want);
        const end = phase === 'up' ? hi : lo;
        if (reaches && store.get(key) !== end) fail(phase, `the target's value at the ${phase === 'up' ? 'top' : 'bottom'}`, end, store.get(key) ?? 'none');
      }
      expect('end', sent && isParam(layer) && paramNull ? sel.nullMessages(ch) : []);
    } else if (kind === 'encoder') {
      for (const [phase, dir] of [['cw', 1], ['ccw', -1]]) {
        sel.reselect(layer, pc);
        const d = encoderDelta(S, layer, dir * pr.clicks, cfg.globalSetting('fine14'));
        let want = [];
        if (layer.type.startsWith('relative')) {
          const r = Math.max(-63, Math.min(63, d));
          if (r) want = send(r);
        } else if (d) {
          const cur = store.get(key), v = encoderValue(S, layer, cur, d);
          if (cur === undefined || v !== cur) want = send(v);
        }
        expect(phase, want);
        expect(`${phase}End`, want.length && isParam(layer) && paramNull ? sel.nullMessages(ch) : []);
      }
    } else {
      sel.reselect(layer, pc);
      const press = buttonValue(S, layer, true, store.get(key));
      expect('press', press === null ? [] : send(press));
      const rel = buttonValue(S, layer, false, store.get(key));
      const want = rel === null ? [] : send(rel);
      if (isParam(layer) && paramNull) want.push(...sel.nullMessages(ch));
      expect('release', want);
      expect('end', []);
    }
  }
  return { probes: vector.probes.length, messages: count, failures };
}

// ---- the binary form (vectors/binary.json, from smcconf's codec) --------------------------------

const toHex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
const fromHex = (h) => Uint8Array.from(h.match(/../g) ?? [], (x) => parseInt(x, 16));

function withoutNames(v) {
  if (Array.isArray(v)) return v.map(withoutNames);
  if (v && typeof v === 'object') return Object.fromEntries(Object.entries(v).filter(([k]) => k !== 'name').map(([k, x]) => [k, withoutNames(x)]));
  return v;
}

function sameValue(a, b) { // key order aside
  if (Array.isArray(a)) return Array.isArray(b) && a.length === b.length && a.every((x, i) => sameValue(x, b[i]));
  if (a && typeof a === 'object') {
    if (!b || typeof b !== 'object' || Array.isArray(b)) return false;
    const ka = Object.keys(a), kb = Object.keys(b);
    return ka.length === kb.length && ka.every((k) => k in b && sameValue(a[k], b[k]));
  }
  return a === b;
}

// The editor's codec (js/cfgbin.js) against smcconf's: the same bytes from each file, the file back
// from them, the same refusals. readFile: path -> text.
export function binaryForm(codec, cases, readFile) {
  const out = [];
  const byName = {};
  for (const c of cases) {
    try {
      if (c.path || c.file) {
        const file = c.file ? JSON.parse(JSON.stringify(c.file)) : JSON.parse(readFile(c.path));
        if (c.id) { file.id = c.id; file.revision = c.revision; }
        const blob = codec.fileToBinary(file);
        if (toHex(blob) !== c.hex) {
          const want = fromHex(c.hex);
          const at = blob.findIndex((b, i) => b !== want[i]);
          out.push(`${c.name}: the editor writes ${blob.length} bytes, smcconf ${want.length}; the first difference at byte ${at < 0 ? Math.min(blob.length, want.length) : at}`);
        }
        const back = codec.binaryToFile(fromHex(c.hex));
        if (!sameValue(back, withoutNames(file))) out.push(`${c.name}: read back, it is not the file without its names`);
        if (JSON.stringify(codec.toJson(codec.fromJson(file))) !== JSON.stringify(file) && !c.id) out.push(`${c.name}: the file does not come back canonical from the editor's model`);
        byName[c.name] = back;
      } else if (c.refused) {
        try {
          codec.binaryToFile(fromHex(c.hex));
          out.push(`${c.name}: the editor takes what smcconf refuses (${c.refused})`);
        } catch (e) {
          if (!e.path && e.path !== '') out.push(`${c.name}: ${e}`);
        }
      } else {
        const back = codec.binaryToFile(fromHex(c.hex));
        if (c.same_as && !sameValue(back, byName[c.same_as])) out.push(`${c.name}: read otherwise than ${c.same_as}`);
      }
    } catch (e) {
      out.push(`${c.name}: ${e.message}`);
    }
  }
  return out;
}

// faults.json's broken files: the editor's codec refuses every one, as smcconf does.
export function binaryRefusals(codec, cases) {
  const out = [];
  for (const c of cases) {
    try {
      codec.fileToBinary(c.file);
      out.push(`the codec takes ${JSON.stringify(c.file).slice(0, 120)} (smcconf: ${c.path})`);
    } catch (e) {
      if (e.path === undefined) out.push(`${JSON.stringify(c.file).slice(0, 80)}: ${e}`);
    }
  }
  return out;
}

// edits.json: Codec.edits() between two files makes the [preset, records] list smcconf's codec
// made, byte for byte (P-1's CFG_SET records).
export function edits(codec, cases, read) {
  const out = [];
  const hex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
  const changed = (doc, set) => {
    const d = JSON.parse(JSON.stringify(doc));
    for (const [path, value] of set) {
      let at = d;
      for (const k of path.slice(0, -1)) at = at[k];
      if (value === null) delete at[path[path.length - 1]];
      else at[path[path.length - 1]] = value;
    }
    return d;
  };
  for (const c of cases) {
    const base = JSON.parse(read(c.old ?? c.new));
    let oldDoc, newDoc;
    if (c.set) [oldDoc, newDoc] = c.old ? [base, changed(base, c.set)] : [changed(base, c.set), base];
    else [oldDoc, newDoc] = [base, JSON.parse(read(c.new))];
    try {
      const got = codec.edits(codec.fromJson(oldDoc), codec.fromJson(newDoc)).map(([p, r]) => [p, hex(r)]);
      if (JSON.stringify(got) !== JSON.stringify(c.edits)) {
        const i = got.findIndex((g, k) => JSON.stringify(g) !== JSON.stringify(c.edits[k]));
        out.push(`${c.name}: ${got.length} edits, smcconf ${c.edits.length}; the first that differs (${i}): ${JSON.stringify(got[i])}, smcconf ${JSON.stringify(c.edits[i])}`);
      }
    } catch (e) {
      out.push(`${c.name}: ${e.message}`);
    }
  }
  return out;
}
