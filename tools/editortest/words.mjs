// Narrow wording checks against configured calculations and the core's recorded program probes.
// This is not the complete planned behavior-ID scheme of docs/integration.md, T-1 test 4.
import { Config } from '../../app/editor/js/config.js';
import { describe, encoderDelta } from '../../app/editor/js/midi.js';
import * as labels from '../../app/editor/js/labels.js';

export function criticalWords(S, cases) {
  const problems = [];
  const fine = labels.GLOBAL_FORM.find((item) => Array.isArray(item) && item[0] === 'fine14');
  // The LED light (SPEC §5.7): two ten-step sliders whose ranges are the schema's; unlit 0 is Off.
  for (const key of ['ledLevel', 'ledUnlit']) {
    const item = labels.GLOBAL_FORM.find((x) => Array.isArray(x) && x[0] === key);
    const f = S.globalFields.find((x) => x.json === key);
    if (!item || !f) { problems.push(`${key}: no setting in the form or the schema`); continue; }
    const [min, max, kind] = item[4] ?? [];
    if (min !== f.min || max !== f.max || kind !== 'slider') problems.push(`${key}: the form's ${min}-${max} ${kind}, the schema's ${f.min}-${f.max} as a slider`);
  }
  const unlit = labels.GLOBAL_FORM.find((x) => Array.isArray(x) && x[0] === 'ledUnlit');
  if (!/encoder/i.test(unlit?.[2] ?? '')) problems.push('ledUnlit help does not say the encoders\' LEDs never glow');
  if (!/bottom row/i.test(unlit?.[2] ?? '')) problems.push('ledUnlit help does not mention the bottom row\'s own glow');
  const step = encoderDelta(S, { type: 'nrpn', number: 0, lo: 0, hi: 16383, mode: 'fine' }, 1, 'range');
  if (!fine?.[3]?.range?.includes(`${step} on 0–16383`)) problems.push(`fine-step label does not state the actual ${step} on 0–16383`);
  if (!/rounded down/.test(fine?.[2] ?? '') || !/at least 1|minimum 1/.test(fine?.[2] ?? '')) problems.push('fine-step help does not state rounding down and minimum step 1');

  // Existing pickup/core scenarios include the first move with no known target value.
  for (const mode of ['pickup', 'scale']) {
    const text = labels.MODE_LABEL.fader[mode][1];
    if (!/known to the mixer/.test(text) || !/first move sends/.test(text)) problems.push(`${mode} help omits the unknown-target first move`);
  }

  const help = labels.PROGRAM_BUTTON_HELP;
  if (!help || !/every press/i.test(help.mode) || !/every mode/i.test(help.mode) || !/release sends nothing/i.test(help.mode)) {
    problems.push('Program Change has no mode help stating every press, every mode and no release');
  }
  if (!help || /release/i.test(help.valuesLabel) || !/LED/.test(help.valuesLabel) || !/only/i.test(help.values)) {
    problems.push('Program Change value labels do not distinguish the LED-off value from the sent program');
  }
  const modes = new Set();
  let programs = 0;
  for (const { text, vector } of cases) {
    const cfg = new Config(S, JSON.parse(text));
    for (const probe of vector.probes) {
      const kind = cfg.kind(probe.id);
      if (kind !== 'button') continue;
      const ctl = cfg.control(probe.p, probe.g, probe.id);
      const layer = probe.shift ? cfg.shiftLayer(ctl, kind) : cfg.normal(ctl);
      if (layer?.type !== 'program') continue;
      programs++;
      modes.add(S.value(layer, 'mode', kind));
      const d = describe(S, kind, layer, cfg.presetChannel(probe.p), cfg.presetSetting(probe.p, 'noteOff'));
      const press = probe.midi.press ?? [], release = probe.midi.release ?? [];
      const hi = S.value(layer, 'hi', kind);
      if (press.length !== 1 || press[0] !== d.bytes[0] || release.length ||
          !d.text.includes(`Program ${hi + 1} `) || !/at every press; nothing on release/.test(d.text)) {
        problems.push(`program wording differs from the core: ${vector.about.config}, ${probe.id}, preset ${probe.p + 1}, page ${probe.g + 1}, shift ${probe.shift}`);
      }
    }
  }
  if (!programs) problems.push('no core program probes checked');
  for (const mode of S.modeEnum('button').values) {
    if (!modes.has(mode.json)) problems.push(`no program probe in ${mode.json} mode`);
  }
  return problems;
}
