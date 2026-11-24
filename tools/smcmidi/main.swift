// smcmidi — host MIDI tool for better-smc-mixer: research of the stock firmware, and its flash
// erase and write (the installation path without extra hardware, SPEC Q-18).
//
// Build:  swiftc -O -swift-version 5 -o build/smcmidi tools/smcmidi/main.swift   (make smcmidi)
//
// Commands:
//   smcmidi list                                   MIDI sources and destinations
//   smcmidi listen [--match S] [--seconds N]       print every incoming message (JSON lines)
//   smcmidi send --port S --msgs "B0 14 64, 90 48 7F" [--gap-ms N]
//                                                  send channel messages (nothing else: no
//                                                  SysEx, no system messages) to a destination,
//                                                  N ms apart (default 5)
//   smcmidi session --scenario F [--match S]       guided capture: shows scenario steps, logs a
//                                                  marker at the start and end of each step and
//                                                  the operator's notes and answers
//   smcmidi sinco-info --port S                    stock service protocol: name/version (cmd 0x11)
//   smcmidi sinco-read --port S --out F [--start A] [--size N] [--chunk N]
//                                                  stock service protocol: flash read (cmd 0x23)
//   smcmidi sinco-erase --port S --addr A --allow START:END [--size N]
//                                                  erase 4 KB sectors (cmd 0x21), each checked
//                                                  erased by a read
//   smcmidi sinco-write --port S --addr A (--in F | --pattern [--size N]) --allow START:END [--chunk N]
//                                                  write into erased sectors (cmd 0x22), each
//                                                  checked by a read
//   smcmidi sinco-sector0 --port S --raw RAW [--in SECTOR] [--apply] [--repair] [--chunk N]
//                                                  rewrite sector 0: SECTOR (default: RAW's own
//                                                  sector 0, the rehearsal); a plan unless --apply
//   smcmidi selftest                               checks of this build that need no device
// Every command takes --log F: all sent and received MIDI goes there as JSON lines with
// host-clock timestamps (ns), so a session can be analysed afterwards
// (tools/stage0/capture_report.py for `session` logs).
//
// Safety:
// - The upgrade-mode SysEx cannot be built by this program.
// - Erase and write packets are built only for a checked sector (SysexSector): whole 4 KB sectors
//   inside --allow, never sector 0, the stock area and its VM (0x1000..0x25000, the running
//   firmware), the stock update loader (0x6E000..0x7B000) or BTIF, EXIF, USRFLASH.
// - sinco-write writes only sectors that read back erased, and checks every one by a read.
// - Sector 0 only through sinco-sector0: RAW must be a raw read of this chip from the ROM mode
//   (jlrom read), the chip's sector 0 must equal it (unless --repair), and SECTOR may differ from
//   it only in the boot pointer (bytes 0x60..0x67, a valid entry). It erases, checks the erase
//   (stops with nothing written if the erase did not take), writes, checks, up to 3 times.
// - The stock firmware answers erase and write with the same acknowledgement whatever happened
//   (docs/hardware.md, "Stock service protocol"): every result here comes from a read.
// - No command here changes the flash without --allow or --apply.

import CoreMIDI
import CryptoKit
import Foundation

// MARK: - Time

var timebase = mach_timebase_info_data_t()
mach_timebase_info(&timebase)
func hostToNs(_ t: UInt64) -> UInt64 { t * UInt64(timebase.numer) / UInt64(timebase.denom) }
func nowNs() -> UInt64 { hostToNs(mach_absolute_time()) }

// MARK: - Output

func hex(_ b: [UInt8]) -> String { b.map { String(format: "%02X", $0) }.joined(separator: " ") }

func fail(_ msg: String) -> Never {
    FileHandle.standardError.write(("smcmidi: " + msg + "\n").data(using: .utf8)!)
    exit(1)
}

func info(_ msg: String) {
    FileHandle.standardError.write((msg + "\n").data(using: .utf8)!)
}

final class EventLog: @unchecked Sendable {
    private let fh: FileHandle?
    private let lock = NSLock()

    init(path: String?) {
        guard let path else { fh = nil; return }
        FileManager.default.createFile(atPath: path, contents: nil)
        guard let h = FileHandle(forWritingAtPath: path) else { fail("cannot open log \(path)") }
        fh = h
    }

    func write(_ obj: [String: Any]) {
        guard let fh else { return }
        var o = obj
        if o["t_ns"] == nil { o["t_ns"] = nowNs() }
        guard let d = try? JSONSerialization.data(withJSONObject: o, options: [.sortedKeys]) else { return }
        lock.lock()
        fh.write(d)
        fh.write("\n".data(using: .utf8)!)
        lock.unlock()
    }
}

// MARK: - MIDI endpoints

func stringProp(_ obj: MIDIObjectRef, _ prop: CFString) -> String {
    var s: Unmanaged<CFString>?
    guard MIDIObjectGetStringProperty(obj, prop, &s) == noErr, let v = s?.takeRetainedValue() else { return "" }
    return v as String
}

func intProp(_ obj: MIDIObjectRef, _ prop: CFString) -> Int32? {
    var v: Int32 = 0
    return MIDIObjectGetIntegerProperty(obj, prop, &v) == noErr ? v : nil
}

struct Endpoint {
    let ref: MIDIEndpointRef
    let name: String
    let display: String
    let device: String
    let uid: Int32

    init(_ ref: MIDIEndpointRef) {
        self.ref = ref
        name = stringProp(ref, kMIDIPropertyName)
        display = stringProp(ref, kMIDIPropertyDisplayName)
        uid = intProp(ref, kMIDIPropertyUniqueID) ?? 0
        var entity: MIDIEntityRef = 0
        var dev: MIDIDeviceRef = 0
        if MIDIEndpointGetEntity(ref, &entity) == noErr, MIDIEntityGetDevice(entity, &dev) == noErr {
            device = stringProp(dev, kMIDIPropertyName)
        } else {
            device = ""
        }
    }

    func matches(_ s: String) -> Bool {
        let l = s.lowercased()
        return display.lowercased().contains(l) || name.lowercased().contains(l)
    }

    var json: [String: Any] { ["name": name, "display": display, "device": device, "uid": uid] }
}

func allSources() -> [Endpoint] { (0..<MIDIGetNumberOfSources()).map { Endpoint(MIDIGetSource($0)) } }
func allDestinations() -> [Endpoint] { (0..<MIDIGetNumberOfDestinations()).map { Endpoint(MIDIGetDestination($0)) } }

/// The one endpoint whose name contains `s`; a display name equal to `s` wins over partial matches.
func pick(_ list: [Endpoint], _ s: String, _ kind: String) -> Endpoint {
    if let exact = list.first(where: { $0.display.lowercased() == s.lowercased() }) { return exact }
    let m = list.filter { $0.matches(s) }
    if m.count == 1 { return m[0] }
    let names = list.map { "  \($0.display)" }.joined(separator: "\n")
    fail("\(m.isEmpty ? "no" : "more than one") \(kind) matches \"\(s)\"; available:\n\(names)")
}

// MARK: - MIDI stream parsing

