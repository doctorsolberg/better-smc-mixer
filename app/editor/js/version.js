// The product's version: the editor, the firmware and the release package carry the same one (the
// owner, 2026-11-14; docs/build.md, "The version"). The source is VERSION at the repository's root;
// tools/release/check_versions.py fails when this copy differs.
export const VERSION = '0.1.2';

// Semantic versions compared: < 0, 0 or > 0, as a - b (a pre-release such as 0.2.0-rc.1 sorts before 0.2.0).
export function compareVersions(a, b) {
  const parse = (v) => { const [core, pre = ''] = String(v).split('-', 2); return [core.split('.').map((n) => Number(n) || 0), pre]; };
  const [x, xp] = parse(a); const [y, yp] = parse(b);
  for (let i = 0; i < 3; i++) if ((x[i] ?? 0) !== (y[i] ?? 0)) return (x[i] ?? 0) - (y[i] ?? 0);
  if (xp === yp) return 0;
  if (!xp) return 1;
  if (!yp) return -1;
  return xp < yp ? -1 : 1;
}
