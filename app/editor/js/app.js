// The configuration editor (docs/app.md). Plain modules, no build step, no dependencies.

import { Schema } from './schema.js';
import { Config, dumps } from './config.js';
import { TYPE_LABEL, shortLabel, describe, messages, decode, hexBytes, noteName, noteNumber, setMiddleC, resolveChannel, ParamSelection, isParam, buttonValue, targetKey as layerTarget, faderValue, encoderDelta, FADER_MAX } from './midi.js';
import { MockDevice } from './mock.js';
import { MixerDevice } from './mixer.js';
import { VERSION, compareVersions } from './version.js';
import { installDevhost } from './devhost.js';
import { findRelease, refreshRelease, releaseFiles, sha256hex } from './release.js';
import { createCycleObserver, deviceSnapshot, observeDevice, releaseSnapshot, previewPlan } from './cycle-observer.js';
import { Codec } from './cfgbin.js';
import { CONFIG_FILE_BASE, configFileName } from './filenames.js';
import { PARAMETER_LISTS, LAYOUTS, fromMidiGuide } from './templates.js';
import { loadIndex, loadDevice, MIDI_GUIDE_SOURCE } from './midiguide.js';
import { createLearnPort } from './learnport.js';
import { icon, ROW_ICON } from './icons.js';
import { TYPE_HELP, MODE_LABEL, PROGRAM_BUTTON_HELP, PRESET_FORM, GLOBAL_FORM, ROW_NAME, KEY_NAME } from './labels.js';

// ---- small helpers ---------------------------------------------------------------------------

const $ = (s) => document.querySelector(s);
function h(tag, attrs = {}, ...kids) {
  const el = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs || {})) {
    if (v === undefined || v === null || v === false) continue;
    if (k === 'class') el.className = v;
    else if (k === 'style' && typeof v === 'object') { for (const [sk, sv] of Object.entries(v)) { if (sk.startsWith('--')) el.style.setProperty(sk, sv); else el.style[sk] = sv; } }
    else if (k.startsWith('on')) el.addEventListener(k.slice(2), v);
    else el.setAttribute(k, v === true ? '' : v);
  }
  for (const c of kids.flat()) if (c !== null && c !== undefined && c !== false) el.append(c instanceof Node ? c : document.createTextNode(String(c)));
  return el;
}

function put(el, ...kids) { el.replaceChildren(...kids.flat().filter((x) => x !== null && x !== undefined && x !== false)); }
let toastTimer;
// A short message at the top. It is a popover, so it shows above an open dialog and its backdrop;
// it stays longer for a longer text, waits while the pointer is on it, and has a close button.
function toast(msg) {
  const t = $('#toast');
  const hide = () => { try { t.hidePopover(); } catch { t.hidden = true; } };
  const ms = Math.min(9000, 2400 + msg.length * 45);
  put(t, h('span', {}, msg), h('button', { class: 'icon-only', 'aria-label': 'Close', onclick: hide }, icon('close', 14)));
  t.onmouseenter = () => clearTimeout(toastTimer);
  t.onmouseleave = () => { clearTimeout(toastTimer); toastTimer = setTimeout(hide, 1500); };
  try { if (t.matches(':popover-open')) t.hidePopover(); t.showPopover(); } catch { t.hidden = false; }
  clearTimeout(toastTimer); toastTimer = setTimeout(hide, ms);
}
const store = {
  get(k) { try { return localStorage.getItem(k); } catch { return null; } },
  set(k, v) { try { localStorage.setItem(k, v); } catch { /* storage may be blocked */ } },
  del(k) { try { localStorage.removeItem(k); } catch { /* storage may be blocked */ } },
};
const plural = (n, one, many = one + 's') => `${n} ${n === 1 ? one : many}`;
const btn = (ic, label, attrs = {}) => h('button', attrs, ic ? icon(ic, 16) : null, label ? h('span', {}, label) : null);


// Shift + these select pages and presets on the mixer (fixed in every preset).
const NAV = { up: ['page', 0], down: ['page', 1], left: ['page', 2], right: ['page', 3], rew: ['preset', 0], ff: ['preset', 1], bankLeft: ['preset', 2], bankRight: ['preset', 3] };
const typeVar = (t) => (t && t.startsWith('relative') ? 'var(--t-rel)' : `var(--t-${t || 'none'})`);

// ---- state -----------------------------------------------------------------------------------

let S, cfg;
// The device: the mixer itself over Web MIDI (js/mixer.js, I-5), or the mocked one with ?mock in the
// address only. Without Web MIDI the normal page remains an offline file editor.
// ?devhost: the mixer is the firmware built for the host, through serve.py (js/devhost.js).
const DEVHOST = installDevhost();
const MOCK = new URLSearchParams(location.search).has('mock');
let dev = new MockDevice();
let cycleInitialized = false, cycleRelease = null, cycleJob = null, cycleJobId = 0;
const cycleManifestHashes = new Map();
const cycle = createCycleObserver({ readState: () => ({ initialized: cycleInitialized,
  ...deviceSnapshot(dev, cfg ? docText() : null), release: cycleRelease, job: cycleJob }) });
const st = {
  preset: 0, page: 0, layer: 'normal', sim: false, tab: 'inspector',
  view: store.get('smc-view') === 'table' ? 'table' : 'panel', // the panel first, then the last one used (A-3)
  listGroup: store.get('smc-list-group') === 'kind' ? 'kind' : 'strip',
  sel: new Set(), multi: false,
  follow: store.get('smc-link') !== 'separate', // Linked: the editor shows and selects what the mixer plays
  undo: [], redo: [], clip: null,
  instrument: null, instrumentKey: null, armed: null, used: new Set(),
  values: new Map(), lit: new Set(),
  monitor: [], monitorOpen: false, monitorFilter: 'all', paused: false,
  errors: [], theme: store.get('smc-theme') || 'auto',
  planned: { global: {}, preset: {}, ctl: {} },
};
let IDS = [];

// ---- theme ---------------------------------------------------------------------------------------

function applyTheme() {
  if (st.theme === 'auto') document.documentElement.removeAttribute('data-theme');
  else document.documentElement.setAttribute('data-theme', st.theme);
  const b = $('#btn-theme');
  if (b) { put(b, icon({ auto: 'auto', light: 'sun', dark: 'moon' }[st.theme])); b.title = `Appearance: ${{ auto: 'as the system', light: 'light', dark: 'dark' }[st.theme]}`; }
}
function setTheme(t) { st.theme = t; store.set('smc-theme', t); applyTheme(); }
applyTheme();

// ---- loading ---------------------------------------------------------------------------------

async function fetchFirst(names) {
  for (const u of names) {
    try { const r = await fetch(u, { cache: 'no-store' }); if (r.ok) return await r.json(); } catch { /* next */ }
  }
  return null;
}
// In the repository the page is app/editor/ and reads config/ itself; anywhere else (a release
// package, GitHub Pages) it has its own data/ beside it: ask that first, with no failed request.
const IN_REPO = typeof location !== 'undefined' && /\/app\/editor\/(index\.html)?$/.test(location.pathname);
const DATA = (n) => (IN_REPO ? [`../../config/${n}`, `data/${n}`] : [`data/${n}`, `../../config/${n}`]);

async function boot() {
  const raw = await fetchFirst(DATA('schema.json'));
  if (!raw) {
    put(document.body, h('p', { style: { padding: '24px' } }, 'The editor could not start: its data files are missing.'));
    return;
  }
  S = new Schema(raw);
  IDS = Config.ids(S);
  if (!MOCK) dev = new MixerDevice(new Codec(raw));
  try { Object.assign(st.planned, JSON.parse(store.get('smc-proto-planned') || '{}')); } catch { /* ignore */ }
  let file = null;
  try { file = JSON.parse(store.get('smc-proto-doc') || 'null'); } catch { file = null; }
  if (!file) file = await fetchFirst(DATA('factory.json'));
  if (!openFile(file ?? { schemaVersion: S.version }, 'factory', true)) openFile({ schemaVersion: S.version }, 'empty', true);
  if (!dev.real) dev.running = dev.saved = docText(); // the mocked mixer starts with the editor's copy
  if (dev.real) findRelease(location.href, async (url) => {
    const response = await fetch(url);
    try { if (response.ok) cycleManifestHashes.set(String(url), await sha256hex(await response.clone().arrayBuffer())); }
    catch { /* hash observation cannot change release selection */ }
    return response;
  }).then(useRelease); // what the installer and updates use
  wire();
  await loadHistory();
  cycleInitialized = true;
  cycle.record('initialized');
  setInterval(renderPill, 300);
  // ?selftest: T-4's checklist, run by the page on its own functions (js/selftest.js)
  if (SELFTEST) {
    import('./selftest.js').then((m) => m.selftest({
      get dev() { return dev; }, get cfg() { return cfg; }, st, devhost: DEVHOST,
      edit, openFile, fetchFirst, DATA, docText, unsaved, connectMixer, saveOnMixer, revertDialog, selectView, ctlName,
    }));
  }
}

function openFile(file, label, quiet) {
  const errs = S.checkFile(file);
  if (errs.length) { showProblems(errs, `“${label}” cannot be opened:`); return false; }
  const prev = cfg && !quiet ? JSON.stringify(cfg.file) : null;
  cfg = new Config(S, file);
  if (prev) { st.undo.push({ label: `open ${label}`, json: prev }); st.redo = []; } else { st.undo = []; st.redo = []; }
  st.sel.clear(); st.errors = [];
  st.values.clear(); st.lit.clear();
  if (!quiet) { saveLocal(); toast(`Opened ${label}. Undo brings back the previous configuration.`); pushToDevice('configuration opened'); }
  if ($('#panel')) renderAll();
  return true;
}

// ---- edits: undo, the device, the working copy -------------------------------------------------

function edit(label, fn) {
  const before = JSON.stringify(cfg.file);
  fn();
  if (JSON.stringify(cfg.file) === before) { renderAll(); return; }
  st.undo.push({ label, json: before });
  if (st.undo.length > 200) st.undo.shift();
  st.redo = [];
  afterChange(label);
}
function afterChange(label) {
  saveLocal();
  st.errors = S.checkFile(cfg.canonical());
  renderAll();
  pushToDevice(label);
}
// ?selftest keeps the user's working copy and its history as they were: its edits stay in the page.
const SELFTEST = new URLSearchParams(location.search).has('selftest');
function saveLocal() { if (SELFTEST) return; store.set('smc-proto-doc', JSON.stringify(cfg.canonical())); saveHistory(); }

// The undo history survives a restart (A-8): kept in the app's storage beside the working copy, not
// in the file. In a browser that is IndexedDB, which holds megabytes where localStorage holds about
// five; the browser may still clear it (site data cleared, or Safari's 7-day rule for sites not added
// to the Home Screen), and then the editor starts with an empty history, as it would anyway.
const HISTORY_KEEP = 100;
function historyDb() {
  return new Promise((resolve) => {
    try {
      const r = indexedDB.open('smc-editor', 1);
      r.onupgradeneeded = () => r.result.createObjectStore('history');
      r.onsuccess = () => resolve(r.result);
      r.onerror = () => resolve(null);
    } catch { resolve(null); }
  });
}
let historyTimer = null;
function saveHistory() {
  clearTimeout(historyTimer);
  historyTimer = setTimeout(async () => {
    const db = await historyDb(); if (!db) return;
    const rec = { doc: JSON.stringify(cfg.canonical()), undo: st.undo.slice(-HISTORY_KEEP), redo: st.redo.slice(-HISTORY_KEEP) };
    try { db.transaction('history', 'readwrite').objectStore('history').put(rec, 'current'); } catch { /* storage may be blocked */ }
  }, 800);
}
async function loadHistory() {
  const db = await historyDb(); if (!db) return;
  const rec = await new Promise((resolve) => {
    try { const q = db.transaction('history').objectStore('history').get('current'); q.onsuccess = () => resolve(q.result); q.onerror = () => resolve(null); } catch { resolve(null); }
  });
  // Only the history of this very configuration: a file changed elsewhere starts afresh.
  if (rec && rec.doc === JSON.stringify(cfg.canonical())) { st.undo = rec.undo; st.redo = rec.redo; renderTop(); }
}
// Saving and trying (A-12, docs/app.md): Linked sends every edit to what the mixer plays; Separate
// keeps edits here until "Send". Neither touches what the mixer starts with: that is Save.
const docText = () => dumps(cfg.canonical());
function pushToDevice(label, force) { if (ours() && (st.follow || force)) dev.apply(docText(), label, force); }
const unsent = () => ours() && (dev.diverged || (dev.running !== null && docText() !== dev.running));
// The mixer reports its storage (CFG_INFO); the mock compares its two copies.
const unsaved = () => ours() && (dev.real ? dev.storage !== 'clean' : dev.running !== dev.saved);
function sendToMixer() { pushToDevice('configuration sent', true); toast(dev.autosave ? 'Sent: the mixer plays it and saves it in a few seconds.' : 'Sent: the mixer plays it now. Save keeps it after the mixer is switched off.'); renderAll(); }
async function saveOnMixer() {
  toast('Saving on the mixer…');
  try { await dev.saveNow(); toast('Saved on the mixer.'); return true; }
  catch (e) { toast(`Not saved: ${e.message}`); return false; }
}
function revertDialog() {
  askConfirm('Go back to the last save?', 'The mixer plays what it starts with again, and the editor shows it. Changes made since the last save leave both; undo here brings them back.', 'Go back', async () => {
    const doc = await dev.revert(); if (!doc) return;
    const before = JSON.stringify(cfg.file);
    cfg = new Config(S, JSON.parse(doc));
    st.undo.push({ label: 'go back to the last save', json: before }); st.redo = [];
    saveLocal(); st.errors = S.checkFile(cfg.canonical()); renderAll();
    toast('Back to the last save. Undo brings your changes back to the editor.');
  });
}
// Switching to Linked while the two differ: whose configuration wins.
function divergeDialog(done, reconnected) {
  dialog(h('div', {},
    h('h2', {}, 'The editor and the mixer differ'),
    h('p', {}, reconnected ? 'While the mixer was not connected, the configuration here changed, or the mixer was switched off unsaved. Linked means both show the same, so one of them has to win.'
      : 'You changed things here that the mixer does not play yet. Linked means both show the same, so one of them has to win.'),
    h('div', { class: 'choices' },
      h('button', { class: 'choice primary', onclick: () => { closeDialog(); pushToDevice('configuration sent', true); done(true); } }, h('b', {}, 'Send mine'), h('small', {}, 'The mixer plays what you see here.')),
      h('button', { class: 'choice', onclick: async () => {
        try {
          const file = dev.real ? await dev.pullFile() : JSON.parse(dev.running);
          const before = JSON.stringify(cfg.file);
          cfg = new Config(S, file); st.undo.push({ label: 'take the mixer’s configuration', json: before }); st.redo = [];
          saveLocal(); closeDialog(); done(true);
        } catch (e) { toast(`Not read: ${e.message}`); }
      } }, h('b', {}, 'Take the mixer’s'), h('small', {}, 'Read what the mixer actually plays. Undo brings yours back.')),
      h('button', { class: 'choice', onclick: () => { closeDialog(); done(false); } }, h('b', {}, 'Stay separate'), h('small', {}, 'Nothing changes.')))));
}
function undo() {
  const u = st.undo.pop(); if (!u) return;
  st.redo.push({ label: u.label, json: JSON.stringify(cfg.file) });
  cfg.file = JSON.parse(u.json);
  toast(`Undone: ${u.label}`);
  afterChange(`undo ${u.label}`);
}
function redo() {
  const r = st.redo.pop(); if (!r) return;
  st.undo.push({ label: r.label, json: JSON.stringify(cfg.file) });
  cfg.file = JSON.parse(r.json);
  toast(`Redone: ${r.label}`);
  afterChange(`redo ${r.label}`);
}

// Settings the firmware does not have yet: kept by the editor, outside the configuration file.
function plannedGet(scope, key, def) { return scope[key] ?? def; }
function plannedSet(scope, key, v, label) {
  scope[key] = v;
  store.set('smc-proto-planned', JSON.stringify(st.planned));
  toast(`${label}: saved in the editor`);
}


// ---- what a control is ------------------------------------------------------------------------------

function ctlName(id) {
  if (id.startsWith('row.')) return ROW_NAME[id.slice(4)];
  const k = cfg.key(id), n = cfg.strip(id) + 1;
  return k === 'fader' || k === 'encoder' ? `${KEY_NAME[k]} ${n}` : `${KEY_NAME[k].split(' ')[0]} ${n}`;
}
function ctlAt(id, p = st.preset, g = st.page) { return cfg.control(p, g, id); }
function targetKey(layer, p) { return layerTarget(S, layer, cfg.presetChannel(p)); }
function targetIndex() {
  const idx = new Map();
  const add = (k, where) => { if (!k) return; if (!idx.has(k)) idx.set(k, []); idx.get(k).push(where); };
  for (let p = 0; p < S.device.presets; p++) {
    for (let g = 0; g < S.device.pages; g++) {
      for (const id of IDS) {
        if (id.startsWith('row.') && g > 0) continue;
        const c = cfg.control(p, g, id), kind = cfg.kind(id);
        add(targetKey(c, p), { p, g, id, layer: 'normal' });
        if (cfg.shiftState(c, kind) === 'own') add(targetKey(c.shift, p), { p, g, id, layer: 'shift' });
      }
    }
  }
  return idx;
}
const presetLabel = (p) => cfg.file.presets[p].name || `Preset ${p + 1}`;
const pageLabel = (p, g) => cfg.file.presets[p].pages[g].name || `Page ${g + 1}`;
const whereText = (w) => `${presetLabel(w.p)}, ${w.id.startsWith('row.') ? 'bottom row' : pageLabel(w.p, w.g)}: ${ctlName(w.id)}${w.layer === 'shift' ? ' with Shift' : ''}`;

// ---- rendering -------------------------------------------------------------------------------

function markLearning() {
  for (const el of document.querySelectorAll('#panel .ctl')) el.classList.toggle('learning', !!st.learn && st.learn.ids[st.learn.k] === el.dataset.id);
}
const STOCK_BT_LABEL = 'Install over Bluetooth';
const STOCK_BT_HELP = 'Choose a mixer running M-Vave’s firmware in the browser’s Bluetooth list to install better-smc-mixer. For a mixer already running better-smc-mixer, use Connect.';
const MIDI_CONNECT_HELP = 'Use USB, or pair Better-SMC-Mixer in your system’s Bluetooth MIDI setup before connecting.';
const connectionType = () => dev.transport === 'USB' ? 'USB' : dev.fw.kind === 'stock' ? 'Web Bluetooth' : 'Bluetooth MIDI';

// What the editor can do with what is (or is not) connected, said once above the page.
function connectMixer() {
  if (dev.real && !MixerDevice.supported()) { toast('Offline file editor: this browser has no Web MIDI. Import, edit and export here; use a Web MIDI browser to connect.'); return; }
  if (!dev.real) {
    dev.setConnected(true);
    toast('Connected to the mixer over USB (preview).');
    return;
  }
  if (st.connecting) return;
  st.connecting = true;
  toast('Looking for the mixer…');
  dev.connect(docText())
    .then(() => toast(dev.fw.kind !== 'ours' ? `Connected over ${connectionType()}: the mixer runs M-Vave’s firmware (${dev.fw.version}).`
      : `Connected over ${connectionType()}: ${dev.running === docText() ? 'the mixer plays this configuration.' : 'the mixer plays another configuration.'}`))
    .catch((e) => { cycle.record('error', { source: 'connect', name: e.name, message: e.message }); toast(e.name === 'NotAllowedError' || e.name === 'SecurityError' ? 'MIDI access was not granted. Allow MIDI and SysEx in your browser’s site settings, then try Connect again.' : `Not connected: ${e.message}${/[.?!]$/.test(e.message) ? '' : '.'} ${MIDI_CONNECT_HELP}`); })
    .finally(() => { st.connecting = false; renderAll(); });
}
// M-Vave's firmware over Bluetooth (MixerDevice.connectBluetooth): the browser lists the devices
// near by, the user picks the mixer. Ours is found over Bluetooth MIDI by Connect.
function connectBluetooth() {
  if (!MOCK && !MixerDevice.supported()) { connectMixer(); return; }
  if (st.connecting || dev.connected) return;
  st.connecting = true;
  toast('Choose the mixer in the browser’s Bluetooth list. Not there? Disconnect it where it is connected as a Bluetooth MIDI device (Audio MIDI Setup, a music app): it takes one connection at a time.');
  dev.connectBluetooth()
    .then(() => toast(`Connected over Web Bluetooth: the mixer runs M-Vave’s firmware (${dev.fw.version}). Choose Install to continue.`))
    .catch((e) => { cycle.record('error', { source: 'bluetooth', name: e.name, message: e.message }); toast(e.name === 'NotFoundError' ? 'No mixer chosen. If it was missing from the list, see “Bluetooth connections” in the Mixer sheet.' : e.name === 'NotAllowedError' || e.name === 'SecurityError' ? 'Bluetooth access was not granted. Allow it in browser and system settings, or install over USB.' : `Stock firmware not connected: ${e.message}`); })
    .finally(() => { st.connecting = false; renderAll(); });
}
// Load from the mixer: its configuration replaces the editor's (undo brings it back). The binary
// form carries no names, so the mixer's comes without them.
function loadFromMixer() {
  if (!dev.real) { toast('Not available in this preview: it will replace this configuration with the mixer’s.'); return; }
  dev.pullFile().then((file) => {
    const before = JSON.stringify(cfg.file);
    cfg = new Config(S, file);
    st.undo.push({ label: 'load from the mixer', json: before }); st.redo = [];
    saveLocal(); st.errors = S.checkFile(cfg.canonical()); renderAll();
    toast('Loaded from the mixer. It keeps no names: undo brings yours back.');
    cycle.record('config_loaded');
  }).catch((e) => { cycle.record('error', { source: 'pull', name: e.name, message: e.message }); toast(`Not loaded: ${e.message}`); });
}
function renderNotice() {
  const n = $('#notice');
  const f = dev.fw;
  const say = !MOCK && !MixerDevice.supported()
    ? ['Offline file editor: this browser has no Web MIDI. Import, edit and export configurations here; nothing is sent to or saved on a mixer.', null, null]
    : MOCK ? ['Simulation (?mock): no physical mixer is connected. Device and Saved states below are simulated.', null, null] : !dev.connected
    ? ['No mixer connected. Everything here works on the configuration in this app: edit, save a copy, use templates. Connect the mixer to try it.', 'Connect', connectMixer]
    : f.kind === 'stock' ? ['This mixer runs M-Vave’s firmware, which this editor cannot configure. You can still prepare a configuration here; install better-smc-mixer to use it.', 'Install…', installDialog]
        : f.kind === 'loader' ? ['The mixer waits for firmware: an update was interrupted. Nothing is lost.', 'Finish the update', () => updateDialog(true)] : null;
  // Working without a mixer is normal (A-19): that notice is calm and can be put away until the
  // connection changes. Firmware that is not ours, or an interrupted update, keeps it on view.
  const key = dev.connected ? `${f.kind}:${f.version}` : 'none';
  if (dev.connected && st.noticeAway === 'none') st.noticeAway = null;
  n.hidden = !say || st.noticeAway === key;
  if (n.hidden) return;
  n.className = `notice ${dev.connected ? 'warn' : ''}`;
  put(n, icon('info', 16), h('span', {}, say[0]), say[1] ? h('button', { onclick: say[2] }, say[1]) : null,
    !dev.connected && dev.real && MixerDevice.supported() && MixerDevice.bluetooth()
      ? h('button', { title: STOCK_BT_HELP, onclick: connectBluetooth }, STOCK_BT_LABEL) : null,
    dev.connected ? null : h('button', { class: 'icon-only ghost', 'aria-label': 'Hide this note', title: 'Hide until the mixer is connected', onclick: () => { st.noticeAway = key; renderNotice(); } }, icon('close', 14)));
}
function renderAll() {
  renderNotice();
  setMiddleC(plannedGet(st.planned.preset[st.preset] ?? {}, 'middleC', 'C4'));
  renderTop();
  renderSelbar();
  if (st.view === 'panel') { renderPanel(); markLearning(); } else renderTable();
  $('#panel-wrap').hidden = st.view !== 'panel';
  $('#table-wrap').hidden = st.view !== 'table';
  renderLegend();
  renderSide();
  renderMonitorBar();
  renderPill();
}

function renderTop() {
  if (document.activeElement !== $('#cfg-name')) $('#cfg-name').value = cfg.file.name ?? '';
  const pb = $('#problems');
  pb.hidden = !st.errors.length;
  pb.textContent = plural(st.errors.length, 'problem');
  $('#undo').disabled = !st.undo.length;
  $('#redo').disabled = !st.redo.length;
  $('#undo').title = st.undo.length ? `Undo ${st.undo.at(-1).label}` : 'Nothing to undo';
  $('#redo').title = st.redo.length ? `Redo ${st.redo.at(-1).label}` : 'Nothing to redo';

  // Where the mixer is, shown only while Separate: Linked, it is always the open tab.
  const live = (p, g) => !st.follow && dev.connected && dev.state.preset === p && (g === undefined || dev.state.page === g);
  const pr = $('#presets');
  put(pr, ...Array.from({ length: S.device.presets }, (_, p) => h('button', {
    class: `tab ${p === st.preset ? 'on' : ''}`, role: 'tab', 'aria-selected': p === st.preset ? 'true' : 'false',
    title: `${presetLabel(p)} · channel ${cfg.presetChannel(p)}${live(p) ? ' · the mixer is here (Separate)' : ''} · Shift + ${['◀◀', '▶▶', '«', '»'][p]} on the mixer`,
    onclick: () => (p === st.preset ? presetMenu(pr.children[p], p) : selectView(p, st.page, true)),
    oncontextmenu: (e) => { e.preventDefault(); presetMenu(e.currentTarget, p); },
  }, live(p) ? h('i', { class: 'live' }) : null, h('span', { class: 'tname' }, presetLabel(p)), h('small', {}, `Ch ${cfg.presetChannel(p)}`),
  p === st.preset ? icon('chevron', 14) : null)));

  const pg = $('#pages');
  put(pg, ...Array.from({ length: S.device.pages }, (_, g) => h('button', {
    class: `tab ${g === st.page ? 'on' : ''}`, role: 'tab', 'aria-selected': g === st.page ? 'true' : 'false',
    title: `${pageLabel(st.preset, g)}${live(st.preset, g) ? ' · the mixer is here (Separate)' : ''} · Shift + ${['△', '▽', '◁', '▷'][g]} on the mixer`,
    onclick: () => (g === st.page ? pageMenu(pg.children[g], g) : selectView(st.preset, g, true)),
    oncontextmenu: (e) => { e.preventDefault(); pageMenu(e.currentTarget, g); },
  }, live(st.preset, g) ? h('i', { class: 'live' }) : null, h('span', { class: 'tname' }, pageLabel(st.preset, g)), g === st.page ? icon('chevron', 14) : null)));

  for (const b of document.querySelectorAll('#layer-seg button')) { b.classList.toggle('on', b.dataset.layer === st.layer); b.setAttribute('aria-selected', b.dataset.layer === st.layer); }
  document.body.classList.toggle('shift-view', st.layer === 'shift');
}