func describe(_ m: [UInt8]) -> String {
    guard let s = m.first else { return "" }
    let ch = Int(s & 0x0F) + 1
    switch s & 0xF0 {
    case 0x80: return "note_off ch\(ch) \(m[1]) \(m[2])"
    case 0x90: return m[2] == 0 ? "note_off ch\(ch) \(m[1]) 0" : "note_on ch\(ch) \(m[1]) \(m[2])"
    case 0xA0: return "poly_at ch\(ch) \(m[1]) \(m[2])"
    case 0xB0: return "cc ch\(ch) \(m[1]) \(m[2])"
    case 0xC0: return "pc ch\(ch) \(m[1])"
    case 0xD0: return "chan_at ch\(ch) \(m[1])"
    case 0xE0: return "pitch ch\(ch) \(Int(m[1]) | Int(m[2]) << 7)"
    default: return s == 0xF0 ? "sysex \(m.count)B" : String(format: "sys %02X", s)
    }
}

func dataLength(_ status: UInt8) -> Int {
    switch status {
    case 0x80...0xBF, 0xE0...0xEF, 0xF2: return 2
    case 0xC0...0xDF, 0xF1, 0xF3: return 1
    default: return 0
    }
}

/// Splits a source's byte stream into whole messages (SysEx may span packets).
struct StreamParser {
    var sysex: [UInt8]? = nil
    var running: UInt8 = 0
    var cur: [UInt8] = []

    mutating func feed(_ bytes: [UInt8], _ emit: ([UInt8]) -> Void) {
        for b in bytes {
            if b >= 0xF8 { emit([b]); continue }            // real-time, may interleave
            if b == 0xF0 { sysex = [b]; continue }
            if var sx = sysex {
                if b == 0xF7 { sx.append(b); sysex = nil; emit(sx); continue }
                if b < 0x80 { sx.append(b); sysex = sx; continue }
                sysex = nil                                    // aborted by a status byte
            }
            if b >= 0x80 {
                running = b < 0xF0 ? b : 0
                cur = [b]
                if dataLength(b) == 0 { emit(cur); cur = [] }
                continue
            }
            if cur.isEmpty {
                guard running != 0 else { continue }
                cur = [running]
            }
            cur.append(b)
            if cur.count == 1 + dataLength(cur[0]) { emit(cur); cur = [] }
        }
    }
}

// MARK: - Receiver

enum Echo {
    case none
    case json       // one JSON line per message on stdout
    case compact    // one short line per message, for a person following a session
}

final class Receiver: @unchecked Sendable {
    let sources: [Endpoint]
    let log: EventLog
    let echo: Echo
    private var parsers: [StreamParser]
    private let cond = NSCondition()
    private var sysexQueue: [(src: Int, bytes: [UInt8])] = []
    private var count = 0

    init(sources: [Endpoint], log: EventLog, echo: Echo) {
        self.sources = sources
        self.log = log
        self.echo = echo
        parsers = Array(repeating: StreamParser(), count: sources.count)
    }

    func handle(_ list: UnsafePointer<MIDIPacketList>, _ srcIndex: Int) {
        let dataOffset = MemoryLayout<MIDIPacket>.offset(of: \MIDIPacket.data)!
        for p in list.unsafeSequence() {
            let n = Int(p.pointee.length)
            let raw = UnsafeRawPointer(p).advanced(by: dataOffset).assumingMemoryBound(to: UInt8.self)
            let bytes = Array(UnsafeBufferPointer(start: raw, count: n))
            let t = p.pointee.timeStamp == 0 ? nowNs() : hostToNs(p.pointee.timeStamp)
            parsers[srcIndex].feed(bytes) { msg in self.deliver(msg, srcIndex, t) }
        }
    }

    private func deliver(_ msg: [UInt8], _ src: Int, _ t: UInt64) {
        let rec: [String: Any] = ["t_ns": t, "dir": "rx", "src": sources[src].display, "hex": hex(msg), "msg": describe(msg)]
        log.write(rec)
        switch echo {
        case .none:
            break
        case .json:
            if let d = try? JSONSerialization.data(withJSONObject: rec, options: [.sortedKeys]) {
                print(String(data: d, encoding: .utf8)!)
            }
        case .compact:
            print("    \(sources[src].display)  \(describe(msg))")
        }
        fflush(stdout)
        cond.lock()
        count += 1
        if msg.first == 0xF0 {
            sysexQueue.append((src, msg))
            cond.signal()
        }
        cond.unlock()
    }

    /// Messages delivered so far, from all sources.
    func received() -> Int {
        cond.lock(); defer { cond.unlock() }
        return count
    }

    func clear() {
        cond.lock(); sysexQueue.removeAll(); cond.unlock()
    }

    /// Next SysEx from `src`, or nil after `timeout` seconds.
    func nextSysex(from src: Int, timeout: Double) -> [UInt8]? {
        let deadline = Date(timeIntervalSinceNow: timeout)
        cond.lock()
        defer { cond.unlock() }
        while true {
            if let i = sysexQueue.firstIndex(where: { $0.src == src }) {
                return sysexQueue.remove(at: i).bytes
            }
            if !cond.wait(until: deadline) { return nil }
        }
    }
}

final class Midi {
    var client = MIDIClientRef()
    var inPort = MIDIPortRef()
    var outPort = MIDIPortRef()
    let receiver: Receiver
    let log: EventLog

    init(sources: [Endpoint], log: EventLog, echo: Echo) {
        self.log = log
        receiver = Receiver(sources: sources, log: log, echo: echo)
        // Device arrivals and departures go to the log, so a session shows when the mixer
        // was unplugged or power-cycled.
        var st = MIDIClientCreateWithBlock("smcmidi" as CFString, &client) { n in logSetupChange(n, log) }
        guard st == noErr else { fail("MIDIClientCreate: \(st)") }
        let r = receiver
        st = MIDIInputPortCreateWithBlock(client, "in" as CFString, &inPort) { list, refCon in
            r.handle(list, Int(bitPattern: refCon) - 1)
        }
        guard st == noErr else { fail("MIDIInputPortCreate: \(st)") }
        st = MIDIOutputPortCreate(client, "out" as CFString, &outPort)
        guard st == noErr else { fail("MIDIOutputPortCreate: \(st)") }
        for (i, s) in sources.enumerated() {
            st = MIDIPortConnectSource(inPort, s.ref, UnsafeMutableRawPointer(bitPattern: i + 1))
            guard st == noErr else { fail("connect \(s.display): \(st)") }
        }
    }

    func send(_ bytes: [UInt8], to dest: Endpoint) {
        let size = 1024 + bytes.count
        let raw = UnsafeMutableRawPointer.allocate(byteCount: size, alignment: 8)
        defer { raw.deallocate() }
        let list = raw.bindMemory(to: MIDIPacketList.self, capacity: 1)
        // The buffer is sized for one packet of `bytes`, so the add always fits.
        _ = MIDIPacketListAdd(list, size, MIDIPacketListInit(list), 0, bytes.count, bytes)
        log.write(["dir": "tx", "dst": dest.display, "hex": hex(bytes), "msg": describe(bytes)])
        let st = MIDISend(outPort, dest.ref, list)
        if st != noErr { fail("MIDISend: \(st)") }
    }
}

