// What a layer sends: a short label for the panel, a sentence and the bytes for the inspector,
// and the messages for the mocked device (docs/core.md, "Message types", "NRPN and RPN").

const hex = (n) => n.toString(16).toUpperCase().padStart(2, '0');
const NOTE_NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
// Note 60 is middle C: "C4" on most instruments, "C3" on some (a preset setting of the editor).
let octaveBase = 1;
export function setMiddleC(name) { octaveBase = name === 'C3' ? 2 : 1; }
export const noteName = (n) => NOTE_NAMES[n % 12] + (Math.floor(n / 12) - octaveBase);
// "C3", "F#2", "Bb-1" or a plain number 0–127, named as noteName names them; NaN if neither.
export function noteNumber(text) {
  const t = String(text).trim();
  if (/^\d+$/.test(t)) return Number(t);
  const m = /^([a-g])(#|b)?(-?\d+)$/i.exec(t);
  if (!m) return NaN;
  const base = { c: 0, d: 2, e: 4, f: 5, g: 7, a: 9, b: 11 }[m[1].toLowerCase()];
  return (Number(m[3]) + octaveBase) * 12 + base + (m[2] === '#' ? 1 : m[2] ? -1 : 0);
}

export const TYPE_LABEL = {
  none: 'Nothing', cc7: 'CC', cc14: 'CC 14-bit', pitchbend: 'Pitch Bend', note: 'Note',
  program: 'Program Change', aftertouch: 'Aftertouch', relative2c: 'Relative CC (1 up, 127 down)',
  relativeBinOffset: 'Relative CC (65 up, 63 down)', relativeSignedBit: 'Relative CC (1 up, 65 down)',
  nrpn: 'NRPN', rpn: 'RPN',
};

export const TYPE_SHORT = {
  none: '—', cc7: 'CC', cc14: 'CC14', pitchbend: 'PB', note: 'Note', program: 'PC',
  aftertouch: 'AT', relative2c: 'Rel', relativeBinOffset: 'Rel', relativeSignedBit: 'Rel',
  nrpn: 'NRPN', rpn: 'RPN',
};

export function resolveChannel(layer, presetChannel) {
  const c = layer.channel ?? 'preset';
  return c === 'preset' ? presetChannel : c;
}

// "CC 74", "NRPN 3:10", "Note C4"…; channel shown only when fixed.
export function shortLabel(S, layer) {
  const t = layer.type ?? 'none';
  if (t === 'none') return '—';
  let s = TYPE_SHORT[t];
  if (S.hasNumber(t)) {
    const n = layer.number ?? 0;
    if (t === 'nrpn' || t === 'rpn') s += ` ${n >> 7}:${n & 127}`;
    else if (t === 'note') s += ` ${noteName(n)}`;
    else s += ` ${n}`;
  }
  if (layer.channel !== undefined && layer.channel !== 'preset') s += ` ·${layer.channel}`;
  return s;
}

// A sentence and byte lines for the inspector's "Sends" box.
export function describe(S, kind, layer, presetChannel, noteOff) {
  const t = layer.type ?? 'none';
  if (t === 'none') return { text: 'Sends nothing.', bytes: [] };
  const ch = resolveChannel(layer, presetChannel);
  const chTxt = (layer.channel ?? 'preset') === 'preset' ? `channel ${ch} (the preset's)` : `channel ${ch} (fixed)`;
  const n = layer.number ?? 0;
  const lo = S.value(layer, 'lo', kind), hi = S.value(layer, 'hi', kind);
  const range = lo !== undefined ? (lo > hi ? `${lo}…${hi} (inverted)` : `${lo}…${hi}`) : '';
  const c = ch - 1;
  const cc = 0xB0 + c;
  switch (t) {
    case 'cc7': return { text: `CC ${n} on ${chTxt}, values ${range}.`, bytes: [`${hex(cc)} ${hex(n)} vv`] };
    case 'cc14': return { text: `CC ${n} (MSB) and CC ${n + 32} (LSB) on ${chTxt}, values ${range}.`, bytes: [`${hex(cc)} ${hex(n)} msb`, `${hex(cc)} ${hex(n + 32)} lsb`] };
    case 'nrpn':
    case 'rpn': {
      const [a, b] = t === 'nrpn' ? [0x63, 0x62] : [0x65, 0x64];
      return {
        text: `${t.toUpperCase()} ${n} (MSB ${n >> 7}, LSB ${n & 127}) on ${chTxt}, values ${range}. The parameter number goes first at the start of each move and each press, and whenever the channel's selection is another parameter; then each value is the two data bytes.`,
        bytes: [`${hex(cc)} ${hex(a)} ${hex(n >> 7)}`, `${hex(cc)} ${hex(b)} ${hex(n & 127)}`, `${hex(cc)} 06 msb`, `${hex(cc)} 26 lsb`],
      };
    }
    case 'pitchbend': return { text: `Pitch Bend on ${chTxt}, values ${range}.`, bytes: [`${hex(0xE0 + c)} lsb msb`] };
    case 'note': {
      const off = noteOff === '8n' ? `${hex(0x80 + c)} ${hex(n)} 00` : `${hex(0x90 + c)} ${hex(n)} 00`;
      return { text: `Note ${n} (${noteName(n)}) on ${chTxt}, velocity ${range}; velocity 0 releases the note as ${noteOff === '8n' ? 'Note Off' : 'Note On, velocity 0'} (the preset's form).`, bytes: [`${hex(0x90 + c)} ${hex(n)} vel`, off] };
    }
    case 'program': {
      // Programs are shown 1–128, as instruments number them; the message carries 0–127.
      if (kind === 'button') {
        // The core sends hi at every press, in every mode, and nothing at a release.
        return { text: `Program ${hi + 1} on ${chTxt} at every press; nothing on release.`, bytes: [`${hex(0xC0 + c)} ${hex(hi)}`] };
      }
      const pr = lo > hi ? `${lo + 1}…${hi + 1} (inverted)` : `${lo + 1}…${hi + 1}`;
      return { text: `Program Change on ${chTxt}, programs ${pr}.`, bytes: [`${hex(0xC0 + c)} vv`] };
    }
    case 'aftertouch': return { text: `Channel Aftertouch on ${chTxt}, values ${range}.`, bytes: [`${hex(0xD0 + c)} vv`] };
    default: {
      const enc = { relative2c: '01 / 7F', relativeBinOffset: '41 / 3F', relativeSignedBit: '01 / 41' }[t];
      return { text: `CC ${n} relative on ${chTxt}: a step up / down per click (${enc} for ±1).`, bytes: [`${hex(cc)} ${hex(n)} step`] };
    }
  }
}

// The NRPN/RPN parameter each channel's output last selected, as the core keeps it (docs/core.md,
// "The channel's selection"; SPEC Q-21 b-1, c-3). A value carries the parameter number only when
// the channel's selection is another parameter; the first value of a fader's or an encoder's move
// and every button press select it again (reselect); a plain CC 98-101 leaves it unknown.
export class ParamSelection {
  constructor() { this.out = new Array(16).fill(null); }
  reselect(layer, presetChannel) {
    if (isParam(layer)) this.out[resolveChannel(layer, presetChannel) - 1] = null;
  }
  // The null parameter (RPN 127/127) at the end of a move or press that sent NRPN or RPN, when the
  // preset's paramNull is "gesture": once per channel.
  nullMessages(channel) {
    const c = channel - 1;
    if (this.out[c] === 'rpn:16383') return []; // the null parameter is RPN 127/127
    this.out[c] = 'rpn:16383';
    return [[0xB0 + c, 101, 127], [0xB0 + c, 100, 127]];
  }
}

export const isParam = (layer) => layer.type === 'nrpn' || layer.type === 'rpn';

// The messages for one value of a layer. sel: the channels' selection (ParamSelection); without
// one the parameter number always goes first, as at the start of a move.
export function messages(S, kind, layer, presetChannel, value, noteOff, sel) {
  const t = layer.type ?? 'none';
  const c = resolveChannel(layer, presetChannel) - 1;
  const n = layer.number ?? 0;
  const out = [];
  const add = (...b) => out.push(b);
  switch (t) {
    case 'cc7':
      add(0xB0 + c, n, value & 127);
      if (sel && n >= 98 && n <= 101) sel.out[c] = null;
      break;
    case 'cc14': add(0xB0 + c, n, value >> 7); add(0xB0 + c, n + 32, value & 127); break;
    case 'nrpn': case 'rpn': {
      const [a, b] = t === 'nrpn' ? [0x63, 0x62] : [0x65, 0x64];
      const p = `${t}:${n}`;
      if (!sel || sel.out[c] !== p) { add(0xB0 + c, a, n >> 7); add(0xB0 + c, b, n & 127); }
      if (sel) sel.out[c] = p;
      add(0xB0 + c, 6, value >> 7); add(0xB0 + c, 38, value & 127); break;
    }
    case 'pitchbend': add(0xE0 + c, value & 127, value >> 7); break;
    case 'note':
      // velocity 0 releases the note in the preset's form; any other is a Note On with it
      if (value === 0) add(noteOff === '8n' ? 0x80 + c : 0x90 + c, n, 0);
      else add(0x90 + c, n, value & 127);
      break;
    case 'program': add(0xC0 + c, value & 127); break;
    case 'aftertouch': add(0xD0 + c, value & 127); break;
    case 'relative2c': add(0xB0 + c, n, value >= 0 ? value : 128 + value); break;
    case 'relativeBinOffset': add(0xB0 + c, n, 64 + value); break;
    case 'relativeSignedBit': add(0xB0 + c, n, value >= 0 ? value : 64 - value); break;
    default: break;
  }
  return out;
}

// The value a button sends at a press (down) or a release, or null for none (docs/core.md,
// "Buttons"). on: the target has a value and it is not lo, as the LED shows it.
export function buttonValue(S, layer, down, current) {
  const t = layer.type ?? 'none';
  if (t === 'none') return null;
  const mode = S.value(layer, 'mode', 'button');
  const lo = S.value(layer, 'lo', 'button') ?? 0, hi = S.value(layer, 'hi', 'button') ?? 0;
  if (down) {
    if (t === 'program' || mode !== 'toggle') return hi;
    return current !== undefined && current !== lo ? lo : hi;
  }
  if (mode === 'momentary' && t !== 'program' && !layer.noRelease) return lo;
  return null;
}

// A fader's value at a position 0-16383 (the scan's 14 bits, docs/core.md, "Faders"): lo…hi
// linearly, rounded half away from zero, as the core's map_pos.
export const FADER_MAX = 16383;
export function faderValue(S, layer, pos) {
  const lo = S.value(layer, 'lo', 'fader'), hi = S.value(layer, 'hi', 'fader');
  const num = (hi - lo) * pos, half = Math.floor(FADER_MAX / 2);
  return lo + (num >= 0 ? Math.floor((num + half) / FADER_MAX) : -Math.floor((-num + half) / FADER_MAX));
}

// An encoder's change for the first event of a move (no acceleration yet) of `clicks` clicks, in
// values (docs/core.md, "Encoders"): div N one step per N clicks, step N N a click, the others
// the fine step (1; on a 14-bit target the range / 2048 unless fine14 is "lsb"). A relative
// encoder sends this change, at most ±63.
export function encoderDelta(S, layer, clicks, fine14) {
  const t = layer.type ?? 'none';
  const mode = S.value(layer, 'mode', 'encoder');
  if (mode === 'div') return Math.trunc(clicks / (layer.n ?? 2));
  if (mode === 'step') return clicks * (layer.n ?? 1);
  let fine = 1;
  if (!t.startsWith('relative') && S.is14(t) && fine14 !== 'lsb') {
    const span = Math.abs(S.value(layer, 'hi', 'encoder') - S.value(layer, 'lo', 'encoder'));
    if (Math.floor(span / 2048) > 1) fine = Math.floor(span / 2048);
  }
  return clicks * fine;
}

// An absolute encoder's next value from its target's (current; undefined: none yet, the layer's
// start) after a change: clockwise towards hi, clamped to the range.
export function encoderValue(S, layer, current, delta) {
  const lo = S.value(layer, 'lo', 'encoder'), hi = S.value(layer, 'hi', 'encoder');
  const min = Math.min(lo, hi), max = Math.max(lo, hi);
  const clamp = (v) => Math.max(min, Math.min(max, v));
  const cur = current ?? clamp(S.value(layer, 'start', 'encoder'));
  return clamp(cur + (hi >= lo ? delta : -delta));
}

// The target a layer's value lives in (D-5): controls with the same key share one value, as the
// core's target store keys them (core_target_key). Null for none and relative encoders.
export function targetKey(S, layer, presetChannel) {
  const t = layer.type ?? 'none';
  if (t === 'none' || t.startsWith('relative')) return null;
  return `${t}:${resolveChannel(layer, presetChannel)}:${S.hasNumber(t) ? layer.number ?? 0 : ''}`;
}

export function decode(bytes) {
  const [st, d1, d2] = bytes;
  const ch = (st & 15) + 1;
  switch (st & 0xF0) {
    case 0x80: return `Note Off ${d1} (${noteName(d1)}) ch ${ch}`;
    case 0x90: return d2 ? `Note On ${d1} (${noteName(d1)}) vel ${d2} ch ${ch}` : `Note On ${d1} (${noteName(d1)}) vel 0 ch ${ch}`;
    case 0xB0: return `CC ${d1} = ${d2} ch ${ch}` + ({ 99: ' (NRPN MSB)', 98: ' (NRPN LSB)', 101: ' (RPN MSB)', 100: ' (RPN LSB)', 6: ' (data MSB)', 38: ' (data LSB)' }[d1] ?? '');
    case 0xC0: return `Program ${d1 + 1} ch ${ch}`;
    case 0xD0: return `Aftertouch ${d1} ch ${ch}`;
    case 0xE0: return `Pitch Bend ${d1 | (d2 << 7)} ch ${ch}`;
    default: return '';
  }
}

export const hexBytes = (b) => b.map(hex).join(' ');