function selectView(p, g, fromEditor) {
  st.preset = p; st.page = g;
  if (fromEditor && st.follow && ours() && (dev.state.preset !== p || dev.state.page !== g)) dev.select(p, g);
  renderAll();
}

function groups() {
  return [
    ['Faders', IDS.filter((i) => i.endsWith('.fader'))], ['Encoders', IDS.filter((i) => i.endsWith('.encoder'))],
    ['M', IDS.filter((i) => i.endsWith('.m'))], ['S', IDS.filter((i) => i.endsWith('.s'))],
    ['R', IDS.filter((i) => i.endsWith('.r'))], ['□', IDS.filter((i) => i.endsWith('.select'))],
    ['Bottom row', IDS.filter((i) => i.startsWith('row.') && !(st.layer === 'shift' && cfg.isNav(i)))],
  ];
}

function renderSelbar() {
  const same = (a) => a.length === st.sel.size && a.every((i) => st.sel.has(i));
  put($('#selbar'),
    h('div', { class: 'chips', role: 'group', 'aria-label': 'Select' },
      ...groups().map(([n, ids]) => h('button', { class: `chip ${same(ids) ? 'on' : ''}`, title: `Select all ${n === '□' ? 'Select buttons' : n}`, onclick: (e) => selectIds(ids, e.shiftKey || e.metaKey || st.multi) }, n)),
      h('button', { class: `chip toggle ${st.multi ? 'on' : ''}`, 'aria-pressed': st.multi ? 'true' : 'false', title: 'Taps add to the selection', onclick: () => { st.multi = !st.multi; renderSelbar(); } }, st.multi ? icon('check', 14) : null, 'Select several'),
      st.sel.size ? h('button', { class: 'chip ghost', onclick: () => { st.sel.clear(); renderAll(); } }, icon('close', 14), `${st.sel.size} selected`) : null),
    // How the page is shown: Panel | List at the right edge. The list's grouping sits on top of the
    // list itself, so switching the view moves nothing in this bar.
    h('div', { class: 'view-bar' },
      h('div', { class: 'seg small', role: 'tablist', 'aria-label': 'View' },
        ...[['panel', 'Panel'], ['table', 'List']].map(([v, n]) => h('button', { class: st.view === v ? 'on' : '', onclick: () => { st.view = v; store.set('smc-view', v); renderAll(); } }, n)))));
  const chips = $('#selbar .chips');
  chips.addEventListener('scroll', () => fadeChips(chips), { passive: true });
  fadeChips(chips);
}

// The chip row scrolls inside its box on a narrow screen: its edge fades where more chips hide.
function fadeChips(el) {
  if (!el) return;
  el.classList.toggle('more-start', el.scrollLeft > 1);
  el.classList.toggle('more-end', el.scrollLeft + el.clientWidth < el.scrollWidth - 1);
}
if (typeof window !== 'undefined') window.addEventListener('resize', () => fadeChips($('#selbar .chips')));

function renderLegend() {
  const types = [['cc7', 'CC'], ['cc14', 'CC 14-bit'], ['nrpn', 'NRPN'], ['rpn', 'RPN'], ['note', 'Note'], ['pitchbend', 'Pitch Bend'], ['program', 'Program'], ['aftertouch', 'Aftertouch'], ['relative2c', 'Relative']];
  put($('#legend'),
    ...types.map(([t, n]) => h('span', {}, h('i', { style: { background: typeVar(t) } }), n)),
    h('span', { class: 'sep' }), h('span', { title: 'Another control on this page sends the same message. Each keeps its own settings: changing one changes nothing in the other' }, icon('copy', 13), 'Also assigned elsewhere'),
    st.layer === 'shift' ? h('span', { class: 'sep' }) : null,
    st.layer === 'shift' ? [h('span', {}, h('i', { class: 'lg-ghost' }), 'Same as Normal'), h('span', {}, h('i', { class: 'lg-block' }), 'Nothing'), h('span', {}, icon('lock', 13), 'Page and preset keys')] : null);
}

function selectIds(ids, add) {
  if (!add) st.sel.clear();
  const all = ids.length && ids.every((i) => st.sel.has(i));
  for (const i of ids) { if (add && all) st.sel.delete(i); else st.sel.add(i); }
  renderAll();
}

function shownLayer(id) {
  const c = ctlAt(id), kind = cfg.kind(id);
  if (st.layer === 'normal') return cfg.normal(c);
  if (cfg.isNav(id)) return null;
  return cfg.shiftLayer(c, kind);
}

function valueOf(layer, p) { const k = targetKey(layer ?? {}, p); return k ? st.values.get(k) : undefined; }
function frac(layer, kind, p = st.preset) {
  if (!layer || !S.hasValue(layer.type ?? 'none')) return 0;
  const lo = S.value(layer, 'lo', kind), hi = S.value(layer, 'hi', kind);
  const cur = valueOf(layer, p) ?? (kind === 'encoder' ? S.value(layer, 'start', kind) : lo);
  return hi === lo ? 0 : Math.max(0, Math.min(1, (cur - lo) / (hi - lo)));
}
function modeShort(kind, layer) {
  if (!layer || (layer.type ?? 'none') === 'none') return '';
  const m = S.value(layer, 'mode', kind);
  if (kind === 'fader') return m === 'global' ? '' : MODE_LABEL.fader[m][0];
  if (kind === 'encoder') {
    if ((layer.type ?? '').startsWith('relative')) return 'relative';
    return m === 'div' ? `÷${layer.n}` : m === 'step' ? `step ${layer.n}` : m === 'fine' ? 'fine' : m === 'acc0' ? 'no accel.' : `accel. ${m.slice(3)}`;
  }
  return layer.type === 'program' ? 'press' : MODE_LABEL.button[m][0].toLowerCase();
}

let shared = new Set();
function computeShared() {
  shared = new Set();
  for (const list of targetIndex().values()) {
    const here = list.filter((w) => w.p === st.preset && (w.g === st.page || w.id.startsWith('row.')) && w.layer === 'normal');
    if (here.length > 1) for (const w of here) shared.add(w.id);
  }
}

function ctlEl(id, cls) {
  const kind = cfg.kind(id), c = ctlAt(id);
  const isRow = id.startsWith('row.');
  const face = isRow ? h('span', { class: 'face' }, icon(ROW_ICON[id.slice(4)], 16)) : null;
  if (st.layer === 'shift' && cfg.isNav(id)) {
    const [what, n] = NAV[id.slice(4)];
    return h('div', { class: `ctl ${cls} t-none nav-fixed ${st.sel.has(id) ? 'sel' : ''}`, 'data-id': id, title: `Shift + ${ROW_NAME[id.slice(4)]}: ${what} ${n + 1}. Always the same, so you can find your way in every preset.` },
      h('span', { class: 'lbl' }, `${what === 'page' ? 'Page' : 'Preset'} ${n + 1}`), h('span', { class: 'lock' }, icon('lock', 14)));
  }
  let layer = cfg.normal(c), extra = '', tag = null;
  if (st.layer === 'shift') {
    const s = cfg.shiftState(c, kind);
    layer = cfg.shiftLayer(c, kind);
    if (s === 'inherit') extra = 'ghost';
    else if (s === 'block') { extra = 'block'; }
    else if (s === 'fine') tag = 'fine';
    else tag = 'own';
  }
  const type = layer?.type ?? 'none';
  // A Shift layer of its own shows its own name, or its message: the control's name would say the
  // wrong thing there, since it does something else.
  const named = st.layer === 'shift' && cfg.shiftState(c, kind) === 'own' ? (c.shift.name ?? null) : c.name;
  let label = layer ? shortLabel(S, layer) : '—';
  if (cls === 'btn' && type === 'note') label = noteName(layer.number ?? 0);
  const title = `${ctlName(id)}${named ? ` · ${named}` : ''}: ${type === 'none' || !layer ? 'nothing' : shortLabel(S, layer)}`;
  const el = h('div', {
    class: `ctl ${cls} t-${type} ${extra} ${st.sel.has(id) ? 'sel' : ''} ${st.lit.has(id) ? 'lit' : ''}`,
    'data-id': id, style: { '--tc': typeVar(type) }, title, role: 'button', tabindex: '-1', 'aria-pressed': st.sel.has(id) ? 'true' : 'false',
  });
  const main = st.layer === 'shift' && extra === 'block' ? 'nothing' : (named || (type === 'none' ? '—' : label));
  const sub = named ? label : (type === 'none' ? '' : modeShort(kind, layer));
  const lbl = h('span', { class: 'lbl' }, main);
  const subEl = sub ? h('span', { class: 'sub' }, sub) : null;
  const tagEl = tag ? h('span', { class: 'tag' }, tag) : null;
  if (tagEl && kind !== 'encoder') el.append(tagEl);
  if (shared.has(id) && st.layer === 'normal') el.append(h('span', { class: 'share', title: 'Another control on this page sends the same message (an independent copy: its settings are its own)' }, icon('copy', 12)));
  if (kind === 'encoder') {
    el.append(h('div', { class: 'knob', style: { '--v': frac(layer, kind) } }), h('div', { class: 'txt' }, lbl, tagEl ?? subEl));
  } else if (kind === 'fader') {
    el.append(h('div', { class: 'vtxt' }, lbl), h('div', { class: 'track', style: { '--v': frac(layer, kind) } }, h('div', { class: 'cap' })));
  } else {
    for (const k of [lbl, isRow && sub && sub !== 'momentary' ? subEl : null, face]) if (k) el.append(k);
  }
  return el;
}

function renderPanel() {
  computeShared();
  const panel = $('#panel');
  panel.classList.toggle('sim', st.sim);
  panel.classList.toggle('armed', st.armed !== null && st.tab === 'params' && !st.sim);
  put(panel);
  for (let s = 0; s < S.device.strips; s++) {
    const ids = S.stripControls.map((c) => `s${s}.${c.json}`);
    panel.append(h('div', { class: 'strip' },
      h('button', { class: 'strip-head', title: `Select strip ${s + 1}`, onclick: (e) => selectIds(ids, e.shiftKey || e.metaKey || st.multi) }, `${s + 1}`),
      ctlEl(`s${s}.encoder`, 'enc'),
      h('div', { class: 'strip-body' },
        ctlEl(`s${s}.fader`, 'fader'),
        h('div', { class: 'btn-col' }, ...['m', 's', 'r', 'select'].map((k) => ctlEl(`s${s}.${k}`, 'btn'))))));
  }
  const shiftOn = dev.state.shift;
  panel.append(h('div', { class: 'side-col' },
    h('div', { class: 'fixed-btn', 'data-fixed': 'bt', title: 'Bluetooth: hold to turn on or off' }, h('b', {}, 'BT')),
    h('div', { class: `fixed-btn shift ${shiftOn ? 'shift-on' : ''}`, 'data-fixed': 'shift', title: `Shift (${cfg.presetSetting(st.preset, 'shiftMode')} in this preset)` }, h('b', {}, 'SHIFT'))));
  // The bottom row belongs to the preset: one assignment for all its pages (FR-MOD-01). Said where
  // it is, so a change or a clear here is not taken for this page's.
  panel.append(h('div', { class: 'row-btns' },
    h('div', { class: 'row-note', title: 'These 11 buttons do the same on every page of this preset. Changing one here changes it on all four pages.' },
      icon('link', 12), h('span', {}, `Bottom row · the same on all pages of ${presetLabel(st.preset)}`)),
    ...S.rowButtons.map((b) => ctlEl(`row.${b.json}`, 'btn'))));
}

function renderTable() {
  computeShared();
  const t = h('table', { class: 'grid' });
  t.append(h('thead', {}, h('tr', {}, ...['', 'Control', 'Name', 'Sends', 'Channel', 'Number', 'Mode', 'Range', 'With Shift'].map((x) => h('th', {}, x)))));
  const tb = h('tbody');
  let group = null;
  // By strip, as the panel reads; or by kind: encoders, faders, the strip buttons row by row, the bottom row.
  const KIND_GROUP = { encoder: 'Encoders', fader: 'Faders', m: 'Strip buttons', s: 'Strip buttons', r: 'Strip buttons', select: 'Strip buttons' };
  const byKind = st.listGroup === 'kind';
  const ids = byKind ? orderIds(IDS, 'rows') : IDS;
  for (const id of ids) {
    const g = id.startsWith('row.') ? 'Bottom row · every page' : byKind ? KIND_GROUP[cfg.key(id)] : `Strip ${cfg.strip(id) + 1}`;
    if (g !== group) { group = g; tb.append(h('tr', { class: 'group' }, h('td', { colspan: 9 }, g))); }
    tb.append(tableRow(id));
  }
  t.append(tb);
  put($('#table-wrap'), h('div', { class: 'list-tools' }, h('span', { class: 'lbl' }, 'Group'),
    h('div', { class: 'seg small', role: 'tablist', 'aria-label': 'Group the list' },
      ...[['strip', 'By strip'], ['kind', 'By kind']].map(([v, n]) => h('button', { class: st.listGroup === v ? 'on' : '', onclick: () => { st.listGroup = v; store.set('smc-list-group', v); renderAll(); } }, n)))), t);
}

function tableRow(id) {
  const kind = cfg.kind(id), c = ctlAt(id);
  const navShift = st.layer === 'shift' && cfg.isNav(id);
  const sstate = navShift ? 'fixed' : cfg.shiftState(c, kind);
  const editable = st.layer === 'normal' || sstate === 'own';
  const layer = navShift ? null : (st.layer === 'normal' ? c : (sstate === 'own' ? c.shift : cfg.shiftLayer(c, kind)));
  const type = layer?.type ?? 'none';
  const tr = h('tr', { class: st.sel.has(id) ? 'sel' : '', onclick: (e) => { if (e.target.closest('input,select')) return; clickCtl(id, e); } });
  const cell = (...k) => h('td', {}, ...k);
  tr.append(cell(h('input', { type: 'checkbox', 'aria-label': `Select ${ctlName(id)}`, checked: st.sel.has(id), onclick: (e) => { e.stopPropagation(); if (st.sel.has(id)) st.sel.delete(id); else st.sel.add(id); renderAll(); } })));
  tr.append(cell(h('span', { class: 'tdot', style: { background: typeVar(type) } }), ctlName(id)));
  const renamable = !navShift && cfg.canRename(c, kind, st.layer);
  tr.append(cell(h('input', { value: (renamable ? cfg.nameOn(c, kind, st.layer) : c.name) ?? '', placeholder: 'Name', 'aria-label': st.layer === 'shift' ? 'Name with Shift' : 'Name', disabled: !renamable,
    title: renamable ? '' : 'With Shift this control does what it does without it, under the same name. “Own” gives it a name of its own.',
    onchange: (e) => edit('rename', () => cfg.rename(c, kind, st.layer, e.target.value)) })));
  if (navShift) { tr.append(h('td', { colspan: 6, class: 'muted' }, h('span', { class: 'inline-ic' }, icon('lock', 13)), `Selects ${NAV[id.slice(4)][0]} ${NAV[id.slice(4)][1] + 1}`)); return tr; }
  tr.append(cell(h('select', { disabled: !editable, 'aria-label': 'Message', onchange: (e) => edit('message', () => cfg.setField(layer, kind, 'type', e.target.value)) },
    ...S.types(kind).map((x) => h('option', { value: x, selected: x === type }, TYPE_LABEL[x])))));
  const chv = S.value(layer, 'channel', kind);
  tr.append(cell(chv === undefined ? '' : chv === 'preset' ? `Preset (${cfg.presetChannel(st.preset)})` : chv));
  const num = S.value(layer, 'number', kind);
  tr.append(h('td', { class: 'n' }, num === undefined ? '' : h('input', { type: 'number', inputmode: 'numeric', 'aria-label': 'Number', value: num, disabled: !editable, min: 0, max: S.numberMax(type), onchange: (e) => edit('number', () => cfg.setField(layer, kind, 'number', Number(e.target.value))) })));
  tr.append(cell(modeShort(kind, layer)));
  const lo = S.value(layer, 'lo', kind), hi = S.value(layer, 'hi', kind);
  const po = (layer.type ?? 'none') === 'program' ? 1 : 0; // programs are shown 1–128
  tr.append(cell(lo === undefined ? '' : `${lo + po}–${hi + po}`));
  tr.append(cell({ fine: 'Fine', inherit: 'Same', own: 'Own', block: 'Nothing' }[sstate]));
  return tr;
}

// ---- the side panel ---------------------------------------------------------------------------------

function renderSide() {
  put($('#side-title'), st.tab === 'params' ? [h('b', {}, 'Mapping an instrument'), h('button', { class: 'primary', onclick: leaveMapping }, 'Done')] : null);
  document.body.classList.toggle('mapping', st.tab === 'params');
  $('#inspector').hidden = st.tab !== 'inspector';
  $('#params').hidden = st.tab !== 'params';
  if (st.tab === 'inspector') renderInspector(); else renderParams();
}

function renderInspector() {
  const box = $('#inspector');
  put(box);
  if (st.learn) box.append(learnBanner());
  const ids = IDS.filter((i) => st.sel.has(i));
  if (!ids.length) box.append(emptyInspector());
  else if (ids.length === 1) singleInspector(box, ids[0]);
  else bulkInspector(box, ids);
}

function emptyInspector() {
  return h('div', { class: 'empty' },
    h('h2', {}, 'Choose a control'),
    h('p', {}, 'Tap a control on the panel, or touch it on the mixer.'),
    h('ul', {},
      h('li', {}, h('b', {}, 'Several at once: '), 'the buttons above the panel (Faders, Encoders…), a strip number, “Select several”, or drag across the panel.'),
      h('li', {}, h('b', {}, 'Shift: '), 'the Layer switch above the panel, or hold Shift on the mixer.'),
      h('li', {}, h('b', {}, 'An instrument’s parameters: '), '“Map instrument” at the top: choose the instrument, then touch the controls one after another.')),
    h('p', { class: 'note' }, icon('info', 16), h('span', {}, !ours() ? 'No mixer with better-smc-mixer is connected: changes stay in the editor.'
      : st.follow ? `Changes play on the mixer at once. ${dev.autosave ? 'It saves them by itself a few seconds later.' : 'Save keeps them when the mixer is switched off.'}`
        : 'Separate: changes stay here until you Send them to the mixer.')));
}

// The two copies on the mixer, in plain words (A-12).
function savingExplainer() {
  return h('div', { class: 'copies' },
    h('div', {}, h('small', {}, 'Plays now'), h('b', {}, !ours() ? '—' : unsent() ? 'Not your latest changes' : 'What you see here'), h('p', {}, st.follow ? 'Linked: every change goes here at once.' : 'Separate: your changes go here when you press Send.')),
    h('div', {}, h('small', {}, 'Starts with'), h('b', {}, !ours() ? '—' : unsaved() ? 'The last save' : 'The same'), h('p', {}, dev.autosave ? 'Saved by itself a few seconds after the last change.' : 'Changes only when you press Save. Switched off unsaved, the mixer forgets what it played.')));
}
// The saving setting (Q-26): the configuration's global "save" (manual | auto); the mixer follows the
// configuration it plays.
function savingSetting(after) {
  const auto = cfg.globalSetting('save') === 'auto';
  return field('The mixer saves', h('div', { class: 'seg small' },
    ...[[false, 'When I press Save'], [true, 'By itself']].map(([v, n]) => h('button', { class: auto === v ? 'on' : '', onclick: () => {
      if (auto === v) return;
      edit('saving setting', () => { if (v) cfg.file.global.save = 'auto'; else delete cfg.file.global.save; });
      toast(!ours() || st.follow ? (v ? 'The mixer saves by itself a few seconds after the last change.' : 'The mixer saves only when you press Save.') : 'Changed here: the mixer follows it once you Send.');
      after?.();
    } }, n))),
  'Part of the configuration: the mixer follows the one it plays, whichever editor is connected. Saving by hand lets you try things freely; the mixer’s memory is written only when you choose.');
}

function insHead(where, title, nameInput) {
  return h('div', { class: 'insp-head' }, h('div', { class: 'where' }, where), h('h2', {}, title), nameInput);
}

// The name field follows the layer shown: a Shift layer of its own has a name of its own (it does
// something else); a Shift layer that is the same as Normal, fine or nothing shows the control's,
// read-only, with the way to change that.
function nameInput(id, c, kind) {
  const shiftNav = st.layer === 'shift' && cfg.isNav(id);
  const own = !shiftNav && cfg.canRename(c, kind, st.layer);
  if (!own) return h('input', { id: 'ctl-name', value: c.name ?? '', placeholder: 'Name', 'aria-label': 'Name', disabled: true,
    title: 'With Shift this control has no layer of its own, so it keeps its name. “Own” below gives it one, with a name.' });
  const isShift = st.layer === 'shift';
  return h('input', { id: 'ctl-name', value: (isShift ? c.shift.name : c.name) ?? '', maxlength: S.nameMax,
    placeholder: isShift ? 'Name with Shift, e.g. Resonance' : 'Name, e.g. Cutoff',
    'aria-label': isShift ? 'Name with Shift' : 'Name',
    onchange: (e) => edit('rename', () => cfg.rename(c, kind, st.layer, e.target.value)) });
}

function singleInspector(box, id) {
  const kind = cfg.kind(id), c = ctlAt(id);
  box.append(insHead(`${presetLabel(st.preset)} · ${id.startsWith('row.') ? 'Bottom row, every page' : pageLabel(st.preset, st.page)}`, ctlName(id),
    nameInput(id, c, kind)));
  box.append(layerSwitch());

  if (st.layer === 'shift') {
    if (cfg.isNav(id)) {
      const [what, n] = NAV[id.slice(4)];
      box.append(h('p', { class: 'note warn' }, icon('lock', 16), h('span', {}, `With Shift this button selects ${what} ${n + 1}. That stays the same in every preset, so you can always find your way.`)));
      return;
    }
    const s = cfg.shiftState(c, kind);
    box.append(shiftBox([id], s));
    if (s !== 'own') { if (s !== 'block') box.append(sendsBox(kind, cfg.shiftLayer(c, kind) ?? {})); box.append(actionsRow()); return; }
    fieldsFor(box, kind, [c.shift], true);
    box.append(sendsBox(kind, c.shift));
  } else {
    fieldsFor(box, kind, [c], false);
    box.append(sendsBox(kind, cfg.normal(c)));
  }
  const tk = targetKey(st.layer === 'normal' ? c : c.shift ?? {}, st.preset);
  if (tk) {
    const others = (targetIndex().get(tk) ?? []).filter((w) => !(w.p === st.preset && w.id === id && (w.g === st.page || id.startsWith('row.')) && w.layer === st.layer));
    if (others.length) {
      box.append(h('details', { class: 'shared' }, h('summary', {}, icon('copy', 14), `Also assigned to ${plural(others.length, 'other control')}`),
        h('p', { class: 'help' }, 'The same message is sent by these controls too. Each one is independent: its settings are its own, and changing this control changes none of them. They reach the same parameter on the instrument, so the mixer keeps that parameter\'s current value once, as the instrument does: after one of them moves, the others continue from where it left the parameter.'),
        h('ul', {}, ...others.slice(0, 8).map((w) => h('li', {}, whereText(w)))), others.length > 8 ? h('p', { class: 'help' }, `and ${others.length - 8} more`) : null));
    }
  }

  box.append(actionsRow());
}

function layerSwitch() {
  return h('div', { class: 'seg full layer-seg', role: 'tablist' },
    h('button', { class: st.layer === 'normal' ? 'on' : '', onclick: () => setLayer('normal') }, 'Normal'),
    h('button', { class: st.layer === 'shift' ? 'on' : '', onclick: () => setLayer('shift') }, 'Shift'));
}
function setLayer(l) { st.layer = l; renderAll(); }

function shiftBox(ids, state) {
  const kinds = new Set(ids.map((i) => cfg.kind(i)));
  const enc = kinds.size === 1 && kinds.has('encoder');
  const opts = [
    ...(enc ? [['fine', 'Fine', 'The same parameter in small steps: for precise settings.']] : []),
    ['inherit', 'Same', enc ? 'Exactly as without Shift, with acceleration.' : 'Exactly as without Shift.'],
    ['own', 'Own', 'Something else under Shift: another parameter, range or mode.'],
    ['block', 'Nothing', 'Does nothing while Shift is on.'],
  ];
  const cur = opts.find((o) => o[0] === state);
  return h('div', { class: 'shift-box' },
    h('div', { class: 'flabel' }, 'With Shift held, this control'),
    h('div', { class: 'seg full' }, ...opts.map(([k, n]) => h('button', {
      class: state === k ? 'on' : '',
      onclick: () => edit(`Shift: ${n.toLowerCase()}`, () => { for (const i of ids) cfg.setShiftState(ctlAt(i), cfg.kind(i), k); }),
    }, n))),
    h('div', { class: 'help' }, cur ? cur[2] : 'The selected controls differ.'),
    state === 'own' && ids.length === 1 ? h('div', { class: 'help' }, 'Starts as a copy and then stays as you set it, whatever changes without Shift.') : null);
}