func logSetupChange(_ n: UnsafePointer<MIDINotification>, _ log: EventLog) {
    let id = n.pointee.messageID
    var rec: [String: Any] = ["dir": "meta", "event": "midi_setup", "id": Int(id.rawValue)]
    switch id {
    case .msgSetupChanged: rec["what"] = "setup_changed"
    case .msgObjectAdded, .msgObjectRemoved:
        rec["what"] = id == .msgObjectAdded ? "object_added" : "object_removed"
        n.withMemoryRebound(to: MIDIObjectAddRemoveNotification.self, capacity: 1) { a in
            rec["child"] = stringProp(a.pointee.child, kMIDIPropertyName)
            rec["child_type"] = Int(a.pointee.childType.rawValue)
        }
        info("midi: \(rec["what"]!) \(rec["child"]!)")
    case .msgPropertyChanged:
        rec["what"] = "property_changed"
        n.withMemoryRebound(to: MIDIObjectPropertyChangeNotification.self, capacity: 1) { p in
            rec["object"] = stringProp(p.pointee.object, kMIDIPropertyName)
            rec["property"] = p.pointee.propertyName.takeUnretainedValue() as String
        }
    case .msgIOError:
        rec["what"] = "io_error"
        info("midi: I/O error")
    default: break
    }
    log.write(rec)
}

// MARK: - Stock service protocol

/// The commands this program can build. The upgrade-mode SysEx is deliberately absent. Erase and
/// write packets come only from `erasePacket` and `writePacket`, which take a checked sector.
enum SincoCmd: UInt8 {
    case query = 0x11
    case erase = 0x21
    case write = 0x22
    case read = 0x23
}

func sincoPacket(_ cmd: SincoCmd, _ payload: [UInt8]) -> [UInt8] {
    precondition(cmd != .erase && cmd != .write, "erase and write packets come from erasePacket/writePacket")
    return anySincoPacket(cmd, payload)
}

private func anySincoPacket(_ cmd: SincoCmd, _ payload: [UInt8]) -> [UInt8] {
    let n = payload.count
    let sum = payload.reduce(0) { $0 &+ $1 }
    return [0x00, 0x59, cmd.rawValue, UInt8(n & 0xFF), UInt8((n >> 8) & 0xFF), UInt8((n >> 16) & 0xFF)] + payload + [~sum]
}

/// 8-bit bytes bit-packed LSB-first into 7-bit bytes, wrapped in F0 ... F7.
func pack7(_ data: [UInt8]) -> [UInt8] {
    var out: [UInt8] = [0xF0]
    var acc = 0, bits = 0
    for b in data {
        acc |= Int(b) << bits
        bits += 8
        while bits >= 7 { out.append(UInt8(acc & 0x7F)); acc >>= 7; bits -= 7 }
    }
    if bits > 0 { out.append(UInt8(acc & 0x7F)) }
    out.append(0xF7)
    return out
}

func unpack7(_ sysex: [UInt8]) -> [UInt8] {
    let body = sysex.first == 0xF0 ? Array(sysex.dropFirst().dropLast()) : sysex
    var out: [UInt8] = []
    var acc = 0, bits = 0
    for b in body {
        acc |= Int(b & 0x7F) << bits
        bits += 7
        while bits >= 8 { out.append(UInt8(acc & 0xFF)); acc >>= 8; bits -= 8 }
    }
    return out
}

struct SincoReply {
    let cmd: UInt8
    let payload: [UInt8]
    let checksumOK: Bool
}

func parseSinco(_ pkt: [UInt8]) -> SincoReply? {
    guard pkt.count >= 7, pkt[0] == 0x00, pkt[1] == 0x59 else { return nil }
    let n = Int(pkt[3]) | Int(pkt[4]) << 8 | Int(pkt[5]) << 16
    let end = min(6 + n, pkt.count)
    let payload = Array(pkt[6..<end])
    let sum = payload.reduce(0) { $0 &+ $1 }
    return SincoReply(cmd: pkt[2], payload: payload, checksumOK: pkt.count > 6 + n && pkt[6 + n] == ~sum)
}

final class SincoLink {
    let midi: Midi
    let dest: Endpoint
    let srcIndex = 0

    init(port: String, log: EventLog) {
        let src = pick(allSources(), port, "source")
        dest = pick(allDestinations(), port, "destination")
        info("source: \(src.display)   destination: \(dest.display)")
        midi = Midi(sources: [src], log: log, echo: .none)
    }

    func request(_ cmd: SincoCmd, _ payload: [UInt8], timeout: Double) -> SincoReply? {
        send(sincoPacket(cmd, payload), timeout: timeout)
    }

    /// One packet out, the first SINCO reply back (or nil after `timeout` seconds).
    func send(_ packet: [UInt8], timeout: Double) -> SincoReply? {
        midi.receiver.clear()
        midi.send(pack7(packet), to: dest)
        let deadline = Date(timeIntervalSinceNow: timeout)
        while true {
            let left = deadline.timeIntervalSinceNow
            guard left > 0, let sx = midi.receiver.nextSysex(from: srcIndex, timeout: left) else { return nil }
            if let r = parseSinco(unpack7(sx)) { return r }
        }
    }

    func read(addr: Int, len: Int, type: UInt8 = 0, retries: Int = 3) -> [UInt8] {
        let payload: [UInt8] = [type] + (0..<4).map { UInt8((addr >> (8 * $0)) & 0xFF) } + (0..<3).map { UInt8((len >> (8 * $0)) & 0xFF) }
        for attempt in 1...retries {
            guard let r = request(.read, payload, timeout: 1.0) else {
                info(String(format: "read 0x%06X: no reply (attempt %d)", addr, attempt))
                continue
            }
            guard r.cmd == SincoCmd.read.rawValue else {
                fail(String(format: "read 0x%06X refused: cmd %02X payload %@", addr, r.cmd, hex(r.payload)))
            }
            let p = r.payload
            let raddr = p.count >= 5 ? Int(p[1]) | Int(p[2]) << 8 | Int(p[3]) << 16 | Int(p[4]) << 24 : -1
            let rlen = p.count >= 8 ? Int(p[5]) | Int(p[6]) << 8 | Int(p[7]) << 16 : -1
            if r.checksumOK, p.count == 8 + len, p[0] == type, raddr == addr, rlen == len {
                return Array(p[8...])
            }
            info(String(format: "read 0x%06X: bad reply (attempt %d, chk %@, len %d)", addr, attempt, r.checksumOK ? "ok" : "BAD", p.count))
        }
        fail(String(format: "read 0x%06X failed after %d attempts", addr, retries))
    }
}

// MARK: - Erase and write through the stock firmware (SPEC Q-18)
//
// What the stock code does (docs/hardware.md, "Stock service protocol", static analysis of V012):
// 0x21 erases the 4 KB sector holding the address; 0x22 programs the bytes as received; both
// always answer the same acknowledgement, so only a read shows what happened. Reads at and above
// the stock VM (0x24000) return the flash as stored; sector 0 comes back XORed with the ENC
// keystream of 0xFFFF, restarted at every read call.

struct ToolError: Error, CustomStringConvertible {
    let description: String
    init(_ s: String) { description = s }
}

func hex32(_ v: Int) -> String { String(format: "0x%05X", v) }

/// A 4 KB sector this program may erase or write through the stock firmware.
struct SysexSector {
    static let size = 0x1000
    static let flashEnd = 0x80000
    /// Never written by `sinco-erase`/`sinco-write`: sector 0 (the uboot and the boot pointer; only
    /// `sinco-sector0` writes it), the stock area and its VM (the running firmware's code and
    /// settings), the stock update loader, and BTIF, EXIF, USRFLASH (Bluetooth keys, factory data,
    /// the control tables). See docs/image.md, "The layout".
    static let forbidden = [0x00000..<0x01000, 0x01000..<0x25000, 0x6E000..<0x7B000, 0x7B000..<0x7F000]
    static func isForbidden(_ a: Int) -> Bool { forbidden.contains { $0.contains(a) } }

