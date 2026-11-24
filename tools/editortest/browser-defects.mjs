// Isolated headless Chrome regression runner. No packages, device APIs or existing browser.
// Requires Node 22+ (built-in WebSocket) and installed Chrome; no download or install fallback.
// Run from the repository root: node tools/editortest/browser-defects.mjs
import { spawn } from 'node:child_process';
import { createServer } from 'node:http';
import { readFile, mkdtemp, rm } from 'node:fs/promises';
import { join, resolve, extname } from 'node:path';
import { tmpdir } from 'node:os';
import assert from 'node:assert/strict';

const root = process.cwd();
const chrome = process.env.SMC_CHROME || '/Applications/Google Chrome.app/Contents/MacOS/Google Chrome';
const scratch = process.env.TMPDIR || tmpdir();
const profile = await mkdtemp(join(scratch, 'smc-editor-defects-'));
const expose = `\nglobalThis.editorTest = { get dev(){return dev}, set dev(v){dev=v}, get cfg(){return cfg}, st, docText, openFile, edit, renderAll, installReal, updateReal, stockReal, closeDialog, deviceDialog, addMonitor, showJson, startLearn, instrumentDialog, get job(){return dev.firmwareJob}, set release(v){REAL_RELEASE=v}, get release(){return REAL_RELEASE} };`;
const server = createServer(async (req, res) => {
  try {
    const pathname = decodeURIComponent(new URL(req.url, 'http://localhost').pathname);
    const file = resolve(root, '.' + (pathname.endsWith('/') ? pathname + 'index.html' : pathname));
    if (!file.startsWith(root + '/')) { res.writeHead(403).end(); return; }
    let data = await readFile(file);
    if (file === join(root, 'app/editor/js/app.js')) data = Buffer.from(data.toString() + expose);
    res.setHeader('Content-Type', ({ '.js': 'text/javascript', '.mjs': 'text/javascript', '.json': 'application/json', '.html': 'text/html', '.css': 'text/css' })[extname(file)] || 'application/octet-stream');
    res.end(data);
  } catch { res.writeHead(404).end(); }
});
await new Promise((r) => server.listen(0, '127.0.0.1', r));
const base = `http://127.0.0.1:${server.address().port}`;
const proc = spawn(chrome, ['--headless=new', '--no-first-run', '--no-default-browser-check', '--disable-background-networking', '--disable-extensions', '--disable-sync', '--disable-features=WebMidi,WebBluetooth', '--remote-debugging-port=0', `--user-data-dir=${profile}`, 'about:blank'], { stdio: ['ignore', 'ignore', 'pipe'] });
let socket;
try {
  let endpoint = '';
  await new Promise((ok, bad) => {
    const timer = setTimeout(() => bad(new Error('headless Chrome did not start')), 15000);
    proc.once('error', bad);
    proc.stderr.on('data', (b) => { endpoint += b; if (/DevTools listening on (ws:\/\/\S+)/.test(endpoint)) { clearTimeout(timer); endpoint = endpoint.match(/DevTools listening on (ws:\/\/\S+)/)[1]; ok(); } });
  });
  socket = new WebSocket(endpoint);
  await new Promise((ok, bad) => { socket.onopen = ok; socket.onerror = bad; });
  let id = 0; const pending = new Map();
  socket.onmessage = (e) => { const m = JSON.parse(e.data); if (m.id) { const p = pending.get(m.id); pending.delete(m.id); if (m.error) p.reject(new Error(JSON.stringify(m.error))); else p.resolve(m.result); } };
  const call = (method, params = {}, sessionId) => new Promise((resolve, reject) => { const n = ++id; pending.set(n, { resolve, reject }); socket.send(JSON.stringify({ id: n, method, params, ...(sessionId ? { sessionId } : {}) })); });
  const targets = await call('Target.getTargets');
  const target = targets.targetInfos.find((t) => t.type === 'page' && t.url === 'about:blank');
  assert.ok(target);
  const { sessionId } = await call('Target.attachToTarget', { targetId: target.targetId, flatten: true });
  const cdp = (m, p) => call(m, p, sessionId);
  const evaluate = async (expression) => { const r = await cdp('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true }); if (r.exceptionDetails) throw new Error(JSON.stringify(r.exceptionDetails)); return r.result.value; };
  await cdp('Page.enable');
  await cdp('Page.addScriptToEvaluateOnNewDocument', { source: `delete Navigator.prototype.requestMIDIAccess; delete Navigator.prototype.bluetooth; globalThis.__errors=[]; addEventListener('error',e=>__errors.push(e.message)); addEventListener('unhandledrejection',e=>__errors.push(String(e.reason)));` });
  await cdp('Page.navigate', { url: base + '/app/editor/' });
  for (let i = 0; i < 200; i++) { if (await evaluate('!!globalThis.editorTest?.cfg')) break; await new Promise((r) => setTimeout(r, 50)); }

  const offline = await evaluate(`({connected:editorTest.dev.connected,real:editorTest.dev.real,pill:document.querySelector('#device-pill').textContent,notice:document.querySelector('#notice').textContent})`);
  assert.equal(offline.connected, false); assert.equal(offline.real, true); assert.equal(offline.pill.trim(), 'Offline'); assert.match(offline.notice, /Offline file editor/);
  const normalUi = await evaluate(`(() => {
    const text = () => document.body.textContent;
    const initial = text();
    document.querySelector('#btn-settings').click();
    const settings = text();
    Array.from(document.querySelectorAll('#dlg button')).find(b => b.textContent === 'Preset').click();
    const preset = text();
    document.querySelector('#dlg').close();
    return { initial, settings, preset };
  })()`);
  assert.doesNotMatch(normalUi.initial, /Simulator|Instrument sends/);
  assert.doesNotMatch(normalUi.settings + normalUi.preset, /Coming in a later firmware|Planned settings/);
  assert.match(normalUi.preset, /Middle C/i);
  const defaultFilename = await evaluate(`(() => {
    const T = editorTest;
    T.edit('clear name', () => { delete T.cfg.file.name; });
    T.showJson(T.cfg.canonical(), 'Everything');
    const value = document.querySelector('#save-name').value;
    T.closeDialog();
    return value;
  })()`);
  assert.equal(defaultFilename, 'BSM-Config');
  const exported = await evaluate(`(async()=>{const T=editorTest; const f=await (await fetch('../../config/test.json')).json(); T.openFile(f,'offline import',false); T.edit('offline name',()=>T.cfg.file.name='Offline regression'); T.showJson(T.cfg.canonical(),'Everything'); return {name:T.cfg.file.name,json:document.querySelector('#dlg pre').textContent,connected:T.dev.connected};})()`);
  assert.equal(exported.name, 'Offline regression'); assert.match(exported.json, /Offline regression/); assert.equal(exported.connected, false);
  const reports = await evaluate(`(async () => {
    const T = editorTest;
    const original = URL.createObjectURL;
    const originalClick = HTMLAnchorElement.prototype.click;
    const out = [];
    let blob;
    URL.createObjectURL = b => { blob = b; return original(b); };
    HTMLAnchorElement.prototype.click = function () { out.push({ name: this.download, blob }); };
    try {
      T.deviceDialog();
      [...document.querySelectorAll('#dlg button')].find(b => b.textContent === 'Save a report for support').click();
      const include = document.querySelector('#report-config');
      const download = [...document.querySelectorAll('#dlg button')].find(b => b.textContent === 'Download report');
      if (include.checked) throw Error('configuration inclusion must default to off');
      download.click();
      include.checked = true;
      download.click();
      return Promise.all(out.map(async ({ name, blob }) => ({ name, json: JSON.parse(await blob.text()) })));
    } finally { URL.createObjectURL = original; HTMLAnchorElement.prototype.click = originalClick; T.closeDialog(); }
  })()`);
  assert.equal(reports.length, 2);
  assert.match(reports[0].name, /^BSM-Report .*\.json$/);
  assert.equal(reports[0].json.device.connected, false);
  assert.equal('configuration' in reports[0].json, false);
  assert.equal(reports[1].json.configuration.name, 'Offline regression');
  const connectionLabels = await evaluate(`(() => {
    Object.defineProperty(navigator, 'requestMIDIAccess', { configurable: true, value: () => { throw Error('must not request MIDI'); } });
    Object.defineProperty(navigator, 'bluetooth', { configurable: true, value: { requestDevice: () => { throw Error('must not scan'); } } });
    try {
      editorTest.renderAll();
      return { stock: document.querySelector('#pill-bt').textContent, midi: document.querySelector('#pill-act').title };
    } finally { delete navigator.requestMIDIAccess; delete navigator.bluetooth; editorTest.renderAll(); }
  })()`);
  assert.equal(connectionLabels.stock, 'Install over Bluetooth');
  assert.match(connectionLabels.midi, /Bluetooth MIDI/);
  const learned = await evaluate(`(async () => {
    const T = editorTest;
    const tick = () => new Promise(r => setTimeout(r, 10));
    const port = Object.assign(new EventTarget(), { id:'synth', name:'Instrument', state:'connected', connection:'open' });
    const mixer = { id:'mixer', name:'Better-SMC-Mixer', state:'connected', connection:'open' };
    let sysex;
    Object.defineProperty(navigator, 'requestMIDIAccess', { configurable:true, value: async opts => { sysex = opts.sysex; return { inputs:new Map([[port.id,port],[mixer.id,mixer]]) }; } });
    try {
      const id = document.querySelector('#panel .ctl[data-id$=".fader"]').dataset.id;
      T.st.sel = new Set([id]);
      T.startLearn([id]);
      await tick();
      const select = document.querySelector('#learn-input');
      const inputs = [...select.options].map(o => o.value);
      select.value = 'synth'; select.dispatchEvent(new Event('change'));
      await tick();
      const send = (b) => port.dispatchEvent(Object.assign(new Event('midimessage'), {data:Uint8Array.from(b)}));
      send([0xb2, 74, 120]);
      const c = T.cfg.control(T.st.preset,T.st.page,id);
      send([0xb2, 71, 120]);
      return {inputs, sysex, number:c.number, channel:c.channel, active:!!T.st.learn};
    } finally { delete navigator.requestMIDIAccess; }
  })()`);
  assert.deepEqual(learned, { inputs: ['', 'synth'], sysex: false, number: 74, channel: 3, active: false });
  console.log('ok browser: offline editing/export, no placeholders, distinct connections, opt-in diagnostics');
  await cdp('Page.navigate', { url: base + '/app/editor/?mock' });
  for (let i = 0; i < 200; i++) { if (await evaluate('!!globalThis.editorTest?.cfg')) break; await new Promise((r) => setTimeout(r, 50)); }
  assert.match(await evaluate(`document.querySelector('#notice').textContent`), /Simulation/);
  const monitor = await evaluate(`(async()=>{const T=editorTest; T.st.monitorOpen=true; document.querySelector('#monitor-list').hidden=false; let bar=0,list=0; const a=new MutationObserver(()=>bar++),b=new MutationObserver(()=>list++);a.observe(document.querySelector('#monitor-bar'),{childList:true});b.observe(document.querySelector('#monitor-list'),{childList:true});for(let i=0;i<100;i++)T.addMonitor({dir:'out',bytes:[176,i,1]});await new Promise(requestAnimationFrame);await new Promise(r=>setTimeout(r,0));a.disconnect();b.disconnect();return {bar,list,rows:document.querySelectorAll('#monitor-list tr').length,history:T.st.monitor.length};})()`);
  assert.deepEqual(monitor, { bar: 1, list: 1, rows: 100, history: 100 });
  console.log('ok browser: 100 MIDI events, one actual footer/list DOM redraw');
  const installed = await evaluate(`(async()=>{
    const T=editorTest;
    globalThis.setImmediate=queueMicrotask;
    const {MixerDevice}=await import('./js/mixer.js'), {Codec}=await import('./js/cfgbin.js');
    const {StockDevice}=await import('../../tools/editortest/stockdev.mjs'), {StockLink}=await import('./js/stocklink.js');
    const v=await (await fetch('../../tools/editortest/vectors/install.json')).json();
    const bytes=b=>Uint8Array.from(atob(b),c=>c.charCodeAt(0));
    const flash=new Uint8Array(0x80000).fill(255);for(const [at,b] of v.flashes.stored)flash.set(bytes(b),at);
    const stock=new StockDevice(flash), m=new MixerDevice(new Codec(await (await fetch('../../config/schema.json')).json()));
    m.connected=true;m.stock=new StockLink(stock,{timeout:100,ackTimeout:100});m.fw={kind:'stock',version:'SMC-Mixer_012'};
    T.dev=m;
    const sum=async b=>Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',b)),n=>n.toString(16).padStart(2,'0')).join('');
    const app=bytes(v.inputs.app),cfg=bytes(v.inputs.cfg);
    // The release at its real package paths, served by a fetch stub (releaseFiles accepts nothing else).
    const appPath='firmware/controller-ble.bin',cfgPath='firmware/cfg_tool.bin',relUrl=new URL('../manifest.json',location.href).href;
    const served={[new URL(appPath,relUrl).href]:app,[new URL(cfgPath,relUrl).href]:cfg},realFetch=globalThis.fetch;
    globalThis.fetch=(u,o)=>served[String(u)]?Promise.resolve(new Response(served[String(u)])):realFetch(u,o);
    T.release={url:relUrl,install:{app:appPath,cfg:cfgPath},manifest:{files:{[appPath]:{size:app.length,sha256:await sum(app)},[cfgPath]:{size:cfg.length,sha256:await sum(cfg)}}}};
    const button=text=>[...document.querySelectorAll('#dlg button')].find(b=>b.textContent===text);
    const until=async fn=>{for(let i=0;i<1000;i++){if(fn())return;if(T.job?.error)throw T.job.error;await new Promise(r=>setTimeout(r,5));}throw Error('browser installer wait timed out: '+document.querySelector('#dlg').textContent)};
    T.closeDialog();T.installReal();button('Next').click();button('Read and save the backup').click();
    await until(()=>!!T.job.go);const owner=T.job,view=owner.view;
    T.closeDialog();T.updateReal();if(T.job!==owner||owner.view!==view)throw Error('confirmation owner changed');
    button('Next').click();
    const milestones=[];
    const reopen=stage=>{T.closeDialog();T.stockReal();if(T.job!==owner||owner.view!==view)throw Error(stage+' owner changed');milestones.push(stage)};
    const read=m.stock.readRange.bind(m.stock);let area=false,check=false;
    m.stock.readRange=async(at,n,...rest)=>{if(at>=0x41000&&!area){area=true;reopen('area');}if(at===0x41000&&n>4096&&!check){check=true;reopen('readcheck');}return read(at,n,...rest)};
    button('Install').click();await until(()=>!owner.active);
    if(owner.error)throw owner.error;
    const want=v.cases.find(c=>c.name==='stored, with cfg_tool.bin').sha256.after;
    const actual=await sum(stock.flash);
    T.closeDialog();T.installReal();
    T.closeDialog();T.deviceDialog();
    const status=[...document.querySelectorAll('#dlg button')].find(b=>/Firmware operation/.test(b.textContent));
    if(!status)throw Error('completed firmware status is not discoverable from the mixer sheet');
    status.click();
    return {completed:owner.completed,same:T.job===owner,view:T.job.view===view,actual,want,milestones,errors:__errors};
  })()`);
  assert.equal(installed.completed, true); assert.equal(installed.same, true); assert.equal(installed.view, true);
  assert.equal(installed.actual, installed.want); assert.deepEqual(installed.milestones, ['area', 'readcheck']); assert.deepEqual(installed.errors, []);
  console.log('ok browser: StockDevice install close/reopen at confirmation, area, readcheck and completion; exact flash hash');
  const saveUI = await evaluate(`(async()=>{
    const T=editorTest,m=T.dev;T.closeDialog();m.firmwareJob=null;
    Object.assign(m,{connected:true,fw:{kind:'ours',version:'0.1.0'},running:T.docText(),saved:null,storage:'unsaved',revision:7,mixerId:null,linkCaps:['stock']});
    let saveResolve,cleanResolve,reads=0;
    const saved=new Promise(r=>saveResolve=r),clean=new Promise(r=>cleanResolve=r);
    m.client={stock:async()=>({status:'ok'}),save:()=>saved,info:async()=>++reads===1?{store:'writing',revision:7,id:null}:clean};
    const button=text=>[...document.querySelectorAll('#dlg button')].find(b=>b.textContent===text);
    const wait=()=>new Promise(r=>setTimeout(r,150));
    T.updateReal();await wait();const job=T.job,view=job.view,go=button('Update'),save=button('Save first');
    save.click();const saving=go.disabled&&save.textContent==='Saving…';
    T.closeDialog();T.installReal();if(T.job!==job||job.view!==view)throw Error('Save owner changed on reopen');
    saveResolve();await wait();const writing=go.disabled&&save.textContent==='Saving…';
    cleanResolve({store:'failed',revision:7,id:null});await wait();
    const failure=go.disabled&&/Retry/.test(save.textContent);
    m.client.save=async()=>{};m.client.info=async()=>({store:'clean',revision:7,id:null});save.click();await wait();
    return {saving,writing,failure,clean:!go.disabled&&save.textContent==='Saved',errors:__errors};
  })()`);
  assert.deepEqual(saveUI, { saving: true, writing: true, failure: true, clean: true, errors: [] });
  console.log('ok browser: Update Save first delayed acknowledgement/storage, failed-save retry and close/reopen');
  const workflowGuards = await evaluate(`(async()=>{
    const T=editorTest;
    const {MixerDevice}=await import('./js/mixer.js'), {Codec}=await import('./js/cfgbin.js');
    const {StockDevice}=await import('../../tools/editortest/stockdev.mjs'), {StockLink}=await import('./js/stocklink.js');
    const v=await (await fetch('../../tools/editortest/vectors/install.json')).json();
    const bytes=b=>Uint8Array.from(atob(b),c=>c.charCodeAt(0));
    const codec=new Codec(await (await fetch('../../config/schema.json')).json());
    const button=text=>[...document.querySelectorAll('#dlg button')].find(b=>b.textContent===text);
    const until=async fn=>{for(let i=0;i<2000;i++){if(fn())return;await new Promise(r=>setTimeout(r,5));}throw Error('workflow wait timed out: '+document.querySelector('#dlg').textContent)};
    const sum=async b=>Array.from(new Uint8Array(await crypto.subtle.digest('SHA-256',b)),n=>n.toString(16).padStart(2,'0')).join('');
    const results=[];
    for(const transport of ['USB','BLE']){
      T.closeDialog();
      const flash=new Uint8Array(0x80000).fill(255);for(const [at,b] of v.flashes.stored)flash.set(bytes(b),at);
      const stock=new StockDevice(flash), m=new MixerDevice(codec);
      let switchDone,reconnectDone,reconnectStarted=false,reverts=0;
      const switching=new Promise(r=>switchDone=r), reconnect=new Promise(r=>reconnectDone=r);
      Object.assign(m,{connected:true,transport,fw:{kind:'ours',version:'0.1.0'},running:T.docText(),storage:'clean',revision:7,mixerId:null,linkCaps:['stock']});
      m.client={stock:async write=>write?switching:{status:'ok',pointer:0x41000,tries:0},revert:async()=>{reverts++;throw Error('Revert escaped firmware owner')}};
      m.connect=m.connectBluetooth=async()=>{reconnectStarted=true;await reconnect;m.connected=true;m.stock=new StockLink(stock,{timeout:100,ackTimeout:100});m.fw={kind:'stock',version:'SMC-Mixer_012'}};
      T.dev=m;T.updateReal();await until(()=>!button('Update').disabled);
      const job=T.job,view=job.view;
      m.storage='unsaved';T.closeDialog();T.deviceDialog();
      const disabled=button('Go back to the last save…').disabled;
      await m.revert();if(!disabled||reverts)throw Error('Revert bypassed firmware ownership');
      m.storage='clean';T.closeDialog();T.updateReal();button('Update').click();
      if(job.cancel()!==false)throw Error('cancel allowed while switching');
      switchDone({status:'ok',pointer:0x1000,tries:1});
      if(transport==='BLE'){await until(()=>!!button('Choose the mixer'));button('Choose the mixer').click();}
      await until(()=>reconnectStarted);
      if(job.cancel()!==false)throw Error('cancel allowed before confirmed stock connection');
      reconnectDone();await until(()=>!!button('Read and save the backup'));
      button('Read and save the backup').click();await until(()=>!!job.go);
      T.closeDialog();T.stockReal();if(T.job!==job||job.view!==view)throw Error('backup owner changed on reopen');
      button('Cancel before writing / Dismiss result').click();await until(()=>!job.active);
      if(job.go||!/cancel/i.test(job.error?.message))throw Error('backup cancellation did not settle');
      if(await sum(stock.flash)!==await sum(flash))throw Error('backup cancellation wrote synthetic flash');
      results.push({transport,cancelled:job.cancelled,active:job.active,reverts,disabled});
    }
    return {results,errors:__errors};
  })()`);
  assert.deepEqual(workflowGuards,{results:['USB','BLE'].map(transport=>({transport,cancelled:true,active:false,reverts:0,disabled:true})),errors:[]});
  console.log('ok browser: USB/BLE Update refuses uncertain-switch cancellation, cancels confirmed-stock backup; Revert blocked in sheet and device');
  const identity = await evaluate(`(async()=>{const T=editorTest,m=T.dev;T.closeDialog();m.firmwareJob=null;let sent=0;const before=T.docText();m.client={edit:async()=>sent++,push:async()=>sent++};m.mixerId=new Uint8Array(16).fill(1);m.revision=7;m.running=before;m.saved=before;m.panel({preset:0,page:0,shift:false,touches:0,id:new Uint8Array(16).fill(2),revision:7,store:'clean'});m.apply(before,'stale edit');await Promise.resolve();return {diverged:m.diverged,running:m.running,saved:m.saved,preserved:T.docText()===before,sent};})()`);
  assert.deepEqual(identity,{diverged:true,running:null,saved:null,preserved:true,sent:0});
  console.log('ok browser: equal-revision changed ID invalidates baseline, preserves working copy and blocks stale edits');
  const bounded = await evaluate(`(async()=>{const T=editorTest;T.st.paused=false;T.st.monitor=[];T.st.monitorOpen=true;T.st.monitorFilter='out';for(let i=0;i<500;i++)T.addMonitor({dir:i%2?'in':'out',bytes:[176,i%128,1]});await new Promise(requestAnimationFrame);const rows=document.querySelectorAll('#monitor-list tr').length,history=T.st.monitor.length;T.st.paused=true;T.addMonitor({dir:'out',bytes:[176,1,1]});const paused=T.st.monitor.length===history;T.st.paused=false;T.addMonitor({dir:'out',bytes:[176,2,1]});[...document.querySelectorAll('#monitor-bar button')].find(b=>b.textContent==='Clear').click();await new Promise(requestAnimationFrame);return {history,rows,paused,cleared:T.st.monitor.length===0,visible:document.querySelectorAll('#monitor-list tr').length};})()`);
  assert.deepEqual(bounded,{history:400,rows:200,paused:true,cleared:true,visible:0});
  console.log('ok browser: bounded history/final 200 filtered rows, Pause and Clear before pending frame');
  console.log('browser defects: passed');
} finally {
  socket?.close(); proc.kill();
  await new Promise((r) => proc.exitCode !== null || proc.signalCode !== null ? r() : proc.once('exit', r));
  await new Promise((r) => server.close(r));
  await rm(profile, { recursive: true, force: true, maxRetries: 8, retryDelay: 125 });
}