function field(label, input, help, opts = {}) {
  // The label names the control, also when it sits in a wrapper with its unit
  const target = input.id ? input : input.querySelector?.('input[id], select[id], textarea[id]');
  return h('div', { class: `field ${opts.mixed ? 'mixed' : ''}` }, h('label', { for: target?.id || undefined }, label), input, help ? h('div', { class: 'help' }, help) : null);
}

const numRange = (v) => { const n = v.filter((x) => typeof x === 'number'); return n.length ? `Mixed: ${Math.min(...n)}–${Math.max(...n)}` : 'Mixed'; };
// Field editors for one or more layers of the same kind (several: a field shows "mixed" where they differ).
function fieldsFor(box, kind, layers, isShift) {
  const vals = (json) => layers.map((l) => S.value(l, json, kind));
  const one = (json) => { const v = vals(json); return v.every((x) => x === v[0]) ? v[0] : '__mixed'; };
  const set = (json, v, label) => edit(`${label}${layers.length > 1 ? ` ×${layers.length}` : ''}`, () => { for (const l of layers) cfg.setField(l, kind, json, v); });
  const mixedOpt = (m) => (m ? h('option', { selected: true, disabled: true, value: '' }, 'Mixed') : null);
  const type = one('type');

  const types = S.types(kind).filter((t) => !(isShift && t === 'none'));
  const learnIds = IDS.filter((i) => st.sel.has(i) && !(isShift && cfg.isNav(i)));
  if (!st.learn) box.append(h('div', { class: 'actions learn-row' }, btn('learn', layers.length > 1 ? `Learn ${layers.length} from the instrument` : 'Learn from the instrument', { disabled: dev.real && !MixerDevice.supported(), onclick: () => startLearn(learnIds) }),
    dev.real && !MixerDevice.supported() ? h('span', { class: 'help' }, 'Learn requires a browser with Web MIDI.') : null));
  box.append(field('Sends', h('select', { id: 'f-type', onchange: (e) => set('type', e.target.value, 'message') },
    mixedOpt(type === '__mixed'),
    ...types.map((t) => h('option', { value: t, selected: t === type }, TYPE_LABEL[t]))),
  type !== '__mixed' ? TYPE_HELP[type] : null, { mixed: type === '__mixed' }));
  if (type === '__mixed' || type === 'none') return;
  const pc = cfg.presetChannel(st.preset);

  const ch = one('channel');
  const num = S.hasNumber(type) ? one('number') : undefined;
  const chSel = h('select', { id: 'f-ch', onchange: (e) => set('channel', e.target.value === 'preset' ? 'preset' : Number(e.target.value), 'channel') },
    mixedOpt(ch === '__mixed'),
    h('option', { value: 'preset', selected: ch === 'preset' }, `Preset channel (${pc})`),
    ...Array.from({ length: 16 }, (_, i) => h('option', { value: i + 1, selected: ch === i + 1 }, `Channel ${i + 1}`)));
  const chRow = h('div', { class: 'row' }, chSel, layers.length > 1 ? h('button', { onclick: () => fillDialog(kind, layers, 'channel') }, 'Series…') : null);
  box.append(field('Channel', chRow, ch === 'preset' ? 'Follows the preset’s channel.' : null, { mixed: ch === '__mixed' }));

  if (S.hasNumber(type)) {
    const max = S.numberMax(type);
    const mixed = num === '__mixed';
    const numIn = h('input', { id: 'f-num', type: 'number', inputmode: 'numeric', min: 0, max, value: mixed ? '' : num, placeholder: mixed ? numRange(vals('number')) : '', onchange: (e) => set('number', Number(e.target.value), 'number') });
    const row = h('div', { class: 'row' }, numIn);
    let help = `0–${max}`;
    let label = 'Controller number';
    if (type === 'nrpn' || type === 'rpn') {
      label = `${type.toUpperCase()} parameter`;
      if (!mixed) {
        const msb = h('input', { type: 'number', inputmode: 'numeric', min: 0, max: 127, value: num >> 7, 'aria-label': 'MSB', onchange: (e) => set('number', Number(e.target.value) * 128 + (num & 127), 'number') });
        const lsb = h('input', { type: 'number', inputmode: 'numeric', min: 0, max: 127, value: num & 127, 'aria-label': 'LSB', onchange: (e) => set('number', (num >> 7) * 128 + Number(e.target.value), 'number') });
        row.append(h('span', { class: 'eq' }, 'MSB'), msb, h('span', { class: 'eq' }, 'LSB'), lsb);
      }
      help = 'As in the instrument’s manual: one number (0–16383) or MSB and LSB.';
    }
    if (type === 'note') { label = 'Note'; if (!mixed) help = `${noteName(num)} — middle C (60) is ${noteName(60)}; Settings › Preset › Note names`; }
    if (type === 'cc14') help = mixed ? '0–31' : `0–31: sends on CC ${num} and CC ${num + 32}`;
    if (layers.length > 1) row.append(h('button', { onclick: () => fillDialog(kind, layers, 'number') }, 'Series…'));
    box.append(field(label, row, help, { mixed }));
  }

  const modes = S.modeEnum(kind).values;
  const mode = one('mode');
  const ml = MODE_LABEL[kind];
  const isProgramButton = kind === 'button' && type === 'program';
  const relative = type.startsWith('relative');
  if (!(relative && kind === 'encoder')) {
    const labelOf = (m) => (kind === 'fader' && m === 'global' ? `Default (${ml[cfg.globalSetting('pickup')][0]})` : ml[m][0]);
    box.append(field(kind === 'fader' ? 'Behaviour' : kind === 'encoder' ? 'Turning' : 'Press',
      h('select', { id: 'f-mode', onchange: (e) => set('mode', e.target.value, 'mode') },
        mixedOpt(mode === '__mixed'),
        ...modes.map((m) => h('option', { value: m.json, selected: m.json === mode }, labelOf(m.json)))),
      mode !== '__mixed' ? (isProgramButton ? PROGRAM_BUTTON_HELP.mode : ml[mode][1]) : null, { mixed: mode === '__mixed' }));
  }
  if (mode === 'div' || mode === 'step') {
    const nv = one('n');
    box.append(field(mode === 'div' ? 'Clicks per step' : 'Step size', h('input', { id: 'f-n', type: 'number', inputmode: 'numeric', min: 1, max: 127, value: nv === '__mixed' ? '' : nv, placeholder: nv === '__mixed' ? 'Mixed' : '', onchange: (e) => set('n', Number(e.target.value), 'step') }), '1–127', { mixed: nv === '__mixed' }));
  }

  if (S.hasValue(type)) {
    const lo = one('lo'), hi = one('hi'), max = S.typeMax(type);
    // Programs are shown 1–128, as instruments number them; the file keeps 0–127.
    const off = type === 'program' ? 1 : 0;
    const shown = (v) => (v === '__mixed' ? '' : v + off);
    const loIn = h('input', { id: 'f-lo', type: 'number', inputmode: 'numeric', min: off, max: max + off, 'aria-label': isProgramButton ? 'LED off program' : kind === 'button' ? 'Off value' : 'Bottom value', value: shown(lo), placeholder: lo === '__mixed' ? 'Mixed' : '', onchange: (e) => set('lo', Number(e.target.value) - off, 'range') });
    const hiIn = h('input', { type: 'number', inputmode: 'numeric', min: off, max: max + off, 'aria-label': isProgramButton ? 'Program on press' : kind === 'button' ? 'On value' : 'Top value', value: shown(hi), placeholder: hi === '__mixed' ? 'Mixed' : '', onchange: (e) => set('hi', Number(e.target.value) - off, 'range') });
    const inv = !isProgramButton && lo !== '__mixed' && hi !== '__mixed' && lo > hi;
    const full = lo === 0 && hi === max;
    const valLabel = type === 'program' ? (isProgramButton ? PROGRAM_BUTTON_HELP.valuesLabel : 'Programs: bottom and top') : kind === 'button' ? 'Values: off and on' : 'Range: bottom and top';
    box.append(field(valLabel, h('div', { class: 'row' }, loIn, h('span', { class: 'eq' }, '→'), hiIn,
      h('button', { class: 'icon-only', title: isProgramButton ? PROGRAM_BUTTON_HELP.swap : 'Swap: reverse the direction', 'aria-label': isProgramButton ? 'Swap program values' : 'Reverse', onclick: () => edit('reverse', () => { for (const l of layers) { const a = S.value(l, 'lo', kind), b = S.value(l, 'hi', kind); cfg.setField(l, kind, 'lo', b); cfg.setField(l, kind, 'hi', a); } }) }, icon('swap', 16)),
      kind === 'button' && layers.length > 1
        ? h('button', { title: 'A different on value for each button', onclick: () => fillDialog(kind, layers, 'hi') }, 'Series…')
        : h('button', { disabled: full, title: isProgramButton ? PROGRAM_BUTTON_HELP.defaults : 'The whole range', onclick: () => edit('full range', () => { for (const l of layers) { delete l.lo; delete l.hi; } }) }, isProgramButton ? 'Defaults' : 'Full')),
    `${inv ? 'Reversed. ' : ''}${off}–${max + off}${isProgramButton ? '. ' + PROGRAM_BUTTON_HELP.values : ''}`, { mixed: lo === '__mixed' || hi === '__mixed' }));
    if (kind === 'encoder') {
      const sv = one('start');
      box.append(field('Starts at', h('input', { id: 'f-start', type: 'number', inputmode: 'numeric', min: off, max: max + off, value: shown(sv), onchange: (e) => set('start', Number(e.target.value) - off, 'start value') }), 'Until the instrument or another control sets the value.', { mixed: sv === '__mixed' }));
    }
  }

  if (kind === 'button') {
    const led = one('led');
    box.append(field('LED', h('div', { class: 'seg full' }, ...[['value', 'When on'], ['off', 'Always off'], ['on', 'Always on']].map(([k, n]) => h('button', { class: led === k ? 'on' : '', onclick: () => set('led', k, 'LED') }, n))),
      led === 'value' ? 'Lit while the value is on, by your presses or by the instrument.' : null, { mixed: led === '__mixed' }));
    const nr = one('noRelease');
    if (!isProgramButton && (S.value(layers[0], 'mode', kind) === 'momentary' || mode === '__mixed')) {
      box.append(h('label', { class: 'check' },
        h('input', { type: 'checkbox', checked: nr === true, onchange: (e) => set('noRelease', e.target.checked || undefined, 'release') }),
        h('span', {}, 'Send nothing on release')));
    }
  }
}

function sendsBox(kind, layer) {
  const d = describe(S, kind, layer, cfg.presetChannel(st.preset), cfg.presetSetting(st.preset, 'noteOff'));
  if (!d.bytes.length) return null;
  const fine = kind === 'encoder' && S.value(layer, 'mode', kind) === 'fine' ? ' Turning moves it in the smallest steps.' : '';
  return h('div', { class: 'sends' }, h('div', { class: 'flabel' }, 'MIDI it sends'), h('p', { class: 'stext' }, d.text + fine), h('div', { class: 'bytes' }, ...d.bytes.map((b) => h('code', {}, b))));
}


function actionsRow() {
  return h('div', { class: 'actions' },
    btn('copy', 'Copy', { onclick: copySel, title: 'Copy (⌘C)' }),
    btn('paste', 'Paste', { onclick: pasteSel, disabled: !st.clip || st.clip.kind !== 'controls', title: 'Paste (⌘V)' }),
    btn('trash', st.layer === 'shift' ? 'Nothing with Shift' : 'Clear', { class: 'danger', onclick: clearSel, title: 'Clear (Delete)' }));
}

function bulkInspector(box, ids) {
  const kinds = [...new Set(ids.map((i) => cfg.kind(i)))];
  const count = (k) => ids.filter((i) => cfg.kind(i) === k).length;
  box.append(insHead(`${presetLabel(st.preset)} · ${pageLabel(st.preset, st.page)}`, plural(ids.length, 'control'), null));
  box.append(h('p', { class: 'help' }, kinds.map((k) => plural(count(k), k)).join(', ')));
  box.append(layerSwitch());

  const navs = ids.filter((i) => cfg.isNav(i));
  const usable = st.layer === 'shift' ? ids.filter((i) => !cfg.isNav(i)) : ids;
  if (st.layer === 'shift' && navs.length) box.append(h('p', { class: 'note warn' }, icon('lock', 16), h('span', {}, `${plural(navs.length, 'page or preset key')} left as they are: with Shift they always select pages and presets.`)));

  const pat = h('input', { id: 'name-pattern', placeholder: 'e.g. Track {n}' });
  box.append(field(st.layer === 'shift' ? 'Names with Shift' : 'Names', h('div', { class: 'row' }, pat,
    h('button', { onclick: () => edit('names', () => { orderIds(usable, 'ltr').forEach((i, k) => { const v = pat.value.replaceAll('{n}', k + 1); if (cfg.canRename(ctlAt(i), cfg.kind(i), st.layer)) cfg.rename(ctlAt(i), cfg.kind(i), st.layer, v); }); }) }, 'Apply')),
  st.layer === 'shift' ? '{n} becomes 1, 2, 3… from left to right; only controls with a Shift layer of their own (“Own”) take a name with Shift. Leave empty to remove them.' : '{n} becomes 1, 2, 3… from left to right. Leave empty to remove the names.'));

  if (st.layer === 'shift') {
    const states = usable.map((i) => cfg.shiftState(ctlAt(i), cfg.kind(i)));
    const s = states.every((x) => x === states[0]) ? states[0] : null;
    box.append(shiftBox(usable, s));
    if (s !== 'own') { box.append(actionsRow()); return; }
  }
  const layers = usable.map((i) => (st.layer === 'normal' ? ctlAt(i) : ctlAt(i).shift));

  if (kinds.length > 1) {
    box.append(h('p', { class: 'note' }, icon('info', 16), h('span', {}, 'Different kinds of controls: only what they have in common. To edit everything, keep one kind:')),
      h('div', { class: 'actions' }, ...kinds.map((k) => h('button', { onclick: () => selectIds(ids.filter((i) => cfg.kind(i) === k), false) }, `Only ${k}s`))));
    const common = S.types('fader').filter((t) => kinds.every((k) => S.types(k).includes(t)) && !(st.layer === 'shift' && t === 'none'));
    box.append(field('Sends', h('select', { onchange: (e) => edit(`message ×${layers.length}`, () => usable.forEach((i, k) => cfg.setField(layers[k], cfg.kind(i), 'type', e.target.value))) },
      h('option', { selected: true, disabled: true }, 'Mixed'), ...common.map((t) => h('option', { value: t }, TYPE_LABEL[t])))));
    box.append(field('Channel', h('div', { class: 'row' },
      h('select', { onchange: (e) => edit(`channel ×${layers.length}`, () => usable.forEach((i, k) => { if ((layers[k].type ?? 'none') !== 'none') cfg.setField(layers[k], cfg.kind(i), 'channel', e.target.value === 'preset' ? 'preset' : Number(e.target.value)); })) },
        h('option', { selected: true, disabled: true }, 'Mixed'), h('option', { value: 'preset' }, `Preset channel (${cfg.presetChannel(st.preset)})`), ...Array.from({ length: 16 }, (_, i) => h('option', { value: i + 1 }, `Channel ${i + 1}`))),
      h('button', { onclick: () => fillDialog(null, layers, 'channel', usable) }, 'Series…'))));
    box.append(actionsRow());
    return;
  }
  fieldsFor(box, kinds[0], layers, st.layer === 'shift');

  box.append(actionsRow());
}

// Strip rows from top to bottom, for the "row by row" order.
const ROWS = ['encoder', 'fader', 'm', 's', 'r', 'select'];
function orderIds(ids, order, reverse = false) {
  const pos = (i) => IDS.indexOf(i);
  const rowOf = (i) => (i.startsWith('row.') ? 99 : ROWS.indexOf(cfg.key(i)));
  const out = [...ids];
  if (order === 'rows') out.sort((a, b) => rowOf(a) - rowOf(b) || pos(a) - pos(b));
  else out.sort((a, b) => pos(a) - pos(b));
  if (order === 'rtl' || reverse) out.reverse();
  return out;
}

// What a layer takes on the wire, to spot two controls that would share one value (D-5).
// A CC 14-bit takes its LSB's CC as well.
function slotsOf(layer, pc) {
  const t = layer?.type ?? 'none';
  if (t === 'none') return [];
  const ch = resolveChannel(layer, pc);
  const n = layer.number ?? 0;
  if (t === 'cc7' || t.startsWith('relative')) return [`${ch}/cc/${n}`];
  if (t === 'cc14') return [`${ch}/cc/${n}`, `${ch}/cc/${n + 32}`];
  if (S.hasNumber(t)) return [`${ch}/${t}/${n}`];
  return [`${ch}/${t}`];
}


// Fill one field of the selected controls with a series: 'channel', 'number', or 'hi' (a button's
// on value, for example the program of a Program Change button). The dialog speaks the type's
// terms: MSB and LSB for NRPN and RPN, note names, programs 1–128, CC n + n+32 for CC 14-bit.
function fillDialog(kind, layers, fld, idsOpt) {
  const ids = idsOpt ?? IDS.filter((i) => st.sel.has(i) && !(st.layer === 'shift' && cfg.isNav(i)));
  const map = new Map(ids.map((i, k) => [i, layers[k]]));
  const pc = cfg.presetChannel(st.preset);
  const usable = (l) => l && (l.type ?? 'none') !== 'none' && (fld !== 'number' || S.hasNumber(l.type)) && (fld !== 'hi' || S.hasValue(l.type));
  const types = [...new Set(ids.map((i) => map.get(i)).filter(usable).map((l) => l.type))];
  const type = types.length === 1 ? types[0] : null;
  const isPair = fld === 'number' && (type === 'nrpn' || type === 'rpn');
  const isNote = fld === 'number' && type === 'note';
  const isProgram = fld === 'hi' && type === 'program';
  const off = isProgram ? 1 : 0; // programs are shown 1–128

  // The row order only matters when the selection spans more than one row.
  const rowCount = new Set(ids.map((i) => (i.startsWith('row.') ? 'row' : cfg.key(i)))).size;
  const order = h('select', { id: 'fill-order' }, ...(rowCount > 1
    ? [h('option', { value: 'rows' }, 'Row by row: along a row, then the next row'), h('option', { value: 'ltr' }, 'Strip by strip: down a strip, then the next strip')]
    : [h('option', { value: 'ltr' }, 'Left to right')]));
  const rev = h('input', { type: 'checkbox', id: 'fill-rev' });
  const ordered = () => orderIds(ids, order.value, rev.checked);

  const num = (id, min, max) => h('input', { id, type: 'number', inputmode: 'numeric', min, max });
  const start = isNote ? h('input', { id: 'fill-start', type: 'text', autocomplete: 'off', spellcheck: 'false' }) : num('fill-start');
  const msb = num('fill-msb', 0, 127), lsb = num('fill-lsb', 0, 127);
  const step = num('fill-step');
  step.value = 1;
  let part = 'lsb';
  let touched = false;
  const setStart = () => {
    const l = ordered().map((i) => map.get(i)).find(usable);
    let v = 0;
    if (l) v = fld === 'channel' ? resolveChannel(l, pc) : fld === 'hi' ? S.value(l, 'hi', cfg.kind(ids[0])) + off : (l.number ?? 0);
    // A start that would run the series out of range (a button's on value is often the top) starts at the bottom.
    const count = ids.filter((i) => usable(map.get(i))).length;
    const top = !l ? 0 : fld === 'channel' ? 16 : fld === 'hi' ? S.typeMax(l.type) + off : isPair && part === 'lsb' ? (v | 127) : S.numberMax(l.type);
    if (l && v + count - 1 > top) v = fld === 'channel' ? 1 : isPair && part === 'lsb' ? v & ~127 : off;
    msb.value = v >> 7; lsb.value = v & 127;
    start.value = isNote ? noteName(v) : v;
  };
  setStart();

  const names = h('input', { id: 'fill-names', placeholder: 'e.g. Osc {n}' });
  const trigger = h('input', { type: 'checkbox', checked: isProgram && ids.some((i) => usable(map.get(i)) && S.value(map.get(i), 'mode', 'button') === 'momentary' && !map.get(i).noRelease) });

  // What the rest of the page sends, to warn when the series would share a value with it.
  const outside = new Map();
  const mine = new Set(ids);
  const note = (l, who) => { for (const s of slotsOf(l, pc)) { if (!outside.has(s)) outside.set(s, []); outside.get(s).push(who); } };
  for (const id of IDS) {
    const c = ctlAt(id);
    if (!c) continue;
    if (!(mine.has(id) && st.layer === 'normal')) note(c, ctlName(id));
    if (c.shift && typeof c.shift === 'object' && !(mine.has(id) && st.layer === 'shift')) note(c.shift, `${ctlName(id)} (Shift)`);
  }

  const valueAt = (k, l) => {
    const d = Number(step.value) || 0;
    if (fld === 'channel') { const v = Number(start.value) + k * d; return { v, label: `Ch ${v}`, err: !(v >= 1 && v <= 16) }; }
    if (fld === 'hi') {
      const shown = Number(start.value) + k * d, v = shown - off;
      return { v, label: isProgram ? `Prog ${shown}` : `${shown}`, err: !(v >= 0 && v <= S.typeMax(l.type)) };
    }
    if (isPair) {
      let m = Number(msb.value), s = Number(lsb.value), v;
      if (part === 'whole') { v = Number(start.value) + k * d; m = v >> 7; s = v & 127; }
      else { if (part === 'lsb') s += k * d; else m += k * d; v = m * 128 + s; }
      const err = part === 'whole' ? !(v >= 0 && v <= 16383) : !(m >= 0 && m <= 127 && s >= 0 && s <= 127);
      return { v, label: `${m}:${s}`, err };
    }
    const v = (isNote ? noteNumber(start.value) : Number(start.value)) + k * d;
    const err = !(Number.isFinite(v) && v >= 0 && v <= S.numberMax(l.type));
    const label = err ? String(v) : isNote ? noteName(v) : l.type === 'cc14' ? `CC ${v}+${v + 32}` : `CC ${v}`;
    return { v, label, err };
  };

  const plan = () => {
    const seen = new Map();
    let k = 0;
    return ordered().map((id) => {
      const l = map.get(id);
      if (!usable(l)) return { id, skip: true };
      const it = { id, l, ...valueAt(k++, l) };
      if (!it.err && fld !== 'hi') {
        const slots = slotsOf({ ...l, [fld]: it.v }, pc);
        const clash = slots.flatMap((s) => [...(outside.get(s) ?? []), ...(seen.has(s) ? [ctlName(seen.get(s))] : [])]);
        for (const s of slots) if (!seen.has(s)) seen.set(s, id);
        it.clash = [...new Set(clash)];
      }
      return it;
    });
  };

  // Start fields: MSB and LSB, or one value.
  const what = fld === 'channel' ? 'First channel' : isNote ? 'First note' : isProgram ? 'First program' : fld === 'hi' ? 'First value' : isPair ? 'First number' : 'First number';
  const fStart = field(what, start, isNote ? 'C3, F#2 or 0–127' : isProgram ? '1–128' : null);
  const fMsb = field('First MSB', msb), fLsb = field('First LSB', lsb);
  const fStep = field(isNote ? 'Step, semitones' : 'Step', step);
  const startRow = h('div', { class: 'form3' });
  const partSeg = h('div', { class: 'seg full', role: 'tablist' }, ...[['lsb', 'LSB'], ['msb', 'MSB'], ['whole', 'Whole number']].map(([key, n]) => h('button', {
    'data-part': key,
    onclick: () => {
      if (key === 'whole' && part !== 'whole') start.value = Number(msb.value) * 128 + Number(lsb.value);
      if (key !== 'whole' && part === 'whole') { const v = Number(start.value); msb.value = v >> 7; lsb.value = v & 127; }
      part = key; show();
    },
  }, n)));

  const prev = h('div', { class: 'preview fill' });
  const summary = h('p', { class: 'help fill-sum' });
  const apply = h('button', { class: 'primary' }, 'Apply');
  const show = () => {
    for (const b of partSeg.children) b.classList.toggle('on', b.dataset.part === part);
    put(startRow, isPair && part !== 'whole' ? [fMsb, fLsb, fStep] : [fStart, fStep]);
    const items = plan();
    put(prev, items.map((it) => h('span', {
      class: ['pv', it.skip && 'skip', it.err && 'err', it.clash?.length && 'clash'].filter(Boolean).join(' '),
      title: it.skip ? 'Sends nothing that this changes: left as it is' : it.err ? 'Out of range' : it.clash?.length ? `Already assigned to ${it.clash.join(', ')}` : '',
    }, h('small', {}, ctlName(it.id)), h('b', {}, it.skip ? '—' : it.label), it.clash?.length ? icon('copy', 12) : null)));
    const active = items.filter((i) => !i.skip);
    const errs = active.filter((i) => i.err).length, clashes = active.filter((i) => i.clash?.length).length;
    const bits = [`${plural(active.length, 'control')} get${active.length === 1 ? 's' : ''} a value`];
    if (items.length > active.length) bits.push(`${items.length - active.length} left as ${items.length - active.length === 1 ? 'it is' : 'they are'} (—)`);
    if (isNote && Number.isNaN(noteNumber(start.value))) bits.push('The first note is not a note: write C3, F#2 or a number 0–127');
    else if (errs) bits.push(`${plural(errs, 'value')} out of range: change the start or the step`);
    if (clashes) bits.push(`${plural(clashes, 'control')} would send a message another control already sends (each keeps its own settings)`);
    if (fld === 'channel' && active.some((i) => (i.l.channel ?? 'preset') === 'preset')) bits.push('they no longer follow the preset channel');
    summary.textContent = `${bits.map((b) => b[0].toUpperCase() + b.slice(1)).join('. ')}.`;
    summary.classList.toggle('bad', errs > 0);
    apply.disabled = !active.length || errs > 0;
  };
  for (const e of [start, msb, lsb]) e.addEventListener('input', () => { touched = true; show(); });
  step.addEventListener('input', show);
  for (const e of [order, rev]) e.addEventListener('input', () => { if (!touched) setStart(); show(); });

  const title = fld === 'channel' ? 'Channels in series' : isPair ? `${type.toUpperCase()} parameters in series` : isNote ? 'Notes in series' : isProgram ? 'Programs in series' : fld === 'hi' ? 'Values in series' : 'Numbers in series';
  const intro = isPair ? 'What goes up: the LSB within one MSB (most synths number their parameters this way), the MSB with the same LSB (one parameter across oscillators or parts), or the whole number.'
    : isProgram ? 'Each button selects the next program.'
      : fld === 'hi' ? 'Each button sends the next value when pressed: for example one CC that picks a waveform.'
        : null;

  apply.addEventListener('click', () => {
    const items = plan().filter((i) => !i.skip);
    edit(`${isProgram ? 'program' : fld === 'hi' ? 'value' : fld} series`, () => {
      items.forEach((it, k) => {
        const kd = cfg.kind(it.id);
        cfg.setField(it.l, kd, fld, it.v);
        if (isProgram && trigger.checked && S.value(it.l, 'mode', kd) === 'momentary') cfg.setField(it.l, kd, 'mode', 'trigger');
        const nm = names.value.replaceAll('{n}', k + 1).trim();
        if (nm) ctlAt(it.id).name = nm;
      });
    });
    closeDialog();
    toast(`${plural(items.length, 'control')} filled. Undo takes it back.`);
  });

  show();
  dialog(h('div', { class: 'fill-dlg' },
    h('h2', {}, title),
    intro ? h('p', { class: 'help' }, intro) : null,
    isPair ? field('What goes up', partSeg) : null,
    startRow,
    h('div', { class: 'form3' }, field('Order', order), field('Names', names, 'Optional. {n} becomes 1, 2, 3…')),
    h('label', { class: 'check' }, rev, h('span', {}, 'Reverse the order')),
    isProgram ? h('label', { class: 'check' }, trigger, h('span', {}, 'Send the program only on press, nothing on release')) : null,
    prev, summary,
    h('div', { class: 'dlg-foot' }, h('button', { onclick: closeDialog }, 'Cancel'), apply)));
  (isPair ? lsb : start).focus();
}

