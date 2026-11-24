// One reader multiplexes browser replies and events across flattened sessions.
export class CDPClient {
  static async connect(url, {WebSocketImpl = globalThis.WebSocket, timeoutMs = 30_000, ...options} = {}) {
    const socket = new WebSocketImpl(url);
    await new Promise((resolve, reject) => {
      const finish = error => {
        clearTimeout(timer);
        socket.removeEventListener('open', open);
        socket.removeEventListener('error', fail);
        socket.removeEventListener('close', fail);
        if (error) { socket.close(); reject(error); } else resolve();
      };
      const open = () => finish();
      const fail = () => finish(new Error('CDP handshake closed'));
      const timer = setTimeout(() => finish(new Error('CDP handshake timeout')), timeoutMs);
      socket.addEventListener('open', open, {once: true});
      socket.addEventListener('error', fail, {once: true});
      socket.addEventListener('close', fail, {once: true});
      if (socket.readyState === 1) finish();
    });
    return new CDPClient(socket, {timeoutMs, ...options});
  }
  constructor(socket, {timeoutMs = 30_000, maxPending = 128, maxMessageBytes = 2_097_152} = {}) {
    this.timeoutMs = timeoutMs;
    this.maxPending = maxPending;
    this.maxMessageBytes = maxMessageBytes;
    this.closed = false;
    this.socket = socket;
    this.nextId = 0;
    this.pending = new Map();
    this.listeners = new Map();
    socket.addEventListener('message', event => {
      let message;
      try {
        if (typeof event.data !== 'string' || Buffer.byteLength(event.data) > maxMessageBytes) throw new Error('Oversized CDP message');
        message = JSON.parse(event.data);
        if (!message || typeof message !== 'object') throw new Error('Invalid frame');
      } catch {
        this.fail(new Error('Invalid or oversized CDP frame'));
        socket.close();
        return;
      }
      if (message.id !== undefined) {
        const pending = this.pending.get(message.id);
        if (!pending) return;
        this.pending.delete(message.id);
        if (message.error) pending.reject(new Error(message.error.message ?? 'CDP error'));
        else if (message.result?.exceptionDetails) {
          const detail = message.result.exceptionDetails;
          pending.reject(new Error(detail.exception?.description ?? detail.text ?? 'Evaluation failed'));
        } else pending.resolve(message.result);
      } else {
        for (const entry of this.listeners.get(message.method) ?? []) {
          if (entry.sessionId === undefined || entry.sessionId === message.sessionId) {
            try {
              const result = entry.fn(message.params, message.sessionId);
              if (result?.then) result.catch(() => {this.fail(new Error('CDP event listener failed')); socket.close();});
            } catch {this.fail(new Error('CDP event listener failed')); socket.close(); return;}
          }
        }
      }
    });
    socket.addEventListener('close', () => this.fail(new Error('CDP transport closed')));
    socket.addEventListener('error', () => this.fail(new Error('CDP transport closed after error')));
  }
  fail(error) {
    this.closed = true;
    for (const pending of this.pending.values()) pending.reject(error);
    this.pending.clear();
  }
  call(method, params = {}, sessionId, {signal, timeoutMs = this.timeoutMs} = {}) {
    if (this.closed || this.socket.readyState !== 1) return Promise.reject(new Error('CDP transport closed'));
    if (this.pending.size >= this.maxPending) return Promise.reject(new Error('Too many pending CDP calls'));
    if (signal?.aborted) return Promise.reject(new Error('CDP call aborted'));
    const id = ++this.nextId;
    return new Promise((resolve, reject) => {
      const finish = (fn, value) => {
        clearTimeout(timer);
        signal?.removeEventListener('abort', abort);
        this.pending.delete(id);
        fn(value);
      };
      const abort = () => finish(reject, new Error('CDP call aborted'));
      const timer = setTimeout(() => finish(reject, new Error(`CDP timeout: ${method}`)), timeoutMs);
      signal?.addEventListener('abort', abort, {once: true});
      this.pending.set(id, {resolve: value => finish(resolve, value), reject: error => finish(reject, error)});
      try {
        const frame = JSON.stringify({id, method, params, ...(sessionId ? {sessionId} : {})});
        if (Buffer.byteLength(frame) > this.maxMessageBytes) throw new Error('Outbound CDP message too large');
        this.socket.send(frame);
      } catch (error) { finish(reject, error); }
    });
  }
  on(method, fn, sessionId) {
    const entries = this.listeners.get(method) ?? new Set();
    this.listeners.set(method, entries);
    const entry = {fn, sessionId};
    entries.add(entry);
    return () => entries.delete(entry);
  }
  close() { this.socket.close(); }
}

