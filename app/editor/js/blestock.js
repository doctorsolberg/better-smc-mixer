// M-Vave's service packets over Bluetooth LE (docs/hardware.md, "Stock service protocol", "Two
// transports"): not SysEx on the BLE-MIDI characteristic but a GATT service of its own, AE40. A
// packet's raw bytes are written to AE41 (write without response), in pieces when it is longer
// than one write; the reply comes as notifications of AE42, put together here until the length its
// header gives (len + 7 bytes). The result is a raw transport for js/stocklink.js. Web Bluetooth:
// Chrome on macOS, Windows, Android; not Safari, not iOS.
// ✅ on the mixer (V013, 2026-11-14, Chrome on macOS): the editor installed ours through it.

export const SERVICE = 0xae40;
export const WRITE = 0xae41;
export const NOTIFY = 0xae42;
export const MAX_PACKET = 1053; // the stock firmware's buffer: len + 7 <= 1046 + 7

// Puts notifications together into whole packets; feed(bytes) returns the packets completed.
export class Reassembler {
  constructor() { this.buf = []; }
  feed(bytes) {
    const out = [];
    for (const b of bytes) {
      if (this.buf.length === 0 && b !== 0x00) continue;       // not the start of a packet
      if (this.buf.length === 1 && b !== 0x59) { this.buf = b === 0x00 ? [0x00] : []; continue; }
      this.buf.push(b);
      if (this.buf.length >= 6) {
        const n = this.buf[3] | (this.buf[4] << 8) | (this.buf[5] << 16);
        if (n + 7 > MAX_PACKET) { this.buf = []; continue; }
        if (this.buf.length === n + 7) { out.push(Uint8Array.from(this.buf)); this.buf = []; }
      }
    }
    return out;
  }
}

// Cuts a packet into writes of at most size bytes.
export function pieces(pkt, size) {
  const out = [];
  for (let at = 0; at < pkt.length; at += size) out.push(pkt.subarray(at, Math.min(at + size, pkt.length)));
  return out;
}

// Asks the user for the mixer (the browser's chooser: needs a click) and opens AE40 on it.
export async function requestMixer(bt = navigator.bluetooth) {
  if (!bt) throw new Error('this browser has no Web Bluetooth (Chrome on macOS, Windows or Android has it)');
  // Every device near by: name filters let macOS offer the mixer's classic Bluetooth entry, which
  // GATT cannot reach ("Unsupported device"), and hid the LE one under the name macOS cached for
  // it; the choice that worked on the mixer was from the whole list (2026-11-14).
  return bt.requestDevice({ acceptAllDevices: true, optionalServices: [SERVICE] });
}

// { send, onmessage, raw: true, close } over AE40 of a BluetoothDevice. writeSize: the bytes of one
// write (ATT MTU - 3); 20 fits every link.
export async function openTransport(device, { writeSize = 20, log = () => {} } = {}) {
  const step = async (what, p) => {
    try { return await p(); }
    catch (e) {
      // DOMException.message is read-only in browsers. Keep the original exception intact.
      const error = new Error(`${what}: ${e.message ?? e}`, { cause: e });
      error.name = e.name || 'Error';
      throw error;
    }
  };
  const server = device.gatt.connected ? device.gatt : await step('connect', () => device.gatt.connect());
  const svc = await step('service AE40', () => server.getPrimaryService(SERVICE));
  const wr = await step('AE41', () => svc.getCharacteristic(WRITE));
  const nt = await step('AE42', () => svc.getCharacteristic(NOTIFY));
  const re = new Reassembler();
  let handler = () => {};
  const onvalue = (e) => {
    const v = e.target.value;
    for (const p of re.feed(new Uint8Array(v.buffer, v.byteOffset, v.byteLength))) handler(p);
  };
  nt.addEventListener('characteristicvaluechanged', onvalue);
  await step('AE42 notifications', () => nt.startNotifications());
  let queue = Promise.resolve();
  const write = (b) => (wr.writeValueWithoutResponse ? wr.writeValueWithoutResponse(b) : wr.writeValue(b));
  return {
    raw: true,
    send(pkt) {
      // writes stay in order: each packet's pieces after the previous packet's
      queue = queue.then(async () => {
        for (const p of pieces(pkt, writeSize)) await write(p);
      }).catch((e) => log('WARN', 'ble', `AE41 write: ${e.message}`));
    },
    set onmessage(fn) { handler = fn; },
    async close() {
      nt.removeEventListener('characteristicvaluechanged', onvalue);
      try { await nt.stopNotifications(); } catch { /* gone already */ }
      if (device.gatt.connected) device.gatt.disconnect();
    },
  };
}
