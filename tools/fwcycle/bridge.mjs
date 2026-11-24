import {pathToFileURL} from 'node:url';
import {CDPClient, DownloadTracker, PromptPolicy} from './cdp.mjs';
import {lstat, realpath} from 'node:fs/promises';
import {isAbsolute} from 'node:path';

export class Bridge {
  constructor({fetchImpl = globalThis.fetch, connect = url => CDPClient.connect(url), loadEditor = () => import('./editor.mjs')} = {}) {
    this.fetchImpl = fetchImpl;
    this.connect = connect;
    this.loadEditor = loadEditor;
  }
  async attach(params) {
    if (this.client) throw new Error('Bridge already attached');
    for (const field of ['endpoint', 'origin']) {
      const value = params[field];
      if (typeof value !== 'string' || !/^http:\/\/127\.0\.0\.1:[1-9][0-9]{0,4}$/.test(value)) throw new Error(`Invalid ${field}: literal loopback origin required`);
      const parsed = new URL(value);
      if (parsed.origin !== value) throw new Error(`Invalid ${field}: exact origin required`);
    }
    if (params.downloads !== undefined) {
      if (typeof params.downloads !== 'string' || !isAbsolute(params.downloads)) throw new Error('Downloads require an absolute private directory');
      const stat = await lstat(params.downloads);
      if (stat.isSymbolicLink() || await realpath(params.downloads) !== params.downloads) throw new Error('Downloads require a canonical nonsymlink directory');
      if (!stat.isDirectory() || (stat.mode & 0o077) !== 0 || (process.getuid && stat.uid !== process.getuid())) throw new Error('Downloads directory must be private and owned by current user');
    }
    const response = await this.fetchImpl(`${params.endpoint}/json/version`, {redirect: 'error', signal: AbortSignal.timeout(10_000)});
    if (!response.ok || response.redirected || (response.url && response.url !== `${params.endpoint}/json/version`)) throw new Error('Invalid CDP metadata response');
    const reader = response.body.getReader();
    const chunks = [];
    let size = 0;
    try {
      while (true) {
        const {done, value} = await reader.read();
        if (done) break;
        size += value.byteLength;
        if (size > 65_536) throw new Error('CDP metadata too large');
        chunks.push(Buffer.from(value));
      }
    } finally {await reader.cancel();}
    let version;
    try {version = JSON.parse(Buffer.concat(chunks).toString('utf8'));}
    catch {throw new Error('Invalid CDP metadata JSON');}
    const prefix = params.endpoint.replace('http:', 'ws:') + '/devtools/browser/';
    if (typeof version.webSocketDebuggerUrl !== 'string' || !version.webSocketDebuggerUrl.startsWith(prefix) || !/^[a-zA-Z0-9_-]+$/.test(version.webSocketDebuggerUrl.slice(prefix.length))) throw new Error('Invalid CDP websocket endpoint');
    this.client = await this.connect(version.webSocketDebuggerUrl);
    try {
      if (params.profile !== undefined) {
        if (typeof params.profile !== 'string' || !isAbsolute(params.profile)) throw new Error('Invalid profile path');
        let command;
        try {command = await this.client.call('Browser.getBrowserCommandLine');}
        catch {throw new Error('Cannot verify browser profile: command line unavailable');}
        const profiles = command.arguments?.filter(arg => arg.startsWith('--user-data-dir=')) ?? [];
        if (profiles.length !== 1 || profiles[0] !== `--user-data-dir=${params.profile}`) throw new Error('Browser profile mismatch');
      }
      const {targetInfos} = await this.client.call('Target.getTargets');
      const candidates = targetInfos.filter(t => {
        try {return t.type === 'page' && new URL(t.url).origin === params.origin;} catch {return false;}
      });
      if (candidates.length !== 1) throw new Error('Expected exactly one editor target');
      this.targetId = candidates[0].targetId;
      if (params.targetId !== undefined && params.targetId !== this.targetId) throw new Error('Explicit target does not match unique editor');
      const {sessionId} = await this.client.call('Target.attachToTarget', {targetId: this.targetId, flatten: true});
      this.sessionId = sessionId;
      const {targetInfo} = await this.client.call('Target.getTargetInfo', {targetId: this.targetId});
      if (targetInfo?.targetId !== this.targetId || targetInfo.type !== 'page' || new URL(targetInfo.url).origin !== params.origin) throw new Error('Editor target changed during attach');
      this.prompts = new PromptPolicy(this.client, sessionId);
      await this.client.call('Page.enable', {}, sessionId);
      await this.client.call('DeviceAccess.enable', {}, sessionId);
      if (params.downloads !== undefined) {
        const {frameTree} = await this.client.call('Page.getFrameTree', {}, sessionId);
        if (new URL(frameTree.frame.url).origin !== params.origin) throw new Error('Download frame origin mismatch');
        this.downloadTracker = new DownloadTracker(this.client, {directory: params.downloads, frameIds: [frameTree.frame.id]});
        await this.client.call('Browser.setDownloadBehavior', {behavior: 'allowAndName', downloadPath: params.downloads, eventsEnabled: true});
      }
      const {EditorAdapter} = await this.loadEditor();
      this.origin = params.origin;
      this.editor = new EditorAdapter(this.client, sessionId, {origin: params.origin});
      return {targetId: this.targetId, origin: params.origin, profileVerified: params.profile !== undefined};
    } catch (error) {await this.detach().catch(() => {}); throw error;}
  }
  async detach() {
    if (this.client) {
      try { if (this.sessionId) await this.client.call('Target.detachFromTarget', {sessionId: this.sessionId}); }
      finally {this.prompts?.close(); this.prompts = null; this.downloadTracker?.close(); this.downloadTracker = null; this.client.close(); this.client = null; this.editor = null; this.sessionId = null;}
    }
    return {detached: true};
  }
  async dispatch(method, params = {}) {
    if (method === 'hello') return {protocol: 1, hostOnly: true, nativeWebSocket: typeof WebSocket === 'function', nativeFetch: typeof fetch === 'function'};
    if (method === 'attach') return this.attach(params);
    if (method === 'disconnectBridge') return this.detach();
    if (method === 'downloads') return {enabled: Boolean(this.downloadTracker), downloads: this.downloadTracker?.snapshot() ?? []};
    if (method === 'nativePrompt') {
      if (Object.keys(params).length) throw new Error('nativePrompt is read-only');
      if (!this.prompts) throw new Error('Bridge is not attached');
      return this.prompts.snapshot();
    }
    if (['snapshot', 'action', 'release', 'exportConfig'].includes(method)) {
      if (!this.editor) throw new Error('Bridge is not attached');
      const {targetInfo} = await this.client.call('Target.getTargetInfo', {targetId: this.targetId});
      if (targetInfo?.targetId !== this.targetId || targetInfo.type !== 'page' || new URL(targetInfo.url).origin !== this.origin) throw new Error('Editor target origin changed');
      const armed = method === 'action' && params.name === 'stock_connect' && params.transport === 'BLE';
      if (armed) this.prompts.arm();
      try {
        const result = await this.editor[method](params);
        await this.prompts.flush();
        return result;
      } catch (error) {
        if (armed) this.prompts.disarm();
        throw error;
      }
    }
    throw new Error(`Unsupported method: ${method}`);
  }
}