// ---- copy, paste, clear ------------------------------------------------------------------------

// Copy, paste and clear act on the layer shown (Normal or Shift), never on the hidden one: what you
// see is what is copied. A layer copied under Normal can go onto Shift and back, so a Shift layer
// that differs in one setting starts as a copy. Names stay with their controls.
function copySel() {
  const ids = IDS.filter((i) => st.sel.has(i) && !(st.layer === 'shift' && cfg.isNav(i)));
  if (!ids.length) return;
  st.clip = { kind: 'controls', layer: st.layer, items: ids.map((i) => ({ id: i, kind: cfg.kind(i), layer: cfg.layerOf(ctlAt(i), cfg.kind(i), st.layer) })) };
  toast(`Copied ${plural(ids.length, 'control')}, ${st.layer === 'shift' ? 'with Shift' : 'without Shift'}`);
  renderSide();
}
function pasteSel() {
  if (!st.clip || st.clip.kind !== 'controls') return;
  const targets = IDS.filter((i) => st.sel.has(i));
  const items = st.clip.items;
  let done = 0, skipped = 0;
  const put = (t, it) => {
    if (st.layer === 'shift' && cfg.isNav(t)) { skipped++; return; }
    if (it.layer && !S.types(cfg.kind(t)).includes(it.layer.type)) { skipped++; return; }
    cfg.putLayer(ctlAt(t), cfg.kind(t), st.layer, it.layer); done++;
  };
  edit('paste', () => {
    if (items.length === 1) {
      for (const t of targets.length ? targets : [items[0].id]) {
        if (cfg.kind(t) !== items[0].kind) { skipped++; continue; }
        put(t, items[0]);
      }
    } else {
      const stripOf = (x) => (x.startsWith('row.') ? 99 : cfg.strip(x));
      const strip0 = Math.min(...items.map((x) => stripOf(x.id)));
      const t0 = targets.length ? Math.min(...targets.map(stripOf)) : strip0;
      for (const it of items) {
        let t = it.id;
        if (!t.startsWith('row.')) { const s = cfg.strip(t) - strip0 + (t0 === 99 ? strip0 : t0); if (s > 7) { skipped++; continue; } t = `s${s}.${cfg.key(t)}`; }
        put(t, it);
      }
    }
  });
  const onto = st.layer === 'shift' ? 'with Shift' : 'without Shift';
  const from = st.clip.layer && st.clip.layer !== st.layer ? ` (copied ${st.clip.layer === 'shift' ? 'with' : 'without'} Shift)` : '';
  toast(`Pasted onto ${plural(done, 'control')}, ${onto}${from}${skipped ? `; ${skipped} skipped (another kind, a message it cannot send, or past strip 8)` : ''}. Undo takes it back.`);
}
function clearSel() {
  const ids = IDS.filter((i) => st.sel.has(i));
  if (!ids.length) return;
  edit(st.layer === 'shift' ? 'nothing with Shift' : `clear ${plural(ids.length, 'control')}`, () => {
    for (const i of ids) {
      if (st.layer === 'shift' && cfg.isNav(i)) continue;
      cfg.putLayer(ctlAt(i), cfg.kind(i), st.layer, null);
    }
  });
  toast(`${st.layer === 'shift' ? 'Set to nothing with Shift' : 'Cleared without Shift (a Shift layer of its own stays)'}. Undo brings it back.`);
}

// ---- mapping a synth's parameters -------------------------------------------------------------------

// ---- mapping an instrument: a mode of its own (docs/app.md, "Mapping an instrument") ------------------
// The side panel shows the chosen instrument's parameters instead of the control's settings, until
// "Done". Instruments come from MIDI Guide or the built-in lists.
const curList = () => st.instrument ?? PARAMETER_LISTS[0];
const recentKeys = () => { try { return JSON.parse(store.get('smc-recent-instruments') || '[]'); } catch { return []; } };
// key → parameter list, kept in app storage so an instrument opened once works offline.
const guideCache = new Map((() => { try { return JSON.parse(store.get('smc-guide-lists-v1') || '[]'); } catch { return []; } })());
const keepGuide = (key, list) => { guideCache.delete(key); guideCache.set(key, list); store.set('smc-guide-lists-v1', JSON.stringify([...guideCache].slice(-20))); };
const guideIndexCache = {
  get: (key) => key === 'midiguide:v1:index' ? JSON.parse(store.get(key) || 'null') : null,
  set: (key, value) => { if (key === 'midiguide:v1:index') store.set(key, JSON.stringify(value)); },
};
function useInstrument(list, key) {
  st.instrument = list; st.instrumentKey = key; st.armed = list.params.findIndex((p) => !p.needs); st.armed = st.armed < 0 ? null : st.armed; st.used.clear(); st.pq = '';
  store.set('smc-recent-instruments', JSON.stringify([key, ...recentKeys().filter((k) => k !== key)].slice(0, 5)));
  closeDialog(); enterMapping();
}
function enterMapping() {
  if (!st.instrument) { const k = recentKeys()[0], l = k && listByKey(k); if (l) { st.instrument = l; st.instrumentKey = k; st.armed = nextUsable(0); st.used.clear(); } }
  if (!st.instrument) { instrumentDialog(); return; }
  st.tab = 'params'; sheet(true); renderAll();
}
function leaveMapping() { st.tab = 'inspector'; st.armed = null; renderAll(); }
const listByKey = (key) => {
  if (key.startsWith('builtin:')) return PARAMETER_LISTS[Number(key.slice(8))];
  return guideCache.get(key) ?? null;
};

function instrumentDialog() {
  const q = h('input', { id: 'inst-search', type: 'search', placeholder: 'Maker or instrument', 'aria-label': 'Find an instrument', autocomplete: 'off' });
  const body = h('div', { class: 'inst-list' });
  const status = h('div', { class: 'inst-status' });
  let index, indexError, request = 0;
  const row = (name, sub, onclick, extra) => h('button', { class: 'inst', onclick }, h('span', { class: 'grow' }, h('b', {}, name), h('small', {}, sub)), extra ?? icon('chevron', 14));
  const loadGuide = async (m, d) => {
    const current = ++request;
    const key = `guide:${m.slug}/${d.slug}`;
    if (guideCache.has(key)) { useInstrument(guideCache.get(key), key); return; }
    put(status, h('div', { class: 'loading' }, h('span', { class: 'spin' }), `Loading ${m.name} ${d.name} from MIDI Guide…`));
    try {
      const list = fromMidiGuide(await loadDevice(m.slug, d.slug, { cache: guideIndexCache }));
      if (!list.params.some((p) => !p.needs)) throw new Error('No parameters supported by this firmware.');
      keepGuide(key, list);
      if (current === request && status.isConnected && $('#dlg').open) useInstrument(list, key);
    } catch (e) {
      if (current === request && status.isConnected) put(status, h('p', { class: 'note warn', role: 'status' }, `Could not load ${m.name} ${d.name}: ${e.message}`));
    }
  };
  const draw = () => {
    const t = q.value.trim().toLowerCase();
    const hit = (s) => !t || s.toLowerCase().includes(t);
    const recent = recentKeys().map((k) => [k, listByKey(k)]).filter(([, l]) => l && hit(l.name));
    const builtin = PARAMETER_LISTS.map((l, i) => [l, i]).filter(([l]) => !l.source && hit(l.name));
    const makers = index?.makers.map((m) => ({ m, devs: m.devices.filter((d) => hit(`${m.name} ${d.name}`)) })).filter((x) => x.devs.length);
    put(body,
      recent.length ? [h('h3', {}, 'Recently used'), ...recent.map(([k, l]) => row(l.name, `${plural(l.params.length, 'parameter')}${k.startsWith('guide:') ? ' · saved for offline use' : ''}`, () => useInstrument(l, k)))] : null,
      h('h3', {}, 'MIDI Guide'),
      h('p', { class: 'help' }, h('a', { href: MIDI_GUIDE_SOURCE.url, target: '_blank', rel: 'noopener' }, MIDI_GUIDE_SOURCE.name), ` by ${MIDI_GUIDE_SOURCE.author} · `, h('a', { href: MIDI_GUIDE_SOURCE.licenseUrl, target: '_blank', rel: 'noopener' }, MIDI_GUIDE_SOURCE.license)),
      index?.stale ? h('p', { class: 'help' }, 'Showing the saved catalogue. Previously opened instruments are available offline.') : null,
      makers?.length ? makers.flatMap(({ m, devs }) => [h('div', { class: 'maker' }, m.name), ...devs.map((d) => row(d.name, guideCache.has(`guide:${m.slug}/${d.slug}`) ? 'Saved for offline use' : d.has_triggers ? 'Includes drum notes' : '', () => loadGuide(m, d)))])
        : h('p', { class: 'help', role: 'status' }, indexError ? `Catalogue unavailable: ${indexError.message}` : index ? 'No matching instrument in MIDI Guide.' : 'Loading the MIDI Guide catalogue…'),
      builtin.length ? [h('h3', {}, 'Built in'), ...builtin.map(([l, i]) => row(l.name, plural(l.params.length, 'parameter'), () => useInstrument(l, `builtin:${i}`)))] : null);
  };
  q.addEventListener('input', draw);
  draw();
  dialog(h('div', { class: 'inst-dlg' },
    h('div', { class: 'dlg-head' }, h('h2', {}, 'Choose an instrument'), h('button', { class: 'icon-only', 'aria-label': 'Close', onclick: closeDialog }, icon('close'))),
    h('p', { class: 'help' }, 'Its parameters, with their names and messages, go onto the mixer’s controls: tap a parameter, then a control.'),
    q, status, body));
  q.focus();
  loadIndex({ cache: guideIndexCache }).then((value) => { index = value; draw(); }, (e) => { indexError = e; draw(); });
}

// A whole section of the instrument onto this page. Predictable on purpose: knobs and sliders go
// onto the faders left to right, then the encoders; switches and drum notes onto the strip buttons,
// row by row (M, S, R, then the select buttons). A control that already holds one of this
// instrument's parameters is kept, so pressing it for several sections fills the page in turn; a
// parameter already on this page is skipped.
function holdsInstrumentParam(id) {
  const c = ctlAt(id);
  return curList().params.some((p) => c.type === p.type && (c.number ?? 0) === (p.number ?? 0) && c.name === p.name);
}
function sectionPlan(group) {
  const list = curList().params;
  const todo = list.map((p, i) => [p, i]).filter(([p]) => p.group === group && !p.needs && !placedPages(p).has(st.page));
  const free = (ids) => ids.filter((i) => !holdsInstrumentParam(i));
  const contIds = free([...IDS.filter((i) => cfg.key(i) === 'fader'), ...IDS.filter((i) => cfg.key(i) === 'encoder')]);
  const btnIds = free(orderIds(IDS.filter((i) => ['m', 's', 'r', 'select'].includes(cfg.key(i))), 'rows'));
  const plan = [], left = [];
  let c = 0, b = 0;
  for (const [p, i] of todo) {
    const ids = p.button ? btnIds : contIds;
    const k = p.button ? b++ : c++;
    if (k < ids.length) plan.push([ids[k], i]); else left.push(p.name);
  }
  return { plan, left, skipped: list.filter((p) => p.group === group && !p.needs).length - todo.length };
}
function markPlan(group) {
  const ids = new Set(group ? sectionPlan(group).plan.map(([id]) => id) : []);
  for (const el of document.querySelectorAll('#panel .ctl')) el.classList.toggle('target', ids.has(el.dataset.id));
}
function mapSection(group) {
  const { plan, left, skipped } = sectionPlan(group);
  if (!plan.length) { toast(left.length ? `${group}: every free control of this kind holds a parameter already. Map the rest one by one, or on another page.` : `${group} is on this page already.`); return; }
  let done = 0;
  edit(`map ${group}`, () => { for (const [id, i] of plan) if (assignParam(id, i)) done++; });
  markPlan(null);
  const where = plan.length > 1 ? `${ctlName(plan[0][0])} to ${ctlName(plan.at(-1)[0])}` : ctlName(plan[0][0]);
  toast(`${group}: ${plural(done, 'parameter')} on ${where}.${left.length ? ` ${left.length} did not fit.` : ''}${skipped ? ` ${skipped} already here.` : ''} Undo takes it back.`);
}

// The instrument card's second line. A MIDI Guide list: its size, then "MIDI Guide" with an icon,
// one link to this instrument's own file there (where its data can be read and corrected; the
// catalogue dialog names the source and its licence). A built-in list: its own description.
function instrumentSource(list) {
  const src = list.source;
  if (!src) return h('p', {}, list.about);
  const href = src.data ?? src.url;
  return h('p', { class: 'inst-src' }, `${plural(list.params.length, 'parameter')} · `,
    h('a', { href, target: '_blank', rel: 'noopener', title: `${list.name} in ${src.name}` }, src.name, icon('external', 12)));
}

function renderParams() {
  const box = $('#params');
  const list = curList();
  put(box,
    h('div', { class: 'inst-card' },
      h('small', {}, 'Instrument'),
      h('button', { class: 'inst-change', onclick: instrumentDialog }, 'Change'),
      h('b', {}, list.name),
      instrumentSource(list)),
    h('ol', { class: 'steps' },
      h('li', { class: st.armed === null ? 'now' : 'done' }, 'Tap a parameter, or “Onto this page” for a whole section: faders, then encoders; switches and drum notes on the buttons. Controls that already hold this instrument’s parameters are kept.'),
      h('li', { class: st.armed === null ? '' : 'now' }, 'Tap a control on the panel, or touch it on the mixer. The next parameter is ready by itself.')),
    st.armed !== null ? h('div', { class: 'armed-bar' },
      h('div', { class: 'ab-next' }, h('small', {}, 'Next'), h('b', {}, list.params[st.armed].name)),
      h('div', { class: 'ab-actions' },
        st.sel.size ? h('button', { title: 'Assign the list in order to the selected controls', onclick: fillSelectionFromList }, `Fill ${plural(st.sel.size, 'selected control')}`) : null,
        h('button', { onclick: () => { st.armed = null; renderAll(); } }, 'Stop'))) : null,
    h('input', { id: 'psearch', type: 'search', placeholder: 'Find a parameter', value: st.pq ?? '', 'aria-label': 'Find a parameter', oninput: (e) => { st.pq = e.target.value; renderParamList(); } }),
    h('ul', { class: 'plist', id: 'plist-items' }));
  renderParamList();
}
// Where a parameter already sits in this preset: the pages whose controls send it.
function placedPages(p) {
  const out = new Set();
  for (let g = 0; g < S.device.pages; g++) {
    for (const id of IDS) {
      const c = cfg.control(st.preset, g, id);
      if (c && c.type === p.type && (c.number ?? 0) === (p.number ?? 0) && c.name === p.name) out.add(id.startsWith('row.') ? st.page : g);
    }
  }
  return out;
}
function renderParamList() {
  const list = curList();
  const q = (st.pq ?? '').trim().toLowerCase();
  const arm = (i) => { st.armed = st.armed === i ? null : i; renderAll(); };
  const items = list.params.map((p, i) => [p, i]).filter(([p]) => !p.needs && (!q || `${p.group ?? ''} ${p.name}`.toLowerCase().includes(q)));
  let group = null;
  put($('#plist-items'), ...items.flatMap(([p, i]) => {
    const head = p.group && p.group !== group ? h('li', { class: 'pgroup' }, h('span', {}, (group = p.group)), h('button', {
      class: 'chip', title: `Put the ${p.group} section onto this page: the outlined controls`,
      onmouseenter: () => markPlan(p.group), onmouseleave: () => markPlan(null), onfocus: () => markPlan(p.group), onblur: () => markPlan(null),
      onclick: (e) => { e.stopPropagation(); mapSection(p.group); } }, 'Onto this page')) : null;
    const pages = placedPages(p);
    const here = pages.has(st.page);
    const elsewhere = [...pages].filter((g) => g !== st.page).map((g) => g + 1);
    return [head, h('li', {
      class: `${st.armed === i ? 'armed' : ''} ${here ? 'used' : ''}`, tabindex: '0',
      title: here ? 'Already on this page' : elsewhere.length ? `On page ${elsewhere.join(', ')}` : '',
      onclick: () => arm(i),
      onkeydown: (e) => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); arm(i); } },
    }, h('span', { class: 'pdot', style: { background: typeVar(p.type) } }), h('span', { class: 'pn' }, p.name),
    here ? icon('check', 14) : elsewhere.length ? h('span', { class: 'pg' }, `page ${elsewhere.join(', ')}`) : null,
    h('span', { class: 'pm' }, p.values ? `${shortLabel(S, p)} · ${p.values.length} values` : shortLabel(S, p)))];
  }), items.length ? null : h('li', { class: 'muted' }, 'Nothing found'));
}

function assignParam(id, i) {
  const p = curList().params[i];
  if (!p || p.needs) return false;
  const kind = cfg.kind(id);
  if (!S.types(kind).includes(p.type)) { toast(`${ctlName(id)} cannot send ${TYPE_LABEL[p.type]}`); return false; }
  const c = ctlAt(id);
  const n = { name: p.name, type: p.type };
  if (S.hasNumber(p.type)) n.number = p.number ?? 0;
  if (S.hasValue(p.type)) { if (p.lo !== undefined) n.lo = p.lo; if (p.hi !== undefined) n.hi = p.hi; }
  if (p.channel && p.channel !== cfg.presetChannel(st.preset)) n.channel = p.channel;
  if (kind === 'button' && p.values?.length === 2) { n.lo = p.values[0][0]; n.hi = p.values[1][0]; }
  if (kind === 'button' && p.button) n.mode = p.type === 'note' ? 'momentary' : 'toggle';
  if (kind === 'encoder' && p.mode) { n.mode = p.mode; n.n = p.n; }
  cfg.cleanLayer(n, kind);
  if (c.shift !== undefined && !(kind === 'encoder' && cfg.shiftState(c, kind) === 'fine')) n.shift = c.shift;
  cfg.setControl(st.preset, st.page, id, n);
  st.used.add(i);
  return true;
}
// The next parameter the firmware supports, from index i on; hidden (`needs`) entries are skipped.
function nextUsable(i) {
  const list = curList()?.params ?? [];
  while (i !== null && i < list.length && list[i].needs) i++;
  return i !== null && i < list.length ? i : null;
}
function assignArmed(id) {
  const i = st.armed;
  if (i === null || curList().params[i]?.needs) { st.armed = nextUsable(i ?? 0); renderAll(); return; }
  let ok = false;
  edit(`assign ${curList().params[i].name}`, () => { ok = assignParam(id, i); });
  if (!ok) return;
  st.armed = nextUsable(i + 1);
  renderAll();
  const el = document.querySelector(`#panel .ctl[data-id="${id}"]`);
  if (el) { el.classList.remove('flash'); void el.offsetWidth; el.classList.add('flash'); }
}
function fillSelectionFromList() {
  const ids = orderIds(IDS.filter((i) => st.sel.has(i)), 'ltr');
  const list = curList().params;
  let i = nextUsable(st.armed ?? 0), done = 0;
  edit('assign from the list', () => { for (const id of ids) { if (i === null) break; if (assignParam(id, i)) { i = nextUsable(i + 1); done++; } } });
  st.armed = i;
  toast(`Assigned ${plural(done, 'control')}`);
  renderAll();
}

// ---- panel input: select, or simulate the mixer ---------------------------------------------------------

// The inspector's bottom sheet on narrow screens: folded to its tabs, or open.
function sheet(open) {
  document.body.classList.toggle('sheet-min', !open);
  const b = $('#sheet-toggle');
  b.setAttribute('aria-expanded', open ? 'true' : 'false');
  b.title = open ? 'Hide the panel below' : 'Show the panel below';
}
function clickCtl(id, e) {
  sheet(true);
  if (st.armed !== null && st.tab === 'params') { assignArmed(id); return; }
  if (e.shiftKey || e.metaKey || e.ctrlKey || st.multi) { if (st.sel.has(id)) st.sel.delete(id); else st.sel.add(id); }
  else st.sel = new Set([id]);
  renderAll();
}

let lasso = null;
let play = null;
// The NRPN/RPN selection of each channel, as the mixer keeps it for its output.
const paramSel = new ParamSelection();
function onPanelDown(e) {
  const el = e.target.closest('.ctl');
  const fixed = e.target.closest('[data-fixed]');
  if (st.sim) {
    if (fixed) { playFixed(fixed.dataset.fixed, e); return; }
    if (!el) return;
    e.preventDefault();
    el.setPointerCapture(e.pointerId);
    playStart(el.dataset.id, e, el);
    return;
  }
  if (el || fixed || e.target.closest('.strip-head')) return;
  const wrap = $('#panel-wrap').getBoundingClientRect();
  lasso = { x: e.clientX, y: e.clientY, wrap, add: e.shiftKey || e.metaKey || st.multi, base: new Set(st.sel), moved: false };
  $('#panel').setPointerCapture(e.pointerId);
}
function onPanelMove(e) {
  if (play) { playMove(e); return; }
  if (!lasso) return;
  if (!lasso.moved && Math.abs(e.clientX - lasso.x) + Math.abs(e.clientY - lasso.y) < 6) return;
  lasso.moved = true;
  $('#panel').classList.add('lassoing');
  const r = { l: Math.min(e.clientX, lasso.x), t: Math.min(e.clientY, lasso.y), r: Math.max(e.clientX, lasso.x), b: Math.max(e.clientY, lasso.y) };
  const box = $('#lasso');
  const wrap = $('#panel-wrap');
  Object.assign(box.style, { left: `${r.l - lasso.wrap.left + wrap.scrollLeft}px`, top: `${r.t - lasso.wrap.top}px`, width: `${r.r - r.l}px`, height: `${r.b - r.t}px` });
  box.hidden = false;
  const sel = lasso.add ? new Set(lasso.base) : new Set();
  for (const c of document.querySelectorAll('#panel .ctl')) {
    const b = c.getBoundingClientRect();
    if (b.right > r.l && b.left < r.r && b.bottom > r.t && b.top < r.b && !(st.layer === 'shift' && c.classList.contains('nav-fixed'))) sel.add(c.dataset.id);
    c.classList.toggle('sel', sel.has(c.dataset.id));
  }
  lasso.sel = sel;
}
function onPanelUp(e) {
  if (st.shiftPress) { releaseShift(); return; }
  if (play) { playEnd(e); return; }
  if (!lasso) return;
  $('#lasso').hidden = true;
  $('#panel').classList.remove('lassoing');
  if (lasso.moved) { st.sel = lasso.sel ?? new Set(); renderAll(); }
  else if (!lasso.add && st.sel.size) { st.sel.clear(); renderAll(); }
  lasso = null;
}
function onPanelClick(e) {
  if (st.sim) return;
  const el = e.target.closest('.ctl');
  if (el) clickCtl(el.dataset.id, e);
}

