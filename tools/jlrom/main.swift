// jlrom — host tool for the ROM download mode ("UBOOT") of the JieLi BR25 in the SMC-Mixer, the
// last line of recovery (SPEC C-6). Wiring, procedure and what to expect: docs/recovery.md.
//
// Build:  swiftc -O -swift-version 5 -o build/jlrom tools/jlrom/main.swift
//
// Commands:
//   jlrom list                         USB devices with a mass-storage interface and whether each
//                                      could be the chip in download mode (no root needed)
//   sudo jlrom info                    start the loader if the ROM is still in charge, then print
//                                      chip key, flash ID, loader version, USB buffer size
//   sudo jlrom read --out F [--start A] [--size N]
//                                      read flash as stored: what the SFC cipher covers comes back
//                                      encrypted (tools/jlrom/compare.py checks it against a dump).
//                                      F must not exist: a read never replaces a file. The data is
//                                      saved as F.unverified as soon as it is read, then checked
//                                      against the chip's own CRC and renamed to F, or to
//                                      F.crc-mismatch if the check fails
//   sudo jlrom crc --start A --size N  CRC-16 of a flash range, computed on the chip
//   sudo jlrom erase --addr A [--size N] --allow START:END
//   sudo jlrom write --addr A (--in F | --pattern [--size N]) --allow START:END
//                                      erase whole 4 KB sectors, and write F (or a test pattern)
//                                      into them; each sector is checked erased (chip CRC), then
//                                      written and checked (chip CRC and a read back). Only inside
//                                      --allow, never sector 0, BTIF, EXIF or USRFLASH
//   sudo jlrom restore --in RAW [--apply --before F] [--protected]
//                                      write a whole raw read (jlrom read) back: reads the flash,
//                                      lists the sectors that differ; with --apply saves the flash
//                                      as it is to F (a new file), rewrites those sectors (sector 0
//                                      and BTIF/EXIF/USRFLASH only with --protected, sector 0 last)
//                                      and checks the whole flash against RAW. Refuses a stock read
//   sudo jlrom run-app                 leave download mode: the loader starts the firmware in flash
//   jlrom selftest                     checks of this build that need no device and no root: the
//                                      CRC, the keystream, the command blocks, the write guards,
//                                      the pinned loader
// Options for all but list: --log F (JSON lines: every SCSI command with its CDB, lengths, status
// and timing; flash contents are never logged, only their CRC-16), --location 0xNNNNNNNN (which
// device, when more than one could be the chip), --loader F (default: the BR25 loader of the
// pinned SDK; its SHA-256 must match).
//
// Safety: flash changes only through erase, write and restore, one 4 KB sector at a time (FB01,
// FB04), each built from a WritableSector, whose constructors refuse what is outside --allow or
// protected; every argument is checked before the device is taken, and unknown options stop the
// run. No block or chip erase, no chip key write, no flash command table: the JieLi commands it can
// build are the cases of JLCmd. The other RAM write is the loader at 0x12000, followed by a jump
// there. Root is needed to take the device from the macOS mass-storage driver (IOUSBHost
// device capture); a device whose SCSI inquiry does not name the BR25 is given back untouched.

import CryptoKit
import Foundation
import IOKit
import IOUSBHost

// MARK: - Time

var timebase = mach_timebase_info_data_t()
mach_timebase_info(&timebase)
func hostToNs(_ t: UInt64) -> UInt64 { t * UInt64(timebase.numer) / UInt64(timebase.denom) }
func nowNs() -> UInt64 { hostToNs(mach_absolute_time()) }

// MARK: - Output

func hex(_ b: [UInt8]) -> String { b.map { String(format: "%02X", $0) }.joined(separator: " ") }
func hex32(_ v: Int) -> String { String(format: "0x%08X", v) }

func fail(_ msg: String) -> Never {
    FileHandle.standardError.write(("jlrom: " + msg + "\n").data(using: .utf8)!)
    exit(1)
}

func info(_ msg: String) {
    FileHandle.standardError.write((msg + "\n").data(using: .utf8)!)
}

struct ToolError: Error, CustomStringConvertible {
    let description: String
    init(_ d: String) { description = d }
}

final class EventLog: @unchecked Sendable {
    private let fh: FileHandle?
    private let lock = NSLock()

