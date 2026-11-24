// Built-in examples for the library. Two kinds (docs/app.md, "Templates"):
// - a parameter list: an instrument's parameters with their messages, to assign onto controls;
// - a layout: an ordinary configuration file holding one preset, applied to a preset slot.
// The parameter-list format is the editor's own, not part of config/schema.json.

export const PARAMETER_LISTS = [
  {
    kind: 'parameters', name: 'GM2 sound controllers', about: 'The MIDI standard\'s sound controllers and effects sends; many synths and iOS apps answer them.',
    params: [
      { name: 'Cutoff', type: 'cc7', number: 74 }, { name: 'Resonance', type: 'cc7', number: 71 },
      { name: 'Attack', type: 'cc7', number: 73 }, { name: 'Decay', type: 'cc7', number: 75 },
      { name: 'Release', type: 'cc7', number: 72 }, { name: 'Vibrato rate', type: 'cc7', number: 76 },
      { name: 'Vibrato depth', type: 'cc7', number: 77 }, { name: 'Vibrato delay', type: 'cc7', number: 78 },
      { name: 'Volume', type: 'cc7', number: 7 }, { name: 'Pan', type: 'cc7', number: 10 },
      { name: 'Expression', type: 'cc7', number: 11 }, { name: 'Mod wheel', type: 'cc7', number: 1 },
      { name: 'Portamento time', type: 'cc7', number: 5 }, { name: 'Reverb send', type: 'cc7', number: 91 },
      { name: 'Chorus send', type: 'cc7', number: 93 }, { name: 'Pitch bend', type: 'pitchbend' },
      { name: 'Sustain', type: 'cc7', number: 64, button: true }, { name: 'Portamento on', type: 'cc7', number: 65, button: true },
    ],
  },
  {
    kind: 'parameters', name: 'Example NRPN synth', about: 'A made-up instrument with 14-bit NRPN parameters, to show how such a list reads. Not a real device.',
    params: [
      { name: 'Osc 1 pitch', type: 'nrpn', number: 0 * 128 + 1 }, { name: 'Osc 1 shape', type: 'nrpn', number: 0 * 128 + 2 },
      { name: 'Osc 2 pitch', type: 'nrpn', number: 0 * 128 + 5 }, { name: 'Osc 2 detune', type: 'nrpn', number: 0 * 128 + 6 },
      { name: 'Osc mix', type: 'nrpn', number: 0 * 128 + 9 }, { name: 'Noise', type: 'nrpn', number: 0 * 128 + 10 },
      { name: 'Filter cutoff', type: 'nrpn', number: 1 * 128 + 0 }, { name: 'Filter reso', type: 'nrpn', number: 1 * 128 + 1 },
      { name: 'Filter env amt', type: 'nrpn', number: 1 * 128 + 2 }, { name: 'Filter keytrack', type: 'nrpn', number: 1 * 128 + 3 },
      { name: 'Env attack', type: 'nrpn', number: 2 * 128 + 0 }, { name: 'Env decay', type: 'nrpn', number: 2 * 128 + 1 },
      { name: 'Env sustain', type: 'nrpn', number: 2 * 128 + 2 }, { name: 'Env release', type: 'nrpn', number: 2 * 128 + 3 },
      { name: 'LFO rate', type: 'nrpn', number: 3 * 128 + 0 }, { name: 'LFO depth', type: 'nrpn', number: 3 * 128 + 1 },
      { name: 'LFO wave', type: 'nrpn', number: 3 * 128 + 2, hi: 4, mode: 'div', n: 6 },
    ],
  },
  {
    kind: 'parameters', name: 'Drum pads (GM notes)', about: 'General MIDI drum notes, for buttons.',
    params: [
      ['Kick', 36], ['Rim', 37], ['Snare', 38], ['Clap', 39], ['Snare 2', 40], ['Low tom', 41], ['Closed hat', 42],
      ['Mid tom', 45], ['Open hat', 46], ['High tom', 48], ['Crash', 49], ['Ride', 51], ['Tambourine', 54], ['Cowbell', 56],
    ].map(([name, number]) => ({ name, type: 'note', number, button: true })),
  },
];

const strips = (f) => Array.from({ length: 8 }, (_, i) => f(i));

export const LAYOUTS = [
  {
    kind: 'layout', name: 'Mixer, 8 channels', about: 'For a mixer app (AUM and the like): strip n on channel n, fader = volume, encoder = pan, M and S toggle mute and solo, R and □ momentary.',
    file: {
      schemaVersion: 1,
      presets: [{
        name: 'Mixer',
        pages: [{
          name: 'Channels 1–8',
          strips: strips((i) => ({
            fader: { name: `Ch ${i + 1} volume`, type: 'cc7', channel: i + 1, number: 7 },
            encoder: { name: `Ch ${i + 1} pan`, type: 'cc7', channel: i + 1, number: 10 },
            m: { name: 'Mute', type: 'cc7', channel: i + 1, number: 20, mode: 'toggle' },
            s: { name: 'Solo', type: 'cc7', channel: i + 1, number: 21, mode: 'toggle' },
            r: { name: 'Arm', type: 'cc7', channel: i + 1, number: 22, mode: 'toggle' },
            select: { name: 'Select', type: 'cc7', channel: i + 1, number: 23 },
          })),
        }],
      }],
    },
  },
  {
    kind: 'layout', name: 'One synth, 14-bit faders', about: 'A preset for one synth: page 1 faders on CC 14-bit (MSB 1, 2, 3, 4, 5, 8, 9, 12; LSB 32 above each), encoders on CC 70–77.',
    file: {
      schemaVersion: 1,
      presets: [{
        name: 'Synth (14-bit)',
        pages: [{
          name: 'Main',
          strips: strips((i) => ({
            fader: { type: 'cc14', number: [1, 2, 3, 4, 5, 8, 9, 12][i] },
            encoder: { type: 'cc7', number: 70 + i },
          })),
        }],
      }],
    },
  },
];