// The simulator: what the mixer would do, enough to show touches and MIDI.
function playLayer(id) {
  const c = ctlAt(id, dev.state.preset, dev.state.page), kind = cfg.kind(id);
  if (!dev.state.shift) return cfg.normal(c);
  if (cfg.isNav(id)) return null;
  return cfg.shiftLayer(c, kind);
}
function playStart(id, e, el) {
  const kind = cfg.kind(id);
  if (dev.state.shift && id.startsWith('row.') && NAV[id.slice(4)]) {
    const [what, n] = NAV[id.slice(4)];
    if (what === 'page') dev.select(dev.state.preset, n); else dev.select(n, 0);
    return;
  }
  play = { id, kind, y: e.clientY, el, preset: dev.state.preset, layer: playLayer(id), sent: false };
  if (play.layer) paramSel.reselect(play.layer, cfg.presetChannel(play.preset));
  if (kind === 'fader') playFader(e);
  else if (kind === 'button') playButton(id, true);
  else dev.touch(id, []);
}
function playMove(e) {
  if (play.kind === 'fader') playFader(e);
  else if (play.kind === 'encoder') {
    const clicks = Math.trunc((play.y - e.clientY) / 6);
    if (clicks) { play.y -= clicks * 6; playEncoder(play.id, clicks); }
  }
}
function playEnd() {
  if (play?.kind === 'button') playButton(play.id, false);
  // a move or press that sent NRPN or RPN ends with the null parameter when the preset asks for it
  const l = play?.layer;
  if (l && play.sent && isParam(l) && cfg.presetSetting(play.preset, 'paramNull') === 'gesture') {
    dev.touch(play.id, paramSel.nullMessages(resolveChannel(l, cfg.presetChannel(play.preset))));
  }
  play = null;
}
function playSent(id, msgs) { if (play && play.id === id && msgs.length) play.sent = true; dev.touch(id, msgs); }
function playFader(e) {
  const track = play.el.querySelector('.track').getBoundingClientRect();
  const f = Math.max(0, Math.min(1, (track.bottom - 9 - e.clientY) / (track.height - 18)));
  const layer = playLayer(play.id);
  if (!layer || (layer.type ?? 'none') === 'none') { dev.touch(play.id, []); return; }
  const v = faderValue(S, layer, Math.round(f * FADER_MAX));
  const k = targetKey(layer, dev.state.preset);
  if (st.values.get(k) === v) return;
  st.values.set(k, v);
  playSent(play.id, messages(S, 'fader', layer, cfg.presetChannel(dev.state.preset), v, cfg.presetSetting(dev.state.preset, 'noteOff'), paramSel));
}
function playEncoder(id, clicks) {
  const layer = playLayer(id);
  if (!layer || (layer.type ?? 'none') === 'none') { dev.touch(id, []); return; }
  const t = layer.type;
  const pc = cfg.presetChannel(dev.state.preset);
  if (t.startsWith('relative')) {
    // div N keeps the clicks it has not sent yet, as the core does within a move
    const div = S.value(layer, 'mode', 'encoder') === 'div';
    const all = clicks + (div && play ? play.rem ?? 0 : 0);
    const d = Math.max(-63, Math.min(63, encoderDelta(S, layer, all, cfg.globalSetting('fine14'))));
    if (div && play) play.rem = all - d * (layer.n ?? 2);
    playSent(id, d ? messages(S, 'encoder', layer, pc, d) : []);
    return;
  }
  const lo = S.value(layer, 'lo', 'encoder'), hi = S.value(layer, 'hi', 'encoder');
  const k = targetKey(layer, dev.state.preset);
  const cur = st.values.get(k) ?? S.value(layer, 'start', 'encoder');
  const mode = S.value(layer, 'mode', 'encoder');
  const fine = S.is14(t) ? (cfg.globalSetting('fine14') === 'lsb' ? 1 : Math.max(1, Math.floor(Math.abs(hi - lo) / 2048))) : 1;
  const stepv = mode === 'step' ? layer.n : mode === 'div' ? 1 / layer.n : fine * (mode === 'fine' || mode === 'acc0' ? 1 : Math.min(8, Math.abs(clicks)));
  const dir = hi >= lo ? 1 : -1;
  const v = Math.max(Math.min(lo, hi), Math.min(Math.max(lo, hi), Math.round(cur + clicks * stepv * dir)));
  if (v === cur) { dev.touch(id, []); return; }
  st.values.set(k, v);
  playSent(id, messages(S, 'encoder', layer, pc, v, undefined, paramSel));
}
function playButton(id, down) {
  const layer = playLayer(id);
  if (!layer || (layer.type ?? 'none') === 'none') { if (down) dev.touch(id, []); return; }
  const lo = S.value(layer, 'lo', 'button') ?? 0;
  const k = targetKey(layer, dev.state.preset);
  const v = buttonValue(S, layer, down, st.values.get(k));
  if (v === null) return;
  if (k) st.values.set(k, v);
  const led = S.value(layer, 'led', 'button');
  if (led === 'on' || (led === 'value' && v !== lo)) st.lit.add(id); else st.lit.delete(id);
  playSent(id, messages(S, 'button', layer, cfg.presetChannel(dev.state.preset), v, cfg.presetSetting(dev.state.preset, 'noteOff'), paramSel));
}
function playFixed(which, e) {
  if (which !== 'shift') { toast('BT: hold it to turn Bluetooth on or off.'); return; }
  const mode = cfg.presetSetting(dev.state.preset, 'shiftMode');
  const was = dev.state.shift;
  if (mode === 'latch') { dev.setShift(!was); return; }
  dev.setShift(true);
  st.shiftPress = { mode, was, t: performance.now() };
  if (e) $('#panel').setPointerCapture(e.pointerId);
}
// Hold: off on release. Both: a short tap latches (or unlatches), a longer press is a hold.
function releaseShift() {
  const p = st.shiftPress; st.shiftPress = null;
  if (!p) return;
  if (p.mode === 'hold' || performance.now() - p.t >= cfg.globalSetting('hybridMs')) dev.setShift(false);
  else dev.setShift(!p.was);
}

function setSim(on) {
  st.sim = on;
  if (on) toast('Simulator on: the panel plays the mixer. Move faders, drag encoders up and down, press buttons; SHIFT works as this preset says.');
  renderAll();
}

// ---- events from the mixer -----------------------------------------------------------------------------

function wireDevice() {
  observeDevice(dev, cycle);
  dev.addEventListener('touch', (e) => {
    const id = e.detail.id;
    if (!st.follow) return; // Separate: touches on the mixer only play
    if (st.armed !== null && st.tab === 'params' && !st.assignLock) {
      st.assignLock = true; setTimeout(() => (st.assignLock = false), 400);
      assignArmed(id); return;
    }
    // A touch inside the selection keeps it, so a group can be tried by hand.
    const changed = !st.sel.has(id);
    if (changed) { st.sel = new Set([id]); renderSide(); renderSelbar(); }
    for (const c of document.querySelectorAll('#panel .ctl')) c.classList.toggle('sel', st.sel.has(c.dataset.id));
    refreshValue(id);
    const el = document.querySelector(`#panel .ctl[data-id="${id}"]`);
    if (el && changed) { el.classList.remove('flash'); void el.offsetWidth; el.classList.add('flash'); }
  });
  dev.addEventListener('midi', (e) => addMonitor(e.detail));
  dev.addEventListener('state', () => {
    if (st.follow) { st.preset = dev.state.preset; st.page = dev.state.page; st.layer = dev.state.shift ? 'shift' : 'normal'; }
    renderAll();
  });
  // On (re)connection the mixer may play something other than the editor's copy (edits made with no
  // mixer, or the mixer switched off unsaved): ask as when switching to Linked (A-12, rule 6).
  let wasOurs = ours();
  dev.addEventListener('connection', () => {
    const now = ours();
    if (now && !wasOurs && st.follow && unsent()) divergeDialog((go) => { if (!go) setLinked2(false); else renderAll(); }, true);
    wasOurs = now;
    renderAll();
  });
  // The storage state changes by itself (a save ends): the pill and an open mixer sheet follow.
  dev.addEventListener('storage', () => { renderPill(); if ($('#dlg').open && $('#dlg .device-sheet')) deviceDialog(); });
  // Closing the editor while the mixer plays unsaved changes, or holds unsent ones: the browser's own warning.
  addEventListener('beforeunload', (e) => { if (dev.firmwareJob?.active || unsaved() || (!st.follow && unsent())) e.preventDefault(); });
  $('#pill-act').hidden = true;
  dev.addEventListener('log', () => { const l = document.querySelector('#dev-logs'); if (l) renderLogs(l); });
  dev.addEventListener('error', (e) => toast(e.detail.text));
  // Another client changed what the mixer plays (P-6: its revision moved without the editor).
  dev.addEventListener('changed', () => {
    if (st.follow) divergeDialog((go) => { if (!go) setLinked2(false); renderAll(); }, true);
    else toast('The mixer’s configuration changed elsewhere. Load from the mixer or explicitly send yours.');
  });
}

function refreshValue(id) {
  const el = document.querySelector(`#panel .ctl[data-id="${id}"]`);
  if (!el) return;
  const kind = cfg.kind(id);
  const layer = st.sim ? playLayer(id) : shownLayer(id);
  const v = el.querySelector('.track, .knob');
  if (v) v.style.setProperty('--v', frac(layer, kind, st.sim ? dev.state.preset : st.preset));
  el.classList.toggle('lit', st.lit.has(id));
}

function addMonitor(d) {
  if (st.paused) return;
  const t = (performance.now() / 1000).toFixed(3);
  const c = d.id ? ctlAt(d.id, d.preset ?? dev.state.preset, d.page ?? dev.state.page) : null;
  st.monitor.push({ t, dir: d.dir, bytes: d.bytes, text: decode(d.bytes), who: d.id ? `${ctlName(d.id)}${d.layer === 'shift' ? ' (Shift)' : ''}${(() => { const n = c && cfg.nameOn(c, cfg.kind(d.id), d.layer === 'shift' ? 'shift' : 'normal'); return n ? ` · ${n}` : ''; })()}` : d.who ?? '', tr: d.transport });
  if (st.monitor.length > 400) st.monitor.shift();
  if (monitorFrame === null) monitorFrame = requestAnimationFrame(() => {
    monitorFrame = null;
    renderMonitorBar();
    if (st.monitorOpen) renderMonitorList();
  });
}
let monitorFrame = null;

function renderMonitorBar() {
  const last = st.monitor.at(-1);
  put($('#monitor-bar'),
    h('button', { class: `tb ${st.monitorOpen ? 'on' : ''}`, 'aria-expanded': st.monitorOpen ? 'true' : 'false', onclick: () => { st.monitorOpen = !st.monitorOpen; $('#monitor-list').hidden = !st.monitorOpen; if (st.monitorOpen) renderMonitorList(); renderMonitorBar(); } },
      icon('monitor', 16), h('span', {}, 'MIDI'), h('span', { class: `chev ${st.monitorOpen ? '' : 'up'}` }, icon('chevron', 14))),
    h('span', { class: 'state' }, dev.connected ? `${presetLabel(dev.state.preset)} · ${pageLabel(dev.state.preset, dev.state.page)}${dev.state.shift ? ' · Shift' : ''}` : 'Not connected'),
    h('span', { class: 'last' }, last ? `${last.dir === 'out' ? '→' : '←'} ${last.text} · ${last.who}` : dev.connected ? 'Move a control on the mixer to see what it sends.' : st.sim ? 'Move a control on the panel to see what it sends.' : 'What the mixer sends shows here once it is connected.'),
    st.monitorOpen ? h('div', { class: 'seg small' }, ...[['all', 'All'], ['out', 'Sent'], ['in', 'Received']].map(([f, n]) => h('button', { class: st.monitorFilter === f ? 'on' : '', onclick: () => { st.monitorFilter = f; renderMonitorBar(); renderMonitorList(); } }, n))) : null,
    st.monitorOpen ? h('button', { onclick: () => { st.paused = !st.paused; renderMonitorBar(); } }, st.paused ? 'Resume' : 'Pause') : null,
    st.monitorOpen ? h('button', { onclick: () => { st.monitor = []; renderMonitorBar(); renderMonitorList(); } }, 'Clear') : null,
    h('span', { class: 'sim-sep' }),
    !dev.real ? h('button', { class: `chip toggle sim-toggle ${st.sim ? 'on' : ''}`, 'aria-pressed': st.sim ? 'true' : 'false', title: 'Use the simulated mixer panel', onclick: () => setSim(!st.sim) }, icon('sim', 14), 'Simulator') : null,
    !dev.real && st.sim ? h('button', { class: 'chip', title: 'Simulate an incoming instrument value', onclick: fakeIncoming }, 'Instrument sends') : null);
}
function renderMonitorList() {
  const rows = st.monitor.filter((m) => st.monitorFilter === 'all' || m.dir === st.monitorFilter).slice(-200);
  const list = $('#monitor-list');
  if (!rows.length) { put(list, h('p', { class: 'help' }, 'Nothing yet.')); return; }
  put(list, h('table', { class: 'mon' }, h('tbody', {}, ...rows.map((m) => h('tr', { class: m.dir },
    h('td', { class: 't' }, m.t), h('td', {}, m.dir === 'out' ? '→' : '←'), h('td', { class: 'b' }, hexBytes(m.bytes)), h('td', {}, m.text), h('td', { class: 'who' }, m.who))))));
  list.scrollTop = list.scrollHeight;
}
function fakeIncoming() {
  const cands = IDS.filter((i) => { const l = cfg.normal(ctlAt(i, dev.state.preset, dev.state.page)); return ['cc7', 'note', 'cc14', 'nrpn'].includes(l.type); });
  if (!cands.length) { toast('No control on this page takes a CC, note or NRPN.'); return; }
  const id = cands[Math.floor(Math.random() * cands.length)];
  const kind = cfg.kind(id);
  const l = cfg.normal(ctlAt(id, dev.state.preset, dev.state.page));
  const lo = S.value(l, 'lo', kind), hi = S.value(l, 'hi', kind);
  const v = l.type === 'note' ? (Math.random() < 0.5 ? 0 : hi) : Math.round(Math.min(lo, hi) + Math.random() * Math.abs(hi - lo));
  st.values.set(targetKey(l, dev.state.preset), v);
  if (kind === 'button') { if (v !== lo) st.lit.add(id); else st.lit.delete(id); }
  for (const b of messages(S, kind, l, cfg.presetChannel(dev.state.preset), v, '9n')) addMonitor({ dir: 'in', bytes: b, who: `for ${ctlName(id)}`, transport: dev.transport });
  for (const i of IDS) refreshValue(i);
}

// ---- learn: move a control on the instrument to fill in what a control sends (A-11) ------------------
// The editor listens to MIDI from the instrument (not from the mixer): the first new parameter goes to
// the first selected control, the next to the next one. Moving one knob sends many messages, so a
// parameter already learned in this pass is ignored. The mixer will learn by itself later (FR-CTL-09).

const learnIn = { sel: null, cc: null };
function resetLearnInput() {
  clearTimeout(learnIn.cc?.timer);
  learnIn.sel = learnIn.cc = null;
}
function startLearn(ids) {
  if (!ids.length || st.learn || (dev.real && !MixerDevice.supported())) return;
  const L = st.learn = { ids: orderIds(ids, 'rows'), k: 0, seen: new Set(), done: 0, cfg, preset: st.preset, page: st.page, layer: st.layer, loading: dev.real, inputs: [], selectedId: null };
  resetLearnInput();
  if (dev.real) {
    // The mixer's own inputs are excluded by name (learnport.js) and by the ports this editor's
    // mixer connection uses, compared by id (Learn's MIDIAccess is a separate object).
    L.port = createLearnPort({ onMessage: learnFeed, excludeInput: (p) => !!dev.ports?.some((q) => q.id === p.id), onChange: ({ inputs, selectedId }) => {
      if (st.learn !== L) return;
      if (L.selectedId !== selectedId) resetLearnInput();
      if (L.selectedId && !selectedId && !L.deselecting) L.error = 'The instrument was disconnected. Select an available input to continue.';
      Object.assign(L, { inputs, selectedId, loading: false });
      renderSide();
    } });
    L.port.start().catch((e) => {
      if (st.learn !== L) return;
      L.loading = false;
      L.error = e.name === 'NotAllowedError' || e.name === 'SecurityError' ? 'MIDI access was not granted. Allow MIDI in your browser’s site settings and start Learn again.' : `Could not open MIDI: ${e.message}`;
      renderSide();
    });
  }
  renderAll();
}
function stopLearn(msg) {
  if (!st.learn) return;
  const { done: n, port } = st.learn;
  st.learn = null;
  resetLearnInput();
  port?.stop();
  toast(msg ?? (n ? `Learned ${plural(n, 'control')}. Undo takes them back one by one.` : 'Learn stopped.'));
  renderAll();
}
// One incoming message from the instrument. CC 0–31 waits a moment for its LSB (CC n + 32): a
// 14-bit controller sends both. NRPN and RPN arrive as the parameter number, then the data entry.
function learnFeed(b) {
  if (!st.learn) return;
  const [s, d1, d2] = b;
  const ch = (s & 15) + 1;
  switch (s & 0xF0) {
    case 0x90: if (d2 > 0) learnFound({ type: 'note', ch, number: d1 }); return;
    case 0xE0: learnFound({ type: 'pitchbend', ch }); return;
    case 0xC0: learnFound({ type: 'program', ch }); return;
    case 0xD0: learnFound({ type: 'aftertouch', ch }); return;
    case 0xB0: break;
    default: return;
  }
  if (d1 === 99 || d1 === 101) { learnIn.sel = { type: d1 === 99 ? 'nrpn' : 'rpn', ch, msb: d2 }; return; }
  if ((d1 === 98 || d1 === 100) && learnIn.sel?.ch === ch) { learnIn.sel.lsb = d2; return; }
  if ((d1 === 6 || d1 === 38) && learnIn.sel?.lsb !== undefined && learnIn.sel.ch === ch) {
    learnFound({ type: learnIn.sel.type, ch, number: learnIn.sel.msb * 128 + learnIn.sel.lsb }); return;
  }
  const c = learnIn.cc;
  if (d1 >= 32 && d1 < 64 && c && c.n === d1 - 32 && c.ch === ch) { clearTimeout(c.timer); learnIn.cc = null; learnFound({ type: 'cc14', ch, number: c.n }); return; }
  if (d1 < 32) {
    if (c) clearTimeout(c.timer);
    learnIn.cc = { n: d1, ch, timer: setTimeout(() => { learnIn.cc = null; learnFound({ type: 'cc7', ch, number: d1 }); }, 60) };
    return;
  }
  learnFound({ type: 'cc7', ch, number: d1 });
}
function learnFound(m) {
  const L = st.learn;
  if (!L) return;
  if (L.cfg !== cfg || L.preset !== st.preset || L.page !== st.page || L.layer !== st.layer) {
    stopLearn('Learn stopped because the configuration or page changed.');
    return;
  }
  const key = `${m.type}/${m.ch}/${m.number ?? ''}`;
  if (L.seen.has(key)) return;
  const id = L.ids[L.k];
  const kind = cfg.kind(id);
  if (!S.types(kind).includes(m.type)) { toast(`${ctlName(id)} cannot send ${TYPE_LABEL[m.type]}: move another control on the instrument.`); return; }
  L.seen.add(key);
  const layer = st.layer === 'normal' ? ctlAt(id) : ctlAt(id).shift;
  edit(`learn ${ctlName(id)}`, () => {
    cfg.setField(layer, kind, 'type', m.type);
    cfg.setField(layer, kind, 'channel', m.ch === cfg.presetChannel(st.preset) ? 'preset' : m.ch);
    if (m.number !== undefined) cfg.setField(layer, kind, 'number', m.number);
  });
  L.done++; L.k++;
  if (L.k >= L.ids.length) stopLearn(`Learned ${plural(L.done, 'control')}. Undo takes them back one by one.`);
  else renderAll();
}
// The simulated mixer (?mock) has no instrument: these pretend one sends a parameter it has not sent yet.
function pretendInstrument(what) {
  const c = cfg.presetChannel(st.preset) - 1;
  const rnd = (a, b) => a + Math.floor(Math.random() * (b - a + 1));
  const send = (...msgs) => { for (const m of msgs) { addMonitor({ dir: 'in', bytes: m, who: 'instrument', transport: 'USB' }); learnFeed(m); } };
  if (what === 'cc') send([0xB0 + c, rnd(64, 95), rnd(0, 127)]);
  if (what === 'cc14') { const n = rnd(1, 31); send([0xB0 + c, n, rnd(0, 127)], [0xB0 + c, n + 32, rnd(0, 127)]); }
  if (what === 'nrpn') send([0xB0 + c, 99, rnd(0, 3)], [0xB0 + c, 98, rnd(0, 127)], [0xB0 + c, 6, rnd(0, 127)], [0xB0 + c, 38, 0]);
  if (what === 'note') send([0x90 + c, rnd(36, 84), 100]);
}
function learnBanner() {
  const L = st.learn;
  const id = L.ids[L.k];
  return h('div', { class: 'learn-banner' },
    h('div', { class: 'row' }, h('span', { class: 'learn-dot' }),
      h('div', { class: 'grow' }, h('b', {}, `Learning: ${ctlName(id)}`), L.ids.length > 1 ? h('span', { class: 'muted' }, ` · ${L.k + 1} of ${L.ids.length}`) : null,
        h('div', { class: 'help' }, 'Move a knob, a fader or a key on the instrument. Its message goes to this control, then to the next one.')),
      h('button', { onclick: () => stopLearn() }, L.done ? 'Done' : 'Cancel')),
    !dev.real ? h('div', { class: 'pretend' }, h('span', { class: 'muted' }, 'Simulate instrument MIDI'),
      ...[['cc', 'a CC'], ['cc14', 'a 14-bit CC'], ['nrpn', 'an NRPN'], ['note', 'a note']].map(([k, n]) => h('button', { class: 'chip', onclick: () => pretendInstrument(k) }, n))) :
      L.loading ? h('p', { class: 'help', role: 'status' }, 'Waiting for MIDI access…') :
        L.inputs.length ? field('Instrument MIDI input', h('select', { id: 'learn-input', onchange: async (e) => {
          const id = e.target.value || null;
          L.error = null;
          resetLearnInput();
          L.deselecting = true;
          try {
            if (await L.port.select(id)) { if (id) store.set('smc-learn-input', id); }
            else if (st.learn === L) L.error = 'The selected MIDI input is no longer available.';
          } catch (err) { if (st.learn === L) L.error = `Could not listen to that input: ${err.message}`; }
          finally { L.deselecting = false; }
          if (st.learn === L) renderSide();
        } }, h('option', { value: '', selected: !L.selectedId }, 'Choose an input'),
        ...[...L.inputs].sort((a, b) => Number(b.id === store.get('smc-learn-input')) - Number(a.id === store.get('smc-learn-input'))).map((p) => h('option', { value: p.id, selected: p.id === L.selectedId }, p.name || p.manufacturer || p.id)))) :
          !L.error ? h('p', { class: 'help', role: 'status' }, 'No instrument MIDI inputs. Connect the instrument to this computer and enable its MIDI output.') : null,
    L.error ? h('p', { class: 'note warn', role: 'status' }, L.error) : null);
}

// ---- firmware: install, update, back to M-Vave's (docs/app.md, "Firmware") --------------------------
// The release the editor installs: the manifest published beside it (js/release.js). Without
// one (a simulation, or a page opened outside a release) only the editor's own version is known.
let RELEASE = { version: VERSION, date: '', notes: [] };
let REAL_RELEASE = null; // findRelease()'s, with the real mixer
function useRelease(r) {
  REAL_RELEASE = r;
  releaseSnapshot(r, cycleManifestHashes.get(r?.url)).then((value) => { if (REAL_RELEASE === r) { cycleRelease = value; cycle.record('release', value || {}); } },
    (e) => cycle.record('error', { source: 'release', message: e.message }));
  if (r) RELEASE = { version: r.version || VERSION, date: r.manifest.date || '', notes: [] };
  renderAll();
}
const ours = () => dev.connected && dev.fw.kind === 'ours';
// The real mixer updates when the release installs something and its firmware can go back to
// M-Vave's (STOCK, docs/link.md), the update's first half.
const canUpdate = () => !dev.real || (!!REAL_RELEASE?.install && !!dev.linkCaps?.includes('stock'));
const updateReady = () => ours() && !!dev.fw.version && compareVersions(RELEASE.version, dev.fw.version) > 0 && canUpdate();
const FW_NAME = { ours: 'better-smc-mixer', stock: 'M-Vave firmware', loader: 'Update mode' };
// Installing and updating need Web MIDI with SysEx (Chrome, Edge, Firefox on a computer).
// Any version of M-Vave's firmware: the installer decides on the backup, not on the version's name
// (js/stocklink.js; M-Vave updates its firmware).
const INSTALL_CHECK = (version) => `M-Vave’s firmware ${version}. Before writing anything the installer reads the whole memory and checks it: that M-Vave’s firmware is where it expects it and the part better-smc-mixer uses is free (or holds an earlier better-smc-mixer). If a check fails it stops, and nothing is written.`;
const canFlash = () => typeof navigator !== 'undefined' && 'requestMIDIAccess' in navigator;

function firmwareCard() {
  const f = dev.fw;
  if (!dev.connected) return null;
  // The card holds its action, as the update card does.
  const head = (cls, ic, title, text, action) => h('div', { class: `fwcard ${cls}` }, h('div', { class: 'row' }, icon(ic, 18), h('b', {}, title)), text ? h('p', { class: 'help' }, text) : null, action ? h('div', { class: 'actions' }, action) : null);
  if (f.kind === 'stock') {
    return head('warn', 'info', `This mixer runs M-Vave’s firmware (${f.version})`, 'It works as M-Vave made it, but this editor cannot configure it. Install better-smc-mixer to use the editor; you can go back to M-Vave’s firmware later.', btn('upload', 'Install better-smc-mixer…', { class: 'primary', onclick: installDialog }));
  }
  if (f.kind === 'loader') {
    return head('warn', 'info', 'The mixer is waiting for firmware', 'An update was interrupted (the cable came out, or the power). Nothing is lost: install the firmware again.', btn('upload', `Install ${RELEASE.version}`, { class: 'primary', onclick: () => updateDialog(true) }));
  }
  return h('div', {},
    updateReady()
      ? h('div', { class: 'fwcard info' }, h('div', { class: 'row' }, icon('download', 18), h('b', {}, `Version ${RELEASE.version} is out`), h('span', { class: 'muted' }, ` · you have ${f.version}`)),
        RELEASE.notes.length ? h('ul', { class: 'help' }, ...RELEASE.notes.map((n) => h('li', {}, n))) : null,
        h('div', { class: 'actions' }, btn('download', 'Update…', { class: 'primary', onclick: () => updateDialog(false) })))
      : null);
}