    init(path: String?) {
        guard let path else { fh = nil; return }
        let dir = (path as NSString).deletingLastPathComponent
        if !dir.isEmpty && !FileManager.default.fileExists(atPath: dir) {
            try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
            giveToUser(dir)
        }
        FileManager.default.createFile(atPath: path, contents: nil)
        guard let h = FileHandle(forWritingAtPath: path) else { fail("cannot open log \(path)") }
        fh = h
        giveToUser(path)
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

/// Files written under sudo belong to the user who ran sudo, not to root.
func giveToUser(_ path: String) {
    let env = ProcessInfo.processInfo.environment
    guard getuid() == 0, let u = env["SUDO_UID"].flatMap({ uid_t($0) }), let g = env["SUDO_GID"].flatMap({ gid_t($0) }) else { return }
    chown(path, u, g)
}

// MARK: - JieLi checksum and cipher

/// CRC-16/XMODEM (polynomial 0x1021, initial value 0, no reflection): the CRC of the protocol.
func crc16(_ data: [UInt8], _ start: UInt16 = 0) -> UInt16 {
    var crc = start
    for b in data {
        crc ^= UInt16(b) << 8
        for _ in 0..<8 { crc = crc & 0x8000 != 0 ? (crc << 1) ^ 0x1021 : crc << 1 }
    }
    return crc
}

/// JieLi's "CrcDecode" ("MengLi") keystream: a CRC-16 run over a fixed 16-byte string (GB2312
/// text), seeded from a 32-bit key; each step's low byte is XORed into the data. The SDK's loader
/// file is stored with it per 512-byte block and the BR25 ROM removes it as the blocks arrive; the
/// chip key reply is masked with it.
let mengliMagic: [UInt8] = [0xC3, 0xCF, 0xC0, 0xE8, 0xCE, 0xD2, 0xB0, 0xAE,
                            0xC4, 0xE3, 0xA3, 0xAC, 0xD3, 0xF1, 0xC1, 0xD6]

func mengli(_ data: [UInt8], key: UInt32 = 0xFFFF_FFFF) -> [UInt8] {
    var crc = crc16([UInt8(truncatingIfNeeded: key >> 16), UInt8(truncatingIfNeeded: key >> 24)],
                    UInt16(truncatingIfNeeded: key))
    return data.enumerated().map { i, b in
        crc = crc16([mengliMagic[i % mengliMagic.count]], crc)
        return b ^ UInt8(truncatingIfNeeded: crc)
    }
}

// MARK: - Protocol

/// The commands of the JieLi download protocol this program can send. The ROM ("UBOOT1.00")
/// knows only the memory commands; the loader adds the rest. Block and chip erase (FB00, FB02),
/// chip key write (FC12) and the flash command table (FC0D) are deliberately absent, so no code
/// path can build them. Sector erase and flash write are built only from a WritableSector
/// (below), never through cdb().
enum JLCmd: UInt16, CaseIterable {
    case eraseSector = 0xFB01    // 4 KB sector erase: eraseSectorCDB(WritableSector) only
    case writeFlash = 0xFB04     // flash write: writeFlashCDB(WritableSector, ...) only
    case writeMemory = 0xFB06    // RAM write, used only to upload the loader
    case jump = 0xFB08           // jump to RAM, used only to start the loader
    case readKey = 0xFC09        // chip key, masked
    case onlineDevice = 0xFC0A   // flash type and ID
    case readId = 0xFC0B
    case runApp = 0xFC0C         // start the firmware in flash
    case flashCrc16 = 0xFC0E
    case usbBufferSize = 0xFC14  // largest flash read per command
    case version = 0xFC15        // loader version; the echo is not always right
    case maskromId = 0xFC16
    case readFlash = 0xFD05
}

/// 16-byte command block: big-endian opcode, arguments, 0xFF padding. Not for the flash writes.
func cdb(_ c: JLCmd, _ args: [UInt8] = []) -> [UInt8] {
    precondition(c != .eraseSector && c != .writeFlash, "flash erase and write are built from a WritableSector only")
    return anyCDB(c, args)
}

private func anyCDB(_ c: JLCmd, _ args: [UInt8]) -> [UInt8] {
    precondition(args.count <= 14)
    return [UInt8(c.rawValue >> 8), UInt8(c.rawValue & 0xFF)] + args + [UInt8](repeating: 0xFF, count: 14 - args.count)
}

/// A 4 KB flash sector this program may erase and write. Its only constructors check it, and the
/// flash commands are built from nothing else.
struct WritableSector {
    static let size = 0x1000
    static let flashEnd = 0x80000
    /// Sector 0 (the uboot and the boot pointer) and BTIF, EXIF, USRFLASH (Bluetooth keys, factory
    /// data, the control tables): only `restore` writes them, and only with --protected.
    static let protectedRanges = [0x00000..<0x01000, 0x7B000..<0x7F000]
    static func isProtected(_ a: Int) -> Bool { protectedRanges.contains { $0.contains(a) } }

    let address: Int
    private init(_ a: Int) { address = a }

    private static func checkSector(_ a: Int) throws {
        guard a >= 0, a % size == 0, a + size <= flashEnd else {
            throw ToolError("\(hex32(a)) is not the start of a 4 KB sector of the 512 KB flash")
        }
    }

    /// For `write` and `erase`: a sector inside the range the command line allows, never a
    /// protected one.
    static func within(_ a: Int, allowed: Range<Int>) throws -> WritableSector {
        try checkSector(a)
        guard allowed.lowerBound <= a, a + size <= allowed.upperBound else {
            throw ToolError("sector \(hex32(a)) is outside --allow \(hex32(allowed.lowerBound)):\(hex32(allowed.upperBound))")
        }
        guard !isProtected(a) else {
            throw ToolError("sector \(hex32(a)) is protected (sector 0, BTIF, EXIF, USRFLASH): only restore writes it")
        }
        return WritableSector(a)
    }

    /// For `restore` of a whole raw read: any sector; a protected one only when asked for.
    static func restoring(_ a: Int, protectedAllowed: Bool) throws -> WritableSector {
        try checkSector(a)
        guard protectedAllowed || !isProtected(a) else {
            throw ToolError("sector \(hex32(a)) is protected (sector 0, BTIF, EXIF, USRFLASH): restore writes it only with --protected")
        }
        return WritableSector(a)
    }
}

/// Sector erase (FB01): the sector's address. The BR25 loader puts the erase's result in byte 2
/// of its reply (materials/re/loader, recovery.md "Rehearsal 2").
func eraseSectorCDB(_ s: WritableSector) -> [UInt8] {
    anyCDB(.eraseSector, be32(s.address))
}

/// Flash write (FB04): address, length, then a zero byte and the data's CRC-16 low byte first, as
/// in FB06. The BR25 loader ignores the CRC (it was checked before BR17): it writes what arrives,
/// 256 bytes at a time, into flash that must be erased first.
func writeFlashCDB(_ s: WritableSector, offset: Int, _ data: [UInt8]) -> [UInt8] {
    precondition(offset >= 0 && offset % 256 == 0 && !data.isEmpty && offset + data.count <= WritableSector.size)
    let c = crc16(data)
    return anyCDB(.writeFlash, be32(s.address + offset) + be16(data.count) + [0, UInt8(c & 0xFF), UInt8(c >> 8)])
}

/// A stock read (the stock firmware's 0x23) has the area below 0x24000 decrypted, so its JLFS
/// directory at 0x1000 is plain and the first entry's header CRC holds. In a raw read the area is
/// SFC-encrypted. Written back, a stock read would leave code the chip cannot run.
func looksLikeStockRead(_ d: [UInt8]) -> Bool {
    guard d.count >= 0x1020 else { return false }
    let e = Array(d[0x1000..<0x1020])
    return Int(crc16(Array(e[2...]))) == leInt(e[0..<2])
}

/// RAM write (the ROM checks the block's CRC-16): address, length, a zero byte, the CRC low byte
/// first.
func writeMemoryCDB(_ addr: Int, _ block: [UInt8]) -> [UInt8] {
    let c = crc16(block)
    return cdb(.writeMemory, be32(addr) + be16(block.count) + [0, UInt8(c & 0xFF), UInt8(c >> 8)])
}

/// The command block wrapper of the bulk-only transport: signature, tag, data length (all
/// little-endian), direction, LUN 0, the command block's length and the block.
func buildCBW(tag: UInt32, dataLength: Int, dataIn: Bool, cb: [UInt8]) -> [UInt8] {
    precondition(cb.count >= 1 && cb.count <= 16)
    var cbw = [UInt8](repeating: 0, count: 31)
    for (i, v) in [0x4342_5355, tag, UInt32(dataLength)].enumerated() {
        for k in 0..<4 { cbw[4 * i + k] = UInt8(truncatingIfNeeded: v >> (8 * k)) }
    }
    cbw[12] = dataIn ? 0x80 : 0
    cbw[14] = UInt8(cb.count)
    cbw.replaceSubrange(15..<(15 + cb.count), with: cb)
    return cbw
}

func be32(_ v: Int) -> [UInt8] { (0..<4).map { UInt8(truncatingIfNeeded: v >> (24 - 8 * $0)) } }
func be16(_ v: Int) -> [UInt8] { [UInt8(truncatingIfNeeded: v >> 8), UInt8(truncatingIfNeeded: v)] }
func beInt(_ b: ArraySlice<UInt8>) -> Int { b.reduce(0) { $0 << 8 | Int($1) } }
func leInt(_ b: ArraySlice<UInt8>) -> Int { b.reversed().reduce(0) { $0 << 8 | Int($1) } }

/// The loader: the BR25 USB loader of the pinned SDK, run from RAM. Uploaded as stored (MengLi
/// per 512-byte block) in 512-byte blocks, then started with argument 1 (flash, no clock divider,
/// SPI mode 0), as jl-uboot-tool does for BR25.
let loaderDefault = "deps/fw-AC63_BT_SDK/cpu/br25/tools/br25loader.bin"
let loaderSHA256 = "6cb7f0f9cf6e172d5b698eade179ecffcc4406c834c9664de376c430a160ea83"
let loaderAddress = 0x12000
let loaderBlock = 512
let loaderArgument = 0x0001

// MARK: - Finding the device

func prop(_ e: io_registry_entry_t, _ key: String) -> Any? {
    IORegistryEntryCreateCFProperty(e, key as CFString, kCFAllocatorDefault, 0)?.takeRetainedValue()
}

func searchProp(_ e: io_registry_entry_t, _ key: String) -> Any? {
    IORegistryEntrySearchCFProperty(e, kIOServicePlane, key as CFString, kCFAllocatorDefault,
                                    IOOptionBits(kIORegistryIterateRecursively))
}

/// Calls body for every registry entry below e (service plane); the entries are released after.
func forEachBelow(_ e: io_registry_entry_t, _ body: (io_registry_entry_t) -> Void) {
    var it: io_iterator_t = 0
    guard IORegistryEntryCreateIterator(e, kIOServicePlane, IOOptionBits(kIORegistryIterateRecursively), &it) == KERN_SUCCESS else { return }
    defer { IOObjectRelease(it) }
    while true {
        let o = IOIteratorNext(it)
        if o == 0 { break }
        body(o)
        IOObjectRelease(o)
    }
}

func conforms(_ e: io_registry_entry_t, _ cls: String) -> Bool { IOObjectConformsTo(e, cls) != 0 }

struct UsbDevice {
    let service: io_service_t
    let name: String
    let vid: Int
    let pid: Int
    let location: Int
    let mscInterfaces: Int
    let scsiVendor: String?
    let scsiProduct: String?
    let hasMedia: Bool

    init(_ s: io_service_t) {
        service = s
        name = prop(s, "USB Product Name") as? String ?? ""
        vid = prop(s, "idVendor") as? Int ?? 0
        pid = prop(s, "idProduct") as? Int ?? 0
        location = prop(s, "locationID") as? Int ?? 0
        var msc = 0, media = false
        forEachBelow(s) { e in
            if conforms(e, "IOUSBHostInterface"), prop(e, "bInterfaceClass") as? Int == 8 { msc += 1 }
            if conforms(e, "IOMedia") { media = true }
        }
        mscInterfaces = msc
        hasMedia = media
        scsiVendor = (searchProp(s, "Vendor Identification") as? String)?.trimmingCharacters(in: .whitespaces)
        scsiProduct = (searchProp(s, "Product Identification") as? String)?.trimmingCharacters(in: .whitespaces)
    }

    /// "chip": the mass-storage driver read a BR25 inquiry. "maybe": a mass-storage device macOS
    /// has no SCSI identity and no disk for (for example after jlrom held it); it is checked by
    /// an inquiry after capture. Anything else is never touched.
    var verdict: String {
        if mscInterfaces == 0 { return "no" }
        if let v = scsiVendor { return v.lowercased() == "br25" ? "chip" : "no" }
        return hasMedia ? "no" : "maybe"
    }

    var json: [String: Any] {
        var o: [String: Any] = ["name": name, "vid_pid": String(format: "%04X:%04X", vid, pid),
                                "location": hex32(location), "mass_storage_interfaces": mscInterfaces,
                                "has_disk": hasMedia, "verdict": verdict]
        if let v = scsiVendor { o["scsi_vendor"] = v }
        if let p = scsiProduct { o["scsi_product"] = p }
        return o
    }
}

func usbDevices() -> [UsbDevice] {
    var it: io_iterator_t = 0
    guard IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOUSBHostDevice"), &it) == KERN_SUCCESS else { return [] }
    defer { IOObjectRelease(it) }
    var out: [UsbDevice] = []
    while true {
        let s = IOIteratorNext(it)
        if s == 0 { break }
        out.append(UsbDevice(s))
    }
    return out
}

func pickDevice(location: Int?) -> UsbDevice {
    let all = usbDevices()
    let cands = all.filter { $0.verdict != "no" && (location == nil || $0.location == location) }
    if cands.count == 1 { return cands[0] }
    let lines = all.filter { $0.mscInterfaces > 0 }.map {
        "  \(hex32($0.location))  \(String(format: "%04X:%04X", $0.vid, $0.pid))  \($0.name)  [\($0.verdict)]"
    }.joined(separator: "\n")
    if cands.isEmpty {
        fail("no device that could be the chip in download mode\(location.map { " at \(hex32($0))" } ?? ""); mass-storage devices:\n\(lines.isEmpty ? "  (none)" : lines)")
    }
    fail("more than one device could be the chip; pick one with --location:\n\(lines)")
}

// MARK: - USB mass storage, bulk-only transport

let usbPipeStalled = Int(Int32(bitPattern: 0xE000_404F))

final class BulkOnly {
    let device: IOUSBHostDevice
    let iface: IOUSBHostInterface
    let ifaceNumber: Int
    let pipeIn: IOUSBHostPipe
    let pipeOut: IOUSBHostPipe
    let log: EventLog
    private var tag: UInt32 = 0

    /// Captures the device (the macOS drivers on it are terminated) and opens its mass-storage
    /// interface.
    init(_ dev: UsbDevice, log: EventLog) throws {
        self.log = log
        device = try IOUSBHostDevice(__ioService: dev.service, options: [.deviceCapture], queue: nil, interestHandler: nil)
        log.write(["dir": "meta", "event": "captured", "device": dev.json])
        var ifService: io_service_t = 0
        let deadline = Date().addingTimeInterval(2)
        while ifService == 0 && Date() < deadline {
            var it: io_iterator_t = 0
            if IORegistryEntryGetChildIterator(dev.service, kIOServicePlane, &it) == KERN_SUCCESS {
                while true {
                    let c = IOIteratorNext(it)
                    if c == 0 { break }
                    if ifService == 0, conforms(c, "IOUSBHostInterface"), prop(c, "bInterfaceClass") as? Int == 8 {
                        ifService = c
                    } else {
                        IOObjectRelease(c)
                    }
                }
                IOObjectRelease(it)
            }
            if ifService == 0 { usleep(100_000) }
        }
        guard ifService != 0 else {
            device.destroy()
            throw ToolError("the captured device shows no mass-storage interface")
        }
        defer { IOObjectRelease(ifService) }
        do {
            iface = try IOUSBHostInterface(__ioService: ifService, options: [], queue: nil, interestHandler: nil)
        } catch {
            device.destroy()
            throw error
        }
        ifaceNumber = Int(iface.interfaceDescriptor.pointee.bInterfaceNumber)
        var inAddr = 0, outAddr = 0
        let cfg = iface.configurationDescriptor
        let ifd = iface.interfaceDescriptor
        var cur = UnsafeRawPointer(ifd).assumingMemoryBound(to: IOUSBDescriptorHeader.self)
        while let ep = IOUSBGetNextEndpointDescriptor(cfg, ifd, cur) {
            if ep.pointee.bmAttributes & 3 == 2 {
                if ep.pointee.bEndpointAddress & 0x80 != 0 { inAddr = Int(ep.pointee.bEndpointAddress) } else { outAddr = Int(ep.pointee.bEndpointAddress) }
            }
            cur = UnsafeRawPointer(ep).assumingMemoryBound(to: IOUSBDescriptorHeader.self)
        }
        guard inAddr != 0, outAddr != 0 else {
            iface.destroy()
            device.destroy()
            throw ToolError("the mass-storage interface has no bulk IN/OUT pair")
        }
        pipeIn = try iface.copyPipe(withAddress: inAddr)
        pipeOut = try iface.copyPipe(withAddress: outAddr)
        log.write(["dir": "meta", "event": "opened", "interface": ifaceNumber,
                   "ep_in": String(format: "0x%02X", inAddr), "ep_out": String(format: "0x%02X", outAddr)])
    }

    /// surrender: leave the device without a reset and without macOS drivers (the chip stays in
    /// download mode with the loader running; the next jlrom run takes it again). Otherwise the
    /// device is reset and given back to macOS.
    func close(surrender: Bool) {
        iface.destroy()
        if surrender { device.destroy(options: [.deviceSurrender]) } else { device.destroy() }
        log.write(["dir": "meta", "event": surrender ? "released" : "returned"])
    }

    private func io(_ pipe: IOUSBHostPipe, _ data: NSMutableData, _ timeout: Double) throws -> Int {
        var n = 0
        try pipe.__sendIORequest(with: data, bytesTransferred: &n, completionTimeout: timeout)
        return n
    }

    private func isStall(_ e: Error) -> Bool { (e as NSError).code == usbPipeStalled }

    /// Bulk-only mass storage reset and clearing both halts (USB MSC BOT 5.3.4).
    func resetRecovery() {
        let req = IOUSBDeviceRequest(bmRequestType: 0x21, bRequest: 0xFF, wValue: 0, wIndex: UInt16(ifaceNumber), wLength: 0)
        var n = 0
        let ok = (try? iface.__send(req, data: nil, bytesTransferred: &n, completionTimeout: 5)) != nil
        try? pipeIn.clearStall()
        try? pipeOut.clearStall()
        log.write(["dir": "meta", "event": "reset_recovery", "request_ok": ok])
    }

    /// One SCSI command: CBW, optional data phase (in: dataIn bytes, or out: dataOut), CSW.
    /// Returns the data read (possibly shorter than asked).
    func command(_ cb: [UInt8], dataIn: Int = 0, dataOut: [UInt8]? = nil, timeout: Double = 5) throws -> [UInt8] {
        tag &+= 1
        let len = dataOut?.count ?? dataIn
        let cbw = buildCBW(tag: tag, dataLength: len, dataIn: dataIn > 0, cb: cb)
        let t0 = nowNs()
        var entry: [String: Any] = ["dir": "scsi", "tag": tag, "cdb": hex(cb)]
        var got: [UInt8] = []
        do {
            _ = try io(pipeOut, NSMutableData(bytes: cbw, length: cbw.count), timeout)
            if let out = dataOut, !out.isEmpty {
                entry["out"] = out.count
                entry["out_crc16"] = String(format: "%04X", crc16(out))
                do { _ = try io(pipeOut, NSMutableData(bytes: out, length: out.count), timeout) } catch where isStall(error) {
                    try pipeOut.clearStall()
                    entry["data_stall"] = true
                }
            } else if dataIn > 0 {
                let buf = NSMutableData(length: dataIn)!
                do {
                    let n = try io(pipeIn, buf, timeout)
                    got = [UInt8](Data(referencing: buf).prefix(n))
                } catch where isStall(error) {
                    try pipeIn.clearStall()
                    entry["data_stall"] = true
                }
                entry["in"] = got.count
                entry["in_crc16"] = String(format: "%04X", crc16(got))
                // A short data phase is logged as it is: small, and never flash data (a flash
                // read that comes short fails in readFlash).
                if got.count < dataIn && got.count <= 16 { entry["in_hex"] = hex(got) }
            }
            // A device that has no data for us may send its CSW in the data phase.
            let cswInData = got.count == 13 && leInt(got[0..<4]) == 0x5342_5355
                && UInt32(leInt(got[4..<8])) == tag
            var c: [UInt8]
            if cswInData {
                c = got
                got = []
                entry["csw_in_data_phase"] = true
            } else {
                let csw = NSMutableData(length: 13)!
                var n: Int
                do { n = try io(pipeIn, csw, timeout) } catch where isStall(error) {
                    try pipeIn.clearStall()
                    n = try io(pipeIn, csw, timeout)
                }
                c = [UInt8](Data(referencing: csw).prefix(n))
            }
            let sig = c.count == 13 ? leInt(c[0..<4]) : 0
            let ctag = c.count == 13 ? UInt32(leInt(c[4..<8])) : 0
            guard sig == 0x5342_5355, ctag == tag else {
                entry["csw"] = hex(c)
                throw ToolError("bad CSW \(hex(c))")
            }
            entry["residue"] = leInt(c[8..<12])
            entry["status"] = Int(c[12])
            entry["us"] = (nowNs() - t0) / 1000
            log.write(entry)
            if c[12] != 0 { throw ToolError("command \(hex(Array(cb.prefix(2)))) failed, CSW status \(c[12])") }
            return got
        } catch let e as ToolError {
            if entry["status"] == nil {
                entry["error"] = e.description
                log.write(entry)
                resetRecovery()
            }
            throw e
        } catch {
            entry["error"] = "\(error)"
            entry["us"] = (nowNs() - t0) / 1000
            log.write(entry)
            resetRecovery()
            throw error
        }
    }
}

// MARK: - The chip

final class Chip {
    let bot: BulkOnly
    let log: EventLog

    init(_ bot: BulkOnly, log: EventLog) {
        self.bot = bot
        self.log = log
    }

    func inquiry() throws -> (vendor: String, product: String, revision: String) {
        let d = try bot.command([0x12, 0, 0, 0, 36, 0], dataIn: 36)
        guard d.count >= 36 else { throw ToolError("short inquiry reply (\(d.count) bytes)") }
        func text(_ r: Range<Int>) -> String { String(decoding: d[r], as: UTF8.self).trimmingCharacters(in: .whitespaces) }
        return (text(8..<16), text(16..<32), text(32..<36))
    }

    /// A command without a data phase: the reply is 16 bytes read in its data-in phase, whose
    /// first two echo the opcode; the other 14 are returned.
    func exec(_ c: JLCmd, _ args: [UInt8] = [], checkEcho: Bool = true) throws -> [UInt8] {
        let r = try bot.command(cdb(c, args), dataIn: 16)
        guard r.count == 16 else { throw ToolError(String(format: "short reply to %04X (%d bytes)", c.rawValue, r.count)) }
        let echo = beInt(r[0..<2])
        if checkEcho && echo != Int(c.rawValue) {
            throw ToolError(String(format: "reply to %04X echoes %04X: %@", c.rawValue, echo, hex(r)))
        }
        log.write(["dir": "reply", "cmd": String(format: "%04X", c.rawValue), "data": hex(Array(r[2...]))])
        return Array(r[2...])
    }

    /// Uploads the loader into RAM at loaderAddress and jumps there. The ROM checks a CRC-16 of
    /// each block.
    func startLoader(_ image: [UInt8]) throws {
        var addr = loaderAddress
        for off in stride(from: 0, to: image.count, by: loaderBlock) {
            let block = Array(image[off..<min(off + loaderBlock, image.count)])
            _ = try bot.command(writeMemoryCDB(addr, block), dataOut: block)
            addr += block.count
        }
        _ = try exec(.jump, be32(loaderAddress) + be16(loaderArgument))
        log.write(["dir": "meta", "event": "loader_started", "address": hex32(loaderAddress), "size": image.count])
    }

    func chipKey() throws -> Int {
        let p = try exec(.readKey, be32(0xAC6900))
        return leInt(mengli([p[5], p[4]])[...])
    }

    func identity() throws -> [String: Any] {
        var o: [String: Any] = [:]
        o["chip_key"] = String(format: "0x%04X", try chipKey())
        let od = try exec(.onlineDevice)
        o["flash_type"] = Int(od[0])
        o["flash_id"] = String(format: "0x%08X", leInt(od[2..<6]))
        o["id"] = String(format: "0x%06X", beInt(try exec(.readId)[0..<3]))
        o["usb_buffer_size"] = beInt(try exec(.usbBufferSize)[0..<4])
        // Not asked: loader version (FC15) and mask ROM ID (FC16). On the mixer (2026-11-10) FC15
        // got 13 bytes instead of 16 in its data phase, then no CSW, and the chip restarted into
        // its firmware: br25loader.bin may not know it. recovery.md, "Rehearsal 1".
        return o
    }

    func readFlash(_ addr: Int, _ len: Int) throws -> [UInt8] {
        let d = try bot.command(cdb(.readFlash, be32(addr) + be16(len)), dataIn: len, timeout: 10)
        guard d.count == len else { throw ToolError("flash read at \(hex32(addr)): \(d.count) of \(len) bytes") }
        return d
    }

    func flashCrc(_ addr: Int, _ len: Int) throws -> Int {
        beInt(try exec(.flashCrc16, be32(addr) + be16(len))[0..<2])
    }

    static let erasedCrc = Int(crc16([UInt8](repeating: 0xFF, count: WritableSector.size)))

    /// Erases one sector, then checks with the chip's CRC that it reads back erased.
    func eraseSector(_ s: WritableSector) throws {
        let r = try bot.command(eraseSectorCDB(s), dataIn: 16, timeout: 10)
        guard r.count == 16, beInt(r[0..<2]) == 0xFB01 else {
            throw ToolError("erase \(hex32(s.address)): unexpected reply \(hex(r))")
        }
        log.write(["dir": "reply", "cmd": "FB01", "data": hex(Array(r[2...]))])
        let c = try flashCrc(s.address, WritableSector.size)
        guard c == Chip.erasedCrc else {
            throw ToolError(String(format: "erase %@: the sector does not read back erased (CRC %04X, erased is %04X; the reply's result byte %02X). Write protection?",
                                   hex32(s.address), c, Chip.erasedCrc, r[2]))
        }
        log.write(["dir": "meta", "event": "sector_erased", "address": hex32(s.address), "result": Int(r[2])])
    }

    /// Erases the sector and writes 4 KB into it, then checks them with the chip's CRC and a read
    /// back. All 0xFF is an erase alone.
    func programSector(_ s: WritableSector, _ data: [UInt8]) throws {
        precondition(data.count == WritableSector.size)
        try eraseSector(s)
        guard data.contains(where: { $0 != 0xFF }) else { return }
        _ = try bot.command(writeFlashCDB(s, offset: 0, data), dataOut: data, timeout: 10)
        let want = Int(crc16(data))
        let c = try flashCrc(s.address, data.count)
        guard c == want else {
            throw ToolError(String(format: "write %@: the chip's CRC is %04X, the data's %04X", hex32(s.address), c, want))
        }
        guard try readFlash(s.address, data.count) == data else {
            throw ToolError("write \(hex32(s.address)): the CRC agrees but the read back differs")
        }
        log.write(["dir": "meta", "event": "sector_written", "address": hex32(s.address), "crc16": String(format: "%04X", want)])
    }

    /// Reads a range in chunks of the loader's USB buffer, with progress on stderr.
    func readRange(_ start: Int, _ size: Int) throws -> [UInt8] {
        let chunk = min(max(try exec(.usbBufferSize).prefix(4).reduce(0) { $0 << 8 | Int($1) }, 512), 0x8000)
        var data: [UInt8] = []
        data.reserveCapacity(size)
        let t0 = Date()
        var addr = start
        while addr < start + size {
            let n = min(chunk, start + size - addr)
            data += try readFlash(addr, n)
            addr += n
            if (addr - start) % 0x8000 == 0 || addr == start + size {
                let kbs = Double(addr - start) / 1024 / max(Date().timeIntervalSince(t0), 0.001)
                FileHandle.standardError.write(String(format: "\r0x%06X / 0x%06X  %.1f KB/s", addr - start, size, kbs).data(using: .utf8)!)
            }
        }
        info("")
        return data
    }

    /// The loader's CRC of every 32 KB of a range against the CRC of `data`; the ranges that differ.
    func crcMismatches(_ start: Int, _ data: [UInt8]) throws -> [String] {
        var mismatches: [String] = []
        for off in stride(from: 0, to: data.count, by: 0x8000) {
            let n = min(0x8000, data.count - off)
            let chipCrc = try flashCrc(start + off, n)
            let hostCrc = Int(crc16(Array(data[off..<(off + n)])))
            if chipCrc != hostCrc { mismatches.append(String(format: "%@: chip %04X, host %04X", hex32(start + off), chipCrc, hostCrc)) }
        }
        return mismatches
    }
}

// MARK: - Session

/// Takes the chip, checks it is a BR25, starts the loader unless it already runs.
func openChip(_ log: EventLog) -> (BulkOnly, Chip) {
    guard geteuid() == 0 else { fail("needs root to take the device from macOS: run it with sudo") }
    let loaderPath = option("--loader") ?? loaderDefault
    let location = option("--location").map { s -> Int in
        guard let v = Int(s.hasPrefix("0x") ? String(s.dropFirst(2)) : s, radix: 16) else { fail("bad --location \(s)") }
        return v
    }
    guard let image = FileManager.default.contents(atPath: loaderPath).map({ [UInt8]($0) }) else {
        fail("cannot read the loader \(loaderPath) (run from the repository root, or pass --loader)")
    }
    let sha = SHA256.hash(data: Data(image)).map { String(format: "%02x", $0) }.joined()
    guard sha == loaderSHA256 else { fail("\(loaderPath): SHA-256 \(sha) is not the pinned loader \(loaderSHA256)") }

    let dev = pickDevice(location: location)
    info("device: \(dev.name.isEmpty ? "(no name)" : dev.name) \(String(format: "%04X:%04X", dev.vid, dev.pid)) at \(hex32(dev.location))")
    let bot: BulkOnly
    do { bot = try BulkOnly(dev, log: log) } catch { fail("cannot take the device: \(error)") }
    let chip = Chip(bot, log: log)
    do {
        let inq = try chip.inquiry()
        log.write(["dir": "meta", "event": "inquiry", "vendor": inq.vendor, "product": inq.product, "revision": inq.revision])
        info("inquiry: \(inq.vendor) / \(inq.product) / \(inq.revision)")
        guard inq.vendor.lowercased() == "br25" else {
            bot.close(surrender: false)
            fail("not a BR25 in download mode (inquiry vendor \"\(inq.vendor)\"); the device was given back")
        }
        if inq.product == "UBOOT1.00" {
            info("the ROM is in charge: uploading the loader (\(image.count) bytes) to \(hex32(loaderAddress))")
            try chip.startLoader(image)
        } else {
            info("the ROM's own name is not there, so the loader is taken to run already (inquiry product \"\(inq.product)\")")
        }
    } catch {
        bot.close(surrender: true)
        fail("\(error)")
    }
    return (bot, chip)
}

// MARK: - Commands

var args = Array(CommandLine.arguments.dropFirst())

func option(_ name: String) -> String? {
    guard let i = args.firstIndex(of: name) else { return nil }
    guard i + 1 < args.count else { fail("\(name) needs a value") }
    let v = args[i + 1]
    args.removeSubrange(i...(i + 1))
    return v
}

func intOption(_ name: String, _ def: Int?) -> Int {
    guard let s = option(name) else {
        guard let def else { fail("\(name) is required") }
        return def
    }
    let v = s.hasPrefix("0x") ? Int(s.dropFirst(2), radix: 16) : Int(s)
    guard let v else { fail("bad number for \(name): \(s)") }
    return v
}

func flag(_ name: String) -> Bool {
    guard let i = args.firstIndex(of: name) else { return false }
    args.remove(at: i)
    return true
}

/// The commands that change flash refuse anything they do not know: a mistyped option must not
/// be ignored.
func noMoreArgs() {
    guard args.isEmpty else { fail("unknown arguments: \(args.joined(separator: " "))") }
}

func parseInt(_ s: String) -> Int? {
    s.hasPrefix("0x") ? Int(s.dropFirst(2), radix: 16) : Int(s)
}

/// --allow START:END, required by `write` and `erase`: the only range the run may change, whole
/// sectors, none of them protected.
func allowOption() -> Range<Int> {
    guard let s = option("--allow") else { fail("--allow START:END is required: the range this run may change") }
    let p = s.split(separator: ":", omittingEmptySubsequences: false).map(String.init)
    guard p.count == 2, let lo = parseInt(p[0]), let hi = parseInt(p[1]), lo < hi, hi <= flashSize,
          lo % WritableSector.size == 0, hi % WritableSector.size == 0 else {
        fail("bad --allow \(s): START:END in whole 4 KB sectors inside the flash, for example 0x7F000:0x80000")
    }
    if let p = WritableSector.protectedRanges.first(where: { $0.overlaps(lo..<hi) }) {
        fail("--allow \(s) overlaps the protected \(hex32(p.lowerBound)):\(hex32(p.upperBound)) (sector 0, BTIF, EXIF, USRFLASH)")
    }
    return lo..<hi
}

/// The data `write --pattern` writes: never all 0xFF, different in every sector and every 256-byte
/// page, so a misplaced or missing page shows in the check.
func testPattern(_ start: Int, _ size: Int) -> [UInt8] {
    (0..<size).map { i in
        let a = start + i
        return UInt8(truncatingIfNeeded: (a &* 7 &+ 3) ^ (a >> 8) ^ (a >> 12)) & 0x7F
    }
}

func printJSON(_ o: Any) {
    let d = try! JSONSerialization.data(withJSONObject: o, options: [.sortedKeys, .prettyPrinted])
    print(String(data: d, encoding: .utf8)!)
}

let flashSize = 0x80000

func checkRange(_ start: Int, _ size: Int) {
    guard start >= 0, size > 0, start + size <= flashSize else {
        fail("range \(hex32(start)) + \(hex32(size)) is outside the 512 KB flash")
    }
}

guard let command = args.first else {
    fail("usage: jlrom list | info | read | crc | erase | write | restore | run-app | selftest  (see the header of main.swift)")
}
args.removeFirst()
let log = EventLog(path: option("--log"))
log.write(["dir": "meta", "event": "start", "argv": CommandLine.arguments, "wall": ISO8601DateFormatter().string(from: Date())])

switch command {
case "list":
    printJSON(usbDevices().filter { $0.mscInterfaces > 0 }.map(\.json))

case "info":
    let (bot, chip) = openChip(log)
    do {
        let id = try chip.identity()
        log.write(["dir": "meta", "event": "identity"].merging(id) { a, _ in a })
        printJSON(id)
    } catch {
        bot.close(surrender: true)
        fail("\(error)")
    }
    bot.close(surrender: true)

case "read":
    guard let out = option("--out") else { fail("--out is required") }
    let start = intOption("--start", 0)
    let size = intOption("--size", flashSize - start)
    checkRange(start, size)
    // Before the device is taken: the output must be new and its directory there. A read never
    // replaces a file (under sudo even a read-only dump would give way), and a read is not lost to
    // a path that cannot be written.
    let unverified = out + ".unverified", mismatched = out + ".crc-mismatch"
    for p in [out, unverified, mismatched] where FileManager.default.fileExists(atPath: p) {
        fail("\(p) exists: a read never replaces a file. Pick another --out, or move that file away")
    }
    let outDir = (out as NSString).deletingLastPathComponent
    var isDir: ObjCBool = false
    guard FileManager.default.fileExists(atPath: outDir.isEmpty ? "." : outDir, isDirectory: &isDir), isDir.boolValue else {
        fail("the directory of \(out) does not exist")
    }
    let (bot, chip) = openChip(log)
    let t0 = Date()
    do {
        let data = try chip.readRange(start, size)
        // Saved first, so that a failure of the check below does not lose what was read. Read-only,
        // like the other dumps in materials/dumps.
        guard FileManager.default.createFile(atPath: unverified, contents: Data(data), attributes: [.posixPermissions: 0o444]) else {
            bot.close(surrender: true)
            fail("cannot write \(unverified)")
        }
        giveToUser(unverified)
        // The loader's own CRC of the whole range against the CRC of what arrived (one command per
        // 32 KB: the length field is 16 bits).
        let mismatches = try chip.crcMismatches(start, data)
        bot.close(surrender: true)
        // A read that failed the check keeps a name that says so: it is not a backup.
        let saved = mismatches.isEmpty ? out : mismatched
        do { try FileManager.default.moveItem(atPath: unverified, toPath: saved) } catch {
            fail("the read is in \(unverified); renaming it to \(saved) failed: \(error)")
        }
        let sha = SHA256.hash(data: Data(data)).map { String(format: "%02x", $0) }.joined()
        log.write(["dir": "meta", "event": "dump", "out": saved, "start": start, "size": size, "sha256": sha, "crc_mismatches": mismatches])
        printJSON(["out": saved, "start": hex32(start), "size": hex32(size), "sha256": sha,
                   "seconds": Date().timeIntervalSince(t0), "crc_check": mismatches.isEmpty ? "ok" : mismatches])
        if !mismatches.isEmpty { exit(2) }
    } catch {
        bot.close(surrender: true)
        let kept = FileManager.default.fileExists(atPath: unverified)
        fail("\(error)" + (kept ? "; what was read is in \(unverified), not checked against the chip's CRC" : ""))
    }

case "crc":
    let start = intOption("--start", nil)
    let size = intOption("--size", nil)
    checkRange(start, size)
    guard size <= 0xFFFF else { fail("--size is at most 0xFFFF (16-bit length field)") }
    let (bot, chip) = openChip(log)
    do {
        let c = try chip.flashCrc(start, size)
        printJSON(["start": hex32(start), "size": hex32(size), "crc16": String(format: "0x%04X", c)])
    } catch {
        bot.close(surrender: true)
        fail("\(error)")
    }
    bot.close(surrender: true)

case "erase", "write":
    // Everything is checked before the device is taken.
    let allowed = allowOption()
    let start = intOption("--addr", nil)
    var data: [UInt8]
    var source: String
    if command == "erase" {
        let size = intOption("--size", WritableSector.size)
        data = [UInt8](repeating: 0xFF, count: max(size, 0))
        source = "erase"
    } else if flag("--pattern") {
        let size = intOption("--size", WritableSector.size)
        data = testPattern(start, max(size, 0))
        source = "pattern"
    } else {
        guard let path = option("--in") else { fail("--in F or --pattern is required") }
        guard let d = FileManager.default.contents(atPath: path) else { fail("cannot read \(path)") }
        data = [UInt8](d)
        source = path
    }
    noMoreArgs()
    guard !data.isEmpty, data.count % WritableSector.size == 0 else {
        fail("the size (\(data.count) bytes) must be whole 4 KB sectors")
    }
    let sectors: [WritableSector]
    do {
        sectors = try stride(from: start, to: start + data.count, by: WritableSector.size).map {
            try WritableSector.within($0, allowed: allowed)
        }
    } catch { fail("\(error)") }
    let sha = SHA256.hash(data: Data(data)).map { String(format: "%02x", $0) }.joined()
    log.write(["dir": "meta", "event": command, "start": hex32(start), "size": data.count, "source": source, "sha256": sha,
               "allow": [hex32(allowed.lowerBound), hex32(allowed.upperBound)]])
    let (bot, chip) = openChip(log)
    var done: [String] = []
    do {
        for (i, s) in sectors.enumerated() {
            let off = i * WritableSector.size
            try chip.programSector(s, Array(data[off..<(off + WritableSector.size)]))
            done.append(hex32(s.address))
            info("\(command == "erase" ? "erased" : "written and checked"): \(hex32(s.address))")
        }
    } catch {
        bot.close(surrender: true)
        fail("\(error); done before it: \(done.isEmpty ? "none" : done.joined(separator: ", "))")
    }
    bot.close(surrender: true)
    printJSON([command == "erase" ? "erased" : "written": done, "sha256": sha, "source": source, "check": "ok"])

case "restore":
    // Writes a whole raw read back: reads the flash first, then rewrites only the sectors that
    // differ. Without --apply it only lists them.
    guard let path = option("--in") else { fail("--in RAW is required: a whole raw read (jlrom read)") }
    let apply = flag("--apply")
    let protectedAllowed = flag("--protected")
    let before = option("--before")
    noMoreArgs()
    guard let d = FileManager.default.contents(atPath: path) else { fail("cannot read \(path)") }
    let raw = [UInt8](d)
    guard raw.count == flashSize else { fail("\(path) has \(raw.count) bytes: a raw read of the whole flash has \(flashSize)") }
    guard !looksLikeStockRead(raw) else {
        fail("\(path) looks like a stock read (its directory at 0x1000 is plain): only a raw read (jlrom read) can be written back")
    }
    guard raw[0..<WritableSector.size].contains(where: { $0 != 0xFF }) else { fail("\(path) has an erased sector 0: not a raw read") }
    if apply {
        guard let before else { fail("--apply needs --before F: where the flash as it is now is saved first") }
        guard !FileManager.default.fileExists(atPath: before) else { fail("\(before) exists: pick a new name") }
    }
    let rawSha = SHA256.hash(data: Data(raw)).map { String(format: "%02x", $0) }.joined()
    log.write(["dir": "meta", "event": "restore", "in": path, "sha256": rawSha, "apply": apply, "protected": protectedAllowed])
    let (bot, chip) = openChip(log)
    do {
        let now = try chip.readRange(0, flashSize)
        let bad = try chip.crcMismatches(0, now)
        guard bad.isEmpty else { throw ToolError("the read of the flash as it is now fails the CRC check: \(bad)") }
        let differ = stride(from: 0, to: flashSize, by: WritableSector.size).filter {
            raw[$0..<($0 + WritableSector.size)] != now[$0..<($0 + WritableSector.size)]
        }
        let plan: [String: Any] = ["in": path, "sha256": rawSha, "sectors_differ": differ.map(hex32),
                                   "protected_differ": differ.filter(WritableSector.isProtected).map(hex32)]
        log.write(["dir": "meta", "event": "restore_plan"].merging(plan) { a, _ in a })
        guard apply else {
            bot.close(surrender: true)
            printJSON(plan.merging(["applied": false]) { a, _ in a })
            exit(0)
        }
        // All checked before the first erase: a protected sector without --protected stops here.
        // Protected sectors last, sector 0 the very last.
        let order = differ.filter { !WritableSector.isProtected($0) } + differ.filter(WritableSector.isProtected).reversed()
        let sectors = try order.map { try WritableSector.restoring($0, protectedAllowed: protectedAllowed) }
        if let before {
            guard FileManager.default.createFile(atPath: before, contents: Data(now), attributes: [.posixPermissions: 0o444]) else {
                throw ToolError("cannot write \(before)")
            }
            giveToUser(before)
            info("the flash as it was: \(before)")
        }
        var done: [String] = []
        for s in sectors {
            do { try chip.programSector(s, Array(raw[s.address..<(s.address + WritableSector.size)])) } catch {
                throw ToolError("\(error); restored before it: \(done.isEmpty ? "none" : done.joined(separator: ", "))")
            }
            done.append(hex32(s.address))
            info("restored and checked: \(hex32(s.address))")
        }
        let after = try chip.crcMismatches(0, raw)
        bot.close(surrender: true)
        printJSON(plan.merging(["applied": true, "restored": done, "crc_check": after.isEmpty ? "ok" : after]) { a, _ in a })
        if !after.isEmpty { exit(2) }
    } catch {
        bot.close(surrender: true)
        fail("\(error)")
    }

case "run-app":
    let (bot, chip) = openChip(log)
    do { _ = try chip.exec(.runApp, be32(1)) } catch {
        // The chip may restart before the reply arrives.
        info("no clean reply (\(error)); the chip may already have restarted")
    }
    bot.close(surrender: false)
    printJSON(["run_app": "sent"])

case "selftest":
    // No device and no root: what can be checked of this build before it meets the chip.
    var failed = 0
    func check(_ ok: Bool, _ what: String) {
        if !ok { failed += 1 }
        print((ok ? "ok    " : "FAIL  ") + what)
    }
    func ff(_ n: Int) -> [UInt8] { [UInt8](repeating: 0xFF, count: n) }
    check(crc16(Array("123456789".utf8)) == 0x31C3, "CRC-16/XMODEM of \"123456789\" is 31C3")
    check(crc16([]) == 0 && crc16([0x00]) == 0 && crc16([0x01]) == 0x1021, "CRC-16 of nothing, 00 and 01")
    check(crc16(Array("6789".utf8), crc16(Array("12345".utf8))) == 0x31C3, "CRC-16 continued from a start value")
    let sample = (0..<600).map { UInt8(truncatingIfNeeded: $0 &* 7 &+ 3) }
    check(mengli(sample) != sample && mengli(mengli(sample)) == sample, "the MengLi keystream undoes itself")
    check(be32(0x0102_0304) == [1, 2, 3, 4] && be16(0x0102) == [1, 2], "big-endian fields")
    check(beInt([0x12, 0x34][...]) == 0x1234 && leInt([0x12, 0x34][...]) == 0x3412, "reading big- and little-endian fields")
    check(cdb(.readFlash, be32(0x12345) + be16(0x8000)) == [0xFD, 0x05, 0x00, 0x01, 0x23, 0x45, 0x80, 0x00] + ff(8),
          "command block: read flash, FD05 address length")
    check(cdb(.flashCrc16, be32(0x7F000) + be16(0x1000)) == [0xFC, 0x0E, 0x00, 0x07, 0xF0, 0x00, 0x10, 0x00] + ff(8),
          "command block: flash CRC, FC0E address length")
    check(cdb(.readKey, be32(0xAC6900)) == [0xFC, 0x09, 0x00, 0xAC, 0x69, 0x00] + ff(10), "command block: chip key, FC09 00 AC 69 00")
    check(cdb(.runApp, be32(1)) == [0xFC, 0x0C, 0x00, 0x00, 0x00, 0x01] + ff(10), "command block: run the application, FC0C 00 00 00 01")
    check(cdb(.jump, be32(loaderAddress) + be16(loaderArgument)) == [0xFB, 0x08, 0x00, 0x01, 0x20, 0x00, 0x00, 0x01] + ff(8),
          "command block: jump to the loader at 0x12000 with argument 1")
    let block = [UInt8](repeating: 0xA5, count: 512)
    let bc = crc16(block)
    check(writeMemoryCDB(0x12200, block) == [0xFB, 0x06, 0x00, 0x01, 0x22, 0x00, 0x02, 0x00, 0x00, UInt8(bc & 0xFF), UInt8(bc >> 8)] + ff(5),
          "command block: RAM write, FB06 address length 00 CRC (low byte first)")
    let cbw = buildCBW(tag: 0x1122_3344, dataLength: 0x8000, dataIn: true, cb: cdb(.readFlash, be32(0) + be16(0x8000)))
    check(cbw.count == 31 && Array(cbw[0..<15]) == [0x55, 0x53, 0x42, 0x43, 0x44, 0x33, 0x22, 0x11, 0x00, 0x80, 0x00, 0x00, 0x80, 0x00, 16]
          && Array(cbw[15..<17]) == [0xFD, 0x05], "CBW: USBC, tag, length, data-in, a 16-byte command block")
    check(buildCBW(tag: 1, dataLength: 512, dataIn: false, cb: writeMemoryCDB(loaderAddress, block))[12] == 0, "CBW: data-out for the RAM write")
    // Block and chip erase, chip key write, the flash command table: never.
    let forbidden: Set<UInt16> = [0xFB00, 0xFB02, 0xFC0D, 0xFC12]
    check(JLCmd.allCases.allSatisfy { !forbidden.contains($0.rawValue) },
          "none of the \(JLCmd.allCases.count) commands this build knows is a block or chip erase, a chip key write or the flash command table")
    let flashWriters = JLCmd.allCases.filter { [0xFB01, 0xFB04].contains($0.rawValue) }
    check(flashWriters == [.eraseSector, .writeFlash], "the only flash-changing commands are sector erase FB01 and flash write FB04")
    func refuses(_ f: () throws -> WritableSector) -> Bool { (try? f()) == nil }
    let rehearsal = 0x7F000..<0x80000
    check(eraseSectorCDB(try! WritableSector.within(0x7F000, allowed: rehearsal)) == [0xFB, 0x01, 0x00, 0x07, 0xF0, 0x00] + ff(10),
          "command block: sector erase, FB01 address")
    let page = testPattern(0x7F000, WritableSector.size)
    let pc = crc16(page)
    check(writeFlashCDB(try! WritableSector.within(0x7F000, allowed: rehearsal), offset: 0, page)
            == [0xFB, 0x04, 0x00, 0x07, 0xF0, 0x00, 0x10, 0x00, 0x00, UInt8(pc & 0xFF), UInt8(pc >> 8)] + ff(5),
          "command block: flash write, FB04 address length 00 CRC (low byte first)")
    check(page.contains { $0 != 0xFF } && Set(stride(from: 0, to: page.count, by: 256).map { Array(page[$0..<($0 + 256)]) }).count == 16,
          "the test pattern is not erased flash and differs in each 256-byte page")
    check(refuses { try WritableSector.within(0x0, allowed: 0..<0x80000) }
            && refuses { try WritableSector.within(0x7B000, allowed: 0x7B000..<0x80000) }
            && refuses { try WritableSector.within(0x7E000, allowed: 0x7E000..<0x80000) },
          "write and erase refuse sector 0, BTIF and USRFLASH whatever --allow says")
    check(refuses { try WritableSector.within(0x7E000, allowed: rehearsal) }
            && refuses { try WritableSector.within(0x7F100, allowed: 0x7F000..<0x80000) }
            && refuses { try WritableSector.within(0x80000, allowed: 0x7F000..<0x81000) },
          "write and erase refuse a sector outside --allow, an unaligned address and one past the flash")
    check(!refuses { try WritableSector.within(0x7F000, allowed: rehearsal) } && !refuses { try WritableSector.within(0x30000, allowed: 0x25000..<0x6E000) },
          "write and erase take a whole sector inside --allow")
    check(refuses { try WritableSector.restoring(0x0, protectedAllowed: false) } && refuses { try WritableSector.restoring(0x7D000, protectedAllowed: false) }
            && !refuses { try WritableSector.restoring(0x0, protectedAllowed: true) } && !refuses { try WritableSector.restoring(0x24000, protectedAllowed: false) },
          "restore writes protected sectors only with --protected")
    var plainDir = ff(0x2000)
    let entry: [UInt8] = [0xFF, 0xFF, 0, 0, 0x20, 0x01, 0, 0] + ff(8) + Array("app_area_head".utf8) + [0, 0xFF, 0xFF]
    let ec = crc16(Array(entry[2...]))
    plainDir.replaceSubrange(0x1000..<0x1020, with: [UInt8(ec & 0xFF), UInt8(ec >> 8)] + entry[2...])
    var cipherDir = plainDir
    cipherDir[0x1005] ^= 0x5A
    check(looksLikeStockRead(plainDir) && !looksLikeStockRead(cipherDir), "restore tells a stock read (plain directory) from a raw read")
    let loaderPath = option("--loader") ?? loaderDefault
    if let image = FileManager.default.contents(atPath: loaderPath).map({ [UInt8]($0) }) {
        let sha = SHA256.hash(data: Data(image)).map { String(format: "%02x", $0) }.joined()
        check(sha == loaderSHA256, "the loader \(loaderPath) is the pinned one (\(image.count) bytes)")
        check(image.count % loaderBlock == 0, "the loader is a whole number of \(loaderBlock)-byte blocks")
        var plain: [UInt8] = []
        for off in stride(from: 0, to: image.count, by: loaderBlock) {
            plain += mengli(Array(image[off..<min(off + loaderBlock, image.count)]))
        }
        check(Data(plain).range(of: Data("run loader ok".utf8)) != nil, "the loader descrambled per block reads \"run loader ok\"")
    } else {
        print("skip  the loader \(loaderPath) is not there (run from the repository root, or pass --loader)")
    }
    print(failed == 0 ? "selftest: all ok" : "selftest: \(failed) FAILED")
    if failed != 0 { exit(1) }

default:
    fail("unknown command \(command)")
}