    let address: Int
    private init(_ a: Int) { address = a }

    /// For `sinco-erase` and `sinco-write`: a whole sector inside the range the command line
    /// allows, never a forbidden one.
    static func within(_ a: Int, allowed: Range<Int>) throws -> SysexSector {
        guard a >= 0, a % size == 0, a + size <= flashEnd else {
            throw ToolError("\(hex32(a)) is not the start of a 4 KB sector of the 512 KB flash")
        }
        guard allowed.lowerBound <= a, a + size <= allowed.upperBound else {
            throw ToolError("sector \(hex32(a)) is outside --allow \(hex32(allowed.lowerBound)):\(hex32(allowed.upperBound))")
        }
        guard !isForbidden(a) else {
            throw ToolError("sector \(hex32(a)) is forbidden (sector 0, the stock area and VM, the update loader, BTIF, EXIF, USRFLASH)")
        }
        return SysexSector(a)
    }

    /// Sector 0, only for a plan whose checks passed.
    static func boot(_ plan: Sector0Plan) -> SysexSector { SysexSector(0) }
}

func le(_ v: Int, _ n: Int) -> [UInt8] { (0..<n).map { UInt8(truncatingIfNeeded: v >> (8 * $0)) } }

/// 0x21, type 0 (the flash): the sector's address.
func erasePacket(_ s: SysexSector) -> [UInt8] {
    anySincoPacket(.erase, [0] + le(s.address, 4))
}

/// 0x22, type 0: address, length, data. Pages never straddle: offset and length stay inside one
/// 256-byte page.
let maxWriteChunk = 256
func writePacket(_ s: SysexSector, offset: Int, _ data: [UInt8]) -> [UInt8] {
    precondition(offset >= 0 && !data.isEmpty && data.count <= maxWriteChunk && offset + data.count <= SysexSector.size)
    precondition(offset / 256 == (offset + data.count - 1) / 256)
    return anySincoPacket(.write, [0] + le(s.address + offset, 4) + le(data.count, 3) + data)
}

/// The stock firmware's reply to an erase or a write: command 0, one status byte 0.
func isAck(_ r: SincoReply) -> Bool { r.cmd == 0 && r.payload == [0] && r.checksumOK }

/// JieLi's ENC keystream (16-bit LFSR, polynomial 0x1021) XORed into `data`; symmetric.
func enc(_ data: [UInt8], key: UInt16) -> [UInt8] {
    var k = key
    return data.map { b in
        defer { k = k & 0x8000 != 0 ? (k << 1) ^ 0x1021 : k << 1 }
        return b ^ UInt8(truncatingIfNeeded: k)
    }
}

/// Sector 0 as stored, from what 256-byte reads (0x23) return: each block went through the
/// keystream of 0xFFFF from its start.
func sector0Stored(fromReads d: [UInt8]) -> [UInt8] {
    stride(from: 0, to: d.count, by: 256).flatMap { enc(Array(d[$0..<min($0 + 256, d.count)]), key: 0xFFFF) }
}

func crc16(_ data: [UInt8]) -> UInt16 {
    var crc: UInt16 = 0
    for b in data {
        crc ^= UInt16(b) << 8
        for _ in 0..<8 { crc = crc & 0x8000 != 0 ? (crc << 1) ^ 0x1021 : crc << 1 }
    }
    return crc
}

/// A stock read (0x23 over the whole flash) has the area below 0x24000 decrypted, so the JLFS
/// directory at 0x1000 is plain and its first entry's header CRC holds; in a raw read (jlrom) it
/// is SFC-encrypted.
func looksLikeStockRead(_ d: [UInt8]) -> Bool {
    guard d.count >= 0x1020 else { return false }
    let e = Array(d[0x1000..<0x1020])
    return crc16(Array(e[2...])) == UInt16(e[0]) | UInt16(e[1]) << 8
}

/// The `app_dir_head` entry of sector 0 (docs/image.md, "Sector 0"): stored ENC-encrypted with
/// 0xFFFF on its own. Returns its offset field if the header CRC holds.
let bootEntry = 0x60..<0x80
func bootPointer(_ stored: [UInt8]) -> Int? {
    let e = enc(Array(stored[bootEntry]), key: 0xFFFF)
    guard crc16(Array(e[2...])) == UInt16(e[0]) | UInt16(e[1]) << 8 else { return nil }
    return Int(e[4]) | Int(e[5]) << 8 | Int(e[6]) << 16 | Int(e[7]) << 24
}

/// What `sinco-sector0` writes, checked against a raw read of the same chip (jlrom): the new
/// sector equals the raw read's sector 0, or differs from it only in the boot pointer's header
/// CRC and offset, and both have a valid boot pointer entry.
struct Sector0Plan {
    static let mayDiffer = 0x60..<0x68
    let before: [UInt8]
    let after: [UInt8]
    let differing: [Int]

    init(raw: [UInt8], sector: [UInt8]) throws {
        guard raw.count == SysexSector.flashEnd else { throw ToolError("the raw read must be 512 KB, not \(raw.count) bytes") }
        guard !looksLikeStockRead(raw) else {
            throw ToolError("this is a stock read (0x23): sector 0 needs the raw read from the ROM mode (jlrom read)")
        }
        guard sector.count == SysexSector.size else { throw ToolError("the sector must be 4096 bytes, not \(sector.count)") }
        let b = Array(raw[0..<SysexSector.size])
        guard bootPointer(b) != nil else { throw ToolError("the raw read's sector 0 has no valid boot pointer entry") }
        guard bootPointer(sector) != nil else { throw ToolError("the new sector 0 has no valid boot pointer entry") }
        let diff = (0..<SysexSector.size).filter { b[$0] != sector[$0] }
        if let i = diff.first(where: { !Sector0Plan.mayDiffer.contains($0) }) {
            throw ToolError("the new sector 0 differs from the raw read at \(hex32(i)): only bytes 0x60..0x67 (the boot pointer) may")
        }
        before = b
        after = sector
        differing = diff
    }
}

/// Reads one sector in 256-byte blocks.
func readSector(_ link: SincoLink, _ addr: Int) -> [UInt8] {
    stride(from: 0, to: SysexSector.size, by: 256).flatMap { link.read(addr: addr + $0, len: 256) }
}

/// One erase or write packet, resent on no reply (both are harmless to repeat with the same data).
func sendChecked(_ link: SincoLink, _ packet: [UInt8], _ what: String) throws {
    for attempt in 1...3 {
        guard let r = link.send(packet, timeout: 3.0) else {
            info("\(what): no reply (attempt \(attempt))")
            continue
        }
        guard isAck(r) else {
            throw ToolError("\(what): refused, cmd \(String(format: "%02X", r.cmd)) payload \(hex(r.payload))")
        }
        return
    }
    throw ToolError("\(what): no reply after 3 attempts")
}

func eraseSector(_ link: SincoLink, _ s: SysexSector) throws {
    link.midi.log.write(["dir": "meta", "event": "erase", "addr": s.address])
    try sendChecked(link, erasePacket(s), "erase \(hex32(s.address))")
}