// A dialog with numbered steps and one progress bar; each step's body is redrawn by `draw`.
function stepsDialog(title, steps, intro, job) {
  if (job?.view) { dialog(job.view); return; }
  const box = h('div', { class: 'fwsteps' });
  let at = 0;
  const draw = () => put(box, steps.map((s, i) => h('div', { class: `step ${i < at ? 'done' : i === at ? 'now' : ''}` },
    h('span', { class: 'num' }, i < at ? icon('check', 14) : String(i + 1)), h('div', {}, h('b', {}, s.title), i === at ? s.body(() => { if (job?.cancelled || job?.error) return; at++; draw(); }) : null))));
  // One footer button, named for what it does now: Cancel while the job runs, Close once it has
  // ended (done, failed or cancelled). The job's `active` drives the label wherever it changes. The
  // button sits in the sheet's footer, never cut off.
  const foot = job ? h('button', { onclick: () => {
    if (!job.active) { dev.firmwareJob = null; closeDialog(); return; }
    if (!job.cancel?.()) toast('Writing has started: it cannot be cancelled now. Closing this view does not stop it.');
  } }) : null;
  const view = h('div', {}, h('div', { class: 'dlg-head' }, h('h2', {}, title), h('button', { class: 'icon-only', 'aria-label': 'Close', onclick: closeDialog }, icon('close'))),
    intro ? h('p', { class: 'help' }, intro) : null, box,
    foot ? h('div', { class: 'dlg-foot' }, foot) : null);
  if (job) {
    job.view = view;
    let active = job.active;
    const label = () => { foot.textContent = active ? 'Cancel' : 'Close'; };
    Object.defineProperty(job, 'active', { configurable: true, enumerable: true, get: () => active, set: (v) => { active = v; label(); } });
    label();
  }
  dialog(view);
  draw();
}
// A pretend transfer: a bar that fills, then `done`. The real one writes in 4 KB sectors and checks each by a read.
function progress(text, ms, done) {
  const bar = h('progress', { max: 100, value: 0 });
  const t0 = performance.now();
  const timer = setInterval(() => { const v = Math.min(100, ((performance.now() - t0) / ms) * 100); bar.value = v; if (v >= 100) { clearInterval(timer); done(); } }, 50);
  return h('div', { class: 'prog' }, h('p', { class: 'help' }, text), bar);
}

function installDialog() {
  if (dev.real) { installReal(); return; }
  let backup = false;
  stepsDialog('Install better-smc-mixer', [
    { title: 'Check the mixer', body: (next) => h('div', {},
      h('p', { class: 'help' }, INSTALL_CHECK(dev.fw.version)),
      canFlash() ? null : h('p', { class: 'note warn' }, icon('info', 16), h('span', {}, 'This browser cannot send firmware: use Chrome, Edge or Firefox on a computer. An iPad cannot install.')),
      h('div', { class: 'actions' }, h('button', { class: 'primary', onclick: next }, 'Next'))) },
    { title: 'Save a backup of the mixer', body: (next) => {
      const go = h('button', { class: 'primary', disabled: !backup, onclick: next }, 'Next');
      return h('div', {},
        h('p', { class: 'help' }, 'The mixer’s whole memory as one file (512 KB) on this computer: M-Vave’s firmware, its settings and Bluetooth pairings. Going back to M-Vave’s firmware needs it. It stays with you: it is never uploaded.'),
        h('div', { class: 'actions' }, btn('download', backup ? 'Saved: SMC-Mixer backup.bin' : 'Read and save the backup', { onclick: (e) => { e.currentTarget.disabled = true; e.currentTarget.after(progress('Reading 512 KB… about 5 s', 1500, () => { backup = true; toast('Backup saved: SMC-Mixer backup.bin'); go.disabled = false; })); } }), go));
    } },
    { title: `Install ${RELEASE.version}`, body: (next) => h('div', {},
      h('p', { class: 'help' }, 'About a minute. Keep the USB cable in and the mixer on. M-Vave’s firmware stays in the mixer’s memory, untouched; the mixer just starts ours instead.'),
      h('div', { class: 'actions' }, h('button', { class: 'primary', onclick: (e) => { e.currentTarget.replaceWith(progress('Writing and checking… do not unplug', 2500, next)); } }, 'Install'))) },
    { title: 'Done', body: () => { dev.setFirmware({ kind: 'ours', version: RELEASE.version }); return h('div', {},
      h('p', { class: 'help' }, `The mixer restarted with better-smc-mixer ${RELEASE.version} and the factory layout. Send this configuration to it, or start from the factory one.`),
      h('div', { class: 'actions' }, btn('upload', 'Send this configuration', { class: 'primary', onclick: () => { pushToDevice('configuration sent'); closeDialog(); toast('Sent to the mixer'); } }), h('button', { onclick: closeDialog }, 'Later'))); } },
  ], 'Changes only the program the mixer runs. You can go back to M-Vave’s firmware from the Mixer sheet at any time.');
}

function updateDialog(fromLoader) {
  if (dev.real) { updateReal(); return; }
  stepsDialog(fromLoader ? `Install ${RELEASE.version}` : `Update to ${RELEASE.version}`, [
    { title: 'Before', body: (next) => h('div', {},
      h('p', { class: 'help' }, fromLoader ? 'The mixer is in update mode. The configuration saved on it is kept.' : `${RELEASE.date ? `Released ${RELEASE.date}. ` : ''}The configuration on the mixer stays; the editor keeps a copy of it first.`),
      RELEASE.notes.length ? h('ul', { class: 'help' }, ...RELEASE.notes.map((n) => h('li', {}, n))) : null,
      !fromLoader && unsaved() ? h('p', { class: 'note warn' }, icon('info', 16), h('span', {}, 'The mixer plays changes that are not saved. The update restarts it, and it would start without them.')) : null,
      h('div', { class: 'actions' },
        !fromLoader && unsaved() ? h('button', { onclick: (e) => { dev.saveNow(); e.currentTarget.replaceWith(h('span', { class: 'muted' }, 'Saved')); } }, 'Save first') : null,
        h('button', { class: 'primary', onclick: next }, fromLoader ? 'Install' : 'Update'))) },
    { title: 'Writing', body: (next) => progress('About 30 s. Keep the cable in. If it is interrupted, the mixer waits in update mode and this sheet finishes the job.', 2500, next) },
    { title: 'Done', body: () => { dev.setFirmware({ kind: 'ours', version: RELEASE.version }); return h('div', {}, h('p', { class: 'help' }, `better-smc-mixer ${RELEASE.version} is running, with your configuration.`), h('div', { class: 'actions' }, h('button', { class: 'primary', onclick: deviceDialog }, 'Done'))); } },
  ]);
}

function stockDialog() {
  if (dev.real) { stockReal(); return; }
  let file = false;
  stepsDialog('Go back to M-Vave’s firmware', [
    { title: 'Keep your configuration', body: (next) => h('div', {},
      h('p', { class: 'help' }, 'M-Vave’s firmware does not use it. Save a copy to bring it back later.'),
      h('div', { class: 'actions' }, btn('download', 'Save a copy…', { onclick: () => showJson(cfg.canonical(), 'Everything') }), h('button', { class: 'primary', onclick: next }, 'Next'))) },
    { title: 'Choose the backup', body: (next) => {
      const go = h('button', { class: 'primary', disabled: !file, onclick: next }, 'Next');
      return h('div', {},
        h('p', { class: 'help' }, 'The file saved when better-smc-mixer was installed (SMC-Mixer backup.bin). The editor checks that it comes from this mixer.'),
        h('div', { class: 'actions' }, btn('file', 'Choose the file…', { onclick: (e) => { file = true; e.currentTarget.textContent = 'SMC-Mixer backup.bin: from this mixer, M-Vave 012'; go.disabled = false; } }), go));
    } },
    { title: 'Restore', body: (next) => h('div', {},
      h('p', { class: 'help' }, 'Only the start-up and the parts ours changed are written back; Bluetooth pairings return as they were at the backup. About 30 s; keep the cable in.'),
      h('div', { class: 'actions' }, h('button', { class: 'danger', onclick: (e) => { e.currentTarget.replaceWith(progress('Writing and checking…', 2000, next)); } }, 'Go back'))) },
    { title: 'Done', body: () => { dev.setFirmware({ kind: 'stock', version: 'SMC-Mixer_012' }); return h('div', {}, h('p', { class: 'help' }, 'The mixer runs M-Vave’s firmware again. The Mixer sheet can install better-smc-mixer again whenever you like.'), h('div', { class: 'actions' }, h('button', { class: 'primary', onclick: deviceDialog }, 'Done'))); } },
  ]);
}

// ---- firmware on the real mixer (js/stocklink.js install(), js/mixer.js) -------------------------------

function saveBytes(bytes, name) {
  const a = h('a', { href: URL.createObjectURL(new Blob([bytes], { type: 'application/octet-stream' })), download: name });
  document.body.append(a); a.click(); a.remove();
}
const sleep = (ms) => new Promise((ok) => setTimeout(ok, ms));
const STEP_TEXT = { backup: 'Reading the mixer’s memory…', 'our earlier sectors': 'Clearing an earlier installation…', area: 'Writing better-smc-mixer and reading it back… do not unplug', check: 'Checking all of it once more…', 'sector 0': 'Switching the start-up over…' };
const STOCK_TEXT = {
  'sector 0 does not parse': 'The mixer’s start-up table does not read as expected.',
  'sector 0 does not point at ours': 'The mixer does not start better-smc-mixer from where it was installed.',
  'M-Vave\'s area does not check': 'M-Vave’s firmware in the mixer’s memory does not check (its checksum): starting it could fail.',
  flash: 'The mixer could not read or write its memory.',
  busy: 'The mixer is saving a configuration: try again in a few seconds.',
  'not offered': 'This version of better-smc-mixer cannot do it: update it first.',
};

// One run of the installer, shared by its steps: the backup is read and saved as a file, then the
// installer waits until the user goes on (job.go), then writes. A failure says what holds now.
function installJob(kind = 'install', release = REAL_RELEASE) {
  const owner = dev.beginFirmwareJob(kind);
  const evidence = cycleJob = { id: ++cycleJobId, stage: 'created', plan: null, preview: null, completed: false, error: null };
  const bar = h('progress', { max: 100, value: 0 });
  const text = h('p', { class: 'help' });
  const err = h('p', { class: 'note warn', hidden: true });
  const job = Object.assign(owner, { bar, text, err, file: '', go: null, onBackup: null, onDone: null, written: false });
  job.fail = (e) => {
    job.active = false;
    job.error = e;
    evidence.error = { name: e.name || 'Error', message: e.message ?? String(e) };
    evidence.stage = 'error';
    cycle.record('error', { jobId: evidence.id, source: 'install', ...evidence.error });
    bar.hidden = true;
    put(err, icon('info', 16), h('span', {}, `Stopped: ${e.message ?? e}. ${job.written
      ? 'The start-up switches over last, so the mixer most likely still starts M-Vave’s firmware: installing again picks up where this stopped. Keep the backup file: the recovery procedure puts the mixer back from it.'
      : 'Nothing was written: the mixer is as it was.'}`));
    err.hidden = false;
  };
  job.cancel = () => {
    if (job.written || job.switching || job.saving) return false;
    job.cancelled = true;
    const error = new Error('Cancelled before writing. Nothing was written.');
    if (!job.started) job.fail(error);
    else if (job.rejectGo) { job.rejectGo(error); job.go = job.rejectGo = null; }
    return true;
  };
  job.start = () => {
    if (job.started) return;
    job.started = true;
    evidence.stage = 'release_loading';
    text.textContent = 'Loading the firmware files…';
    releaseFiles(release)
      .then(({ app, cfg: cfgTool }) => dev.installFirmware({
        app, cfg: cfgTool, job,
        hooks: {
          progress: (step, d, t) => { evidence.stage = step; evidence.progress = { stage: step, done: d, total: t }; cycle.record('progress', { jobId: evidence.id, stage: step, done: d, total: t }); bar.value = (100 * d) / t; text.textContent = STEP_TEXT[step] ?? step; },
          plan: (p) => { evidence.plan = p; cycle.record('plan', { jobId: evidence.id, stage: 'validated', valid: true, plan: p }); },
          observe: ({ stage }) => { evidence.stage = stage; cycle.record('stage', { jobId: evidence.id, stage }); },
          backup: async (b) => {
            const epoch = dev.connectionEpoch;
            if (job.cancelled) throw new Error('Cancelled before writing. Nothing was written.');
            // date and time: an update the same day saves a second one beside the first
            job.file = `SMC-Mixer backup ${dev.fw.version} ${new Date().toISOString().slice(0, 16).replace('T', ' ').replace(':', '-')}.bin`;
            saveBytes(b, job.file);
            evidence.stage = 'plan_preview';
            evidence.preview = await previewPlan(b, app, cfgTool);
            if (!dev.connected || dev.connectionEpoch !== epoch) throw new Error('the connection ended before installation was confirmed');
            if (job.cancelled) throw new Error('Cancelled before writing. Nothing was written.');
            cycle.record('plan', { jobId: evidence.id, stage: 'preview', ...evidence.preview });
            if (!evidence.preview.valid) throw new Error('the installation preview plan does not hold');
            evidence.stage = 'backup_waiting';
            cycle.record('backup', { jobId: evidence.id, size: b.length, sha256: evidence.preview.files.backup.sha256, name: job.file });
            return new Promise((ok, reject) => {
              job.go = () => { job.go = job.rejectGo = null; ok(); };
              job.rejectGo = reject;
              job.onBackup?.();
            });
          },
        },
      }))
      .then((r) => { job.active = false; job.completed = true; evidence.stage = 'complete'; evidence.completed = true; evidence.sector0Ms = r.sector0Ms; cycle.record('complete', { jobId: evidence.id, sector0Ms: r.sector0Ms }); job.onDone?.(r); }, job.fail);
  };
  return job;
}
// The installer's two working steps, after a first one of the caller's.
function installSteps(job, doneText, version = RELEASE.version) {
  return [
    { title: 'Save a backup of the mixer', body: (next) => {
      const go = h('button', { class: 'primary', disabled: true, onclick: next }, 'Next');
      job.onBackup = () => { job.bar.hidden = true; job.text.textContent = `Saved: ${job.file}. Keep this file: it puts the mixer back as it is now, should anything go wrong. Nothing has been written yet.`; go.disabled = false; };
      const read = btn('download', 'Read and save the backup', { onclick: (e) => { e.currentTarget.disabled = true; job.start(); } });
      return h('div', {},
        h('p', { class: 'help' }, `The mixer’s whole memory as one file (512 KB, ${dev.transport === 'BLE' ? 'about 2 minutes over Bluetooth' : 'about 5 s'}) on this computer: M-Vave’s firmware, its settings and Bluetooth pairings. It stays with you: it is never uploaded.`),
        job.text, job.bar, job.err, h('div', { class: 'actions' }, read, go),
        dev.transport === 'BLE' ? bluetoothHelp() : null);
    } },
    { title: `Install ${version}`, body: (next) => {
      job.onDone = () => { dev.setConnected(false); next(); };
      job.bar.value = 0;
      job.text.textContent = `${dev.transport === 'BLE' ? 'A few minutes over Bluetooth. Keep the mixer on and near this computer, and this page open.' : 'About a minute. Keep the USB cable in and the mixer on.'} M-Vave’s firmware stays in the mixer’s memory, untouched; the mixer just starts ours instead.`;
      return h('div', {}, job.text, job.bar, job.err,
        h('div', { class: 'actions' }, h('button', { class: 'primary', onclick: (e) => {
          if (!job.active || job.cancelled || job.error || !job.go) return;
          e.currentTarget.disabled = true; job.bar.hidden = false; job.written = true; cycleJob.stage = 'installing'; job.go();
        } }, 'Install')));
    } },
    // The mixer has a battery: without the cable it keeps running, so only its power switch
    // restarts it (docs/recovery.md).
    { title: 'Switch the mixer off and on', body: () => h('div', {},
      h('p', { class: 'help' }, `With the mixer’s power switch: off, then on again (unplugging the cable is not enough: the battery keeps it running). Then connect. ${doneText}`),
      h('div', { class: 'actions' }, btn('usb', 'Connect', { class: 'primary', onclick: () => { closeDialog(); connectMixer(); } })),
      dev.transport === 'BLE' || doneText.includes('Bluetooth') ? bluetoothHelp() : null) },
  ];
}
function releaseNote() {
  return REAL_RELEASE?.install ? null : h('p', { class: 'note warn' }, icon('info', 16), h('span', {}, 'No firmware to install was found beside the editor (a release package: make release builds one).'));
}

function reopenFirmware() {
  if (!dev.firmwareJob?.view) return false;
  dialog(dev.firmwareJob.view);
  return true;
}

function installReal(release = REAL_RELEASE) {
  if (reopenFirmware()) return;
  const job = installJob('install', release);
  stepsDialog('Install better-smc-mixer', [
    { title: 'Check the mixer', body: (next) => {
      const go = h('button', { class: 'primary', disabled: !release?.install, onclick: next }, 'Next');
      return h('div', {},
        h('p', { class: 'help' }, INSTALL_CHECK(dev.fw.version)),
        release?.install ? null : releaseNote(),
        h('div', { class: 'actions' }, go));
    } },
    ...installSteps(job, `better-smc-mixer starts, with the factory layout or the configuration it saved before.${dev.transport === 'BLE' ? BLE_AGAIN : ''}`, release?.version || RELEASE.version),
  ], `Installs better-smc-mixer ${release?.version || RELEASE.version}. Closing this view does not cancel the operation.`, job);
}

// Update: our firmware points the mixer back at M-Vave's (STOCK), which then takes the new version
// as at the installation. The configuration saved on the mixer stays (installplan.js, 'earlier').
function updateReal(release = REAL_RELEASE) {
  if (reopenFirmware()) return;
  const job = installJob('update', release);
  let check = null;
  // Over Bluetooth M-Vave's firmware is reached through the browser's chooser, which needs a click
  // (connectBluetooth): the step waits for it rather than looking by itself as over USB.
  const viaBle = dev.transport === 'BLE';
  stepsDialog(`Update to ${release?.version || RELEASE.version}`, [
    { title: 'Before', body: (next) => {
      const go = h('button', { class: 'primary', disabled: true, onclick: next }, 'Update');
      const needsSave = unsaved();
      const save = needsSave ? saveFirstButton(go, job) : null;
      const said = h('p', { class: 'help' }, 'Checking that the mixer can switch to M-Vave’s firmware for a moment…');
      (check ??= dev.stockCheck(job)).then((r) => {
        if (!job.active || job.cancelled) return;
        if (r.status === 'ok') { said.textContent = 'The mixer restarts on M-Vave’s firmware for a moment, which takes the new version as at the installation. The configuration saved on the mixer stays.'; if (save) save.disabled = false; else go.disabled = false; }
        else { job.active = false; job.error = new Error(r.status); said.textContent = `The update cannot start: ${STOCK_TEXT[r.status] ?? r.status}`; }
      }, (e) => { job.active = false; job.error = e; said.textContent = `The update cannot start: ${e.message}`; });
      return h('div', {}, said,
        unsaved() ? h('p', { class: 'note warn' }, icon('info', 16), h('span', {}, 'The mixer plays changes that are not saved. The update restarts it, and it would start without them.')) : null,
        release?.install ? null : releaseNote(),
        h('div', { class: 'actions' },
          save, go));
    } },
    { title: 'Restart on M-Vave’s firmware', body: (next) => {
      const said = h('p', { class: 'help' }, 'Switching…');
      job.switching = true;
      (async () => {
        const r = await dev.backToStock(job);
        if (r.status !== 'ok') { job.active = false; job.error = new Error(r.status); said.textContent = `Not switched: ${STOCK_TEXT[r.status] ?? r.status} better-smc-mixer still runs.`; return; }
        if (viaBle) {
          said.textContent = 'The mixer restarts on M-Vave’s firmware. Choose it in the browser’s Bluetooth list (often “SMC-Mixer”). The Mac may connect to it automatically first. Not listed? Disconnect it in Audio MIDI Setup or the music app. If it reconnects or stays missing, forget “SMC-Mixer” in System Settings, Bluetooth, then choose again.';
          const choose = btn('bluetooth', 'Choose the mixer', { class: 'primary', onclick: () => {
            choose.disabled = true;
            dev.connectBluetooth().then(() => { if (dev.connected && dev.stock) { job.switching = false; next(); } else choose.disabled = false; },
              (e) => { said.textContent = e.name === 'NotFoundError' ? 'No mixer chosen: choose it to go on.' : `Not connected: ${e.message}`; choose.disabled = false; });
          } });
          said.after(h('div', { class: 'actions' }, choose), bluetoothHelp());
          return;
        }
        said.textContent = 'The mixer restarts on M-Vave’s firmware; waiting for it…';
        for (let i = 0; i < 20 && !(dev.connected && dev.stock); i++) {
          await sleep(1000);
          await dev.connect(docText()).catch(() => {});
        }
        if (dev.connected && dev.stock) { job.switching = false; next(); }
        else { job.active = false; job.error = new Error('stock firmware did not reconnect'); said.textContent = 'The mixer did not come back on M-Vave’s firmware. Switch it off and on with its power switch, connect, and install from the notice.'; }
      })().catch((e) => { job.active = false; job.error = e; said.textContent = `Stopped: ${e.message}`; });
      return h('div', {}, said);
    } },
    ...installSteps(job, `better-smc-mixer starts with the new version and your saved configuration.${viaBle ? BLE_AGAIN : ''}`, release?.version || RELEASE.version),
  ], 'Closing this view does not cancel the update.', job);
}
function saveFirstButton(go, job) {
  go.disabled = true;
  return h('button', { disabled: true, onclick: async (e) => {
    if (job && (!job.active || job.cancelled || job.error)) return;
    const button = e.currentTarget;
    go.disabled = true;
    button.disabled = true;
    button.textContent = 'Saving…';
    if (job) job.saving = true;
    const saved = await saveOnMixer();
    if (job) job.saving = false;
    button.textContent = saved ? 'Saved' : 'Not saved — Retry Save';
    button.disabled = saved;
    go.disabled = !saved;
  } }, 'Save first');
}
// After a restart, ours over Bluetooth: the page finds it as a Bluetooth MIDI device.
const BLE_AGAIN = ' Over Bluetooth: connect it as a Bluetooth MIDI device first (Audio MIDI Setup on a Mac), then Connect here.';

// Back to M-Vave's firmware: one entry of the mixer's start-up table, written by our firmware
// (STOCK). M-Vave's firmware never left the memory, so no file is needed.
function stockReal() {
  if (reopenFirmware()) return;
  const job = dev.beginFirmwareJob('back');
  cycle.record('rollback_prepare');
  let check = null;
  const viaBle = dev.transport === 'BLE';
  stepsDialog('Go back to M-Vave’s firmware', [
    { title: 'Keep your configuration', body: (next) => h('div', {},
      h('p', { class: 'help' }, 'M-Vave’s firmware does not use it. Save a copy to bring it back later.'),
      h('div', { class: 'actions' }, btn('download', 'Save a copy', { onclick: () => download(cfg.canonical(), cfg.file.name || CONFIG_FILE_BASE) }), h('button', { class: 'primary', onclick: next }, 'Next'))) },
    { title: 'Go back', body: (next) => {
      const go = h('button', { class: 'danger', disabled: true }, 'Go back');
      const said = h('p', { class: 'help' }, 'Checking M-Vave’s firmware in the mixer’s memory…');
      (check ??= dev.stockCheck(job)).then((r) => {
        cycle.record('rollback_check', { status: r.status, pointer: r.pointer, tries: r.tries });
        if (r.status !== 'ok') { job.active = false; job.error = new Error(r.status); said.textContent = `Not possible: ${STOCK_TEXT[r.status] ?? r.status}`; return; }
        said.textContent = 'M-Vave’s firmware is intact. Going back rewrites the mixer’s start-up table (a fraction of a second; keep the mixer on), then the mixer restarts on it.';
        go.disabled = false;
        go.onclick = () => {
          if (!job.active || job.cancelled) return;
          job.switching = true;
          cycle.record('rollback', { stage: 'intent' });
          go.disabled = true;
          dev.backToStock(job).then((b) => { job.active = false; job.completed = b.status === 'ok'; cycle.record('rollback', { stage: 'result', status: b.status, pointer: b.pointer, tries: b.tries }); if (b.status === 'ok') next(); else said.textContent = `Not done: ${STOCK_TEXT[b.status] ?? b.status} better-smc-mixer still runs.`; },
            (e) => { job.active = false; job.error = e; cycle.record('error', { source: 'rollback', name: e.name, message: e.message }); said.textContent = `Not done: ${e.message}`; });
        };
      }, (e) => { job.active = false; job.error = e; said.textContent = `Not possible: ${e.message}`; });
      return h('div', {}, said, h('div', { class: 'actions' }, go));
    } },
    { title: 'Done', body: () => h('div', {},
      h('p', { class: 'help' }, 'The mixer restarts on M-Vave’s firmware. better-smc-mixer stays in its memory unused: installing it again takes the same steps as the first time. An update of M-Vave’s firmware with M-Vave’s app erases better-smc-mixer and the configuration saved on the mixer: keep a copy here.'),
      h('div', { class: 'actions' }, viaBle && MixerDevice.bluetooth()
        ? btn('bluetooth', 'Choose the mixer', { class: 'primary', onclick: () => { closeDialog(); connectBluetooth(); } })
        : btn('usb', 'Connect', { class: 'primary', onclick: () => { closeDialog(); connectMixer(); } })),
      viaBle ? bluetoothHelp() : null) },
  ], 'Closing this view does not cancel the operation.', job);
}

