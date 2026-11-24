// Real editor UI adapter. The only page code is this fixed operation allowlist; callers cannot
// supply JavaScript/selectors. No device instance or flash transport is constructed here.
import { isDeepStrictEqual } from 'node:util';
function pageOperation(operation, args) {
  if (location.origin !== args.origin) throw Error('editor origin mismatch');
  const observer = window.__smcCycleObserver;
  if (!observer || typeof observer.snapshot !== 'function') throw Error('cycle observer missing');
  const state = observer.snapshot();
  if (state.version !== 1 || !state.initialized) throw Error('editor observer not initialized');
  const visible = el => !!el && el.getClientRects().length > 0 && !el.closest('[hidden], details:not([open])') && getComputedStyle(el).visibility !== 'hidden' && getComputedStyle(el).display !== 'none';
  const dialog = document.querySelector('#dlg');
  const title = dialog?.open ? document.querySelector('#dlg h2')?.textContent?.trim() ?? '' : '';
  const stage = dialog?.open ? document.querySelector('#dlg .step.now b')?.textContent?.trim() ?? '' : '';
  const control = (label, scope = null, id = null) => {
    const matches = [...document.querySelectorAll('button')].filter(el => visible(el) && (label === null || (el.getAttribute('aria-label') || el.textContent).trim() === label)
      && (!scope || !!el.closest(scope)) && (!id || el.id === id));
    if (matches.length !== 1 || matches[0].disabled) throw Error('intended control must be unique, enabled and visible: ' + label);
    return matches[0];
  };
  const click = (label, scope, id) => control(label, scope, id).click();
  const ownClean = () => {
    if (!state.identity.real || !state.identity.connected || state.identity.kind !== 'ours' || !state.config.clean || !state.config.editorMatchesRunning) throw Error('clean saved own configuration required');
  };
  if (operation === 'install') {
    if (!args.binding || args.binding.pageId!==state.pageId || args.binding.jobId!==state.job?.id) throw Error('install job/page binding changed');
    if (!args.pin || JSON.stringify(state.release) !== JSON.stringify(args.pin)) throw Error('release pin changed or missing');
    for (const role of ['app', 'cfg']) {
      const path = args.pin.manifest.install[role], expected = args.pin.manifest.files[path], got = state.job?.preview?.files?.[role];
      if (!got || got.size !== expected.size || got.sha256 !== expected.sha256) throw Error('preview input not bound to pinned release');
    }
  }
  if (operation === 'snapshot') return { ...state, url: location.href, dom: { title, stage, dialogOpen: !!dialog?.open,
    buttons: [...document.querySelectorAll('button')].filter(visible).map(el => ({ name: (el.getAttribute('aria-label') || el.textContent).trim(), enabled: !el.disabled })),
    progress: [...document.querySelectorAll('#dlg progress')].filter(visible).map(el => ({ value: el.value, max: el.max })) } };
  switch (operation) {
    case 'device': ownClean(); if (dialog?.open) throw Error('unexpected dialog'); click(null, null, 'device-pill'); break;
    case 'load': ownClean(); if (title !== 'SMC-Mixer') throw Error('wrong device sheet'); click('Load from the mixer', '#dlg'); break;
    case 'file_menu': ownClean(); if (dialog?.open) throw Error('unexpected dialog'); {
      const el = document.querySelector('#btn-file'); if (!visible(el) || el.disabled) throw Error('File control unavailable'); el.click(); break;
    }
    case 'everything': ownClean(); click('Everything', '#menu'); break;
    case 'download': ownClean(); if (title !== 'Save a copy: Everything') throw Error('wrong export dialog'); {
      const input = document.querySelector('#save-name'); if (!input || !visible(input)) throw Error('export filename missing'); input.value = args.name;
      click('Download', '#dlg'); break;
    }
    case 'rollback_open': ownClean(); if (title !== 'SMC-Mixer') throw Error('wrong device sheet'); {
      const details = document.querySelectorAll('#dlg details');
      const firmware = [...details].filter(el => el.querySelector('summary')?.textContent?.trim() === 'Firmware');
      if (firmware.length !== 1) throw Error('unique Firmware section required'); firmware[0].open = true;
      click('Go back to M-Vave’s firmware…', '#dlg'); break;
    }
    case 'rollback_next': if (title !== 'Go back to M-Vave’s firmware' || stage !== 'Keep your configuration') throw Error('wrong rollback stage'); click('Save a copy', '#dlg'); click('Next', '#dlg .step.now'); break;
    case 'rollback': ownClean(); if (title !== 'Go back to M-Vave’s firmware' || stage !== 'Go back') throw Error('wrong rollback stage'); click('Go back', '#dlg .step.now'); break;
    case 'backup_open': if (!state.identity.connected || state.identity.kind !== 'stock' || dialog?.open) throw Error('stock connection and idle page required'); click('Install', null, 'pill-act'); break;
    case 'backup_begin': if (title !== 'Install better-smc-mixer' || stage !== 'Check the mixer') throw Error('wrong install stage'); click('Next', '#dlg .step.now'); click('Read and save the backup', '#dlg .step.now'); break;
    case 'backup_next': if (title !== 'Install better-smc-mixer' || stage !== 'Save a backup of the mixer' || state.job?.stage !== 'backup_waiting' || !state.job.preview?.valid) throw Error('backup or plan not ready'); click('Next', '#dlg .step.now'); break;
    case 'install': if (title !== 'Install better-smc-mixer' || !stage.startsWith('Install ') || state.job?.stage !== 'backup_waiting' || !state.job.preview?.valid || state.job.error) throw Error('validated backup plan and Install stage required'); click('Install', '#dlg .step.now'); break;
    case 'close_dialog': if (!dialog?.open || (state.job && !['created', 'backup_waiting', 'complete', 'error'].includes(state.job.stage))) throw Error('dialog is not safely closable'); click('Close', '#dlg'); break;
    case 'connect': {
      if (state.identity.connected) throw Error('device already connected');
      if (args.transport !== 'USB' && args.transport !== 'BLE') throw Error('explicit USB or BLE transport required');
      const label = args.stock && args.transport === 'BLE' ? (title === 'Go back to M-Vave’s firmware' ? 'Choose the mixer' : 'Bluetooth') : 'Connect';
      const button = control(label, dialog?.open ? '#dlg' : null, dialog?.open ? null : (label === 'Bluetooth' ? 'pill-bt' : 'pill-act'));
      // connectMixer normally probes all MIDI pairs. Scope this single authorized UI attempt to
      // one exact endpoint and transport. The page retains its existing MixerDevice/client.
      if (!(args.stock && args.transport === 'BLE')) {
        if (!args.endpoint || typeof args.endpoint !== 'string') throw Error('exact MIDI endpoint required');
        const endpoint = args.endpoint, transport = args.transport;
        if ((transport === 'USB') !== /config|private/i.test(endpoint)) throw Error('endpoint does not match requested transport');
        const original = navigator.requestMIDIAccess;
        if (typeof original !== 'function') throw Error('Web MIDI unavailable');
        let invoked = false;
        const scoped = async function(options) {
          invoked = true;
          if (navigator.requestMIDIAccess === scoped) navigator.requestMIDIAccess = original;
          try {
            const access = await original.call(navigator, options);
            const filter = ports => new Map([...ports].filter(([, port]) => port.name === endpoint));
            const inputs = filter(access.inputs), outputs = filter(access.outputs);
            if (inputs.size !== 1 || outputs.size !== 1) throw Error('exact MIDI endpoint must be unique');
            return new Proxy(access, { get(target, key) { if (key === 'inputs') return inputs; if (key === 'outputs') return outputs; const value = Reflect.get(target, key, target); return typeof value === 'function' ? value.bind(target) : value; }, set(target, key, value) { return Reflect.set(target, key, value, target); } });
          } finally { if (navigator.requestMIDIAccess === scoped) navigator.requestMIDIAccess = original; }
        };
        navigator.requestMIDIAccess = scoped;
        if (navigator.requestMIDIAccess !== scoped) throw Error('cannot scope MIDI endpoint');
        try { button.click(); if (!invoked) throw Error('Connect did not initiate MIDI access'); }
        finally { if (navigator.requestMIDIAccess === scoped) navigator.requestMIDIAccess = original; }
      } else button.click();
      break;
    }
    default: throw Error('unknown editor action');
  }
  return { dispatched: true, operation };
}