/// Programs `data` (one sector) into an erased sector, page by page; all-0xFF pieces are skipped.
func programSector(_ link: SincoLink, _ s: SysexSector, _ data: [UInt8], chunk: Int) throws {
    precondition(data.count == SysexSector.size && chunk > 0 && 256 % chunk == 0)
    link.midi.log.write(["dir": "meta", "event": "write", "addr": s.address, "crc16": Int(crc16(data))])
    for off in stride(from: 0, to: SysexSector.size, by: chunk) {
        let piece = Array(data[off..<(off + chunk)])
        if piece.allSatisfy({ $0 == 0xFF }) { continue }
        try sendChecked(link, writePacket(s, offset: off, piece), "write \(hex32(s.address + off))")
    }
}

/// The data `sinco-write --pattern` writes; the same as `jlrom write --pattern`.
func testPattern(_ start: Int, _ size: Int) -> [UInt8] {
    (0..<size).map { i in
        let a = start + i
        return UInt8(truncatingIfNeeded: (a &* 7 &+ 3) ^ (a >> 8) ^ (a >> 12)) & 0x7F
    }
}

func parseInt(_ s: String) -> Int? {
    s.hasPrefix("0x") ? Int(s.dropFirst(2), radix: 16) : Int(s)
}

/// `--allow START:END`: whole sectors, none of them forbidden.
func parseAllow(_ s: String) throws -> Range<Int> {
    let p = s.split(separator: ":", omittingEmptySubsequences: false).map(String.init)
    guard p.count == 2, let lo = parseInt(p[0]), let hi = parseInt(p[1]), lo < hi, hi <= SysexSector.flashEnd,
          lo % SysexSector.size == 0, hi % SysexSector.size == 0 else {
        throw ToolError("bad --allow \(s): START:END in whole 4 KB sectors inside the flash, for example 0x7F000:0x80000")
    }
    if let f = SysexSector.forbidden.first(where: { $0.overlaps(lo..<hi) }) {
        throw ToolError("--allow \(s) overlaps the forbidden \(hex32(f.lowerBound)):\(hex32(f.upperBound))")
    }
    return lo..<hi
}

// MARK: - Guided capture

/// A capture scenario (tools/stage0/scenarios/*.json). A step without `seconds` waits for the
/// operator; `ask` is a question whose typed answer goes to the log after the step.
struct Scenario: Decodable {
    struct Step: Decodable {
        let id: String
        let text: String
        let seconds: Double?
        let ask: String?
    }
    let name: String
    let description: String?
    let steps: [Step]
}

func loadScenario(_ path: String) -> Scenario {
    guard let d = FileManager.default.contents(atPath: path) else { fail("cannot read \(path)") }
    let sc: Scenario
    do { sc = try JSONDecoder().decode(Scenario.self, from: d) } catch { fail("\(path): \(error)") }
    var seen = Set<String>()
    for s in sc.steps where !seen.insert(s.id).inserted { fail("\(path): duplicate step id \(s.id)") }
    guard !sc.steps.isEmpty else { fail("\(path): no steps") }
    return sc
}

/// Runs the steps on the calling thread; returns how the session ended ("done", "quit", "eof").
func runScenario(_ sc: Scenario, _ midi: Midi, _ log: EventLog) -> String {
    print("\(sc.name)\n\(sc.description ?? "")")
    print("Enter — step done, s — skip, r — redo, q — quit; any other text is saved as a note.")
    func prompt() -> String? {
        print("> ", terminator: "")
        fflush(stdout)
        return readLine()?.trimmingCharacters(in: .whitespaces)
    }
    var i = 0
    while i < sc.steps.count {
        let step = sc.steps[i]
        print("\n[\(i + 1)/\(sc.steps.count)] \(step.text)")
        let before = midi.receiver.received()
        log.write(["dir": "marker", "event": "step_begin", "step": step.id, "index": i + 1, "text": step.text])
        var result = "done"
        if let s = step.seconds {
            print("    (\(Int(s)) s, no key needed)")
            fflush(stdout)
            Thread.sleep(forTimeInterval: s)
        } else {
            keys: while true {
                guard let line = prompt() else { result = "eof"; break }
                switch line {
                case "": break keys
                case "s": result = "skipped"; break keys
                case "r": result = "redo"; break keys
                case "q": result = "quit"; break keys
                default:
                    log.write(["dir": "marker", "event": "note", "step": step.id, "text": line])
                    print("    note saved")
                }
            }
        }
        let got = midi.receiver.received() - before
        log.write(["dir": "marker", "event": "step_end", "step": step.id, "index": i + 1, "result": result, "messages": got])
        print("    \(got) message(s)\(result == "done" ? "" : ", " + result)")
        if result == "quit" || result == "eof" { return result }
        if result == "done", let q = step.ask {
            print("    \(q)")
            let answer = prompt()
            log.write(["dir": "marker", "event": "answer", "step": step.id, "question": q, "text": answer ?? ""])
            if answer == nil { return "eof" }
        }
        if result != "redo" { i += 1 }
    }
    return "done"
}

// MARK: - Commands

var args = Array(CommandLine.arguments.dropFirst())

/// "B0 14 64, 90 48 7F": channel messages of the right length, or nil (SysEx and system messages
/// are refused: this command must not reach the stock firmware's service protocol).
func parseChannelMessages(_ s: String) -> [[UInt8]]? {
    var out: [[UInt8]] = []
    for part in s.split(separator: ",") {
        let bytes = part.split(separator: " ").map { UInt8($0, radix: 16) }
        guard !bytes.isEmpty, !bytes.contains(nil) else { return nil }
        let b = bytes.map { $0! }
        guard b[0] >= 0x80 && b[0] < 0xF0, b.count == 1 + dataLength(b[0]), !b.dropFirst().contains(where: { $0 >= 0x80 }) else { return nil }
        out.append(b)
    }
    return out.isEmpty ? nil : out
}

func option(_ name: String) -> String? {
    guard let i = args.firstIndex(of: name) else { return nil }
    guard i + 1 < args.count else { fail("\(name) needs a value") }
    let v = args[i + 1]
    args.removeSubrange(i...(i + 1))
    return v
}

func intOption(_ name: String, _ def: Int) -> Int {
    guard let s = option(name) else { return def }
    let v = s.hasPrefix("0x") ? Int(s.dropFirst(2), radix: 16) : Int(s)
    guard let v else { fail("bad number for \(name): \(s)") }
    return v
}

func flag(_ name: String) -> Bool {
    guard let i = args.firstIndex(of: name) else { return false }
    args.remove(at: i)
    return true
}

/// The commands that change the flash refuse anything they did not understand.
func noMoreArgs() {
    if !args.isEmpty { fail("unexpected arguments: \(args.joined(separator: " "))") }
}

func loadFile(_ path: String) -> [UInt8] {
    guard let d = FileManager.default.contents(atPath: path) else { fail("cannot read \(path)") }
    return [UInt8](d)
}

func printJSON(_ o: Any) {
    let d = try! JSONSerialization.data(withJSONObject: o, options: [.sortedKeys, .prettyPrinted])
    print(String(data: d, encoding: .utf8)!)
}

/// What to do when a sector 0 write stopped after its erase.
let sector0Hint = """
    Sector 0 may now be erased or half written. Do not switch the mixer off: the stock firmware
    keeps running without it, but the chip needs it at the next start. Run the same command with
    --repair to write it again, or recover in the ROM mode with the key:
    jlrom restore --in RAW --apply --before F --protected (docs/recovery.md).
    """