// ---- the mixer: pill and sheet ------------------------------------------------------------------------------

// Linked or Separate (A-1): one visible switch beside the mixer pill.
const LINK_HELP = {
  linked: 'Linked: every change plays on the mixer at once. The editor shows the preset, page and Shift the mixer is on, and touching a control on the mixer selects it here.',
  separate: 'Separate: changes stay in the editor until you press Send. The editor and the mixer each keep their own preset and page; touching the mixer only plays it.',
};
function setLinked(on) {
  if (on && unsent()) { divergeDialog((go) => { if (go) setLinked2(true); }); return; }
  setLinked2(on);
}
function setLinked2(on) {
  st.follow = on;
  store.set('smc-link', on ? 'linked' : 'separate');
  if (on && dev.connected) { st.preset = dev.state.preset; st.page = dev.state.page; st.layer = dev.state.shift ? 'shift' : 'normal'; }
  toast(on ? LINK_HELP.linked : LINK_HELP.separate);
  renderAll();
}
function linkButtons(after) {
  return [['linked', 'link', 'Linked', true], ['separate', 'unlink', 'Separate', false]].map(([k, ic, label, v]) => h('button', {
    role: 'radio', 'aria-checked': String(st.follow === v), class: st.follow === v ? 'on' : '', title: LINK_HELP[k],
    onclick: () => { if (st.follow !== v) setLinked(v); after?.(); },
  }, icon(ic, 16), h('span', {}, label)));
}
function renderLink() {
  const seg = $('#link-seg');
  const key = `${st.follow}${ours()}`;
  if (seg.dataset.key === key) return;
  seg.dataset.key = key;
  put(seg, linkButtons());
  seg.hidden = !ours(); // Linked and Separate mean nothing without a mixer running our firmware
}

function renderPill() {
  renderLink();
  const pill = $('#device-pill');
  pill.className = 'pill';
  let text;
  if (!MOCK && !MixerDevice.supported()) { text = 'Offline'; pill.classList.add('off'); }
  else if (!dev.connected) { text = 'No mixer'; pill.classList.add('off'); }
  else if (dev.fw.kind !== 'ours') { text = FW_NAME[dev.fw.kind]; pill.classList.add('wait'); }
  else if (dev.sending) { text = 'Sending…'; pill.classList.add('wait'); }
  else if (unsent()) { text = 'Not sent'; pill.classList.add('wait'); }
  else if (dev.storage === 'waiting') { text = `Saving in ${Math.max(1, dev.secondsToSave())} s`; pill.classList.add('wait'); }
  else if (dev.storage === 'writing') { text = 'Saving…'; pill.classList.add('wait'); }
  else if (dev.storage === 'failed') { text = 'Save failed'; pill.classList.add('bad'); }
  else if (dev.storage === 'unsaved') { text = 'Not saved'; pill.classList.add('wait'); }
  else { text = 'Saved'; pill.classList.add('ok'); }
  renderPillAction(text);
  const upd = updateReady();
  const key = `${dev.connected}${dev.transport}${text}${upd}`;
  if (pill.dataset.key === key) return;
  pill.dataset.key = key;
  // Left: the connection (and a new firmware, which belongs to the mixer); right: where the
  // configuration stands, with Save or Send joined to it.
  put(pill, dev.connected ? icon(dev.transport === 'USB' ? 'usb' : 'bluetooth', 16) : null,
    upd ? h('span', { class: 'upd', title: `Firmware ${RELEASE.version} is out: open the mixer to update` }, icon('up', 12), RELEASE.version) : null,
    h('span', { class: 'psep' }), h('span', { class: 'dot' }), h('span', { class: 'ptext' }, text));
  pill.title = !dev.connected ? 'No mixer connected: changes stay in the editor'
    : dev.fw.kind !== 'ours' ? `Mixer on ${dev.transport === 'USB' ? 'USB' : 'Bluetooth'}: ${text}` : PILL_HELP[text] ?? PILL_HELP.Saving;
}
const PILL_HELP = {
  'Sending…': 'Sending this editor copy to the mixer. It has not been confirmed there yet; saving is separate.',
  'Not sent': 'This editor copy is not confirmed on the mixer. Send it to try your changes; saving is separate.',
  'Not saved': 'The mixer plays your changes, but it would start without them if switched off. Save keeps them.',
  'Save failed': 'The mixer could not save. It still plays your changes; try Save again.',
  Saved: 'Saved: the mixer plays this configuration and starts with it.',
  Saving: 'The mixer is saving: it will start with this configuration.',
};
// The one action the pill's state asks for, beside it.
function renderPillAction(text) {
  // M-Vave's firmware over Bluetooth (connectBluetooth): beside Connect while nothing is connected
  const bt = $('#pill-bt');
  bt.hidden = dev.connected || !dev.real || !MixerDevice.supported() || !MixerDevice.bluetooth();
  if (!bt.hidden && !bt.dataset.key) {
    bt.dataset.key = 'bt';
    put(bt, icon('bluetooth', 15), h('span', {}, STOCK_BT_LABEL));
    bt.title = STOCK_BT_HELP;
    bt.setAttribute('aria-label', STOCK_BT_LABEL);
    bt.onclick = connectBluetooth;
  }
  const b = $('#pill-act');
  const act = !MOCK && !MixerDevice.supported() ? null : !dev.connected ? ['Connect', MIDI_CONNECT_HELP, connectMixer]
    : dev.fw.kind === 'stock' ? ['Install', 'Install better-smc-mixer so the editor can configure this mixer', installDialog]
    : dev.fw.kind === 'loader' ? ['Finish', 'An update was interrupted: install the firmware again', () => updateDialog(true)]
    : text === 'Not sent' ? ['Send', 'Send your changes to the mixer to play them. They are not saved yet.', sendToMixer]
    : text === 'Not saved' || text === 'Save failed' ? ['Save', 'Keep these changes on the mixer: it starts with them next time.', saveOnMixer] : null;
  b.hidden = !act;
  if (!act || b.dataset.key === act[0]) return;
  b.dataset.key = act[0];
  put(b, icon({ Send: 'upload', Save: 'download', Connect: 'usb', Install: 'upload', Finish: 'upload' }[act[0]], 15), h('span', {}, act[0]));
  b.setAttribute('aria-label', act[0]);
  b.title = act[1];
  b.onclick = act[2];
}

// Bluetooth: where each firmware is connected, and what to do when the mixer is not listed or is
// refused (the owner, 2026-11-14; docs/app.md, "Bluetooth help"). One text, shown where Bluetooth is
// used: the mixer sheet, the installer's and the update's Bluetooth steps, going back.
function bluetoothHelp(open = false) {
  const li = (...c) => h('li', {}, ...c);
  return h('details', { class: 'more bt-help', open },
    h('summary', {}, icon('bluetooth', 14), 'Bluetooth connections'),
    h('p', { class: 'help' }, h('b', {}, 'better-smc-mixer'), ' appears as “Better-SMC-Mixer”, a Bluetooth MIDI device. Connect it in the system first, then Connect here:'),
    h('ul', { class: 'help' },
      li('Mac: Audio MIDI Setup, Window, Show MIDI Studio, the Bluetooth button: Connect beside the mixer.'),
      li('iPhone and iPad: in the music app’s Bluetooth MIDI list (AUM, GarageBand and others). The system’s Bluetooth settings do not list MIDI devices.')),
    h('p', { class: 'help' }, h('b', {}, 'M-Vave’s firmware'), ' appears as “SMC-Mixer”. For installation without a USB cable, choose “Install over Bluetooth”, then select the mixer in the browser’s list. This uses Web Bluetooth, not a Bluetooth MIDI connection.'),
    h('p', { class: 'help' }, h('b', {}, 'Not in the list?')),
    h('ul', { class: 'help' },
      li('The mixer takes one Bluetooth connection at a time and is not listed while it has one: disconnect it where it is connected (Audio MIDI Setup, a music app, another phone or tablet, M-Vave’s Bluetooth MIDI adapter).'),
      li('After going back to M-Vave’s firmware, the Mac may reconnect to “SMC-Mixer” automatically before the browser can find it. Disconnect it in Audio MIDI Setup first. If it reconnects or stays missing, use System Settings, Bluetooth, the ⓘ beside “SMC-Mixer”, Forget This Device; then use “Install over Bluetooth” again.'),
      li('Bluetooth on the mixer: its BT light blinks while it waits; holding BT turns Bluetooth on and off.')),
    h('p', { class: 'help' }, h('b', {}, 'Refused, “Unsupported device”, or it connects and drops?'), ' Another Bluetooth connection or a cached pairing may be responsible, including one from a test build that used M-Vave’s address. Disconnect it elsewhere first. If that does not help, forget the mixer there, then connect again: Mac: System Settings, Bluetooth, the ⓘ beside it, Forget This Device; iPhone and iPad: Settings, Bluetooth, the ⓘ, Forget This Device.'),
    h('p', { class: 'help' }, 'Each firmware has its own entry and pairing: the one that is not running shows as not connected or offline. That is expected.'));
}
function deviceDialog() {
  const logs = h('div', { class: 'logs', id: 'dev-logs' });
  renderLogs(logs);
  dialog(h('div', { class: 'device-sheet' },
    h('div', { class: 'dlg-head' }, h('h2', {}, 'SMC-Mixer'), h('button', { class: 'icon-only', 'aria-label': 'Close', onclick: closeDialog }, icon('close'))),
    h('div', { class: 'conn' },
      h('span', { class: `dot ${dev.connected ? 'ok' : ''}` }), h('b', {}, dev.connected ? `Connected over ${connectionType()}` : 'Not connected'),
      h('div', { class: 'spacer' }),
      dev.connected && !dev.real ? h('div', { class: 'seg small' }, ...['USB', 'BLE'].map((t) => h('button', { class: dev.transport === t ? 'on' : '', onclick: () => { dev.setConnected(true, t); deviceDialog(); } }, t === 'USB' ? 'USB' : 'Bluetooth'))) : null,
      h('button', { title: dev.connected ? 'Disconnect the editor from the mixer' : MIDI_CONNECT_HELP, disabled: !!dev.firmwareJob?.active || (dev.real && !MixerDevice.supported()), onclick: () => { if (dev.real && !dev.connected) { closeDialog(); connectMixer(); return; } dev.setConnected(!dev.connected); deviceDialog(); } }, dev.connected ? 'Disconnect' : 'Connect')),
    !dev.connected && dev.real && MixerDevice.supported() && MixerDevice.bluetooth()
      ? h('div', { class: 'actions' }, btn('bluetooth', STOCK_BT_LABEL, { title: STOCK_BT_HELP, onclick: () => { closeDialog(); connectBluetooth(); } })) : null,
    firmwareCard(),
    dev.firmwareJob?.view ? h('div', { class: 'actions' }, h('button', { onclick: reopenFirmware }, 'Firmware operation — progress / result')) : null,
    dev.real && (!dev.connected || dev.transport === 'BLE') ? bluetoothHelp() : null,
    dev.connected ? h('dl', { class: 'kv' },
      h('dt', {}, 'Firmware'), h('dd', {}, `${FW_NAME[dev.fw.kind]}${dev.fw.version ? ` ${dev.fw.version}` : ''}`, ours() && !updateReady() && !dev.real ? h('span', { class: 'muted' }, '· up to date') : null),
      h('dt', {}, 'Editor'), h('dd', {}, VERSION),
      ours() ? [h('dt', {}, 'Saved on the mixer'), h('dd', {}, dev.storage === 'clean' ? 'Yes' : dev.storage === 'failed' ? 'No: the last save failed' : 'Not yet')] : null,
      dev.real ? (ours() ? [h('dt', {}, 'Build'), h('dd', {}, dev.info.buildId), h('dt', {}, 'Versions'), h('dd', {}, `protocol ${dev.info.protocol}, binary form ${dev.info.binary}, schema ${dev.info.schema}`)] : null) : h('dt', {}, 'Power'), dev.real ? null : h('dd', {}, 'USB')) : null,
    ours() ? deviceOurs() : null,
    dev.real && !ours() ? firmwareActions() : null,
    h('details', { class: 'more' }, h('summary', {}, 'Diagnostics'),
      h('p', { class: 'help' }, 'The editor’s connection and operation log. Nothing is sent automatically.'),
      logs,
      h('div', { class: 'actions' }, btn('download', 'Save a report for support', { onclick: reportDialog }))),
    dev.real ? null : h('details', { class: 'more' }, h('summary', {}, 'Preview: another mixer'),
      h('p', { class: 'help' }, 'The simulated mixer: pick which firmware it pretends to run.'),
      h('div', { class: 'actions' }, ...[['Ours 0.0.1', { kind: 'ours', version: '0.0.1' }], [`Ours ${RELEASE.version}`, { kind: 'ours', version: RELEASE.version }], ['M-Vave 012', { kind: 'stock', version: 'SMC-Mixer_012' }], ['M-Vave 013', { kind: 'stock', version: 'SMC-Mixer_013' }], ['Update mode', { kind: 'loader', version: '' }]]
        .map(([n, f]) => h('button', { class: `chip ${dev.fw.kind === f.kind && dev.fw.version === f.version ? 'on' : ''}`, onclick: () => { dev.setFirmware(f); deviceDialog(); } }, n)))),
    h('div', { class: 'dlg-foot' }, h('button', { class: 'primary', onclick: closeDialog }, 'Done'))));
}
// The parts of the sheet that need our firmware on the mixer.
function deviceOurs() {
  return [
    h('h3', {}, 'While connected'),
    h('div', { class: 'seg full link-seg' }, ...linkButtons(() => deviceDialog())),
    h('p', { class: 'help' }, st.follow ? LINK_HELP.linked : LINK_HELP.separate),
    h('h3', {}, 'Saving'),
    savingExplainer(),
    h('div', { class: 'actions' },
      !st.follow ? btn('upload', 'Send', { disabled: !unsent(), title: 'Play your changes on the mixer, without saving them', onclick: () => { sendToMixer(); deviceDialog(); } }) : null,
      btn('download', 'Save', { class: unsaved() ? 'primary' : '', disabled: !unsaved(), title: 'The mixer starts with this configuration from now on', onclick: async () => { await saveOnMixer(); if ($('#dlg').open) deviceDialog(); } }),
      h('button', { disabled: !!dev.firmwareJob?.active || !unsaved(), onclick: revertDialog }, 'Go back to the last save…')),
    h('p', { class: 'help row' }, `The mixer saves ${dev.autosave ? 'by itself, a few seconds after the last change' : 'when you press Save'}.`, h('button', { class: 'link', onclick: () => settingsDialog('global') }, 'Change in Settings')),
    h('h3', {}, 'Configuration'),
    h('div', { class: 'actions' },
      btn('download', 'Load from the mixer', { onclick: () => { closeDialog(); loadFromMixer(); } })),

    firmwareActions(true),
  ];
}
function firmwareActions(own = false) {
  return h('details', { class: 'more' }, h('summary', {}, 'Firmware'),
    h('p', { class: 'help' }, 'Firmware comes from the release published with this editor.'),
    h('div', { class: 'actions' },
      dev.real ? btn('download', 'Check for updates', { onclick: checkForUpdates }) : null,
      own ? h('button', { class: 'danger', onclick: stockDialog }, 'Go back to M-Vave’s firmware…') : null));
}
async function checkForUpdates(e) {
  const button = e.currentTarget;
  button.disabled = true;
  toast('Checking the firmware manifest…');
  try {
    const result = await refreshRelease(location.href);
    if (result.status === 'missing') toast('No release manifest was found at this editor’s address.');
    else if (result.status !== 'found') toast(result.status === 'invalid' ? 'The release manifest is invalid.' : result.status === 'network' ? 'The release server could not be reached.' : `The release server returned HTTP ${result.httpStatus}.`);
    else {
      const rel = result.release;
      useRelease(rel);
      if ($('#dlg').open && !dev.firmwareJob?.active) deviceDialog(); // the update card appears in the open sheet
      if (!rel.install) toast('This release contains no installable firmware.');
      else if (!ours() || !dev.fw.version) toast(`Firmware ${RELEASE.version} is available. Connect to better-smc-mixer to compare versions.`);
      else if (compareVersions(RELEASE.version, dev.fw.version) <= 0) toast(`This release has no newer firmware. The mixer runs ${dev.fw.version}.`);
      else if (!canUpdate()) toast(`Firmware ${RELEASE.version} is available, but this mixer cannot use the editor’s update route.`);
      else toast(`Firmware ${RELEASE.version} is available: use Update… above.`);
    }
  } finally { button.disabled = false; }
}

function reportDialog() {
  const include = h('input', { type: 'checkbox', id: 'report-config' });
  const save = () => {
    const now = new Date().toISOString();
    const connected = dev.connected;
    const firmware = connected ? { kind: dev.fw.kind, version: dev.fw.version } : null;
    const info = ours() ? dev.info : {};
    const report = {
      format: 1, created: now, editor: VERSION, browser: navigator.userAgent,
      device: { connected, simulated: !dev.real, transport: connected ? dev.transport : null, firmware,
        build: info.buildId ?? null, protocol: info.protocol ?? null, binary: info.binary ?? null,
        schema: info.schema ?? null, storage: ours() ? dev.storage : null,
        revision: ours() ? (dev.revision ?? null) : null },
      editorLog: dev.logLines.slice(-80), bufferedMidiEvents: st.monitor.length,
    };
    if (include.checked) report.configuration = cfg.canonical();
    download(report, `BSM-Report ${now.slice(0, 10)}`);
  };
  dialog(h('div', {}, h('h2', {}, 'Save a report for support'),
    h('p', { class: 'help' }, 'The file includes browser and firmware information and recent editor logs, which may contain MIDI port names. Review it before sharing. It does not include flash backups or raw MIDI events.'),
    h('label', { class: 'row', for: include.id }, include, 'Include the configuration open in the editor'),
    h('div', { class: 'dlg-foot' }, h('button', { onclick: deviceDialog }, 'Back'),
      btn('download', 'Download report', { class: 'primary', onclick: save }))));
}
function renderLogs(el) {
  put(el, ...dev.logLines.slice(-80).map((l) => h('div', {}, `${l.t} ${l.level.padEnd(5)} ${l.text}`)));
  el.scrollTop = el.scrollHeight;
}

// ---- settings ------------------------------------------------------------------------------------------

// Settings by level: the mixer's (every preset), then this preset's. The theme is the app's, in the top bar.
function settingsDialog(tab = 'global') {
  const p = st.preset;
  const pr = cfg.file.presets[p];
  const rows = [];
  const form = tab === 'preset' ? PRESET_FORM : GLOBAL_FORM;
  {
    if (tab === 'global') rows.push(h('h3', {}, 'Saving'), savingSetting(() => settingsDialog('global')));
    if (tab === 'preset') rows.push(field('Name', h('input', { id: 'preset-name', value: pr.name ?? '', placeholder: `Preset ${p + 1}`, maxlength: S.nameMax, onchange: (e) => edit('preset name', () => { if (e.target.value) pr.name = e.target.value; else delete pr.name; }) })));
    let sec = null, shown = null;
    for (const item of form) {
      if (item.h) { sec = item.h; continue; }
      if (Array.isArray(item)) {
        if (sec !== shown) { rows.push(h('h3', {}, sec)); shown = sec; }
        rows.push(schemaField(item, tab === 'preset' ? pr : cfg.file.global, tab));
      } else if (item.editor) { // the editor's own setting: works now
        if (sec !== shown) { rows.push(h('h3', {}, sec)); shown = sec; }
        rows.push(plannedField(item, tab === 'preset' ? (st.planned.preset[p] ??= {}) : st.planned.global, () => { renderAll(); settingsDialog(tab); }));
      }
    }
  }
  dialog(h('div', { class: 'settings' },
    h('div', { class: 'dlg-top' },
      h('div', { class: 'dlg-head' }, h('h2', {}, 'Settings'), h('button', { class: 'icon-only', 'aria-label': 'Close', onclick: closeDialog }, icon('close'))),
      h('div', { class: 'seg full' },
        h('button', { class: tab === 'global' ? 'on' : '', onclick: () => settingsDialog('global') }, 'Mixer'),
        h('button', { class: tab === 'preset' ? 'on' : '', onclick: () => settingsDialog('preset') }, 'Preset'))),
    h('p', { class: 'help' }, tab === 'preset' ? `For ${presetLabel(p)} only: a preset is usually one instrument, and each has its own.` : 'For the mixer as a whole: every preset and page.'),
    ...rows,
    h('div', { class: 'dlg-foot' }, h('button', { class: 'primary', onclick: closeDialog }, 'Done'))));
}

function schemaField([key, label, help, options, range], obj, tab) {
  const f = (tab === 'preset' ? S.presetFields : S.globalFields).find((x) => x.json === key);
  const v = obj[key] ?? f.default;
  const set = (x) => edit(label.toLowerCase(), () => { if (x === f.default) delete obj[key]; else obj[key] = x; });
  let input;
  if (key === 'channel') {
    input = h('select', { id: `set-${key}`, onchange: (e) => set(Number(e.target.value)) }, ...Array.from({ length: 16 }, (_, i) => h('option', { value: i + 1, selected: v === i + 1 }, `Channel ${i + 1}`)));
  } else if (options) {
    input = h('select', { id: `set-${key}`, onchange: (e) => set(e.target.value) }, ...Object.entries(options).map(([k, n]) => h('option', { value: k, selected: v === k }, n)));
  } else {
    const [min, max, unit] = range;
    input = h('div', { class: 'row unit' }, h('input', { id: `set-${key}`, type: 'number', inputmode: 'numeric', min, max, value: v, onchange: (e) => set(Math.max(min, Math.min(max, Number(e.target.value)))) }), h('span', { class: 'eq' }, unit));
  }
  return field(label, input, help);
}

function plannedField(item, scope, after) {
  let input;
  const v = plannedGet(scope, item.key, item.def);
  const set = (x) => { plannedSet(scope, item.key, x, item.label); after?.(); };
  if (item.options) input = h('select', { id: `pl-${item.key}`, onchange: (e) => set(e.target.value) }, ...Object.entries(item.options).map(([k, n]) => h('option', { value: k, selected: String(v) === String(k) }, n)));
  else if (item.int) input = h('div', { class: 'row unit' }, h('input', { id: `pl-${item.key}`, type: 'number', inputmode: 'numeric', min: item.int[0], max: item.int[1], value: v, onchange: (e) => set(Number(e.target.value)) }), h('span', { class: 'eq' }, item.int[2]));
  else input = h('input', { id: `pl-${item.key}`, value: v, maxlength: item.text, onchange: (e) => set(e.target.value) });
  return field(item.label, input, item.help);
}

// ---- menus ---------------------------------------------------------------------------------------------

function menu(anchor, items) {
  const m = $('#menu');
  put(m, ...items.map((it) => (it === '-' ? h('div', { class: 'sep' }) : typeof it === 'string' ? h('div', { class: 'mh' }, it)
    : h('button', { disabled: it.disabled, class: it.danger ? 'danger' : '', onclick: () => { m.hidden = true; it.do(); } }, it.icon ? icon(it.icon, 16) : h('span', { class: 'ic' }), h('span', {}, it.label)))));
  const r = anchor.getBoundingClientRect();
  m.hidden = false;
  const w = m.offsetWidth, ht = m.offsetHeight;
  m.style.left = `${Math.max(8, Math.min(r.left, innerWidth - w - 8))}px`;
  m.style.top = `${r.bottom + ht + 8 > innerHeight ? Math.max(8, r.top - ht - 4) : r.bottom + 4}px`;
  m.querySelector('button:not([disabled])')?.focus();
}
document.addEventListener('pointerdown', (e) => { if (!e.target.closest('#menu') && !e.target.closest('.tab.on') && !e.target.closest('#btn-file') && !e.target.closest('#btn-theme')) $('#menu').hidden = true; });

function fileMenu(anchor) {
  menu(anchor, [
    { label: 'Open a file…', icon: 'file', do: () => pickFile((f, name) => openFile(f, name)) },
    { label: 'Templates…', icon: 'library', do: libraryDialog },
    { label: 'Start from the factory settings', do: async () => { const f = await fetchFirst(DATA('factory.json')); if (f) askConfirm('Start from the factory settings?', 'This replaces the configuration in the editor and on the mixer. Undo brings the current one back.', 'Replace', () => openFile(f, 'factory settings')); } },
    '-', 'Save a copy',
    { label: 'Everything', icon: 'download', do: () => showJson(cfg.canonical(), 'Everything') },
    { label: `This preset (${presetLabel(st.preset)})`, do: () => showJson(cfg.canonical({ preset: st.preset }), presetLabel(st.preset)) },
    { label: `This page (${pageLabel(st.preset, st.page)})`, do: () => showJson(cfg.canonical({ preset: st.preset, page: st.page }), pageLabel(st.preset, st.page)) },
    '-', 'Bring in from a file',
    { label: `A preset, into ${presetLabel(st.preset)}`, icon: 'upload', do: () => pickFile((f) => importPreset(f, st.preset)) },
    { label: `A page, into ${pageLabel(st.preset, st.page)}`, do: () => pickFile((f) => importPage(f)) },
  ]);
}

function themeMenu(anchor) {
  menu(anchor, ['Appearance', ...[['auto', 'auto', 'As the system'], ['light', 'sun', 'Light'], ['dark', 'moon', 'Dark']].map(([k, ic, n]) => ({ label: `${n}${st.theme === k ? '  ✓' : ''}`, icon: ic, do: () => setTheme(k) }))]);
}

