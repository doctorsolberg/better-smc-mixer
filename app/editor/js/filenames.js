// Default names for configuration files the editor saves (docs/app.md, "Files and templates").
// Full flash backups keep their own "SMC-Mixer backup …" names: those are images of the mixer,
// not configuration files.

export const CONFIG_FILE_BASE = 'BSM-Config';

const clean = (s) => String(s ?? '').replace(/[^\w\- ]+/g, '').replace(/\s+/g, ' ').trim();

// The file name (without .json) offered for a configuration export. `what` is the scope label:
// 'Everything' (or nothing) for the whole configuration, a preset or page label otherwise.
export function configFileName(current, what) {
  const base = clean(current) || CONFIG_FILE_BASE;
  const scope = what && what !== 'Everything' ? clean(what) : '';
  return scope ? `${base} - ${scope}` : base;
}
