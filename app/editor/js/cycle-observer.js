// Versioned, read-only firmware-cycle diagnostics. Never publishes the device, configuration,
// flash bytes, MIDI traffic or an action API. Poll snapshots; no page-owned listener can throw
// into a firmware operation. This is evidence, not a security boundary against page scripts.
import * as IP from './installplan.js';
import { sha256hex } from './release.js';
const clone = (value) => JSON.parse(JSON.stringify(value));
const FIELDS = ['stage', 'done', 'total', 'size', 'sha256', 'name', 'message', 'status', 'pointer',
  'tries', 'sector0Ms', 'jobId', 'valid', 'source', 'url', 'manifest', 'manifestSha256', 'files', 'plan'];
export function deviceSnapshot(dev, editorText) {
  const ours = dev.connected && dev.fw?.kind === 'ours';
  const panel = ours ? dev.panelState : null;
  return {
    identity: { real: !!dev.real, connected: !!dev.connected, kind: dev.fw?.kind ?? null,
      transport: dev.transport ?? null, version: dev.fw?.version ?? null,
      build: ours ? dev.info?.buildId ?? null : null, protocol: ours ? dev.cfgInfo?.version ?? dev.info?.protocol ?? null : null,
      binaryForm: ours ? dev.cfgInfo?.binaryVersion ?? null : null, schema: ours ? dev.cfgInfo?.schemaVersion ?? null : null,
      storage: ours ? dev.storage : null },
    config: { clean: !!ours && dev.storage === 'clean' && !dev.sending && !dev.pushing && !dev.pending,
      loaded: !!ours && dev.running !== null, editorMatchesRunning: !!ours && dev.running !== null && dev.running === editorText,
      savedMatchesRunning: !!ours && dev.saved !== null && dev.saved === dev.running },
    // Actual protocol state, not the optimistic editor tabs or constructor defaults.
    panel: { preset: panel?.preset ?? null, page: panel?.page ?? null, shift: panel?.shift ?? null,
      bluetooth: panel?.bluetooth ?? null },
    // Separate from panel values: compare physical values, gate freshness with this marker.
    // readSequence advances only after a successful explicit CFG_STATE on this connection.
    panelObservation: { fresh: !!ours && dev.panelObservation?.fresh === true && dev.panelObservation.connectionEpoch === dev.connectionEpoch,
      readSequence: dev.panelObservation?.readSequence ?? 0, connectionEpoch: dev.connectionEpoch ?? null },
  };
}

export function createCycleObserver({ target = globalThis, limit = 128, readState = () => ({}) } = {}) {
  const pageId = globalThis.crypto.randomUUID();
  let sequence = 0;
  const events = [];
  const latest = {};
  const record = (type, detail = {}) => {
    try {
      const safe = Object.fromEntries(FIELDS.filter((k) => detail[k] !== undefined).map((k) => [k, detail[k]]));
      const event = clone({ sequence: ++sequence, type, ...safe });
      events.push(event);
      if (events.length > limit) events.shift();
      latest[type] = event;
    } catch { /* diagnostics cannot stop the owner operation */ }
  };
  const snapshot = () => {
    try { return clone({ version: 1, pageId, sequence, ...readState(), latest, events }); }
    catch { return { version: 1, sequence, initialized: false, observerError: true, events: [] }; }
  };
  try { Object.defineProperty(target, '__smcCycleObserver', { value: Object.freeze({ snapshot }), configurable: false, writable: false }); }
  catch { /* an untrusted/pre-existing property must not prevent editor initialization */ }
  return Object.freeze({ record });
}

export function observeDevice(dev, observer) {
  dev.addEventListener('source-error', (event) => {
    try { observer.record('source_error', { source: event.detail?.source, name: event.detail?.name, message: event.detail?.message }); }
    catch { /* observer only */ }
  });
  for (const type of ['connection', 'storage', 'state', 'changed', 'error']) {
    dev.addEventListener(type, (event) => {
      try { observer.record(type, type === 'error' ? { source: 'device', message: event.detail?.text } : {}); }
      catch { /* diagnostic listener never disrupts the device */ }
    });
  }
}

export async function releaseSnapshot(release, manifestSha256 = null) {
  if (!release) return null;
  const semanticManifestSha256 = await sha256hex(new TextEncoder().encode(JSON.stringify(release.manifest)));
  return { url: release.url, manifest: clone(release.manifest),
    manifestSha256, semanticManifestSha256,
    files: Object.fromEntries(Object.entries(release.install || {}).map(([kind, path]) => [kind,
      { path, url: new URL(path, release.url).href, ...release.manifest.files?.[path] }])) };
}

// A second, pure plan evaluation while the backup callback is paused. The stock installer
// still computes and validates its own plan after approval; this does not change its algorithm.
export async function previewPlan(backup, app, cfg) {
  const files = {};
  for (const [kind, bytes] of Object.entries({ backup, app, cfg })) files[kind] = { size: bytes.length, sha256: await sha256hex(bytes) };
  const plan = IP.asJson(IP.plan(backup, app, cfg));
  return { valid: !plan.failed && plan.view === 'stock', plan, files };
}
