// A stand-in for M-Vave's firmware on the mixer, for the installer's tests (js/stocklink.js): its
// service SysEx as docs/hardware.md, "Stock service protocol" describes it (static analysis of V012,
// and what smcmidi saw on the mixer on 2026-11-10):
// - 0x11 answers the name;
// - 0x23 reads: below 0x1000 the raw bytes through the ENC keystream of 0xFFFF, restarted at every
//   call; up to the stock VM (0x24000) decrypted through the SFC window; above, as stored;
// - 0x21 erases the 4 KB sector of the address, 0x22 programs (NOR: only clears bits); both answer
//   the same acknowledgement whatever happened.
// Faults to test against: a write-protected range, lost replies, a different name, a mixer that
// stops answering after so many requests (the cable pulled: dieAfter).
import * as F from '../../app/editor/js/flashimg.js';
import { parse, pack7, unpack7, packet } from '../../app/editor/js/stocklink.js';

const ACK = Uint8Array.from([0x00, 0x59, 0x00, 0x01, 0x00, 0x00, 0x00, 0xFF]);

export class StockDevice {
  // flash: the 512 KB as stored (physical).
  // raw: a packet's bytes as they are (BLE, M-Vave's AE40 service) rather than SysEx (USB MIDI).
  constructor(flash, { name = 'SMC-Mixer_012', protect = [], loseEvery = 0, dieAfter = 0, raw = false } = {}) {
    Object.assign(this, { flash: Uint8Array.from(flash), name, protect, loseEvery, dieAfter, raw, n: 0, erases: [], writes: 0, onmessage: null });
  }

  send(bytes) {
    const r = parse(this.raw ? bytes : unpack7(bytes));
    if (!r || !r.ok || (this.dieAfter && this.n >= this.dieAfter)) return;
    const reply = this.handle(r.cmd, r.payload);
    this.n++;
    if (!reply || (this.loseEvery && this.n % this.loseEvery === 0)) return;
    setImmediate(() => this.onmessage && this.onmessage(this.raw ? reply : pack7(reply)));
  }

  isProtected(a) { return this.protect.some(([lo, hi]) => lo <= a && a < hi); }

  readView(addr, len) {
    const raw = this.flash.subarray(addr, addr + len);
    if (addr < 0x1000) return F.enc(raw, 0xFFFF);
    if (addr < 0x24000) return F.sfc(raw, addr);
    return Uint8Array.from(raw);
  }

  handle(cmd, p) {
    if (cmd === 0x11) return packet(0x11, Array.from(this.name, (c) => c.charCodeAt(0)));
    const addr = F.u32(p, 1);
    if (cmd === 0x23) {
      const len = p[5] | (p[6] << 8) | (p[7] << 16);
      return packet(0x23, [...p.subarray(0, 8), ...this.readView(addr, len)]);
    }
    if (cmd === 0x21) {
      const s = addr & ~0xFFF;
      this.erases.push(s);
      if (!this.isProtected(s)) this.flash.fill(0xFF, s, s + F.SECTOR);
      return ACK;
    }
    if (cmd === 0x22) {
      const len = p[5] | (p[6] << 8) | (p[7] << 16);
      this.writes++;
      if (addr + len <= this.flash.length && !this.isProtected(addr)) for (let i = 0; i < len; i++) this.flash[addr + i] &= p[8 + i];
      return ACK;
    }
    return packet(0x12, [0, 0, 0, 0, 0, 0]);
  }
}