export async function run(input = process.stdin, output = process.stdout, bridge = new Bridge()) {
  const {once} = await import('node:events');
  const maxBytes = 65_536;
  async function respond(line, oversized = false) {
    let id = null;
    let reply;
    try {
      if (oversized) throw new Error('Request line too large');
      const request = JSON.parse(line);
      if (!request || typeof request !== 'object' || Array.isArray(request) ||
          !((typeof request.id === 'string' && request.id.length <= 128) || Number.isSafeInteger(request.id))) throw new Error('Invalid request id');
      id = request.id;
      if (typeof request.method !== 'string' || request.method.length > 64 || (request.params !== undefined && (!request.params || typeof request.params !== 'object' || Array.isArray(request.params)))) throw new Error('Invalid request envelope');
      reply = {id, result: await bridge.dispatch(request.method, request.params)};
    } catch (error) {reply = {id, error: {message: String(error.message ?? error).slice(0, 1024)}};}
    let text = JSON.stringify(reply);
    if (Buffer.byteLength(text) > maxBytes) text = JSON.stringify({id, error: {message: 'Response too large; use private evidence files'}});
    if (!output.write(text + '\n')) await once(output, 'drain');
  }
  let buffer = Buffer.alloc(0);
  let dropping = false;
  try {
    for await (const chunk of input) {
      const bytes = Buffer.isBuffer(chunk) ? chunk : Buffer.from(chunk);
      let offset = 0;
      while (offset < bytes.length) {
        const newline = bytes.indexOf(10, offset);
        const end = newline < 0 ? bytes.length : newline;
        const part = bytes.subarray(offset, end);
        if (!dropping) {
          if (buffer.length + part.length > maxBytes) {dropping = true; buffer = Buffer.alloc(0);}
          else buffer = Buffer.concat([buffer, part]);
        }
        if (newline >= 0) {
          await respond(buffer.toString('utf8'), dropping);
          buffer = Buffer.alloc(0); dropping = false;
        }
        offset = end + 1;
      }
    }
    if (buffer.length || dropping) await respond(buffer.toString('utf8'), dropping);
  } finally {await bridge.detach().catch(() => {});}
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) {
  await run();
}