function presetMenu(anchor, p) {
  // Narrow screens show only the current preset's tab: its menu switches presets.
  const others = matchMedia('(max-width: 1000px)').matches
    ? ['Go to', ...Array.from({ length: S.device.presets }, (_, q) => q).filter((q) => q !== p).map((q) => ({ label: presetLabel(q), do: () => selectView(q, st.page, true) })), '-'] : [];
  menu(anchor, [
    ...others,
    presetLabel(p),
    { label: 'Rename…', do: () => askName('Preset name', cfg.file.presets[p].name, `Preset ${p + 1}`, (n) => edit('preset name', () => { if (n) cfg.file.presets[p].name = n; else delete cfg.file.presets[p].name; })) },
    { label: 'Channel and Shift…', icon: 'settings', do: () => { st.preset = p; settingsDialog('preset'); } },
    { label: 'Use a template…', icon: 'library', do: () => { st.preset = p; libraryDialog(); } },
    { label: 'Save as a template…', icon: 'plus', do: () => saveTemplate(p) },
    '-',
    { label: 'Copy preset', icon: 'copy', do: () => { st.clip = { kind: 'preset', data: structuredClone(cfg.file.presets[p]) }; toast('Preset copied'); } },
    { label: 'Paste preset here', icon: 'paste', disabled: st.clip?.kind !== 'preset', do: () => edit('paste preset', () => { cfg.file.presets[p] = structuredClone(st.clip.data); cfg.fill(cfg.file); }) },
    { label: 'Save a copy…', icon: 'download', do: () => showJson(cfg.canonical({ preset: p }), presetLabel(p)) },
    '-',
    { label: 'Clear preset…', icon: 'trash', danger: true, do: () => askConfirm(`Clear ${presetLabel(p)}?`, 'All its pages and the bottom row will send nothing. Undo brings it back.', 'Clear', () => edit('clear preset', () => { cfg.file.presets[p] = {}; cfg.fill(cfg.file); })) },
  ]);
}

function pageMenu(anchor, g) {
  const p = st.preset;
  menu(anchor, [
    pageLabel(p, g),
    { label: 'Rename…', do: () => askName('Page name', cfg.file.presets[p].pages[g].name, `Page ${g + 1}`, (n) => edit('page name', () => { if (n) cfg.file.presets[p].pages[g].name = n; else delete cfg.file.presets[p].pages[g].name; })) },
    '-',
    { label: 'Copy page', icon: 'copy', do: () => { st.clip = { kind: 'page', data: structuredClone(cfg.file.presets[p].pages[g]) }; toast('Page copied'); } },
    { label: 'Paste page here', icon: 'paste', disabled: st.clip?.kind !== 'page', do: () => edit('paste page', () => { cfg.file.presets[p].pages[g] = structuredClone(st.clip.data); }) },
    { label: 'Paste with numbers moved on…', disabled: st.clip?.kind !== 'page', do: () => offsetDialog(g) },
    { label: 'Save a copy…', icon: 'download', do: () => showJson(cfg.canonical({ preset: p, page: g }), pageLabel(p, g)) },
    '-',
    { label: 'Clear page…', icon: 'trash', danger: true, do: () => clearPageDialog(p, g) },
  ]);
}

// Paste the copied page onto page g with its numbers moved on (FR-MOD-04 as a copy, A-6). Each type
// moves what identifies it: a CC its number (a CC 14-bit its LSB's CC with it), an NRPN or RPN its
// LSB, MSB or whole number, a note only when asked. Program Change, Pitch Bend and Aftertouch have no
// number. A value that would leave its range is not clamped (that would make two controls share
// one): the control keeps its number and the preview says so. Own Shift layers move by the same
// rules; "Same as Normal" layers follow by themselves. The bottom row is not part of a page.
function offsetDialog(g) {
  const src = st.clip.data;
  const p = st.preset;
  const pc = cfg.presetChannel(p);
  const dn = h('input', { id: 'off-n', type: 'number', inputmode: 'numeric', value: 8 });
  const dc = h('input', { id: 'off-c', type: 'number', inputmode: 'numeric', value: 0 });
  const notes = h('input', { type: 'checkbox', id: 'off-notes' });
  const names = h('select', { id: 'off-names' }, h('option', { value: 'count' }, 'Count on: “Track 1” becomes “Track 9”'), h('option', { value: 'keep' }, 'Keep them as they are'), h('option', { value: 'drop' }, 'Remove them'));
  let part = 'lsb';
  const layersOf = (ctl) => [['', ctl], [' (Shift)', ctl && typeof ctl.shift === 'object' ? ctl.shift : null]].filter(([, l]) => l && (l.type ?? 'none') !== 'none');
  const has = (pred) => (src.strips ?? []).some((s) => Object.values(s ?? {}).some((c) => layersOf(c).some(([, l]) => pred(l.type))));
  const hasPair = has((t) => t === 'nrpn' || t === 'rpn');
  const partSeg = h('div', { class: 'seg full' }, ...[['lsb', 'LSB'], ['msb', 'MSB'], ['whole', 'Whole number']].map(([k, n]) => h('button', { 'data-part': k, onclick: () => { part = k; show(); } }, n)));

  // One layer moved on: { l: the new layer, what: before → after, err } or null when nothing moves.
  const moved = (l, kind) => {
    const k = Number(dn.value) || 0, c = Number(dc.value) || 0;
    const out = structuredClone(l);
    const errs = [];
    const t = l.type;
    if (S.hasNumber(t) && (t !== 'note' || notes.checked) && k) {
      const n = l.number ?? 0;
      let v = n + k;
      if (t === 'nrpn' || t === 'rpn') {
        const m = n >> 7, s = n & 127;
        if (part === 'lsb') v = s + k >= 0 && s + k <= 127 ? m * 128 + s + k : -1;
        if (part === 'msb') v = m + k >= 0 && m + k <= 127 ? (m + k) * 128 + s : -1;
      }
      if (v >= 0 && v <= S.numberMax(t)) out.number = v; else errs.push(`${t === 'note' ? 'note' : 'number'} out of range`);
    }
    if (typeof l.channel === 'number' && c) {
      const v = l.channel + c;
      if (v >= 1 && v <= 16) out.channel = v; else errs.push('channel out of range');
    }
    const a = shortLabel(S, l), b = shortLabel(S, out);
    if (a === b && !errs.length) return null;
    return { l: out, a, b, err: errs.join(', ') };
  };
  const renamed = (name) => {
    if (!name || names.value === 'keep') return name;
    if (names.value === 'drop') return undefined;
    const k = Number(dn.value) || 0;
    return name.replace(/(\d+)(\D*)$/, (_, d, rest) => `${Number(d) + k}${rest}`);
  };

  const plan = () => {
    const rows = [];
    (src.strips ?? []).forEach((s, i) => {
      for (const [key, ctl] of Object.entries(s ?? {})) {
        const kind = S.kindOfStrip(key);
        for (const [tag, l] of layersOf(ctl)) {
          const m = moved(l, kind);
          if (m) rows.push({ id: `s${i}.${key}`, tag, ...m });
        }
      }
    });
    // A message the bottom row (which every page keeps) already sends: shown, not prevented.
    const row = new Map();
    for (const id of IDS.filter((x) => x.startsWith('row.'))) for (const [tag, l] of layersOf(ctlAt(id, p, g))) for (const sl of slotsOf(l, pc)) row.set(sl, `${ctlName(id)}${tag}`);
    for (const r of rows) if (!r.err) r.clash = slotsOf(r.l, pc).map((sl) => row.get(sl)).filter(Boolean);
    return rows.sort((x, y) => IDS.indexOf(x.id) - IDS.indexOf(y.id) || x.tag.length - y.tag.length);
  };

  const kinds = [
    has((t) => t === 'cc7' || t?.startsWith('relative')) && 'CC numbers',
    has((t) => t === 'cc14') && 'CC 14-bit numbers (the LSB’s CC moves with them)',
    hasPair && 'NRPN and RPN numbers, as chosen below',
    has((t) => t === 'note') && 'notes, if ticked',
  ].filter(Boolean);
  const fixedOnly = [has((t) => t === 'program') && 'Program Change', has((t) => t === 'pitchbend') && 'Pitch Bend', has((t) => t === 'aftertouch') && 'Aftertouch'].filter(Boolean);

  const prev = h('div', { class: 'offset-prev' });
  const summary = h('p', { class: 'help' });
  const apply = h('button', { class: 'primary' }, `Paste on ${pageLabel(p, g)}`);
  const pairBox = hasPair ? field('NRPN and RPN: what moves', partSeg, 'LSB: 3:10 → 3:18. MSB: 3:10 → 11:10. Whole number: counts across the end of the LSB.') : null;
  const show = () => {
    for (const b of partSeg.children) b.classList.toggle('on', b.dataset.part === part);
    const rows = plan();
    put(prev, rows.length ? h('table', { class: 'grid' }, h('tbody', {}, ...rows.map((r) => h('tr', { class: r.err ? 'err' : r.clash?.length ? 'clash' : '' },
      h('td', {}, `${ctlName(r.id)}${r.tag}`), h('td', { class: 'muted' }, r.a), h('td', {}, '→'),
      h('td', {}, r.err ? h('span', { class: 'bad' }, `stays ${r.a}: ${r.err}`) : r.b, r.clash?.length ? h('span', { class: 'muted', title: `Already assigned to ${r.clash.join(', ')}` }, ' ', icon('copy', 12), ` = ${r.clash.join(', ')}`) : null)))))
      : h('p', { class: 'help' }, 'Nothing moves: the page is pasted as it is.'));
    const errs = rows.filter((r) => r.err).length, clashes = rows.filter((r) => r.clash?.length).length;
    const bits = [`${plural(rows.length - errs, 'control')} change${rows.length - errs === 1 ? 's' : ''}`];
    if (errs) bits.push(`${errs} would leave the range and keep ${errs === 1 ? 'its number' : 'their numbers'} (red)`);
    if (clashes) bits.push(`${clashes} would send the same as a bottom-row key`);
    summary.textContent = `${bits.join('. ')}.`;
  };
  for (const e of [dn, dc, notes, names]) e.addEventListener('input', show);
  apply.addEventListener('click', () => {
    const k = Number(dn.value) || 0;
    edit(`paste page shifted by ${k}`, () => {
      const page = structuredClone(src);
      (page.strips ?? []).forEach((s) => {
        for (const [key, ctl] of Object.entries(s ?? {})) {
          const kind = S.kindOfStrip(key);
          for (const [tag, l] of layersOf(ctl)) {
            const m = moved(l, kind);
            if (m && !m.err) { if (tag) ctl.shift = m.l; else { const { shift, name, ...rest } = m.l; for (const f of Object.keys(ctl)) if (f !== 'shift' && f !== 'name') delete ctl[f]; Object.assign(ctl, rest); } }
          }
          const nm = renamed(ctl.name);
          if (nm) ctl.name = nm; else delete ctl.name;
          if (ctl.shift && typeof ctl.shift === 'object') { const sn = renamed(ctl.shift.name); if (sn) ctl.shift.name = sn; else delete ctl.shift.name; }
        }
      });
      if (names.value === 'drop') delete page.name; else if (page.name) page.name = renamed(page.name);
      cfg.file.presets[p].pages[g] = page;
      cfg.fill(cfg.file);
    });
    closeDialog();
    toast(`Pasted on ${pageLabel(p, g)}. Undo takes it back.`);
  });

  dialog(h('div', { class: 'fill-dlg' },
    h('h2', {}, `Paste on ${pageLabel(p, g)}, numbers moved on`),
    h('p', { class: 'help' }, `For more tracks than one page holds: copy page 1, paste it on page 2 with +8. ${pageLabel(p, g)} is replaced; undo brings it back.`),
    h('div', { class: 'form3' }, field('Add to numbers', dn, 'Negative counts back.'), field('Add to fixed channels', dc, 'Controls on the preset channel stay on it.')),
    pairBox,
    has((t) => t === 'note') ? h('label', { class: 'check' }, notes, h('span', {}, 'Move notes too (off: drum pads keep their sounds)')) : null,
    field('Names', names),
    h('div', { class: 'note' }, icon('info', 16), h('span', {}, `What moves: ${kinds.length ? kinds.join('; ') : 'nothing on this page has a number'}.`,
      fixedOnly.length ? ` ${fixedOnly.join(', ')} ${fixedOnly.length === 1 ? 'has' : 'have'} no number: only a fixed channel moves.` : '',
      ' Values and ranges stay; own Shift layers move by the same rules. The bottom row belongs to every page and is not pasted.')),
    prev, summary,
    h('div', { class: 'dlg-foot' }, h('button', { onclick: closeDialog }, 'Cancel'), apply)));
  show();
}

function importPreset(file, p) {
  const errs = S.checkFile(file);
  if (errs.length) { showProblems(errs, 'This file cannot be used:'); return; }
  const src = (file.presets ?? [])[0];
  if (!src) { toast('The file has no preset.'); return; }
  edit('bring in a preset', () => { cfg.file.presets[p] = structuredClone(src); cfg.fill(cfg.file); });
}
function importPage(file) {
  const errs = S.checkFile(file);
  if (errs.length) { showProblems(errs, 'This file cannot be used:'); return; }
  const src = file.presets?.[0]?.pages?.[0];
  if (!src) { toast('The file has no page.'); return; }
  edit('bring in a page', () => { cfg.file.presets[st.preset].pages[st.page] = structuredClone(src); cfg.fill(cfg.file); });
}

// The user's own templates: a preset kept in the app's storage, with the editor's settings for it.
const myTemplates = () => { try { return JSON.parse(store.get('smc-my-templates') || '[]'); } catch { return []; } };
function saveTemplate(p) {
  const name = h('input', { id: 'tpl-name', value: cfg.file.presets[p].name ?? '', placeholder: presetLabel(p), maxlength: 40 });
  const about = h('textarea', { id: 'tpl-about', rows: 2, maxlength: 160, placeholder: 'What it is for, e.g. “Filter and envelopes of my bass synth on channel 2”' });
  const ok = () => {
    const n = name.value.trim() || presetLabel(p);
    const list = myTemplates().filter((t) => t.name !== n);
    list.unshift({ name: n, about: about.value.trim(), saved: new Date().toISOString().slice(0, 10), file: cfg.canonical({ preset: p }), editor: structuredClone(st.planned.preset[p] ?? {}) });
    store.set('smc-my-templates', JSON.stringify(list));
    closeDialog();
    toast(`Saved “${n}” in My templates. Any preset can start from it.`);
  };
  dialog(h('div', {}, h('h2', {}, 'Save as a template'),
    h('p', { class: 'help' }, `Keeps ${presetLabel(p)} (its pages, controls, names and channel) in this app on this computer, to start any preset from it later.`),
    field('Name', name), field('Note', about, 'Optional. Shown on the template’s card.'),
    h('div', { class: 'dlg-foot' }, h('button', { onclick: closeDialog }, 'Cancel'), h('button', { class: 'primary', onclick: ok }, 'Save template'))));
  name.focus(); name.select();
}
// A template replaces the current preset, its name included: say so in the button and the message.
const replaceLabel = () => `Replace ${presetLabel(st.preset)}`;
function useTemplate(t) {
  const was = presetLabel(st.preset);
  importPreset(t.file, st.preset);
  st.planned.preset[st.preset] = structuredClone(t.editor ?? {});
  store.set('smc-proto-planned', JSON.stringify(st.planned));
  closeDialog(); renderAll();
  toast(`${was} now holds the template “${t.name}”${presetLabel(st.preset) !== was ? `, and is named ${presetLabel(st.preset)}` : ''}. Undo brings the old one back.`);
}
function libraryDialog() {
  const mine = myTemplates();
  dialog(h('div', {},
    h('div', { class: 'dlg-head' }, h('h2', {}, 'Templates'), h('button', { class: 'icon-only', 'aria-label': 'Close', onclick: closeDialog }, icon('close'))),
    h('p', { class: 'help' }, `A template is a whole preset to start from: pages, controls, names and channel. “Replace” turns ${presetLabel(st.preset)} into a copy of it, name included; undo brings the old one back. To fill another preset, open its tab first.`),
    h('div', { class: 'compare' },
      h('div', {}, h('b', {}, 'A template'), h('p', {}, 'One preset. Kept here, in this app on this computer, ready to use in any preset with one click.')),
      h('div', {}, h('b', {}, 'Save a copy (a file)'), h('p', {}, 'The configuration, or a part of it, as a file: to keep as a backup, send to someone, or open on another computer.'))),
    h('h3', {}, 'My templates'),
    mine.length
      ? h('div', { class: 'cards' }, ...mine.map((t) => h('div', { class: 'card' }, h('b', {}, t.name), h('p', {}, t.about ? [t.about, h('br'), h('span', { class: 'muted' }, `Saved ${t.saved}`)] : `Saved ${t.saved}.`),
        h('div', { class: 'row' },
          h('button', { title: `${replaceLabel()}: it becomes a copy of this template, with its pages, controls, names and channel`, onclick: () => useTemplate(t) }, h('span', { class: 'ell' }, 'Replace')),
          h('button', { class: 'icon-only danger', title: 'Remove from My templates', 'aria-label': `Remove ${t.name}`, onclick: () => { store.set('smc-my-templates', JSON.stringify(myTemplates().filter((x) => x.name !== t.name))); libraryDialog(); } }, icon('trash', 16))))))
      : h('p', { class: 'help' }, 'None yet. A preset’s menu (its tab) has “Save as a template…”.'),
    h('h3', {}, 'Ready-made templates'),
    h('div', { class: 'cards' }, ...LAYOUTS.map((l) => h('div', { class: 'card' }, h('b', {}, l.name), h('p', {}, l.about),
      h('button', { title: `${replaceLabel()}: it becomes a copy of this template, with its pages, controls, names and channel`, onclick: () => useTemplate({ name: l.name, file: l.file, editor: {} }) }, h('span', { class: 'ell' }, 'Replace'))))),

    h('div', { class: 'dlg-foot' }, h('button', { class: 'primary', onclick: closeDialog }, 'Close'))));
}

function showJson(obj, what) {
  const fname = h('input', { id: 'save-name', value: configFileName(cfg.file.name, what) });
  dialog(h('div', {},
    h('div', { class: 'dlg-head' }, h('h2', {}, `Save a copy: ${what}`), h('button', { class: 'icon-only', 'aria-label': 'Close', onclick: closeDialog }, icon('close'))),
    h('p', { class: 'help' }, 'A configuration file to keep as a backup, share, or open later with “Open a file”.'),
    field('File name', fname),
    h('details', { class: 'more' }, h('summary', {}, 'Show the file'), h('pre', { class: 'json' }, dumps(obj))),
    h('div', { class: 'dlg-foot' },
      btn('copy', 'Copy the text', { onclick: () => { const t = dumps(obj); if (navigator.clipboard) navigator.clipboard.writeText(t).then(() => toast('Copied'), selectPre); else selectPre(); } }),
      btn('download', 'Download', { class: 'primary', onclick: () => download(obj, fname.value) }))));
}
function selectPre() {
  const pre = document.querySelector('#dlg pre.json'); if (!pre) return;
  pre.closest('details')?.setAttribute('open', '');
  const r = document.createRange(); r.selectNodeContents(pre); const sel = getSelection(); sel.removeAllRanges(); sel.addRange(r);
  toast('Selected: copy it with ⌘C');
}

function showProblems(errs, title) {
  dialog(h('div', {}, h('h2', {}, errs.length ? title : 'No problems'),
    errs.length ? h('div', { class: 'errlist' }, ...errs.slice(0, 200).map((e) => h('div', {}, e))) : null,
    h('div', { class: 'dlg-foot' }, h('button', { class: 'primary', onclick: closeDialog }, 'Close'))));
}

function askName(title, value, placeholder, done) {
  const inp = h('input', { id: 'ask-name', value: value ?? '', maxlength: S.nameMax, placeholder });
  const ok = () => { done(inp.value.trim()); closeDialog(); };
  inp.addEventListener('keydown', (e) => { if (e.key === 'Enter') ok(); });
  dialog(h('div', {}, h('h2', {}, title), inp, h('p', { class: 'help' }, 'Leave empty for the default name.'),
    h('div', { class: 'dlg-foot' }, h('button', { onclick: closeDialog }, 'Cancel'), h('button', { class: 'primary', onclick: ok }, 'Rename'))));
  inp.focus(); inp.select();
}
// Clearing a page: its strips, and on request the bottom row, which is the preset's (every page).
function clearPageDialog(p, g) {
  const row = h('input', { type: 'checkbox', id: 'clear-row' });
  dialog(h('div', {}, h('h2', {}, `Clear ${pageLabel(p, g)}?`),
    h('p', {}, 'Its faders, encoders and strip buttons will send nothing. Undo brings them back.'),
    h('label', { class: 'check', for: 'clear-row' }, row, h('span', {}, `Also clear the bottom row. It is shared by all four pages of ${presetLabel(p)}, so it is cleared on every page.`)),
    h('div', { class: 'dlg-foot' }, h('button', { onclick: closeDialog }, 'Cancel'), h('button', { class: 'primary danger-fill', onclick: () => {
      const withRow = row.checked;
      closeDialog();
      edit(withRow ? 'clear page and bottom row' : 'clear page', () => { cfg.file.presets[p].pages[g] = {}; if (withRow) cfg.file.presets[p].row = {}; cfg.fill(cfg.file); });
      toast(`${withRow ? 'Page and bottom row cleared' : 'Page cleared; the bottom row is as it was'}. Undo brings it back.`);
    } }, 'Clear'))));
}

function askConfirm(title, text, action, done) {
  dialog(h('div', {}, h('h2', {}, title), h('p', {}, text),
    h('div', { class: 'dlg-foot' }, h('button', { onclick: closeDialog }, 'Cancel'), h('button', { class: 'primary danger-fill', onclick: () => { closeDialog(); done(); } }, action))));
}

function dialog(content) { put($('#dlg-body'), content); const d = $('#dlg'); if (!d.open) d.showModal(); }
function closeDialog() { $('#dlg').close(); }

function download(obj, name) {
  const blob = new Blob([dumps(obj)], { type: 'application/json' });
  const url = URL.createObjectURL(blob);
  const a = h('a', { href: url, download: `${name.replace(/[^\w\- ]+/g, '').trim() || CONFIG_FILE_BASE}.json` });
  document.body.append(a); a.click(); a.remove();
  setTimeout(() => URL.revokeObjectURL(url), 1000);
}
function pickFile(cb) {
  const inp = h('input', { type: 'file', accept: '.json,application/json' });
  inp.addEventListener('change', async () => {
    const f = inp.files[0]; if (!f) return;
    try { cb(JSON.parse(await f.text()), f.name); } catch { showProblems([`${f.name} is not a configuration file.`], 'This file cannot be used:'); }
  });
  inp.click();
}

// ---- wiring ---------------------------------------------------------------------------------------

function wire() {
  wireDevice();
  put($('#btn-map'), icon('keys'), h('span', { class: 'tb-label' }, 'Map instrument'));
  put($('#undo'), icon('undo'));
  put($('#redo'), icon('redo'));
  put($('#btn-file'), icon('file'), h('span', { class: 'tb-label' }, 'File'), icon('chevron', 14));
  put($('#btn-settings'), icon('sliders'), h('span', { class: 'tb-label' }, 'Settings'));
  applyTheme();
  $('#cfg-name').addEventListener('change', (e) => edit('configuration name', () => { if (e.target.value) cfg.file.name = e.target.value; else delete cfg.file.name; }));
  $('#problems').addEventListener('click', () => showProblems(st.errors, 'Problems in this configuration:'));
  $('#undo').addEventListener('click', undo);
  $('#redo').addEventListener('click', redo);
  $('#btn-file').addEventListener('click', (e) => fileMenu(e.currentTarget));
  $('#btn-map').addEventListener('click', () => (st.tab === 'params' ? leaveMapping() : enterMapping()));
  $('#btn-settings').addEventListener('click', () => settingsDialog('global'));
  $('#btn-theme').addEventListener('click', (e) => themeMenu(e.currentTarget));
  $('#device-pill').addEventListener('click', deviceDialog);
  for (const b of document.querySelectorAll('#layer-seg button')) b.addEventListener('click', () => setLayer(b.dataset.layer));
  put($('#sheet-toggle'), icon('chevron'));
  $('#sheet-toggle').addEventListener('click', () => sheet(document.body.classList.contains('sheet-min')));
  sheet(!matchMedia('(max-width: 600px)').matches);
  const panel = $('#panel');
  panel.addEventListener('pointerdown', onPanelDown);
  panel.addEventListener('pointermove', onPanelMove);
  panel.addEventListener('pointerup', onPanelUp);
  panel.addEventListener('pointercancel', onPanelUp);
  panel.addEventListener('click', onPanelClick);
  panel.addEventListener('wheel', (e) => {
    if (!st.sim) return;
    const el = e.target.closest('.ctl.enc'); if (!el) return;
    e.preventDefault();
    playEncoder(el.dataset.id, e.deltaY < 0 ? 1 : -1);
  }, { passive: false });
  document.addEventListener('keydown', (e) => {
    if (e.target.closest('input,select,textarea') || $('#dlg').open) return;
    const mod = e.metaKey || e.ctrlKey;
    if (mod && e.key.toLowerCase() === 'z') { e.preventDefault(); if (e.shiftKey) redo(); else undo(); }
    else if (mod && e.key === 'c') copySel();
    else if (mod && e.key === 'v') pasteSel();
    else if (mod && e.key === 'a') { e.preventDefault(); selectIds(IDS.filter((i) => !i.startsWith('row.')), false); }
    else if (e.key === 'Escape' && st.learn) stopLearn();
    else if (e.key === 'Escape') { $('#menu').hidden = true; st.armed = null; st.sel.clear(); renderAll(); }
    else if (e.key === 'Backspace' || e.key === 'Delete') { if (st.sel.size) clearSel(); }
  });
  matchMedia('(prefers-color-scheme: dark)').addEventListener?.('change', () => applyTheme());
  renderAll();
}

boot();
