import assert from 'node:assert/strict';
import { createHash, webcrypto } from 'node:crypto';
import { test } from 'node:test';
import * as release from '../../app/editor/js/release.js';

if (!globalThis.crypto) globalThis.crypto = webcrypto;

const INSTALL = { app: 'firmware/controller-ble.bin', cfg: 'firmware/cfg_tool.bin' };
const APP = new Uint8Array([1, 2, 3, 4]), CFG = new Uint8Array([5, 6]);
const entry = (bytes) => ({ size: bytes.length, sha256: createHash('sha256').update(bytes).digest('hex') });
const manifest = () => ({ product: 'better-smc-mixer', version: '1.2.3', install: { ...INSTALL },
  files: { [INSTALL.app]: entry(APP), [INSTALL.cfg]: entry(CFG) } });
const BASE = 'https://project.example/app/editor/index.html';
const PACKAGED = 'https://project.example/app/manifest.json';
const DRY = 'https://project.example/build/release/manifest.json';
const jsonResponse = (m) => new Response(JSON.stringify(m));

test('the editor offers no local-package firmware route (owner decision D, 2026-11-21)', () => {
  assert.equal(release.readLocalRelease, undefined);
});

test('startup discovery applies the strict manifest checks: invalid installs nothing', async () => {
  const bad = { ...manifest(), install: { app: 'https://evil.example/x.bin', cfg: INSTALL.cfg } };
  const rel = await release.findRelease(BASE, async () => jsonResponse(bad));
  assert.equal(rel.install, null);
  assert.equal(rel.invalid, true);
  const good = await release.findRelease(BASE, async () => jsonResponse(manifest()));
  assert.deepEqual(good.install, INSTALL);
});

test('releaseFiles reads only the fixed same-site paths and verifies size and hash', async () => {
  const rel = { url: PACKAGED, manifest: manifest(), install: { ...INSTALL } };
  const seen = [];
  const files = await release.releaseFiles(rel, async (url) => { seen.push(url.href); return new Response(url.href.endsWith('cfg_tool.bin') ? CFG : APP); });
  assert.deepEqual(seen, ['https://project.example/app/firmware/controller-ble.bin', 'https://project.example/app/firmware/cfg_tool.bin']);
  assert.deepEqual([...files.app], [...APP]);
  await assert.rejects(release.releaseFiles({ ...rel, install: { app: '//evil.example/a.bin', cfg: INSTALL.cfg } }, assert.fail), /package paths/);
  await assert.rejects(release.releaseFiles(rel, async () => new Response(new Uint8Array([9, 9, 9, 9]))), /does not match/);
});

test('refresh requests a fresh manifest, falling back only when the first candidate is absent', async () => {
  const requested = [];
  const m = manifest();
  const result = await release.refreshRelease(BASE, async (url, options) => {
    requested.push([url.href, options.cache, options.redirect]);
    return url.href === PACKAGED ? new Response(null, { status: 404 }) : jsonResponse(m);
  });
  assert.deepEqual(requested, [[PACKAGED, 'no-store', 'error'], [DRY, 'no-store', 'error']]);
  assert.deepEqual(result, { status: 'found', release: { url: DRY, manifest: m, version: '1.2.3', install: INSTALL } });
});

test('check reports missing only if every candidate was absent', async () => {
  assert.deepEqual(await release.refreshRelease(BASE, async (url) => (
    new Response(null, { status: url.href === PACKAGED ? 404 : 410 })
  )), { status: 'missing' });
});

test('check reports the HTTP error from a fallback instead of missing', async () => {
  assert.deepEqual(await release.refreshRelease(BASE, async (url) => (
    new Response(null, { status: url.href === PACKAGED ? 404 : 503 })
  )), { status: 'http', url: DRY, httpStatus: 503 });
});

test('check distinguishes a request failure from malformed manifest JSON', async () => {
  const error = new TypeError('offline');
  const fail = async (url) => { assert.equal(url.href, PACKAGED); throw error; };
  assert.deepEqual(await release.refreshRelease(BASE, fail), { status: 'network', url: PACKAGED, error });
  const invalid = await release.refreshRelease(BASE, async (url) => {
    assert.equal(url.href, PACKAGED);
    return new Response('<html>not a manifest</html>');
  });
  assert.equal(invalid.status, 'invalid');
  assert.equal(invalid.url, PACKAGED);
  assert.ok(invalid.error instanceof SyntaxError);
});

test('check reports network failure when the body is interrupted', async () => {
  const response = new Response(new ReadableStream({ start(controller) { controller.error(new TypeError('interrupted')); } }));
  assert.equal((await release.refreshRelease(BASE, async () => response)).status, 'network');
});

test('check rejects wrong products, malformed versions and undeclared install payloads', async () => {
  for (const invalid of [null, {}, { ...manifest(), product: 'different' }, { ...manifest(), version: 'latest' },
    { ...manifest(), version: 2 }, { ...manifest(), version: '1.2.3-01' },
    { ...manifest(), install: undefined }, { ...manifest(), files: {} }]) {
    const result = await release.refreshRelease(BASE, async (url) => {
      assert.equal(url.href, PACKAGED);
      return jsonResponse(invalid);
    });
    assert.equal(result.status, 'invalid');
  }
});

test('each check uses the latest manifest and leaves payload loading to releaseFiles', async () => {
  let m = manifest();
  const get = async (url, options) => {
    assert.equal(url.href, PACKAGED);
    assert.equal(options.cache, 'no-store');
    return jsonResponse(m);
  };
  assert.equal((await release.refreshRelease(BASE, get)).release.version, '1.2.3');
  m = { ...m, version: '1.2.4-rc.1+build.01' };
  const { release: rel } = await release.refreshRelease(BASE, get);
  assert.equal(rel.version, m.version);
  const blobs = new Map([[new URL(INSTALL.app, PACKAGED).href, APP], [new URL(INSTALL.cfg, PACKAGED).href, CFG]]);
  assert.deepEqual(await release.releaseFiles(rel, async (url) => {
    assert.ok(blobs.has(url.href));
    return new Response(blobs.get(url.href));
  }), { app: APP, cfg: CFG });
});

test('check reports a legitimate package with no installable payload as found', async () => {
  const m = { ...manifest(), version: null, install: null };
  assert.deepEqual(await release.refreshRelease(BASE, async () => jsonResponse(m)),
    { status: 'found', release: { url: PACKAGED, manifest: m, version: null, install: null } });
});

test('startup discovery remains best-effort: a failed candidate falls through to the next', async () => {
  const m = { product: 'better-smc-mixer', version: null, install: null };
  const startup = await release.findRelease(BASE, async (url, options) => {
    assert.equal(options, undefined);
    return url.href === PACKAGED ? new Response(null, { status: 503 }) : jsonResponse(m);
  });
  assert.deepEqual(startup, { url: DRY, manifest: m, version: null, install: null });
});
