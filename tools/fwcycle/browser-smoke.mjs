// Explicit opt-in host browser smoke: simulated devhost only, no physical MIDI/Bluetooth.
import { spawn } from 'node:child_process';
import { mkdtemp, readFile, rm, mkdir } from 'node:fs/promises';
import { resolve } from 'node:path';
import { CDPClient } from './cdp.mjs';
import { EditorAdapter } from './editor.mjs';
const root=resolve(import.meta.dirname,'../..');
const work=await mkdtemp(resolve(root,'build/fwcycle-browser-'));
const profile=resolve(work,'profile');await mkdir(profile);
const server=spawn('python3',['-I','-c',"import importlib.util,functools; s=importlib.util.spec_from_file_location('serve','app/editor/serve.py'); m=importlib.util.module_from_spec(s); s.loader.exec_module(m); m.Server.request_queue_size=128; server=m.Server(('127.0.0.1',0),functools.partial(m.Handler,directory=m.ROOT)); print(server.server_address[1],flush=True); server.serve_forever()"],{cwd:root,stdio:['ignore','pipe','ignore']});
const serverPort=await new Promise((ok,bad)=>{const timer=setTimeout(()=>bad(Error('host-test HTTP startup timeout')),5000);server.stdout.once('data',b=>{clearTimeout(timer);ok(Number(b.toString().trim()));});server.once('exit',()=>{clearTimeout(timer);bad(Error('host-test HTTP exited'));});});
const chrome=spawn('/Applications/Google Chrome.app/Contents/MacOS/Google Chrome',[
 '--headless=new','--remote-debugging-port=0',`--user-data-dir=${profile}`,'--no-first-run','--no-default-browser-check','about:blank'],{stdio:'ignore'});
let client;
try{
 let port;
 for(let i=0;i<100;i++){
  try{port=Number((await readFile(resolve(profile,'DevToolsActivePort'),'utf8')).split('\n')[0]);break;}catch{await new Promise(r=>setTimeout(r,100));}
 }
 if(!port)throw Error('isolated host-test browser startup failed');
 const meta=await(await fetch(`http://127.0.0.1:${port}/json/version`)).json();
 client=await CDPClient.connect(meta.webSocketDebuggerUrl);
 const {targetInfos}=await client.call('Target.getTargets');
 const target=targetInfos.find(t=>t.type==='page');
 const {sessionId}=await client.call('Target.attachToTarget',{targetId:target.targetId,flatten:true});
 const origin=`http://127.0.0.1:${serverPort}`;
 await client.call('Runtime.enable',{},sessionId);await client.call('Network.enable',{},sessionId);
 const errors=[];
 client.on('Runtime.exceptionThrown',e=>errors.push(e.exceptionDetails?.exception?.description||e.exceptionDetails?.text),sessionId);
 client.on('Network.loadingFailed',e=>errors.push({network:e.errorText}),sessionId);
 await client.call('Page.navigate',{url:origin+'/app/editor/?devhost'},sessionId);
 const until=Date.now()+30000;
 let state;
 while(Date.now()<until){
  const result=await client.call('Runtime.evaluate',{expression:'window.__smcCycleObserver?.snapshot()',returnByValue:true},sessionId);
  state=result.result.value;if(state?.initialized)break;
  await new Promise(r=>setTimeout(r,200));
 }
 if(!state?.initialized)throw Error('actual editor failed initialization: '+JSON.stringify(errors));
 const adapter=new EditorAdapter(client,sessionId,{origin});
 let readOnlyRefused=false;
 try{await adapter.action({name:'ours_connect',transport:'USB',endpoint:'SMC-Mixer Config'});}catch(e){if(!/cannot scope/.test(e.message))throw e;readOnlyRefused=true;}
 if(!readOnlyRefused)throw Error('read-only devhost MIDI override was not detected');
 // The host fixture intentionally installs a configurable but nonwritable MIDI method.
 // Make only this isolated simulated page override writable, then exercise normal connection.
 await client.call('Runtime.evaluate',{expression:"{const d=Object.getOwnPropertyDescriptor(navigator,'requestMIDIAccess');Object.defineProperty(navigator,'requestMIDIAccess',{...d,writable:true});}",returnByValue:true},sessionId);
 await adapter.action({name:'ours_connect',transport:'USB',endpoint:'SMC-Mixer Config'});
 for(let i=0;i<100;i++){
  state=await adapter.snapshot();if(state.identity.connected)break;
  await new Promise(r=>setTimeout(r,100));
 }
 if(!state.identity.connected||state.identity.transport!=='USB')throw Error('simulated USB not connected');
 // devhost initialization supplies a stored baseline; exercise the actual UI pull.
 await adapter.action({name:'baseline'});
 for(let i=0;i<100;i++){
  state=await adapter.snapshot();if(state.config.pulled)break;
  await new Promise(r=>setTimeout(r,100));
 }
 if(!state.config.pulled)throw Error('actual UI configuration pull was not observed');
 if((await adapter.snapshot()).dom.dialogOpen)await adapter.action({name:'close_dialog'});
 const downloadDir=resolve(work,'downloads');await mkdir(downloadDir,{mode:0o700});
 await client.call('Browser.setDownloadBehavior',{behavior:'allow',downloadPath:downloadDir,eventsEnabled:true});
 await adapter.exportConfig({name:'host-smoke'});
 let exported;
 for(let i=0;i<50;i++){
  try{exported=JSON.parse(await readFile(resolve(downloadDir,'host-smoke.json'),'utf8'));break;}catch{await new Promise(r=>setTimeout(r,100));}
 }
 if(!exported)throw Error('real editor download did not complete');
 console.log(JSON.stringify({host_browser_smoke:true,physical_hardware:false,initialized:state.initialized,simulated_transport:state.identity.transport,read_only_scope_refused:readOnlyRefused,actual_ui_pull:state.config.pulled,actual_ui_export:true}));
}finally{
 if(client){try{await client.call('Browser.close');}catch{}client.close();}
 if(chrome.exitCode===null)chrome.kill('SIGTERM');
 server.kill('SIGTERM');
 await new Promise(r=>setTimeout(r,300));
 await rm(work,{recursive:true,force:true,maxRetries:5,retryDelay:200}).catch(e=>console.error('Host-test cleanup:',e.message));
}