export function compactSnapshot(snapshot) {
  const value=structuredClone(snapshot);
  const smallPlan=p=>p ? {failed:p.failed,view:p.view,found:p.found,writes:p.writes} : p;
  if(value.job){value.job.plan=smallPlan(value.job.plan);if(value.job.preview)value.job.preview.plan=smallPlan(value.job.preview.plan);}
  value.events=[];
  if(value.latest)for(const e of Object.values(value.latest))if(e?.plan)e.plan=smallPlan(e.plan);
  return value;
}

export class EditorAdapter {
  constructor(client, sessionId, { origin }) {
    const parsed = new URL(origin);
    if (parsed.origin !== origin || !['http:', 'https:'].includes(parsed.protocol)) throw Error('exact editor origin required');
    this.client = client; this.sessionId = sessionId; this.origin = origin; this.pin = null;
  }
  async _operation(operation, args = {}) {
    const result = await this.client.call('Runtime.evaluate', { expression: `(${pageOperation.toString()})(${JSON.stringify(operation)},${JSON.stringify({ ...args, origin: this.origin })})`, returnByValue: true, awaitPromise: true, userGesture: operation !== 'snapshot' }, this.sessionId);
    if (result.exceptionDetails) throw Error(result.exceptionDetails.exception?.description || result.exceptionDetails.text || 'editor action failed');
    if (!result.result || !('value' in result.result)) throw Error('editor returned no snapshot');
    return result.result.value;
  }
  async snapshot() {
    const state = compactSnapshot(await this._operation('snapshot'));
    const rollback = state.latest?.rollback, preview = state.job?.preview;
    const binding=state.pageId && Number.isSafeInteger(state.job?.id) ? {pageId:state.pageId,jobId:state.job.id}:null;
    const backupEvent=state.latest?.backup, completeEvent=state.latest?.complete;
    const backupCurrent=!!binding && backupEvent?.jobId===binding.jobId && backupEvent.sequence>(rollback?.sequence ?? 0);
    const completed=backupCurrent && completeEvent?.jobId===binding.jobId && completeEvent.sequence>backupEvent.sequence && state.job?.completed===true;
    const releaseMatch = !!this.pin && isDeepStrictEqual(state.release, this.pin) && ['app', 'cfg'].every(role => {
      const expected = this.pin.manifest.files[this.pin.manifest.install[role]], got = preview?.files?.[role];
      return got?.size === expected.size && got?.sha256 === expected.sha256;
    });
    return { ...state, config: { ...state.config, saved: !!state.config.clean, dirty: !state.config.clean || !state.config.editorMatchesRunning,
      pulled: !!state.latest?.config_loaded && state.latest.config_loaded.sequence > (state.latest.connection?.sequence || 0) },
      rollback: { active: rollback?.stage === 'intent', ready: state.latest?.rollback_check?.status === 'ok' && state.latest.rollback_check.sequence > (state.latest?.rollback_prepare?.sequence ?? Infinity) && state.dom.stage === 'Go back' && state.dom.buttons.some(b=>b.name==='Go back' && b.enabled), completed: rollback?.stage === 'result' && rollback.status === 'ok' && rollback.sequence > (state.latest?.rollback_prepare?.sequence ?? Infinity) && !state.identity.connected },
      stock: { verified: state.identity.connected && state.identity.kind === 'stock' },
      backup: { binding, complete: backupCurrent && (state.job?.stage === 'backup_waiting' || completed), path: null, plan_ok: !!preview?.valid, release_match: releaseMatch },
      install: { binding, completion_sequence:completed?completeEvent.sequence:null, active: !!state.job && !['created', 'backup_waiting', 'complete', 'error'].includes(state.job.stage), completed, readback_verified: completed && !!state.job?.plan && !state.job.plan.failed,
        boot_selection_verified: completed && !!state.job?.plan && !state.job.plan.failed },
      restart: { binding, verified: false } };
  }
  async action({ name, transport, endpoint, binding }) {
    const single = { rollback: 'rollback', backup_next: 'backup_next', install: 'install', close_dialog: 'close_dialog' };
    if (name in single) return this._operation(single[name], name === 'install' ? { pin: this.pin, binding } : {});
    if (name === 'ours_connect' || name === 'stock_connect') return this._operation('connect', { transport, endpoint, stock: name === 'stock_connect' });
    if (name === 'baseline' || name === 'final_export') {
      await this._operation('device'); return this._operation('load');
    }
    if (name === 'rollback_prepare') { await this._operation('device'); await this._operation('rollback_open'); return this._operation('rollback_next'); }
    if (name === 'backup_prepare') { await this._operation('backup_open'); return this._operation('backup_begin'); }
    throw Error('unknown editor action: ' + name);
  }
  async release({ manifest, manifest_sha256 }) {
    this.pin = null;
    const state = await this.snapshot();
    if (!isDeepStrictEqual(state.release?.manifest, manifest)) throw Error('editor selected a different release manifest');
    if (manifest_sha256 !== undefined && state.release.manifestSha256 !== manifest_sha256) throw Error('editor manifest response hash mismatch');
    this.pin = structuredClone(state.release); return this.pin;
  }
  async exportConfig({ name }) {
    if (typeof name !== 'string' || !/^[\w -]{1,100}$/.test(name)) throw Error('safe export name required');
    await this._operation('file_menu'); await this._operation('everything');
    await this._operation('download', { name });
    return { dispatched: true, filename: name + '.json', completion: 'Browser.downloadProgress required' };
  }
}
