// The release the editor installs (docs/app.md, "Firmware"): the package's manifest.json
// (tools/release/release.py) and the two files its "install" names, the controller with BLE-MIDI
// and the SDK's cfg_tool.bin. The page looks beside itself first (../manifest.json: the package's
// editor/), then at the dry package `make release` leaves in build/release/ (the editor, served
// from the repository's root by serve.py). Every file is checked against the manifest's size and
// SHA-256 before it is handed to the installer.

export const PLACES = ['../manifest.json', '../../build/release/manifest.json'];

export async function sha256hex(bytes) {
  const d = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
  return [...d].map((b) => b.toString(16).padStart(2, '0')).join('');
}

// { url, manifest, version, install } of the first manifest found, or null. version: the
// manifest's (null in a dry package: the caller falls back to the editor's own, one package
// carrying both); install: { app, cfg } paths, or null when the package installs nothing.
export async function findRelease(base, get = fetch) {
  for (const p of PLACES) {
    const url = new URL(p, base);
    let m;
    try {
      const r = await get(url);
      if (!r.ok) continue;
      m = await r.json();
    } catch {
      continue;
    }
    if (!m || m.product !== 'better-smc-mixer') continue;
    // The same strict checks as refreshRelease(): an invalid manifest installs nothing.
    try {
      const { version, install } = validateManifest(m);
      return { url: url.href, manifest: m, version, install };
    } catch {
      return { url: url.href, manifest: m, version: null, install: null, invalid: true };
    }
  }
  return null;
}

// The install pair emitted by tools/release/release.py, not paths supplied by the caller.
const INSTALL = Object.freeze({ app: 'firmware/controller-ble.bin', cfg: 'firmware/cfg_tool.bin' });
const INSTALL_PATHS = Object.values(INSTALL);
const MANIFEST_LIMIT = 256 * 1024;
const IMAGE_LIMIT = 512 * 1024; // Read limit: a single file cannot exceed the whole mixer flash.

// A deliberate check, unlike startup's best-effort findRelease(). The second location is only
// a fallback for an absent first manifest; it must not hide a failed or invalid first response.
// Returns { status: 'found', release }, { status: 'missing' },
// { status: 'http', url, httpStatus }, or { status: 'network' | 'invalid', url, error }.
export async function refreshRelease(base, get = fetch) {
  for (const p of PLACES) {
    const url = new URL(p, base);
    let response;
    try {
      response = await get(url, { cache: 'no-store', redirect: 'error' });
    } catch (error) {
      return { status: 'network', url: url.href, error };
    }
    if (response.status === 404 || response.status === 410) continue;
    if (!response.ok) return { status: 'http', url: url.href, httpStatus: response.status };
    let manifest;
    try {
      manifest = await response.json();
    } catch (error) {
      return { status: error instanceof SyntaxError ? 'invalid' : 'network', url: url.href, error };
    }
    try {
      const { version, install } = validateManifest(manifest);
      return { status: 'found', release: { url: url.href, manifest, version, install } };
    } catch (error) {
      return { status: 'invalid', url: url.href, error };
    }
  }
  return { status: 'missing' };
}

function validVersion(v) {
  if (v === null) return true; // Legacy dry-manifest representation.
  if (typeof v !== 'string' || v.length > 128 || v.trim() !== v) return false;
  const m = /^(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)\.(?:0|[1-9]\d*)(?:-([\da-z-]+(?:\.[\da-z-]+)*))?(?:\+[\da-z-]+(?:\.[\da-z-]+)*)?$/i.exec(v);
  return !!m && !m[1]?.split('.').some((id) => /^0\d+$/.test(id));
}

function validateManifest(m) {
  if (m?.product !== 'better-smc-mixer') throw new Error('not a better-smc-mixer release manifest');
  const { version } = m;
  if (!validVersion(version)) throw new Error('the release manifest has an invalid version');
  if (m.install === null) return { version, install: null };
  if (!m.install || Object.keys(m.install).length !== INSTALL_PATHS.length ||
      m.install.app !== INSTALL.app || m.install.cfg !== INSTALL.cfg) {
    throw new Error('the release manifest must name the controller and settings at their package paths');
  }
  for (const path of INSTALL_PATHS) {
    const want = m.files?.[path];
    if (!Number.isSafeInteger(want?.size) || want.size <= 0 || want.size > IMAGE_LIMIT ||
        typeof want.sha256 !== 'string' || !/^[a-f0-9]{64}$/.test(want.sha256)) {
      throw new Error(`${path} has an invalid size or SHA-256 in the release manifest`);
    }
  }
  return { version, install: INSTALL };
}

async function verifyFile(path, bytes, want) {
  if (bytes.length !== want.size || (await sha256hex(bytes)) !== want.sha256) {
    throw new Error(`${path} does not match the release's manifest`);
  }
}

// The files the release installs: { app, cfg } as bytes, each checked against the manifest.
export async function releaseFiles(rel, get = fetch) {
  if (!rel?.install) throw new Error('this release has no firmware to install');
  const out = {};
  if (rel.install.app !== INSTALL.app || rel.install.cfg !== INSTALL.cfg || Object.keys(rel.install).length !== INSTALL_PATHS.length) {
    throw new Error('the release names firmware outside its package paths');
  }
  for (const [k, path] of Object.entries(INSTALL)) {
    const want = rel.manifest.files?.[path];
    if (!Number.isSafeInteger(want?.size) || want.size <= 0 || want.size > IMAGE_LIMIT) throw new Error(`${path} is not in the release's manifest`);
    const url = new URL(path, rel.url);
    if (url.origin !== new URL(rel.url).origin) throw new Error(`${path} is outside the release's site`);
    const r = await get(url);
    if (!r.ok) throw new Error(`${path} could not be read (${r.status})`);
    const length = Number(r.headers?.get?.('content-length'));
    if (length > IMAGE_LIMIT) throw new Error(`${path} is larger than the mixer's flash`);
    const b = new Uint8Array(await r.arrayBuffer());
    if (b.length > IMAGE_LIMIT) throw new Error(`${path} is larger than the mixer's flash`);
    await verifyFile(path, b, want);
    out[k] = b;
  }
  return out;
}
