// One layer of a control at a time: what the editor's copy, paste and clear act on (docs/app.md,
// "Copy and paste"). The panel shows one layer, so these operations touch only that layer.
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { Schema } from '../../app/editor/js/schema.js';
import { Config } from '../../app/editor/js/config.js';

const S = new Schema(JSON.parse(readFileSync('config/schema.json', 'utf8')));
const fresh = () => new Config(S, { schemaVersion: S.version });
const own = { name: 'Cutoff', type: 'cc7', number: 74, shift: { type: 'cc7', number: 71 } };

test('layerOf: the Normal layer leaves out the name and the Shift layer', () => {
  const cfg = fresh();
  assert.deepEqual(cfg.layerOf(own, 'fader', 'normal'), { type: 'cc7', number: 74 });
});

test('layerOf: the Shift layer is what plays under Shift, or null for nothing', () => {
  const cfg = fresh();
  assert.deepEqual(cfg.layerOf(own, 'fader', 'shift'), { type: 'cc7', number: 71 });
  assert.deepEqual(cfg.layerOf({ type: 'cc7', number: 5 }, 'fader', 'shift'), { type: 'cc7', number: 5 });
  assert.equal(cfg.layerOf({ type: 'cc7', number: 5, shift: 'block' }, 'fader', 'shift'), null);
  assert.equal(cfg.layerOf({ type: 'cc7', number: 5 }, 'encoder', 'shift').mode, 'fine');
});

test('putLayer onto Normal changes only the Normal layer: the name and the Shift layer stay', () => {
  const cfg = fresh();
  const c = structuredClone(own);
  cfg.putLayer(c, 'fader', 'normal', { type: 'cc7', number: 10 });
  assert.deepEqual(c, { name: 'Cutoff', type: 'cc7', number: 10, shift: { type: 'cc7', number: 71 } });
});

test('putLayer onto Normal keeps an inheriting Shift layer inheriting, and a fine encoder fine', () => {
  const cfg = fresh();
  const f = { type: 'cc7', number: 5 };
  cfg.putLayer(f, 'fader', 'normal', { type: 'nrpn', number: 300 });
  assert.equal(cfg.shiftState(f, 'fader'), 'inherit');
  const e = { type: 'cc7', number: 5 };
  cfg.putLayer(e, 'encoder', 'normal', { type: 'cc7', number: 6 });
  assert.equal(cfg.shiftState(e, 'encoder'), 'fine');
});

test('putLayer onto Shift makes a layer of its own and leaves Normal and the name as they are', () => {
  const cfg = fresh();
  const c = { name: 'Volume', type: 'cc7', number: 7 };
  cfg.putLayer(c, 'fader', 'shift', { type: 'cc7', number: 11 });
  assert.deepEqual(c, { name: 'Volume', type: 'cc7', number: 7, shift: { type: 'cc7', number: 11 } });
});

test('putLayer of nothing: Normal sends nothing (Shift kept), Shift becomes "nothing"', () => {
  const cfg = fresh();
  const c = structuredClone(own);
  cfg.putLayer(c, 'fader', 'shift', null);
  assert.equal(c.shift, 'block');
  assert.equal(c.number, 74);
  const d = structuredClone(own);
  cfg.putLayer(d, 'fader', 'normal', null);
  assert.equal(d.type, undefined);
  assert.equal(d.name, 'Cutoff');
  assert.deepEqual(d.shift, { type: 'cc7', number: 71 });
});

