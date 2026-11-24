// Read-only instrument MIDI for Learn. Known USB, BLE and stock names are listed in
// docs/usb.md, docs/ble.md and docs/hardware.md. The editor can also exclude ports it found
// via the mixer protocol, regardless of their names.
const mixerNames = /^(?:(?:better-smc-mixer|smc-mixer hardware test)(?: smc-mixer(?: config)?)?|(?:better-)?smc-mixer bluetooth|smc-mixer(?: config| ?-(?:private|master))?)$/i;
// Windows names a second port "MIDIIN2 (SMC-Mixer)"; ALSA reports "client:port 20:1".
// Each part is checked, so a user's own port such as "SMC-Mixer Editor Loop" stays listed.
const mixerName = { test(name = '') {
  const n = name.trim();
  const windows = /^midi(?:in|out)\d+ \((.*)\)$/i.exec(n);
  const alsa = /^([^:]+):(.+?)(?: \d+:\d+)?$/.exec(n);
  return [n, windows?.[1], alsa?.[1], alsa?.[2]].some((x) => x !== undefined && mixerNames.test(x.trim()));
} };

// Only channel status can start or continue a message. A system common or SysEx status
// clears it; realtime bytes can occur anywhere without disturbing it.
function channelStream(emit) {
  let status = 0, data = [];
  return (bytes) => {
    for (const b of bytes) {
      if (b >= 0xF8) continue;
      if (b >= 0x80) {
        status = b < 0xF0 ? b : 0;
        data = [];
      } else if (status) {
        data.push(b);
        const type = status >> 4;
        if (data.length === (type === 0xC || type === 0xD ? 1 : 2)) {
          const message = [type === 0x9 && data[1] === 0 ? status - 0x10 : status, ...data];
          data = [];
          emit(message);
        }
      }
    }
  };
}

// start() requests permission; select(id) starts listening (null deselects). Both resolve false
// if cancelled or superseded, and reject on a current permission/open error. onChange receives
// { inputs, selectedId } while started. stop() detaches synchronously, then waits for port cleanup.
export function createLearnPort({ onMessage, onChange = () => {}, excludeInput = () => false,
  requestAccess = (options) => {
    if (!globalThis.navigator?.requestMIDIAccess) throw new Error('Web MIDI is not available in this browser.');
    return navigator.requestMIDIAccess(options);
  },
} = {}) {
  let session = null;
  const closing = new Map(); // a new selection of the same port waits for the old one's cleanup
  const available = (port) => port.state === 'connected' && !mixerName.test(port.name?.trim()) && !excludeInput(port);

  function publish(s) {
    const inputs = [...s.access.inputs.values()].filter(available).map(({ id, name, manufacturer }) => ({ id, name, manufacturer }));
    onChange({ inputs, selectedId: s.selected?.receive ? s.selected.port.id : null });
  }

  async function start() {
    stop();
    const s = session = {};
    try {
      const access = await requestAccess({ sysex: false });
      if (session !== s) return false;
      s.access = access;
      s.stateChange = (e) => { if (e.port?.type !== 'output' && session === s) refresh(); };
      access.addEventListener?.('statechange', s.stateChange); // devhost's static stand-in has no EventTarget
      publish(s);
      return session === s;
    } catch (e) {
      if (session !== s) return false;
      stop();
      throw e;
    }
  }

  async function select(id) {
    const s = session;
    if (!s?.access) return false;
    if (id === null) {
      const closed = detach(s);
      publish(s);
      await closed;
      return session === s && !s.selected;
    }
    const port = s.access.inputs.get(id);
    if (!port || !available(port)) return false;
    if (s.selected?.port === port) return s.selected.ready;
    detach(s);
    const sub = s.selected = { port };
    sub.ready = Promise.resolve().then(() => attach(s, sub));
    return sub.ready;
  }

  async function attach(s, sub) {
    const { port } = sub;
    const current = () => s.selected === sub && available(port) && s.access.inputs.get(port.id) === port;
    try {
      const pendingClose = closing.get(port.id);
      if (pendingClose) await pendingClose;
      if (!current()) {
        if (s.selected === sub) refresh();
        return false;
      }
      if (port.connection === 'closed' && !port.onmidimessage) {
        sub.opening = Promise.resolve(port.open());
        try { await sub.opening; } finally { sub.opening = null; }
        sub.owned = true;
      }
      if (!current()) {
        if (s.selected === sub) refresh();
        return false;
      }
      const feed = channelStream((bytes) => {
        if (current()) onMessage(bytes);
      });
      sub.receive = (e) => feed(e.data);
      port.addEventListener('midimessage', sub.receive);
      publish(s);
      return s.selected === sub;
    } catch (e) {
      if (s.selected !== sub) return false;
      detach(s);
      publish(s);
      throw e;
    }
  }

  // Also callable when MixerDevice identifies a renamed input without a Web MIDI statechange.
  function refresh() {
    const s = session;
    if (!s?.access) return;
    const port = s.selected?.port;
    if (port && (!available(port) || s.access.inputs.get(port.id) !== port ||
      (s.selected.receive && port.connection === 'closed'))) detach(s);
    publish(s);
  }

  function detach(s) {
    if (!s?.selected) return;
    const { port, owned, receive, opening } = s.selected;
    s.selected = null;
    if (receive) port.removeEventListener('midimessage', receive);
    if (!owned && !opening) return;
    // Web MIDI has no listener-count API. Never close a pre-opened port or a port since
    // claimed by the editor or exposed through someone else's onmidimessage handler.
    const closeOwned = async () => {
      try {
        if (!port.onmidimessage && !excludeInput(port)) await port.close();
      } catch { /* the listener was already removed; the port may have gone away */ }
    };
    const done = (opening ? opening.then(closeOwned, () => {}) : closeOwned())
      .finally(() => { if (closing.get(port.id) === done) closing.delete(port.id); });
    closing.set(port.id, done);
    return done;
  }

  async function stop() {
    const s = session;
    session = null;
    s?.access?.removeEventListener?.('statechange', s.stateChange);
    detach(s);
    await Promise.all(closing.values());
  }

  return { start, select, refresh, stop };
}