// Browser events prove transfer state, not file integrity; Python verifies bytes.
export class DownloadTracker {
  constructor(client, {directory, frameIds, limit = 32}) {
    this.items = new Map();
    this.off = [
      client.on('Browser.downloadWillBegin', p => {
        if (!frameIds.includes(p.frameId) || !/^[A-Za-z0-9_-]{1,128}$/.test(p.guid)) return;
        if (!this.items.has(p.guid) && this.items.size >= limit) this.items.delete(this.items.keys().next().value);
        this.items.set(p.guid, {guid: p.guid, name: String(p.suggestedFilename ?? '').slice(0, 256), state: 'pending', receivedBytes: 0, totalBytes: 0, path: `${directory}/${p.guid}`});
      }),
      client.on('Browser.downloadProgress', p => {
        const item = this.items.get(p.guid);
        if (!item || !['inProgress', 'completed', 'canceled'].includes(p.state)) return;
        Object.assign(item, {state: p.state, receivedBytes: p.receivedBytes ?? item.receivedBytes, totalBytes: p.totalBytes ?? item.totalBytes});
      }),
    ];
  }
  snapshot() {return [...this.items.values()].map(item => ({...item}));}
  close() {for (const off of this.off) off();}
}

export class PromptPolicy {
  constructor(client, sessionId, {timeoutMs = 40_000} = {}) {
    this.client = client;
    this.sessionId = sessionId;
    this.timeoutMs = timeoutMs;
    this.chooser = {state: 'idle'};
    this.tasks = new Set();
    this.seen = new Set();
    this.dialog = {state: 'idle'};
    this.off = [
      client.on('DeviceAccess.deviceRequestPrompted', p => this.prompt(p), sessionId),
      client.on('Page.javascriptDialogOpening', () => {
        this.dialog = {state: 'rejecting'};
        this.track(this.client.call('Page.handleJavaScriptDialog', {accept: false}, this.sessionId)
          .then(() => {this.dialog = {state: 'rejected'};}, error => {this.dialog = {state: 'error', message: error.message};}));
      }, sessionId),
    ];
  }
  arm() {
    this.chooser = {state: 'armed'};
    this.promptId = null;
    this.armed = true;
    clearTimeout(this.timer);
    this.timer = setTimeout(() => {
      if (this.promptId) this.cancel(this.promptId, 'timeout');
      else {this.armed = false; this.chooser = {state: 'timeout'};}
    }, this.timeoutMs);
  }
  disarm() {this.armed = false; clearTimeout(this.timer); this.chooser = {state: 'disarmed'};}
  track(promise) {
    const task = promise.finally(() => this.tasks.delete(task));
    this.tasks.add(task);
  }
  cancel(id, state) {
    this.armed = false;
    clearTimeout(this.timer);
    this.chooser = {state};
    this.seen.add(id);
    if (this.seen.size > 128) this.seen.delete(this.seen.values().next().value);
    this.track(this.client.call('DeviceAccess.cancelPrompt', {id}, this.sessionId).catch(error => {this.chooser = {state: 'error', message: error.message};}));
  }
  prompt(p) {
    if (this.seen.has(p.id)) return;
    if (!this.armed) return this.cancel(p.id, 'unexpected');
    if (this.promptId && this.promptId !== p.id) return this.cancel(p.id, 'stale');
    this.promptId = p.id;
    const matches = p.devices.filter(d => d.name === 'SMC-Mixer');
    if (matches.length > 1) return this.cancel(p.id, 'ambiguous');
    if (matches.length === 0) {this.chooser = {state: 'pending'}; return;}
    this.armed = false;
    this.seen.add(p.id);
    clearTimeout(this.timer);
    this.chooser = {state: 'selecting'};
    this.track(this.client.call('DeviceAccess.selectPrompt', {id: p.id, deviceId: matches[0].id}, this.sessionId)
      .then(() => {this.chooser = {state: 'selected'};}, error => {this.chooser = {state: 'error', message: error.message};}));
  }
  async flush() {await Promise.all(this.tasks);}
  snapshot() {return {chooser: {...this.chooser}, dialog: {...this.dialog}};}
  close() {clearTimeout(this.timer); for (const off of this.off) off();}
}