test('putLayer drops fields another kind cannot take, and fits values into the type', () => {
  const cfg = fresh();
  const b = {};
  // a fader layer pasted onto a button: same type; a button keeps its own mode default
  cfg.putLayer(b, 'button', 'normal', { type: 'cc7', number: 20, mode: 'jump' });
  assert.equal(b.type, 'cc7');
  assert.equal(b.number, 20);
  assert.equal(b.mode, undefined, 'a fader pickup is not a button mode');
  // an encoder's divider needs its n: a layer moved onto an encoder in mode div gets one
  const e = {};
  cfg.putLayer(e, 'encoder', 'normal', { type: 'cc7', number: 1, mode: 'div' });
  assert.equal(e.n, 2);
  // a type this kind cannot send: the layer sends nothing
  const f = { type: 'cc7', number: 3 };
  cfg.putLayer(f, 'fader', 'shift', { type: 'relative2c', number: 9 });
  assert.equal(f.shift, 'block');
});

test('copying a Normal layer onto the Shift layer of the same control: one parameter differs', () => {
  const cfg = fresh();
  const c = { name: 'Filter', type: 'cc7', number: 74, lo: 10, hi: 100 };
  const l = cfg.layerOf(c, 'fader', 'normal');
  cfg.putLayer(c, 'fader', 'shift', l);
  c.shift.number = 71;
  assert.deepEqual(c, { name: 'Filter', type: 'cc7', number: 74, lo: 10, hi: 100, shift: { type: 'cc7', number: 71, lo: 10, hi: 100 } });
});

test('the result is a valid file', () => {
  const cfg = fresh();
  const c = cfg.control(0, 0, 's0.fader');
  cfg.putLayer(c, 'fader', 'shift', { type: 'cc7', number: 11 });
  cfg.putLayer(c, 'fader', 'normal', { type: 'cc14', number: 1 });
  const nav = cfg.control(0, 0, 'row.up');
  cfg.putLayer(nav, 'button', 'normal', { type: 'note', number: 60 });
  assert.deepEqual(S.checkFile(cfg.canonical()), []);
});

test('names: a Shift layer of its own has its own name; otherwise Shift shows the control\'s', () => {
  const cfg = fresh();
  const c = { name: 'Volume', type: 'cc7', number: 7 };
  assert.equal(cfg.nameOn(c, 'fader', 'shift'), 'Volume');
  assert.equal(cfg.canRename(c, 'fader', 'shift'), false, 'Same as Normal: the same name');
  cfg.setShiftState(c, 'fader', 'own');
  assert.equal(cfg.canRename(c, 'fader', 'shift'), true);
  assert.equal(cfg.nameOn(c, 'fader', 'shift'), undefined, 'no own name yet: none (it does something else)');
  cfg.rename(c, 'fader', 'shift', 'Pan');
  assert.equal(cfg.nameOn(c, 'fader', 'shift'), 'Pan');
  assert.equal(cfg.nameOn(c, 'fader', 'normal'), 'Volume');
  assert.deepEqual(cfg.canonControl(c, 'fader', false), { name: 'Volume', type: 'cc7', number: 7, shift: { name: 'Pan', type: 'cc7', number: 7 } });
  // pasting another layer onto Shift keeps the Shift layer's name, as a control keeps its name
  cfg.putLayer(c, 'fader', 'shift', { type: 'cc7', number: 10 });
  assert.equal(cfg.nameOn(c, 'fader', 'shift'), 'Pan');
  // the name is not part of what plays
  assert.equal(cfg.layerOf(c, 'fader', 'shift').name, undefined);
  cfg.rename(c, 'fader', 'shift', '');
  assert.equal(c.shift.name, undefined);
  assert.equal(cfg.nameOn(c, 'fader', 'shift'), undefined);
});

test('names: an encoder\'s fine Shift layer named keeps its name in the file', () => {
  const cfg = fresh();
  const e = { name: 'Cutoff', type: 'cc7', number: 74 };
  cfg.setShiftState(e, 'encoder', 'own');
  cfg.rename(e, 'encoder', 'shift', 'Cutoff fine');
  const out = cfg.canonControl(e, 'encoder', false);
  assert.equal(out.shift.name, 'Cutoff fine');
  assert.deepEqual(S.checkFile({ schemaVersion: S.version, presets: [{ pages: [{ strips: [{ encoder: out }] }] }] }), []);
});