guard let command = args.first else {
    fail("usage: smcmidi list | listen | session | sinco-info | sinco-read | sinco-erase | sinco-write | sinco-sector0 | selftest  (see the header of main.swift)")
}
args.removeFirst()
let log = EventLog(path: option("--log"))
log.write(["dir": "meta", "event": "start", "argv": CommandLine.arguments, "wall": ISO8601DateFormatter().string(from: Date())])

switch command {
case "list":
    printJSON(["sources": allSources().map(\.json), "destinations": allDestinations().map(\.json)])

case "listen":
    let match = option("--match")
    let seconds = Double(intOption("--seconds", 0))
    let srcs = allSources().filter { match == nil || $0.matches(match!) }
    guard !srcs.isEmpty else { fail("no MIDI source matches") }
    info("listening to: " + srcs.map(\.display).joined(separator: ", ") + (seconds > 0 ? " for \(Int(seconds)) s" : " (Ctrl-C to stop)"))
    let midi = Midi(sources: srcs, log: log, echo: .json)
    signal(SIGINT, SIG_IGN)
    let sig = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
    sig.setEventHandler {
        log.write(["dir": "meta", "event": "stop"])
        exit(0)
    }
    sig.resume()
    withExtendedLifetime(midi) {
        if seconds > 0 { RunLoop.main.run(until: Date(timeIntervalSinceNow: seconds)) } else { RunLoop.main.run() }
    }
    log.write(["dir": "meta", "event": "stop"])

case "send":
    guard let port = option("--port") else { fail("--port is required") }
    guard let text = option("--msgs") else { fail("--msgs is required") }
    let gap = intOption("--gap-ms", 5)
    guard let msgs = parseChannelMessages(text) else { fail("--msgs: channel messages only, e.g. \"B0 14 64, 90 48 7F\"") }
    let dest = pick(allDestinations(), port, "destination")
    info("sending \(msgs.count) messages to \(dest.display), \(gap) ms apart")
    let midi = Midi(sources: [], log: log, echo: .none)
    for m in msgs {
        midi.send(m, to: dest)
        RunLoop.main.run(until: Date(timeIntervalSinceNow: Double(gap) / 1000))
    }
    RunLoop.main.run(until: Date(timeIntervalSinceNow: 0.2))
    log.write(["dir": "meta", "event": "stop"])

case "session":
    guard let path = option("--scenario") else { fail("--scenario is required") }
    let match = option("--match") ?? "SMC-Mixer"
    let scenario = loadScenario(path)
    let srcs = allSources().filter { $0.matches(match) }
    guard !srcs.isEmpty else { fail("no MIDI source matches \"\(match)\"") }
    log.write(["dir": "meta", "event": "scenario", "name": scenario.name, "file": path, "steps": scenario.steps.count, "sources": srcs.map(\.json)])
    info("listening to: " + srcs.map(\.display).joined(separator: ", "))
    let midi = Midi(sources: srcs, log: log, echo: .compact)
    // The steps run on their own thread; the main run loop stays free for CoreMIDI
    // notifications and Ctrl-C.
    let runner = Thread {
        let how = runScenario(scenario, midi, log)
        log.write(["dir": "meta", "event": "stop", "how": how])
        print("\nsession \(how)")
        exit(0)
    }
    runner.stackSize = 1 << 20
    signal(SIGINT, SIG_IGN)
    let sig = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
    sig.setEventHandler {
        log.write(["dir": "meta", "event": "stop", "how": "interrupted"])
        print("\nsession interrupted")
        exit(130)
    }
    sig.resume()
    runner.start()
    withExtendedLifetime(midi) { RunLoop.main.run() }

case "sinco-info":
    guard let port = option("--port") else { fail("--port is required") }
    let link = SincoLink(port: port, log: log)
    guard let r = link.request(.query, [], timeout: 2.0) else { fail("no reply to query (0x11)") }
    let text = String(decoding: r.payload.filter { $0 >= 0x20 && $0 < 0x7F }, as: UTF8.self)
    printJSON(["cmd": String(format: "0x%02X", r.cmd), "checksum_ok": r.checksumOK, "payload_hex": hex(r.payload), "text": text])

case "sinco-read":
    guard let port = option("--port"), let out = option("--out") else { fail("--port and --out are required") }
    let start = intOption("--start", 0)
    let size = intOption("--size", 0x80000)
    let chunk = intOption("--chunk", 256)
    guard chunk > 0, chunk <= 1024 else { fail("--chunk must be 1...1024") }
    let link = SincoLink(port: port, log: log)
    var data: [UInt8] = []
    data.reserveCapacity(size)
    let t0 = Date()
    var addr = start
    while addr < start + size {
        let n = min(chunk, start + size - addr)
        data += link.read(addr: addr, len: n)
        addr += n
        if (addr - start) % 0x8000 == 0 || addr == start + size {
            let kbs = Double(addr - start) / 1024 / max(Date().timeIntervalSince(t0), 0.001)
            FileHandle.standardError.write(String(format: "\r0x%06X / 0x%06X  %.1f KB/s", addr - start, size, kbs).data(using: .utf8)!)
        }
    }
    info("")
    guard FileManager.default.createFile(atPath: out, contents: Data(data)) else { fail("cannot write \(out)") }
    let sha = SHA256.hash(data: Data(data)).map { String(format: "%02x", $0) }.joined()
    log.write(["dir": "meta", "event": "dump", "out": out, "start": start, "size": size, "sha256": sha])
    printJSON(["out": out, "start": String(format: "0x%X", start), "size": String(format: "0x%X", size), "sha256": sha, "seconds": Date().timeIntervalSince(t0)])

case "sinco-erase":
    guard let port = option("--port"), let addrS = option("--addr"), let allowS = option("--allow") else {
        fail("--port, --addr and --allow are required")
    }
    let size = intOption("--size", SysexSector.size)
    noMoreArgs()
    do {
        guard let a = parseInt(addrS) else { throw ToolError("bad --addr \(addrS)") }
        let allowed = try parseAllow(allowS)
        guard size > 0, size % SysexSector.size == 0 else { throw ToolError("--size must be whole 4 KB sectors") }
        let sectors = try stride(from: a, to: a + size, by: SysexSector.size).map { try SysexSector.within($0, allowed: allowed) }
        let link = SincoLink(port: port, log: log)
        var out: [[String: Any]] = []
        for s in sectors {
            let t0 = Date()
            try eraseSector(link, s)
            guard readSector(link, s.address).allSatisfy({ $0 == 0xFF }) else {
                throw ToolError("sector \(hex32(s.address)) does not read back erased: the erase did not take (write protection?)")
            }
            log.write(["dir": "meta", "event": "sector_erased", "addr": s.address])
            out.append(["addr": hex32(s.address), "erased": true, "seconds": Date().timeIntervalSince(t0)])
        }
        printJSON(["result": "ok", "sectors": out])
    } catch {
        fail("\(error)")
    }

case "sinco-write":
    guard let port = option("--port"), let addrS = option("--addr"), let allowS = option("--allow") else {
        fail("--port, --addr and --allow are required")
    }
    let inPath = option("--in")
    let pattern = flag("--pattern")
    let size = intOption("--size", SysexSector.size)
    let chunk = intOption("--chunk", maxWriteChunk)
    noMoreArgs()
    do {
        guard let a = parseInt(addrS) else { throw ToolError("bad --addr \(addrS)") }
        guard chunk >= 16, chunk <= maxWriteChunk, 256 % chunk == 0 else { throw ToolError("--chunk must be 16, 32, 64, 128 or 256") }
        let allowed = try parseAllow(allowS)
        var data: [UInt8]
        switch (inPath, pattern) {
        case (let p?, false): data = loadFile(p)
        case (nil, true):
            guard size > 0 else { throw ToolError("--size must be positive") }
            data = testPattern(a, size)
        default: throw ToolError("give exactly one of --in F and --pattern")
        }
        guard !data.isEmpty else { throw ToolError("nothing to write") }
        // Whole sectors: the rest of the last one stays erased.
        data += [UInt8](repeating: 0xFF, count: (SysexSector.size - data.count % SysexSector.size) % SysexSector.size)
        let sectors = try stride(from: a, to: a + data.count, by: SysexSector.size).map { try SysexSector.within($0, allowed: allowed) }
        let link = SincoLink(port: port, log: log)
        // Every sector must read erased before anything is written: a wrong address cannot
        // overwrite data, and the program needs no erase of its own.
        for s in sectors where !readSector(link, s.address).allSatisfy({ $0 == 0xFF }) {
            throw ToolError("sector \(hex32(s.address)) is not erased (its contents are in the log): nothing written. sinco-erase it first if it may go")
        }
        var out: [[String: Any]] = []
        for s in sectors {
            let t0 = Date()
            let want = Array(data[(s.address - a)..<(s.address - a + SysexSector.size)])
            try programSector(link, s, want, chunk: chunk)
            let back = readSector(link, s.address)
            let bad = (0..<SysexSector.size).filter { back[$0] != want[$0] }
            log.write(["dir": "meta", "event": "sector_written", "addr": s.address, "crc16": Int(crc16(want)), "differing": bad.count])
            guard bad.isEmpty else {
                throw ToolError("sector \(hex32(s.address)) reads back different in \(bad.count) bytes, the first at \(hex32(s.address + bad[0])): the write did not take as sent")
            }
            out.append(["addr": hex32(s.address), "crc16": String(format: "%04X", crc16(want)), "seconds": Date().timeIntervalSince(t0)])
        }
        printJSON(["result": "ok", "sectors": out])
    } catch {
        fail("\(error)")
    }

case "sinco-sector0":
    guard let port = option("--port"), let rawPath = option("--raw") else { fail("--port and --raw are required") }
    let inPath = option("--in")
    let apply = flag("--apply")
    let repair = flag("--repair")
    let chunk = intOption("--chunk", maxWriteChunk)
    noMoreArgs()
    let raw = loadFile(rawPath)
    let plan: Sector0Plan
    do {
        guard chunk >= 16, chunk <= maxWriteChunk, 256 % chunk == 0 else { throw ToolError("--chunk must be 16, 32, 64, 128 or 256") }
        guard raw.count >= SysexSector.size else { throw ToolError("the raw read must be 512 KB, not \(raw.count) bytes") }
        plan = try Sector0Plan(raw: raw, sector: inPath.map(loadFile) ?? Array(raw[0..<SysexSector.size]))
    } catch {
        fail("\(error)")
    }
    let link = SincoLink(port: port, log: log)
    let now = sector0Stored(fromReads: readSector(link, 0))
    let state = now == plan.before ? "as_raw_read" : now == plan.after ? "as_asked" : "other"
    var report: [String: Any] = [
        "boot_pointer_before": hex32(bootPointer(plan.before)!), "boot_pointer_after": hex32(bootPointer(plan.after)!),
        "differing_bytes": plan.differing.map { String(format: "0x%02X", $0) },
        "crc16_before": String(format: "%04X", crc16(plan.before)), "crc16_after": String(format: "%04X", crc16(plan.after)),
        "chip_now": state, "apply": apply,
    ]
    log.write(["dir": "meta", "event": "sector0_plan", "chip_now": state, "differing": plan.differing.count, "apply": apply, "repair": repair])
    if state == "other" && !repair {
        printJSON(report)
        fail("the chip's sector 0 equals neither the raw read nor the new sector: it is not the chip the plan was made for. --repair writes anyway")
    }
    if state == "as_asked" && !plan.differing.isEmpty {
        printJSON(report)
        info("sector 0 is already as asked: nothing to do")
        exit(0)
    }
    guard apply else {
        printJSON(report)
        info("plan only: --apply erases sector 0 and writes it")
        exit(0)
    }
    do {
        let s = SysexSector.boot(plan)
        let t0 = nowNs()
        var tWritten: UInt64 = 0
        var attempts = 0
        var verified = false
        while attempts < 3 && !verified {
            attempts += 1
            try eraseSector(link, s)
            let erased = sector0Stored(fromReads: readSector(link, 0))
            if !erased.allSatisfy({ $0 == 0xFF }) {
                if attempts == 1 && erased == now {
                    printJSON(report)
                    fail("the erase did not take: sector 0 is unchanged (write protection?). Nothing was written")
                }
                info("sector 0 does not read back erased (attempt \(attempts)): erasing again")
                continue
            }
            try programSector(link, s, plan.after, chunk: chunk)
            tWritten = nowNs()
            verified = sector0Stored(fromReads: readSector(link, 0)) == plan.after
            if !verified { info("sector 0 reads back different (attempt \(attempts)): writing it again") }
        }
        let t1 = nowNs()
        report["attempts"] = attempts
        report["ms_erase_to_written"] = tWritten > t0 ? Double(tWritten - t0) / 1e6 : -1
        report["ms_erase_to_checked"] = Double(t1 - t0) / 1e6
        report["result"] = verified ? "ok" : "failed"
        log.write(["dir": "meta", "event": "sector0_done", "verified": verified, "attempts": attempts,
                   "ns_erase_to_written": tWritten > t0 ? Int(tWritten - t0) : -1, "ns_erase_to_checked": Int(t1 - t0)])
        printJSON(report)
        guard verified else { fail("sector 0 is not as asked after \(attempts) attempts.\n\(sector0Hint)") }
    } catch {
        fail("\(error)\n\(sector0Hint)")
    }

case "selftest":
    // No device: the packets, the guards and the sector 0 checks of this build.
    var failed = 0
    func check(_ ok: Bool, _ what: String) {
        if !ok { failed += 1 }
        print((ok ? "ok    " : "FAIL  ") + what)
    }
    func refused(_ f: () throws -> Any) -> Bool {
        do { _ = try f(); return false } catch { return true }
    }
    let all = 0..<SysexSector.flashEnd
    let sample = (0..<300).map { UInt8(truncatingIfNeeded: $0 &* 37 &+ 11) }
    check(unpack7(pack7(sample)) == sample, "7-bit packing undoes itself")
    check(parseChannelMessages("B0 14 64, C0 05, 90 48 7F")! == [[0xB0, 0x14, 0x64], [0xC0, 0x05], [0x90, 0x48, 0x7F]],
          "send: channel messages parsed")
    for bad in ["F0 00 59 F7", "F8", "B0 14", "B0 14 80", "C0 05 01", "", "B0 xx 01"] {
        check(parseChannelMessages(bad) == nil, "send refuses \"\(bad)\"")
    }
    check(pack7([0xFF]) == [0xF0, 0x7F, 0x01, 0xF7], "7-bit packing: LSB first")
    check(sincoPacket(.read, [0, 0, 0x10, 0, 0, 0, 1, 0]) == [0x00, 0x59, 0x23, 8, 0, 0, 0, 0, 0x10, 0, 0, 0, 1, 0, 0xEE],
          "read packet: 00 59 23, length, payload, inverted sum")
    let s7f = try! SysexSector.within(0x7F000, allowed: all)
    check(erasePacket(s7f) == [0x00, 0x59, 0x21, 5, 0, 0, 0, 0x00, 0xF0, 0x07, 0x00, 0x08],
          "erase packet: type 0, address little-endian")
    check(writePacket(s7f, offset: 0x100, [1, 2, 3]) == [0x00, 0x59, 0x22, 11, 0, 0, 0, 0x00, 0xF1, 0x07, 0x00, 3, 0, 0, 1, 2, 3, 0xFE],
          "write packet: type 0, address, length u24, data")
    check(isAck(parseSinco([0x00, 0x59, 0x00, 0x01, 0x00, 0x00, 0x00, 0xFF])!), "the stock acknowledgement is an ack")
    check(!isAck(parseSinco([0x00, 0x59, 0x12, 0x06, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF])!), "the unknown-type reply is not")
    check(!isAck(parseSinco([0x00, 0x59, 0x00, 0x01, 0x00, 0x00, 0x00, 0xFE])!), "a bad checksum is not")
    for a in [0x0, 0x1000, 0x23000, 0x24000, 0x6E000, 0x7A000, 0x7B000, 0x7E000] {
        check(refused { try SysexSector.within(a, allowed: all) }, "sector \(hex32(a)) is forbidden")
    }
    for a in [0x25000, 0x41000, 0x6D000, 0x7F000] {
        check(!refused { try SysexSector.within(a, allowed: all) }, "sector \(hex32(a)) may be written")
    }
    check(refused { try SysexSector.within(0x7F100, allowed: all) }, "an unaligned address is refused")
    check(refused { try SysexSector.within(0x80000, allowed: all) }, "an address past the flash is refused")
    check(refused { try SysexSector.within(0x41000, allowed: 0x7F000..<0x80000) }, "a sector outside --allow is refused")
    check((try? parseAllow("0x7F000:0x80000")) == 0x7F000..<0x80000, "--allow 0x7F000:0x80000")
    for s in ["0x0:0x80000", "0x24000:0x26000", "0x7F000:0x7F800", "0x7F000", "0x7B000:0x80000"] {
        check(refused { try parseAllow(s) }, "--allow \(s) is refused")
    }
    check(enc([0, 0, 0, 0, 0, 0], key: 0xFFFF) == [0xFF, 0xDF, 0x9F, 0x1F, 0x1F, 0x3E], "the ENC keystream of 0xFFFF")
    check(enc(enc(sample, key: 0x1234), key: 0x1234) == sample, "ENC undoes itself")
    let block = Array(sample[0..<256]) + Array(sample[0..<256])
    let viaReads = enc(Array(block[0..<256]), key: 0xFFFF) + enc(Array(block[256...]), key: 0xFFFF)
    check(sector0Stored(fromReads: viaReads) == block, "sector 0 from 256-byte reads: the keystream restarts per block")
    let page = testPattern(0x7F000, SysexSector.size)
    check(Set(stride(from: 0, to: SysexSector.size, by: 256).map { Array(page[$0..<($0 + 256)]) }).count == 16 && !page.contains(0xFF),
          "the test pattern differs in every page and has no 0xFF")

    // A synthetic raw read: a valid boot pointer entry in sector 0 (stored ENC-encrypted), and an
    // SFC-encrypted-looking directory at 0x1000 (its header CRC does not hold).
    func entry(_ offset: Int) -> [UInt8] {
        var e = [UInt8](repeating: 0xFF, count: 32)
        e[2] = 0xFF; e[3] = 0xFF
        for k in 0..<4 { e[4 + k] = UInt8(truncatingIfNeeded: offset >> (8 * k)) }
        for k in 8..<16 { e[k] = 0 }
        let c = crc16(Array(e[2...]))
        e[0] = UInt8(c & 0xFF); e[1] = UInt8(c >> 8)
        return enc(e, key: 0xFFFF)
    }
    var raw = (0..<SysexSector.flashEnd).map { UInt8(truncatingIfNeeded: ($0 &* 13) ^ ($0 >> 9)) }
    raw.replaceSubrange(bootEntry, with: entry(0x1000))
    if looksLikeStockRead(raw) { raw[0x1000] ^= 0x55 }
    let s0 = Array(raw[0..<SysexSector.size])
    var moved = s0
    moved.replaceSubrange(bootEntry, with: entry(0x41000))
    check(bootPointer(s0) == 0x1000 && bootPointer(moved) == 0x41000, "the boot pointer entry decodes")
    let same = try? Sector0Plan(raw: raw, sector: s0)
    check(same != nil && same!.differing.isEmpty, "sector 0 rewritten with its own contents is accepted")
    let mv = try? Sector0Plan(raw: raw, sector: moved)
    check(mv != nil && !mv!.differing.isEmpty && mv!.differing.allSatisfy { Sector0Plan.mayDiffer.contains($0) },
          "a moved boot pointer differs in bytes 0x60..0x67 only")
    var other = s0; other[0x10] ^= 1
    check(refused { try Sector0Plan(raw: raw, sector: other) }, "a change outside the boot pointer is refused")
    var broken = moved; broken[0x70] ^= 1
    check(refused { try Sector0Plan(raw: raw, sector: broken) }, "a boot pointer entry with a bad CRC is refused")
    var stock = raw
    stock.replaceSubrange(0x1000..<0x1020, with: enc(entry(0x2000), key: 0xFFFF))
    check(looksLikeStockRead(stock) && refused { try Sector0Plan(raw: stock, sector: s0) }, "a stock read is refused as the raw read")
    check(refused { try Sector0Plan(raw: Array(raw[0..<0x40000]), sector: s0) }, "a raw read that is not 512 KB is refused")
    check(refused { try Sector0Plan(raw: raw, sector: Array(s0[0..<0x800])) }, "a sector that is not 4 KB is refused")

    // On real files, still without a device: --raw a raw read (jlrom), --stock a stock read of
    // the same chip (0x23 in 256-byte blocks), --in a new sector 0 (installplan.py's).
    if let rp = option("--raw") {
        let r = loadFile(rp)
        let own = try? Sector0Plan(raw: r, sector: Array(r[0..<min(r.count, SysexSector.size)]))
        check(own != nil, "\(rp): accepted as the raw read, its own sector 0 as the rehearsal's")
        if let own { print("      boot pointer \(hex32(bootPointer(own.before)!))") }
        if let sp = option("--stock") {
            let st = loadFile(sp)
            check(st.count >= SysexSector.size && sector0Stored(fromReads: Array(st[0..<SysexSector.size])) == Array(r[0..<SysexSector.size]),
                  "\(sp): its sector 0, through the per-read keystream, equals the raw read's")
        }
        if let ip = option("--in") {
            let p = try? Sector0Plan(raw: r, sector: loadFile(ip))
            check(p != nil, "\(ip): accepted as the new sector 0")
            if let p { print("      boot pointer \(hex32(bootPointer(p.after)!)), bytes \(p.differing.map { String(format: "0x%02X", $0) }.joined(separator: " "))") }
        }
    }
    noMoreArgs()
    print(failed == 0 ? "selftest: all ok" : "selftest: \(failed) FAILED")
    exit(failed == 0 ? 0 : 1)

default:
    fail("unknown command \(command)")
}