// ---- parameter lists from MIDI Guide (midi.guide; docs/app.md, "Parameter lists from midi.guide") ----
// MIDI Guide gives each instrument's CCs and NRPNs, grouped by section, with names, ranges and value
// meanings ("0: Off; 1: On"), and its drum notes ("triggers"). Its data is CC BY-SA 4.0: a list made
// from it says so and links back. This turns one device (in the shape of MIDI Guide's device API,
// including the equivalent records read from its CSV files) into our parameter list.
export function fromMidiGuide(dev) {
  const m = dev.metadata;
  const params = [];
  for (const [group, list] of Object.entries(dev.ccs ?? {})) {
    for (const p of list) {
      const prefix = `${group}: `;
      const out = { name: p.parameter_name.startsWith(prefix) ? p.parameter_name.slice(prefix.length) : p.parameter_name, group, about: p.parameter_description ?? undefined };
      const nrpn = p.nrpn_msb !== null && p.nrpn_msb !== undefined && p.nrpn_lsb !== null && p.nrpn_lsb !== undefined;
      const cc = p.cc_msb !== null && p.cc_msb !== undefined;
      // The finest form the mixer can send: NRPN when it has more than 128 steps, a CC 14-bit when
      // the LSB is CC n + 32, otherwise the CC. A 7-bit NRPN with no CC is not supported by the firmware: marked `needs` and hidden.
      if (nrpn && (p.nrpn_max_value ?? 127) > 127) Object.assign(out, { type: 'nrpn', number: p.nrpn_msb * 128 + p.nrpn_lsb, lo: p.nrpn_min_value ?? 0, hi: p.nrpn_max_value });
      else if (cc && p.cc_lsb === p.cc_msb + 32 && p.cc_msb < 32) {
        // A 7-bit range given for the pair means the whole 14-bit range.
        const fullResolution = (p.cc_max_value ?? 0) > 127;
        Object.assign(out, { type: 'cc14', number: p.cc_msb, lo: fullResolution ? p.cc_min_value ?? 0 : 0, hi: fullResolution ? p.cc_max_value : 16383 });
      }
      else if (cc) Object.assign(out, { type: 'cc7', number: p.cc_msb, lo: p.cc_min_value ?? 0, hi: p.cc_max_value ?? 127 });
      else if (nrpn) Object.assign(out, { type: 'nrpn', number: p.nrpn_msb * 128 + p.nrpn_lsb, lo: p.nrpn_min_value ?? 0, hi: p.nrpn_max_value ?? 127, needs: 'NRPN 7-bit' });
      else continue;
      const values = switchValues(p.usage, out.lo, out.hi);
      if (values.length >= 2) { out.values = values; if (values.length === 2) out.button = true; }
      if (p.orientation === '1-based') out.oneBased = true;
      params.push(out);
    }
  }
  for (const [group, list] of Object.entries(dev.triggers ?? {})) {
    for (const t of list) params.push({ name: t.sound_name, group: `Notes: ${group}`, type: 'note', number: t.note_number_default_value ?? t.note_number_min_value, button: true, channel: /^\d+$/.test(t.midi_channel) ? Number(t.midi_channel) : undefined });
  }
  return {
    kind: 'parameters', name: `${m.manufacturer} ${m.device}`, params,
    about: `${params.length} parameters from MIDI Guide (midi.guide), CC BY-SA 4.0.`,
    source: dev.source ? { ...dev.source, updated: m.last_updated } : { name: 'MIDI Guide', url: 'https://midi.guide', data: m.repo_url, updated: m.last_updated },
  };
}

// The first value of an equivalent range is enough to select it. A tilde range is continuous;
// do not infer a switch from the discrete fragments of a mixed usage string.
function switchValues(usage, lo, hi) {
  const options = (usage ?? '').split(';').map((s) => s.trim()).filter(Boolean);
  const ranges = options.map((s) => {
    const m = /^(\d+)(?:\s*-\s*(\d+))?\s*:\s*(.+)$/.exec(s);
    if (!m) return null;
    const start = Number(m[1]), end = Number(m[2] ?? m[1]);
    return start >= lo && end <= hi && start <= end ? { start, end, label: m[3] } : null;
  });
  if (ranges.some((r) => !r)) return [];
  ranges.sort((a, b) => a.start - b.start);
  if (ranges.some((r, i) => i > 0 && r.start <= ranges[i - 1].end)) return [];
  return ranges.map(({ start, label }) => [start, label]);
}
