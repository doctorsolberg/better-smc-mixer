"""smclog: host side of the device logging (SPEC §8.2, format in docs/logging.md).

Usage: python3 -I tools/smclog/smclog.py COMMAND ...

  dict BINARY [-o DICT.json]
        Build the dictionary of a firmware ELF (or a host test binary, Mach-O): every log call site
        of the "smclog" section with its ID, and the build ID.
  decode (--dict DICT.json | --binary BINARY) INPUT [-o OUT.jsonl] [--text]
        Decode a raw log stream (frames as the device sends them) into JSON lines, or text.
  capture --port DEV (--dict DICT.json | --binary BINARY) --session DIR [--seconds N]
          [--midi MATCH] [--smcmidi PATH] [--no-subscribe] [--sync SECONDS]
        Record a session: subscribe to the device's log over the command link (docs/link.md), the
        raw stream from the serial port (USB CDC) and its decoded records with host timestamps,
        PING replies for clock synchronization, markers typed on the keyboard (a line of text and
        Enter; an empty line is a plain marker), and, with --midi, the device's MIDI traffic
        through smcmidi. Session layout (LOG-21): meta.json, device.bin, device.jsonl,
        markers.jsonl, midi.jsonl.
  cmd (--port DEV | --midi PORT) [--dict DICT.json | --binary BINARY] [--json] COMMAND [ARGS]
        Send one command over the command link and print its reply. COMMAND: ping, info,
        subscribe, unsubscribe, level MODULE LEVEL, levels, raw off|matrix|faders|on|probeN (on: both; probeN: encoder N alone, every 100 us), metrics, boot (the boot
        journal again), reset soft|fault|watchdog (fault and watchdog need inject on),
        inject on|off|EVENT... (EVENT: button:N:press|release, encoder:N:CLICKS, fader:N:VALUE,
        midi:usb|ble:B0,B1,B2 in hex), setup [NAME=VALUE...] (the core on a built-in configuration
        with settings, docs/link.md; needs inject on). MODULE is a name from the dictionary, a
        number or "all".

  metrics INPUT [--json] [--check] [--capture-window]
        Summary of a session's metrics heartbeats (LOG-06).
  boot INPUT [--binary ELF] [--json]
        The boot journal of a session (LOG-11): reset cause, the previous run's crash record and
        latest records, init times. With --binary, code addresses are named from the ELF's symbols.
  clock INPUT [--json]
        The device's µs clock against the host's (LOG-20), from the session's PING replies: a line
        per run of the device, its drift in ppm, round trips and how far the samples lie from it.

Records of the run before the last reset (replayed from the device's recent ring) decode with
"past": true and that run's seq and t_us; they take no part in gap detection or time unwrapping.
A stream whose build ID differs from the dictionary's is an error (LOG-20), not garbage. Host
timestamps are time.monotonic_ns(), the same clock as smcmidi (mach_absolute_time on macOS).
Standard library only.
"""
import argparse
import json
import os
import platform
import re
import select
import struct
import subprocess
import sys
import time

FORMAT_VERSION = 1
META_MAGIC = 0x01A54C53
KIND_LOG, KIND_LOST, KIND_HEADER, KIND_LOG_TRUNC = 1, 2, 3, 4
KIND_PAST = 0x80  # flag: a record of the run before the last reset (smclog.h)
KIND_CMD, KIND_REPLY, KIND_EVENT, KIND_SENT = 0x10, 0x11, 0x12, 0x13  # the command link (docs/link.md)
LINK_VERSION = 1
CMDS = {"ping": 0x01, "info": 0x02, "subscribe": 0x20, "unsubscribe": 0x21, "level": 0x22,
        "levels": 0x23, "raw": 0x24, "metrics": 0x25, "boot": 0x26, "reset": 0x27, "events": 0x28,
        "stock": 0x29, "inject": 0x30, "setup": 0x31}
# The configuration protocol (docs/link.md, "Configuration"): tools/smcconf/devlink.py sends them.
CFG_CMDS = {"cfg-info": 0x40, "cfg-get-begin": 0x41, "cfg-get-chunk": 0x42, "cfg-put-begin": 0x43,
            "cfg-put-chunk": 0x44, "cfg-put-commit": 0x45, "cfg-abort": 0x46, "cfg-set": 0x47,
            "cfg-save": 0x48, "cfg-revert": 0x49, "cfg-state": 0x4A, "cfg-select": 0x4B, "cfg-sent": 0x4C}
CMD_NAMES = {v: k for k, v in list(CMDS.items()) + list(CFG_CMDS.items())}
STATUS_NAMES = {0: "ok", 1: "unknown command", 2: "bad length", 3: "bad argument", 4: "unavailable"}
# LOG_RAW_INPUT's bits (link.h): 1 the matrix and the encoder lines, 2 the faders' ADC values.
RAW_WHICH = {"off": 0, "matrix": 1, "faders": 2, "on": 3}
RAW_WHICH.update({"probe%d" % n: 1 | n << 4 for n in range(1, 9)})  # the encoder probe (hwtest.md)
CAP_NAMES = {0: "log", 1: "raw_input", 2: "inject", 3: "metrics", 4: "setup", 5: "boot_log", 6: "reset",
             7: "config", 8: "events", 9: "sent", 10: "stock"}
VARIANT_NAMES = {0: "app", 1: "hwtest"}
INJECT_STATUS = {0: "ok", 1: "disabled", 2: "bad format", 3: "bad event", 4: "full"}
SETUP_STATUS = {0: "ok", 1: "disabled", 2: "bad setting", 3: "busy"}
RESET_HOW = {"soft": 0, "fault": 1, "watchdog": 2}  # link.h, enum link_reset
RESET_STATUS = {0: "ok", 1: "disabled"}
# STOCK (link.h; store/bootsel.h, enum bootsel_status): going back to M-Vave's firmware
STOCK_STATUS = {0: "ok", 1: "sector 0 does not parse", 2: "sector 0 does not point at ours",
                3: "M-Vave's area does not check", 4: "flash", 5: "busy", 6: "not offered"}
ALL_MODULES = 0xFF
T_INT, T_I64, T_F32, T_STR = 0, 1, 2, 3
TYPE_NAMES = {T_INT: "int", T_I64: "i64", T_F32: "f32", T_STR: "str"}
LEVELS = {1: "ERROR", 2: "WARN", 3: "INFO", 4: "DEBUG", 5: "TRACE"}


class SmclogError(Exception):
    pass


class BuildIdMismatch(SmclogError):
    pass


# --- binaries -------------------------------------------------------------------------------------

def _elf_sections(d):
    if d[4] not in (1, 2) or d[5] != 1:
        raise SmclogError("only little-endian ELF32/ELF64 is supported")
    is64 = d[4] == 2
    if is64:
        shoff, = struct.unpack_from("<Q", d, 0x28)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x3A)
        fmt = "<IIQQQQIIQQ"
    else:
        shoff, = struct.unpack_from("<I", d, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
        fmt = "<IIIIIIIIII"
    hdrs = [struct.unpack_from(fmt, d, shoff + i * shentsize) for i in range(shnum)]
    strtab = hdrs[shstrndx]
    names = d[strtab[4]:strtab[4] + strtab[5]]
    out = {}
    for name_off, sh_type, flags, addr, offset, size, *_ in hdrs:
        name = names[name_off:names.index(b"\0", name_off)].decode()
        data = None if sh_type == 8 else d[offset:offset + size]  # SHT_NOBITS has no contents
        out[name] = {"addr": addr, "data": data, "type": sh_type, "alloc": bool(flags & 2)}
    return out


def _gnu_build_id(data, i=0, end=None):
    """The ID of the first NT_GNU_BUILD_ID note in data[i:end], or None."""
    end = len(data) if end is None else end
    while i + 12 <= end:
        namesz, descsz, ntype = struct.unpack_from("<III", data, i)
        name = data[i + 12:i + 12 + namesz]
        desc_at = i + 12 + ((namesz + 3) & ~3)
        if ntype == 3 and name == b"GNU\0":
            return data[desc_at:desc_at + descsz]
        i = desc_at + ((descsz + 3) & ~3)
    return None


def _elf_symbol(d, sections, name):
    """Address of a symbol from .symtab, or None."""
    symtab, strtab = sections.get(".symtab"), sections.get(".strtab")
    if not symtab or not strtab or symtab["data"] is None:
        return None
    is64 = d[4] == 2
    size, fmt = (24, "<IBBHQQ") if is64 else (16, "<IIIBBH")
    want = name.encode() + b"\0"
    data, names = symtab["data"], strtab["data"]
    for i in range(0, len(data) - size + 1, size):
        fields = struct.unpack_from(fmt, data, i)
        if names[fields[0]:fields[0] + len(want)] == want:
            return fields[4] if is64 else fields[1]
    return None


def _elf_build_id(d, sections):
    """From a SHT_NOTE section, or, when the linker script puts the note inside another section
    (our image keeps it in .text, so it is part of app.bin), at the __build_id_note symbol."""
    for s in sections.values():
        if s["type"] == 7 and s["data"]:
            bid = _gnu_build_id(s["data"])
            if bid:
                return bid
    addr = _elf_symbol(d, sections, "__build_id_note")
    for s in sections.values():
        if addr is not None and s["alloc"] and s["data"] and s["addr"] <= addr < s["addr"] + len(s["data"]):
            return _gnu_build_id(s["data"], addr - s["addr"], addr - s["addr"] + 64)
    return None


def _macho(d):
    if struct.unpack_from("<I", d, 0)[0] != 0xFEEDFACF:
        raise SmclogError("only 64-bit little-endian Mach-O is supported (no universal binaries)")
    ncmds, = struct.unpack_from("<I", d, 16)
    at, sections, uuid = 32, {}, None
    for _ in range(ncmds):
        cmd, cmdsize = struct.unpack_from("<II", d, at)
        if cmd == 0x19:  # LC_SEGMENT_64
            nsects, = struct.unpack_from("<I", d, at + 64)
            for k in range(nsects):
                s = at + 72 + 80 * k
                sect = d[s:s + 16].rstrip(b"\0").decode()
                addr, size, offset = struct.unpack_from("<QQI", d, s + 32)
                sections[sect] = {"addr": addr, "data": d[offset:offset + size], "type": 1, "alloc": True}
        elif cmd == 0x1B:  # LC_UUID
            uuid = d[at + 8:at + 24]
        at += cmdsize
    return sections, uuid


def read_binary(path):
    """(the smclog section bytes, build ID bytes or None, a note about the section)."""
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] == b"\x7fELF":
        sections = _elf_sections(d)
        sec = sections.get("smclog")
        if sec is None:
            raise SmclogError("%s: no smclog section (no log call sites?)" % path)
        if sec["data"] is None:
            raise SmclogError("%s: the smclog section has no contents (NOLOAD); it must be INFO" % path)
        note = "allocated: the call sites take memory on the device" if sec["alloc"] else "not loaded"
        return sec["data"], _elf_build_id(d, sections), note
    sections, uuid = _macho(d)
    sec = sections.get("__smclog")
    if sec is None:
        raise SmclogError("%s: no __smclog section" % path)
    return sec["data"], uuid, "host binary"


def parse_sites(data):
    """Call sites of a smclog section: {id: site}; id = offset / 4."""
    sites, off = {}, 0
    while off + 16 <= len(data):
        if struct.unpack_from("<I", data, off)[0] != META_MAGIC:
            off += 4  # alignment padding between entries
            continue
        size, line, level, module, _reserved, types = struct.unpack_from("<HHBBHI", data, off + 4)
        if size < 16 or off + size > len(data):
            raise SmclogError("bad call site entry at offset %d" % off)
        strings = data[off + 16:off + size].split(b"\0")
        if len(strings) < 4:
            raise SmclogError("bad call site strings at offset %d" % off)
        count = types & 0xF
        sites[off // 4] = {
            "level": LEVELS.get(level, str(level)),
            "module": strings[0].decode(),
            "module_index": module,
            "file": strings[1].decode(),
            "line": line,
            "format": strings[2].decode(),
            "types": [TYPE_NAMES[(types >> (4 + 2 * i)) & 3] for i in range(count)],
        }
        off += (size + 3) & ~3
    return sites


def make_dictionary(path):
    data, build_id, note = read_binary(path)
    return {
        "format": FORMAT_VERSION,
        "build_id": build_id.hex() if build_id else None,
        "binary": os.path.basename(path),
        "section": note,
        "sites": {str(k): v for k, v in sorted(parse_sites(data).items())},
    }


def load_dictionary(dict_path=None, binary=None):
    if binary:
        return make_dictionary(binary)
    with open(dict_path) as f:
        d = json.load(f)
    if d.get("format") != FORMAT_VERSION:
        raise SmclogError("%s: dictionary format %r, expected %d" % (dict_path, d.get("format"), FORMAT_VERSION))
    return d


# --- frames ---------------------------------------------------------------------------------------

def crc16(data):
    """CRC-16/CCITT-FALSE."""
    c = 0xFFFF
    for b in data:
        c ^= b << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def cobs_encode(data):
    out, block = bytearray(), bytearray()
    for b in data:
        if b == 0:
            out += bytes([len(block) + 1]) + block
            block = bytearray()
        else:
            block.append(b)
            if len(block) == 254:
                out += b"\xff" + block
                block = bytearray()
    out += bytes([len(block) + 1]) + block
    return bytes(out)


def cobs_decode(data):
    out, i = bytearray(), 0
    while i < len(data):
        code = data[i]
        if code == 0 or i + code > len(data):
            return None
        out += data[i + 1:i + code]
        i += code
        if code < 0xFF and i < len(data):
            out.append(0)
    return bytes(out)


def frame(record):
    """A wire frame for a record (tests, simulators)."""
    return cobs_encode(record + struct.pack("<H", crc16(record))) + b"\0"


# --- records --------------------------------------------------------------------------------------

def _varint(b, i):
    v = shift = 0
    while True:
        if i >= len(b):
            raise IndexError
        v |= (b[i] & 0x7F) << shift
        shift += 7
        i += 1
        if not b[i - 1] & 0x80:
            return v, i


def decode_args(types, b):
    """[(value, bits)] in the order of types; None for arguments the record does not hold."""
    args, i = [], 0
    for t in types:
        try:
            if t in ("int", "i64"):
                v, i = _varint(b, i)
                args.append(((v >> 1) ^ -(v & 1), 32 if t == "int" else 64))
            elif t == "f32":
                if i + 4 > len(b):
                    raise IndexError
                args.append((struct.unpack_from("<f", b, i)[0], 32))
                i += 4
            else:
                n = b[i]
                s = b[i + 1:i + 1 + (n & 0x7F)].decode("utf-8", "replace")
                args.append((s + ("..." if n & 0x80 else ""), 0))
                i += 1 + (n & 0x7F)
        except IndexError:
            args.append(None)
    return args


SPEC = re.compile(r"%([-+ #0]*)(\d+)?(?:\.(\d+))?(hh|h|ll|l|j|z|t|L|q)?([diouxXcsfFeEgGaAp%])")


def render(fmt, args):
    """printf-style formatting of decoded arguments; missing ones show as <?>."""
    out, pos, k = [], 0, 0
    for m in SPEC.finditer(fmt):
        out.append(fmt[pos:m.start()])
        pos = m.end()
        flags, width, prec, _length, conv = m.groups()
        if conv == "%":
            out.append("%")
            continue
        a = args[k] if k < len(args) else None
        k += 1
        if a is None:
            out.append("<?>")
            continue
        v, bits = a
        spec = "%" + flags + (width or "") + ("." + prec if prec is not None else "")
        try:
            if conv in "di":
                out.append((spec + "d") % v)
            elif conv in "ouxX":
                out.append((spec + conv.replace("u", "d")) % (v & ((1 << bits) - 1)))
            elif conv == "c":
                out.append((spec + "c") % (v & 0xFF))
            elif conv == "p":
                out.append("0x%x" % (v & ((1 << bits) - 1)))
            elif conv == "s":
                out.append((spec + "s") % (v,))
            else:
                out.append((spec + (conv if conv not in "aA" else "g")) % v)
        except (TypeError, ValueError, OverflowError):
            out.append("<%%%s? %r>" % (conv, v))
    out.append(fmt[pos:])
    return "".join(out)


class Decoder:
    """Turns stream bytes into records (dicts). Keeps the partial frame, the time base (the
    device's 32-bit µs counter is unwrapped) and the expected sequence number."""

    def __init__(self, dictionary):
        self.sites = {int(k): v for k, v in dictionary["sites"].items()}
        self.build_id = dictionary.get("build_id")
        self.pending = bytearray()
        self.t_base = 0
        self.t_last = None
        self.seq_next = None
        self.header_seen = False
        self.warned_no_header = False

    def _time(self, t):
        if self.t_last is not None and t < self.t_last and self.t_last - t > 1 << 31:
            self.t_base += 1 << 32
        self.t_last = t
        return self.t_base + t

    def _seq(self, seq, out):
        if self.seq_next is not None and seq != self.seq_next:
            out.append({"kind": "gap", "missing": (seq - self.seq_next) & 0xFFFF, "seq": seq})
        self.seq_next = (seq + 1) & 0xFFFF

    def feed(self, data, t_host_ns=None):
        out = []
        self.pending += data
        while True:
            try:
                end = self.pending.index(0)
            except ValueError:
                return out
            raw = bytes(self.pending[:end])
            del self.pending[:end + 1]
            for r in self.decode_frame(raw):
                if t_host_ns is not None:
                    r["t_host_ns"] = t_host_ns
                out.append(r)

    def decode_frame(self, raw):
        rec = cobs_decode(raw)
        if rec is None or len(rec) < 3 or crc16(rec[:-2]) != struct.unpack_from("<H", rec, len(rec) - 2)[0]:
            return [{"kind": "bad_frame", "len": len(raw), "hex": raw[:32].hex()}]
        rec = rec[:-2]
        kind, out = rec[0], []
        past = bool(kind & KIND_PAST) and (kind & 0x7F) in (KIND_LOG, KIND_LOG_TRUNC)
        if past:
            kind &= 0x7F
        if kind == KIND_REPLY and len(rec) >= 4:
            return [self.decode_reply(rec)]
        if kind in (KIND_EVENT, KIND_SENT) and len(rec) >= 2:
            return [event_record(rec)]
        if kind == KIND_HEADER and len(rec) >= 19:
            version, hz, t, lost, ring = struct.unpack_from("<BIIII", rec, 1)
            bid = rec[19:19 + rec[18]].hex()
            if version != FORMAT_VERSION:
                raise SmclogError("stream format %d, this tool reads %d" % (version, FORMAT_VERSION))
            if self.build_id and bid != self.build_id:
                raise BuildIdMismatch("stream build ID %s, dictionary build ID %s" % (bid, self.build_id))
            self.header_seen = True
            self.seq_next = None
            out.append({"kind": "header", "version": version, "timer_hz": hz, "t_us": self._time(t),
                        "lost_total": lost, "ring_size": ring, "build_id": bid})
            return out
        if not self.header_seen and not self.warned_no_header:
            self.warned_no_header = True
            out.append({"kind": "warning", "msg": "records before a stream header: build ID not checked"})
        if kind in (KIND_LOG, KIND_LOG_TRUNC) and len(rec) >= 11:
            seq, t, sid, cause = struct.unpack_from("<HIHH", rec, 1)
            if past:  # the previous run's numbers: not this stream's
                r = {"kind": "log", "seq": seq, "t_us": t, "id": sid, "cause": cause, "past": True}
            else:
                self._seq(seq, out)
                r = {"kind": "log", "seq": seq, "t_us": self._time(t), "id": sid, "cause": cause}
            site = self.sites.get(sid)
            if site is None:
                r["error"] = "unknown call site"
                r["hex"] = rec[11:].hex()
            else:
                args = decode_args(site["types"], rec[11:])
                r.update(level=site["level"], module=site["module"], msg=render(site["format"], args),
                         args=[a[0] if a else None for a in args], file=site["file"], line=site["line"])
                if kind == KIND_LOG_TRUNC:
                    r["truncated"] = True
            out.append(r)
        elif kind == KIND_LOST and len(rec) >= 11:
            seq, t, count = struct.unpack_from("<HII", rec, 1)
            self._seq(seq, out)
            out.append({"kind": "lost", "seq": seq, "t_us": self._time(t), "count": count})
        else:
            out.append({"kind": "unknown", "hex": rec.hex()})
        return out


    def decode_reply(self, rec):
        seq, cmd, status = rec[1], rec[2], rec[3]
        p = rec[4:]
        r = {"kind": "reply", "seq": seq, "cmd": CMD_NAMES.get(cmd, "0x%02x" % cmd),
             "status": STATUS_NAMES.get(status, str(status)), "payload": p.hex()}
        if status != 0:
            return r
        name = r["cmd"]
        if name == "ping" and len(p) >= 4:
            r["t_us"] = self._time(struct.unpack_from("<I", p)[0])
            r["echo"] = p[4:].hex()
            if len(p) == 12:  # capture's clock sync: the host's send time
                r["t_host_send_ns"] = struct.unpack_from("<Q", p, 4)[0]
        elif name == "info" and len(p) >= 16:
            version, fmt, variant, caps, ring, hz, idlen = struct.unpack_from("<BBBIIIB", p)
            r.update(link_version=version, log_format=fmt, variant=VARIANT_NAMES.get(variant, variant),
                     caps=[n for b, n in CAP_NAMES.items() if caps >> b & 1], ring_size=ring, timer_hz=hz,
                     build_id=p[16:16 + idlen].hex())
            if len(p) > 16 + idlen:  # the product's version, after the build ID (docs/link.md)
                vlen = p[16 + idlen]
                r["fw_version"] = p[17 + idlen:17 + idlen + vlen].decode("ascii", "replace")
        elif name == "levels" and len(p) >= 1:
            r["levels"] = list(p[1:1 + p[0]])
        elif name == "inject" and len(p) >= 3:
            r.update(inject=INJECT_STATUS.get(p[0], str(p[0])), accepted=p[1], refused=p[2])
        elif name == "setup" and len(p) >= 2:
            r.update(setup=SETUP_STATUS.get(p[0], str(p[0])), bad_pair=p[1])
        elif name == "reset" and len(p) >= 1:
            r["reset"] = RESET_STATUS.get(p[0], str(p[0]))
        elif name == "stock" and len(p) >= 6:
            r.update(stock=STOCK_STATUS.get(p[0], str(p[0])), pointer=struct.unpack_from("<I", p, 1)[0], tries=p[5])
        return r


def event_record(rec):
    """An EVENT (docs/link.md, "Rules": events): its count and the panel's state, as hex
    (tools/smcconf/devlink.py, parse_state(), reads its fields); or a SENT (P-5): the first entry's
    number, the entries lost before it, and the entries of 7 bytes, as hex (devlink.py,
    parse_sent())."""
    if rec[0] == KIND_SENT:
        if len(rec) < 7 or len(rec) != 7 + 7 * rec[6]:
            return {"kind": "sent", "bad": rec.hex()}
        first, lost, n = struct.unpack_from("<IBB", rec, 1)
        return {"kind": "sent", "first": first, "lost": lost,
                "entries": [rec[7 + 7 * i:14 + 7 * i].hex() for i in range(n)]}
    return {"kind": "event", "count": rec[1], "state": rec[2:].hex()}


def command_frame(seq, cmd, payload=b""):
    """A CMD frame, with a leading delimiter that ends any partial frame on the device."""
    return b"\0" + frame(bytes([KIND_CMD, seq & 0xFF, cmd]) + bytes(payload))


# --- the command link over SysEx (docs/link.md, "Over SysEx"; SPEC Q-28) -------------------------

SYSEX_HEADER = b"\xF0\x7D\x53\x4D"  # F0, the non-commercial ID 7D, the tag "SM"


def pack7(data):
    """Each group of up to 7 bytes: a byte of their top bits (bit i: byte i's), then the bytes with
    their top bits cleared."""
    out = bytearray()
    for i in range(0, len(data), 7):
        g = data[i:i + 7]
        out.append(sum(((b >> 7) & 1) << j for j, b in enumerate(g)))
        out += bytes(b & 0x7F for b in g)
    return bytes(out)


def unpack7(data):
    """The reverse; None when it is not valid packing."""
    out = bytearray()
    for i in range(0, len(data), 8):
        g = data[i:i + 8]
        if len(g) < 2 or any(b > 0x7F for b in g):
            return None
        out += bytes(b | ((g[0] >> (j - 1)) & 1) << 7 for j, b in enumerate(g) if j)
    return bytes(out)


def sysex_frame(record):
    """A record (kind to the end of its payload) as a whole SysEx: the header, the record and its
    CRC-16 (little-endian) 7-bit packed, F7."""
    return SYSEX_HEADER + pack7(bytes(record) + struct.pack("<H", crc16(record))) + b"\xF7"


def sysex_command(seq, cmd, payload=b""):
    return sysex_frame(bytes([KIND_CMD, seq & 0xFF, cmd]) + bytes(payload))


def sysex_record(msg):
    """The record a whole SysEx carries (CRC checked and removed); None when it is not ours or
    broken."""
    if not msg.startswith(SYSEX_HEADER) or not msg.endswith(b"\xF7"):
        return None
    rec = unpack7(msg[len(SYSEX_HEADER):-1])
    if rec is None or len(rec) < 3 or crc16(rec[:-2]) != struct.unpack_from("<H", rec, len(rec) - 2)[0]:
        return None
    return rec[:-2]


class SysexLink:
    """The command link over SysEx on a MIDI port (tools/hostmidi: send(msg), receive(timeout)):
    the same calls as Link, without the log stream."""

    def __init__(self, port):
        self.port = port
        self.dec = Decoder({"sites": {}})
        self.seq = os.urandom(1)[0]
        self.foreign = 0  # SysEx that was not a reply of ours
        self.events = []  # EVENT and SENT records (event_record()) not taken yet: poll() takes them

    def _event(self, rec):
        if rec is not None and rec[0] in (KIND_EVENT, KIND_SENT) and len(rec) >= 2:
            self.events.append(event_record(rec))
            return True
        return False

    def poll(self, timeout):
        """The EVENTs that came, waiting up to timeout for the first one if none has."""
        deadline = time.monotonic() + timeout
        while not self.events and time.monotonic() < deadline:
            for m in self.port.receive(max(0.0, deadline - time.monotonic())):
                if not self._event(sysex_record(m)):
                    self.foreign += 1
        out, self.events = self.events, []
        return out

    def call(self, cmd, payload=b"", timeout=1.0, tries=3):
        self.seq = seq = (self.seq + 1) & 0xFF
        msg = sysex_command(seq, cmd, payload)
        for _ in range(tries):
            self.port.send(msg)
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                found = None
                for m in self.port.receive(max(0.0, deadline - time.monotonic())):
                    rec = sysex_record(m)
                    if self._event(rec):  # an EVENT after the reply, in the same batch, is kept too
                        continue
                    if rec is None or rec[0] != KIND_REPLY or len(rec) < 4:
                        self.foreign += 1
                        continue
                    r = self.dec.decode_reply(rec)
                    if found is None and r["seq"] == seq and r["cmd"] == CMD_NAMES.get(cmd):
                        found = r
                if found:
                    return found
        raise SmclogError("no reply to %s after %d tries" % (CMD_NAMES.get(cmd, cmd), tries))


def as_text(r):
    k = r["kind"]
    if k == "log":
        t = ("prev%8.3f" if r.get("past") else "%12.6f") % (r["t_us"] / 1e6)
        cause = " #%d" % r["cause"] if r["cause"] else ""
        if "error" in r:
            return "%s ?     id %d%s: %s %s" % (t, r["id"], cause, r["error"], r["hex"])
        return "%s %-5s %-8s%s %s%s" % (t, r["level"], r["module"], cause, r["msg"],
                                        " [truncated]" if r.get("truncated") else "")
    if k == "lost":
        return "%12.6f LOST  %d records dropped on the device" % (r["t_us"] / 1e6, r["count"])
    if k == "header":
        return "%12.6f ---   stream start: build %s, format %d, ring %d bytes, %d lost so far" % (
            r["t_us"] / 1e6, r["build_id"], r["version"], r["ring_size"], r["lost_total"])
    if k == "reply":
        extra = {x: y for x, y in r.items() if x not in ("kind", "seq", "cmd", "status", "payload", "t_host_ns")}
        return "             REPLY %s seq %d: %s%s" % (r["cmd"], r["seq"], r["status"],
                                                      " " + json.dumps(extra, sort_keys=True) if extra else "")
    return "             %-5s %s" % (k.upper(), json.dumps({x: y for x, y in r.items() if x != "kind"}))


# --- commands -------------------------------------------------------------------------------------

def cmd_dict(a):
    d = make_dictionary(a.binary)
    text = json.dumps(d, indent=1, sort_keys=True)
    if a.output:
        with open(a.output, "w") as f:
            f.write(text + "\n")
    print("%s: %d call sites, build ID %s, section %s" % (a.binary, len(d["sites"]), d["build_id"], d["section"]),
          file=sys.stderr)
    if not a.output:
        print(text)


def cmd_decode(a):
    dec = Decoder(load_dictionary(a.dict, a.binary))
    with open(a.input, "rb") as f:
        recs = dec.feed(f.read())
    if dec.pending:
        recs.append({"kind": "partial_frame", "len": len(dec.pending)})
    out = open(a.output, "w") if a.output else sys.stdout
    for r in recs:
        out.write((as_text(r) if a.text else json.dumps(r, sort_keys=True)) + "\n")
    if a.output:
        out.close()


def open_serial(path):
    """An open CDC port's file descriptor, raw. "auto": the mixer's port found by its USB IDs
    (tools/hostserial; every tool's --port goes through here)."""
    if path == "auto":
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "hostserial"))
        import hostserial
        path = hostserial.find_one()
    fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    if os.isatty(fd):
        import termios
        import tty
        tty.setraw(fd, termios.TCSANOW)
    return fd


class Link:
    """The command link (docs/link.md) on an open serial port: sends commands, waits for their
    replies, retries with the same seq (the device answers a retry without running it again,
    FR-CFG-03). Records that are not the awaited reply go to on_record."""

    def __init__(self, fd, decoder, on_record=None):
        self.fd = fd
        self.dec = decoder
        self.on_record = on_record
        self.seq = os.urandom(1)[0]  # a restarted tool must not repeat the last seq it used
        self.events = []  # EVENT records not taken yet (poll())

    def write(self, data):
        while data:
            select.select([], [self.fd], [], 1.0)
            try:
                n = os.write(self.fd, data)
            except BlockingIOError:
                continue
            data = data[n:]

    def send(self, cmd, payload=b""):
        self.seq = (self.seq + 1) & 0xFF
        self.write(command_frame(self.seq, cmd, payload))
        return self.seq

    def read(self, timeout):
        """Records from what arrives within timeout (at least one read attempt)."""
        ready, _, _ = select.select([self.fd], [], [], timeout)
        if not ready:
            return [], b""
        try:
            data = os.read(self.fd, 4096)
        except BlockingIOError:
            return [], b""
        if data == b"":
            raise SmclogError("the port was closed")
        return self.dec.feed(data, time.monotonic_ns()), data

    def call(self, cmd, payload=b"", timeout=1.0, tries=3):
        seq = self.send(cmd, payload)
        for attempt in range(tries):
            if attempt:
                self.write(command_frame(seq, cmd, payload))
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                recs, _ = self.read(max(0.0, deadline - time.monotonic()))
                found = None
                for r in recs:
                    if found is None and r["kind"] == "reply" and r["seq"] == seq and r["cmd"] == CMD_NAMES.get(cmd):
                        found = r
                    elif r["kind"] in ("event", "sent"):
                        self.events.append(r)
                    elif self.on_record:
                        self.on_record(r)
                if found:
                    return found
        raise SmclogError("no reply to %s after %d tries" % (CMD_NAMES.get(cmd, cmd), tries))

    def poll(self, timeout):
        """The EVENTs that came, waiting up to timeout for the first one if none has; other records
        go to on_record."""
        deadline = time.monotonic() + timeout
        while not self.events and time.monotonic() < deadline:
            recs, _ = self.read(max(0.0, deadline - time.monotonic()))
            for r in recs:
                if r["kind"] in ("event", "sent"):
                    self.events.append(r)
                elif self.on_record:
                    self.on_record(r)
        out, self.events = self.events, []
        return out


def module_index(name, dictionary):
    if name == "all":
        return ALL_MODULES
    if name.isdigit():
        return int(name)
    names = {s["module"]: s["module_index"] for s in dictionary.get("sites", {}).values() if "module_index" in s}
    if name not in names:
        raise SmclogError("module %r: not in the dictionary (known: %s); give its number" %
                          (name, ", ".join(sorted(names)) or "none, no dictionary given"))
    return names[name]


def level_value(name):
    by_name = {v.lower(): k for k, v in LEVELS.items()}
    by_name["off"] = 0
    if name.isdigit() and int(name) <= 5:
        return int(name)
    if name.lower() not in by_name:
        raise SmclogError("level %r: one of off, error, warn, info, debug, trace or 0-5" % name)
    return by_name[name.lower()]


INJECT_TYPES = {"button": 1, "encoder": 2, "fader": 3, "midi": 4}
TRANSPORTS = {"usb": 0, "ble": 1}
SETUP_TEXT_MAX = 180  # CORE_SETTINGS_TEXT_MAX


def inject_payload(args):
    """INPUT_INJECT payload (docs/hwtest.md): on, off or events."""
    if args == ["on"]:
        return b"\x01"
    if args == ["off"]:
        return b"\x00"
    out = bytearray([2])
    for e in args:
        parts = e.split(":")
        try:
            kind = parts[0]
            if kind == "button" and len(parts) == 3 and parts[2] in ("press", "release"):
                out += bytes([1, int(parts[1]), parts[2] == "press", 0, 0])
            elif kind in ("encoder", "fader") and len(parts) == 3:
                out += bytes([INJECT_TYPES[kind], int(parts[1])]) + struct.pack("<h", int(parts[2])) + b"\0"
            elif kind == "midi" and len(parts) == 3 and parts[1] in TRANSPORTS:
                msg = bytes(int(x, 16) for x in parts[2].split(","))
                if len(msg) != 3:
                    raise ValueError
                out += bytes([4, TRANSPORTS[parts[1]]]) + msg
            else:
                raise ValueError
        except (ValueError, IndexError, struct.error, OverflowError):
            raise SmclogError("inject: bad event %r (button:N:press|release, encoder:N:CLICKS, fader:N:VALUE, "
                              "midi:usb|ble:B0,B1,B2)" % e)
    if len(out) == 1:
        raise SmclogError("inject: on, off or at least one event")
    return bytes(out)


def setup_payload(args):
    """CORE_SETUP payload (docs/link.md): NAME=VALUE arguments as "NAME VALUE" lines."""
    lines = []
    for x in args:
        name, eq, value = x.partition("=")
        if not eq or not name or not value or any(c.isspace() for c in x):
            raise SmclogError("setup: NAME=VALUE (for example config=test note_off_p2=8n), not %r" % x)
        lines.append("%s %s" % (name, value))
    text = "\n".join(lines).encode()
    if len(text) > SETUP_TEXT_MAX:
        raise SmclogError("setup: at most %d bytes of settings" % SETUP_TEXT_MAX)
    return text


def command_payload(a, dictionary):
    c, args = a.command, a.args
    want = {"ping": (0, 1), "info": (0, 0), "subscribe": (0, 0), "unsubscribe": (0, 0), "level": (2, 2),
            "levels": (0, 0), "raw": (1, 1), "metrics": (0, 0), "boot": (0, 0), "reset": (1, 1),
            "inject": (1, 32), "setup": (0, 16), "events": (1, 1), "stock": (1, 1)}[c]
    if not want[0] <= len(args) <= want[1]:
        raise SmclogError("%s takes %s arguments" % (c, want[0] if want[0] == want[1] else "%d-%d" % want))
    if c == "ping":
        return args[0].encode() if args else b""
    if c == "level":
        return bytes([module_index(args[0], dictionary), level_value(args[1])])
    if c == "raw":
        if args[0] not in RAW_WHICH:
            raise SmclogError("raw %s" % "|".join(RAW_WHICH))
        return bytes([RAW_WHICH[args[0]]])
    if c == "reset":
        if args[0] not in RESET_HOW:
            raise SmclogError("reset %s" % "|".join(RESET_HOW))
        return bytes([RESET_HOW[args[0]]])
    if c == "stock":
        if args[0] not in ("check", "back"):
            raise SmclogError("stock check|back")
        return b"\x00" if args[0] == "check" else b"\x01BACK"
    if c == "events":
        what = {"off": 0, "on": 1, "panel": 1, "sent": 2, "both": 3}
        if args[0] not in what:
            raise SmclogError("events %s" % "|".join(what))
        return bytes([what[args[0]]])
    if c == "inject":
        return inject_payload(args)
    if c == "setup":
        return setup_payload(args)
    return b""


# --- metrics (LOG-06) -------------------------------------------------------------------------------

LAT_BOUNDS_US = (250, 500, 1000, 2000, 3000, 5000, 10000)  # firmware/metrics/metrics.c, lat_bound_us
LAT_BOUNDS_WIDE_US = (5000, 10000, 15000, 20000, 25000, 35000, 50000)  # lat_bound_wide_us
LAT_PATHS = {"core": "input to core", "usb": "input to USB", "ble": "input to BLE"}
LAT_PATH_BOUNDS = {"core": LAT_BOUNDS_US, "usb": LAT_BOUNDS_US, "ble": LAT_BOUNDS_WIDE_US}
NFR01_P50_US, NFR01_P99_US = 3000, 5000  # SPEC NFR-01: input -> MIDI, typical and p99
NFR02_P50_US = 25000  # SPEC NFR-02: input -> BLE-MIDI, median
STACK_MARGIN = 256  # bytes free below the deepest point sampled, at least (--check)


def lat_percentile(buckets, max_us, permille, bounds=LAT_BOUNDS_US):
    """As lat_hist_percentile(): the bound of the bucket the sample of that rank falls in, at most
    max_us; max_us for the last bucket; 0 when empty."""
    n = sum(buckets)
    if not n:
        return 0
    rank = max(1, -(-n * min(permille, 1000) // 1000))
    seen = 0
    for count, bound in zip(buckets, bounds):
        seen += count
        if seen >= rank:
            return min(bound, max_us)
    return max_us


def read_metrics(records):
    """The heartbeats in decoded records (module "metrics", docs/logging.md): a list of dicts."""
    beats = []
    for r in records:
        if r.get("kind") != "log" or r.get("module") != "metrics" or not r.get("args"):
            continue
        m, a = r["msg"], r["args"]
        if m.startswith("heartbeat "):
            beats.append({"t_us": r["t_us"], "seq": a[0], "window_ms": a[1],
                          "cpu": {"scan": a[2], "control": a[3], "usb": a[4]}, "latency": {}, "stacks": {}})
            if "t_host_ns" in r:
                beats[-1]["t_host_ns"] = r["t_host_ns"]
            continue
        if not beats:
            continue
        b = beats[-1]
        for key, name in LAT_PATHS.items():
            if m.startswith("latency %s, events by bucket:" % name):
                b["latency"].setdefault(key, {})["buckets"] = list(a[:8])
            elif m.startswith("latency %s:" % name):
                b["latency"][key] = dict(zip(("n", "min", "avg", "max", "p50", "p99"), a),
                                         buckets=[0] * len(LAT_BOUNDS_US) + [0])
        if m.startswith("scan steps:"):
            b["scan_steps"] = dict(zip(("steps", "skipped", "avg_ns", "max_ns", "late_max_us"), a))
        elif m.startswith("scan passes:"):
            b["scan_passes"] = dict(zip(("n", "min_us", "avg_us", "max_us"), a))
        elif m.startswith("clock:"):
            b["clock"] = dict(zip(("resyncs", "drift_min_us", "drift_max_us"), a))
        elif m.startswith("CPU load:"):
            b["load"] = dict(zip(("per_mille", "busy", "samples"), a))
        elif m.startswith("fader ADC:"):
            b["adc"] = dict(zip(("samples", "fader_every_us", "deferred", "lost", "timeouts", "steps_max"), a))
        elif m.startswith("BLE:"):
            b["ble"] = dict(zip(("out", "notify", "merged", "dropped", "queue_max", "refused", "in",
                                 "in_bad"), a))
        elif m.startswith("traffic:"):
            b["traffic"] = dict(zip(("events", "midi_out", "midi_in", "midi_dropped", "input_queue_max",
                                     "midi_queue_max", "input_refused", "midi_refused"), a))
        elif m.startswith("log:"):
            b["log"] = dict(zip(("records", "lost", "ring_max", "ring"), a))
        elif m.startswith("heap:"):
            b["heap"] = dict(zip(("free", "lowest"), a))
        elif m.startswith("stack ") and "free below the deepest" in m:
            b["stacks"][a[0]] = {"unused": a[1], "size": a[2]}
        elif m.startswith("stack ") and "not measured" in m:
            b["stacks"][a[0]] = {"unused": None, "size": a[1]}
    return beats


def summarize_metrics(beats):
    """One summary of a session's heartbeats: the latency histograms added up, the worst of the rest,
    and the checks (NFR-01 on the device's side, stack margins) with what failed."""
    s = {"heartbeats": len(beats), "cpu_max": {}, "latency": {}, "stacks": {}, "failed": []}
    if not beats:
        return s
    if "t_host_ns" in beats[0] and "t_host_ns" in beats[-1]:  # the host's time: the device's restarts at a reset
        s["span_s"] = round((beats[-1]["t_host_ns"] - beats[0]["t_host_ns"]) / 1e9, 3)
    else:
        s["span_s"] = round((beats[-1]["t_us"] - beats[0]["t_us"]) / 1e6, 3)
    for k in ("scan", "control", "usb"):
        s["cpu_max"][k] = max(b["cpu"][k] for b in beats)
    for key in LAT_PATHS:
        parts = [b["latency"][key] for b in beats if key in b["latency"]]
        buckets = [sum(p["buckets"][i] for p in parts) for i in range(len(LAT_BOUNDS_US) + 1)]
        n = sum(p["n"] for p in parts)
        mx = max([p["max"] for p in parts if p["n"]] or [0])
        bounds = LAT_PATH_BOUNDS[key]
        s["latency"][key] = {"n": n, "max": mx, "buckets": buckets,
                             "p50": lat_percentile(buckets, mx, 500, bounds),
                             "p99": lat_percentile(buckets, mx, 990, bounds)}
    logs = [b["log"] for b in beats if "log" in b]
    if logs:
        s["log"] = {"lost": sum(x["lost"] for x in logs), "ring_max": max(x["ring_max"] for x in logs),
                    "ring": logs[-1]["ring"]}
    heaps = [b["heap"] for b in beats if "heap" in b]
    if heaps:
        s["heap"] = {"free_min": min(x["free"] for x in heaps), "lowest": min(x["lowest"] for x in heaps)}
    passes = [b["scan_passes"] for b in beats if b.get("scan_passes", {}).get("n")]
    steps = [b["scan_steps"] for b in beats if "scan_steps" in b]
    if steps:
        s["scan"] = {"skipped": sum(x["skipped"] for x in steps), "handler_max_ns": max(x["max_ns"] for x in steps),
                     "late_max_us": max(x["late_max_us"] for x in steps),
                     "pass_max_us": max([x["max_us"] for x in passes] or [0])}
    clocks = [b["clock"] for b in beats if "clock" in b]
    if clocks:
        s["clock"] = {"resyncs": sum(x["resyncs"] for x in clocks),
                      "drift_min_us": min(x["drift_min_us"] for x in clocks),
                      "drift_max_us": max(x["drift_max_us"] for x in clocks)}
    loads = [b["load"] for b in beats if b.get("load", {}).get("samples")]
    if loads:
        s["load"] = {"max": max(x["per_mille"] for x in loads),
                     "mean": sum(x["busy"] for x in loads) * 1000 // sum(x["samples"] for x in loads)}
    adcs = [b["adc"] for b in beats if "adc" in b]
    if adcs:
        s["adc"] = {k: sum(x[k] for x in adcs) for k in ("samples", "deferred", "lost", "timeouts")}
        s["adc"]["steps_max"] = max(x["steps_max"] for x in adcs)
        s["adc"]["fader_every_max_us"] = max(x["fader_every_us"] for x in adcs)
        if s["adc"]["timeouts"]:
            s["failed"].append("fader ADC: %d conversions never finished" % s["adc"]["timeouts"])
    traffic = [b["traffic"] for b in beats if "traffic" in b]
    if traffic:
        s["traffic"] = {k: (max if k.endswith("_max") else sum)(x[k] for x in traffic) for k in traffic[0]}
    bles = [b["ble"] for b in beats if "ble" in b]
    if bles:
        s["ble"] = {k: (max if k.endswith("_max") else sum)(x[k] for x in bles) for k in bles[0]}
    for b in beats:
        for name, st in b["stacks"].items():
            prev = s["stacks"].get(name)
            if prev is None or (st["unused"] is not None and (prev["unused"] is None or st["unused"] < prev["unused"])):
                s["stacks"][name] = dict(st)
    usb = s["latency"]["usb"]
    if usb["n"] and usb["p50"] > NFR01_P50_US:
        s["failed"].append("input to USB p50 <= %d us > NFR-01's %d us" % (usb["p50"], NFR01_P50_US))
    if usb["n"] and usb["p99"] > NFR01_P99_US:
        s["failed"].append("input to USB p99 <= %d us > NFR-01's %d us" % (usb["p99"], NFR01_P99_US))
    ble = s["latency"]["ble"]
    if ble["n"] and ble["p50"] > NFR02_P50_US:
        s["failed"].append("input to BLE p50 <= %d us > NFR-02's %d us" % (ble["p50"], NFR02_P50_US))
    for name, st in sorted(s["stacks"].items()):
        if st["unused"] is not None and st["unused"] < STACK_MARGIN:
            s["failed"].append("stack %s: only %d bytes free at the deepest point sampled" % (name, st["unused"]))
    return s


def summarize_capture_metrics(records):
    """Complete time windows since subscription, excluding buffered history and partial windows.

    Device time alone is insufficient: old buffered timestamps can look newer than a raw u32
    header after a wrap. Use host arrival bounds too, and align the header's clock epoch with
    a fresh capture PING (its echoed send time excludes stale replies from a previous capture).
    This does not reset the firmware's lifetime watermarks.
    """
    records = list(records)
    headers = [r for r in records if r.get("kind") == "header"]
    if len(headers) != 1 or "t_host_ns" not in headers[0] or "t_us" not in headers[0]:
        raise SmclogError("capture-window metrics need one stream header with host timestamps; split separate captures")
    header = headers[0]
    host_start = header["t_host_ns"]
    reference = next((r for r in records if r.get("kind") == "reply" and r.get("cmd") == "ping"
                      and r.get("status") == "ok" and "t_us" in r
                      and "t_host_send_ns" in r and "t_host_ns" in r
                      and r["t_host_send_ns"] >= host_start
                      and r["t_host_ns"] >= r["t_host_send_ns"]), None)
    if reference is None:
        raise SmclogError("capture-window metrics need capture's clock-sync PINGs; keep --sync enabled")
    modulus = 1 << 32
    elapsed_device = (reference["t_us"] - header["t_us"]) & (modulus - 1)
    elapsed_host = (reference["t_host_send_ns"] - host_start) // 1000
    # A slow sync period may span multiple wraps before the first PING; use its host echo.
    wraps = max(0, (elapsed_host - elapsed_device + modulus // 2) // modulus)
    device_start = reference["t_us"] - elapsed_device - wraps * modulus
    beats = read_metrics(records)
    current = [b for b in beats if "t_host_ns" in b
               and b["t_host_ns"] - host_start >= b["window_ms"] * 1000000
               and b["t_us"] - b["window_ms"] * 1000 >= device_start]
    summary = summarize_metrics(current)
    summary["capture_window"] = {"included": len(current), "excluded": len(beats) - len(current),
                                  "start_device_us": device_start, "start_host_ns": host_start,
                                  "initial_lost_total": header.get("lost_total", 0)}
    if not current:
        summary["failed"].append("no complete heartbeat window after capture began")
    return summary


def metrics_text(s):
    lines = ["%d heartbeats over %s s" % (s["heartbeats"], s.get("span_s", 0))]
    if s.get("capture_window"):
        lines.append("capture window: %(included)d complete time windows, %(excluded)d buffered/partial "
                     "excluded; initial cumulative loss %(initial_lost_total)d" % s["capture_window"])
    if not s["heartbeats"]:
        return "\n".join(lines + ["FAIL " + f for f in s["failed"]])
    lines.append("CPU of our code, per mille, the most in a window: scan %(scan)d, control %(control)d, "
                 "USB %(usb)d" % s["cpu_max"])
    for key, name in LAT_PATHS.items():
        x = s["latency"][key]
        if key == "ble" and not x["n"] and "ble" not in s:
            continue  # a build without BLE
        if x["n"]:
            lines.append("latency %s: %d events; p50 <= %d, p99 <= %d, max %d us; by bucket %s" % (
                name, x["n"], x["p50"], x["p99"], x["max"], " ".join(str(c) for c in x["buckets"])))
        else:
            lines.append("latency %s: no events" % name)
    if "scan" in s:
        lines.append("scan: handler max %(handler_max_ns)d ns, late max %(late_max_us)d us, pass max "
                     "%(pass_max_us)d us, %(skipped)d steps skipped" % s["scan"])
    if "clock" in s:
        lines.append("clock: %(resyncs)d resyncs, drift %(drift_min_us)d..%(drift_max_us)d us" % s["clock"])
    if "load" in s:
        lines.append("CPU load, per mille: %(mean)d over the session, %(max)d the most in a window" % s["load"])
    if "adc" in s:
        lines.append("fader ADC: %(samples)d samples, each fader every %(fader_every_max_us)d us at the slowest; "
                     "longest conversion %(steps_max)d steps; %(deferred)d steps the SDK had it, %(lost)d lost "
                     "to it, %(timeouts)d timeouts" % s["adc"])
    if "traffic" in s:
        lines.append("traffic: %(events)d events, MIDI %(midi_out)d out, %(midi_in)d in, %(midi_dropped)d dropped; "
                     "queues max %(input_queue_max)d, %(midi_queue_max)d; refused %(input_refused)d, "
                     "%(midi_refused)d" % s["traffic"])
    if "ble" in s:
        lines.append("BLE: %(out)d messages in %(notify)d notifications, %(merged)d merged, %(dropped)d dropped; "
                     "queue max %(queue_max)d, refused %(refused)d; %(in)d in, %(in_bad)d faults" % s["ble"])
    if "log" in s:
        lines.append("log: %(lost)d lost, ring max %(ring_max)d of %(ring)d bytes" % s["log"])
    if "heap" in s:
        lines.append("heap: %(free_min)d bytes free at the least, %(lowest)d lowest" % s["heap"])
    for name, st in sorted(s["stacks"].items()):
        lines.append("stack %s: %s of %d bytes free at the deepest point sampled" % (
            name, "not measured" if st["unused"] is None else st["unused"], st["size"]))
    lines += ["FAIL " + f for f in s["failed"]] or ["checks: ok (NFR-01 on the device's side, stack margins)"]
    return "\n".join(lines)


# --- the boot journal (LOG-11) ----------------------------------------------------------------------

# BR25 reset cause bits as firmware/platform/br25/diag_br25.c reports them: P3_RST_SRC in bits 0-7,
# P3_PMU_CON1 in bits 8-15 (docs/logging.md, "Boot journal").
RESET_BITS = {0: "power-on", 1: "low voltage", 2: "watchdog", 3: "VCM", 4: "pin reset", 5: "system reset",
              6: "soft reset", 15: "power return"}
CRASH_CAUSES = {0: "emulator exception (IRQ 0)", 1: "exception (IRQ 1)", 2: "stall (the idle task did not run)",
                9: "signal (devhost)"}
CRASH_REGS = ("RETI", "RETE", "RETX", "RETS", "PSR", "ICFG", "USP", "SSP")
CRASH_MSGS = ("EMU_MSG", "DEBUG_MSG", "PRP_MMU_MSG", "PRP_WR_LIMIT_MSG")


def _u32(v):
    return v & 0xFFFFFFFF


def read_boot(records):
    """The boot journals in decoded records (module "boot", docs/logging.md): a list of dicts, each
    with the previous run's records replayed just before it."""
    journals, past, again = [], [], False
    for r in records:
        if r.get("kind") != "log":
            continue
        if r.get("past"):
            past.append(r)
            continue
        if r.get("module") != "boot" or r.get("args") is None:
            continue
        m, a = r["msg"], r["args"]
        if m.startswith("boot journal, again on request"):
            again = True
            continue
        if m.startswith("boot ") and "since the RAM was lost" in m:
            journals.append({"t_us": r["t_us"], "boot": a[0], "reset": {"raw": _u32(a[1]), "text": a[2],
                             "bits": [n for b, n in sorted(RESET_BITS.items()) if a[1] >> b & 1]},
                             "version": a[3], "build_tag": "%08x" % _u32(a[4]), "again": again,
                             "past": [] if again else past, "marks": []})
            past, again = [], False
            continue
        if not journals:
            continue
        j = journals[-1]
        if m.startswith("previous run: nothing kept"):
            j["previous"] = None
        elif m.startswith("previous run: ended before"):
            j["previous"] = {"heartbeat": 0}
        elif m.startswith("previous run: last heartbeat"):
            j["previous"] = {"heartbeat": a[0], "up_ms": a[1]}
        elif m.startswith("previous run: no crash record"):
            j["crash"] = None
        elif m.startswith("crash in boot"):
            j["crash"] = {"boot": a[0], "t_ms": a[1], "cause": a[2],
                          "cause_name": CRASH_CAUSES.get(a[2], "cause %d" % a[2]), "task": a[3],
                          "frame": _u32(a[4]), "build_tag": "%08x" % _u32(a[5]), "regs": {}, "stack": []}
        elif m.startswith("crash: RETI") and j.get("crash"):
            j["crash"]["regs"].update(zip(CRASH_REGS, map(_u32, a)))
        elif m.startswith("crash: EMU_MSG") and j.get("crash"):
            j["crash"]["regs"].update(zip(CRASH_MSGS, map(_u32, a[:4])))
            j["crash"]["trace"] = [_u32(v) for v in a[4:8]]
        elif m.startswith("crash: R0-R7") and j.get("crash"):
            j["crash"]["regs"].update(("R%d" % i, _u32(v)) for i, v in enumerate(a))
        elif m.startswith("crash: R8-R15") and j.get("crash"):
            j["crash"]["regs"].update(("R%d" % (i + 8), _u32(v)) for i, v in enumerate(a))
        elif m.startswith("crash: stack at") and j.get("crash"):
            j["crash"]["stack"] += [(_u32(a[0]) + 4 * i, _u32(v)) for i, v in enumerate(a[1:])]
        elif m.startswith("previous run: no recent log records"):
            j["recent"] = None
        elif m.startswith("previous run: its recent log records are from another build"):
            j["recent"] = "other build"
        elif m.startswith("previous run: its latest"):
            j["recent"] = a[0]
        elif m.startswith("init "):
            j["marks"].append({"what": a[0], "took_us": a[1], "at_us": a[2]})
        elif m.startswith("boot done:"):
            j["done_us"] = a[0]
        elif m.startswith("boot not done yet"):
            j["done_us"] = None
    return journals


def elf_functions(path):
    """[(address, size, name)] of the FUNC symbols in an ELF's .symtab, sorted; [] for other files."""
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"\x7fELF":
        return []
    secs = _elf_sections(d)
    symtab, strtab = secs.get(".symtab"), secs.get(".strtab")
    if not symtab or not strtab or symtab["data"] is None:
        return []
    is64 = d[4] == 2
    size, fmt = (24, "<IBBHQQ") if is64 else (16, "<IIIBBH")
    out = []
    for i in range(0, len(symtab["data"]) - size + 1, size):
        f = struct.unpack_from(fmt, symtab["data"], i)
        name_off, (info, value, sz) = f[0], ((f[1], f[4], f[5]) if is64 else (f[3], f[1], f[2]))
        if info & 0xF == 2 and value:  # STT_FUNC
            names = strtab["data"]
            out.append((value, sz, names[name_off:names.index(b"\0", name_off)].decode()))
    return sorted(out)


def symbolize(addr, funcs):
    """"name+0xoff" for an address inside a function, else None."""
    lo, hi = 0, len(funcs)
    while lo < hi:  # the last function starting at or below addr
        mid = (lo + hi) // 2
        if funcs[mid][0] <= addr:
            lo = mid + 1
        else:
            hi = mid
    if lo and addr < funcs[lo - 1][0] + max(funcs[lo - 1][1], 1):
        start, _, name = funcs[lo - 1]
        return name if addr == start else "%s+0x%x" % (name, addr - start)
    return None


def boot_symbols(journal, funcs):
    """The crash record's code addresses named: {"RETI": "ctl_run+0x1a", ...}."""
    c, out = journal.get("crash"), {}
    if not c or not funcs:
        return out
    for name, v in c["regs"].items():
        if not name.endswith("MSG") and name not in ("PSR", "ICFG", "USP", "SSP"):
            sym = symbolize(v, funcs)
            if sym:
                out[name] = sym
    for i, v in enumerate(c.get("trace", [])):
        sym = symbolize(v, funcs)
        if sym:
            out["trace%d" % i] = sym
    for at, v in c["stack"]:
        sym = symbolize(v, funcs)
        if sym:
            out["stack 0x%08x" % at] = sym
    return out


def boot_text(j, funcs=()):
    lines = ["boot %d since the RAM was lost%s: reset cause 0x%x (%s); firmware %s, build tag %s" % (
        j["boot"], " (journal again on request)" if j["again"] else "", j["reset"]["raw"], j["reset"]["text"],
        j["version"], j["build_tag"])]
    prev = j.get("previous", "?")
    if prev is None:
        lines.append("previous run: nothing kept (power-on, or the RAM was not kept)")
    elif isinstance(prev, dict):
        lines.append("previous run: ended before its first heartbeat" if not prev["heartbeat"] else
                     "previous run: last heartbeat %d, %d ms after it started" % (prev["heartbeat"], prev["up_ms"]))
    c = j.get("crash")
    if c:
        syms = boot_symbols(j, funcs)
        name = lambda k, v: "0x%08x%s" % (v, " " + syms[k] if k in syms else "")  # noqa: E731
        lines.append("CRASH in boot %d at about %d ms: %s, task %s, frame at 0x%08x, build tag %s%s" % (
            c["boot"], c["t_ms"], c["cause_name"], c["task"], c["frame"], c["build_tag"],
            "" if c["build_tag"] == j["build_tag"] else " (another build: name its addresses with that ELF)"))
        for group in (CRASH_REGS[:4], CRASH_REGS[4:], CRASH_MSGS):
            lines.append("  " + ", ".join("%s %s" % (k, name(k, c["regs"][k])) for k in group if k in c["regs"]))
        if c.get("trace"):
            lines.append("  trace: " + ", ".join(name("trace%d" % i, v) for i, v in enumerate(c["trace"])))
        for row in range(0, 16, 4):
            lines.append("  " + " ".join("R%-2d %s" % (i, name("R%d" % i, c["regs"]["R%d" % i]))
                                         for i in range(row, row + 4) if "R%d" % i in c["regs"]))
        for at, v in c["stack"]:
            if "stack 0x%08x" % at in syms:
                lines.append("  stack 0x%08x: %s" % (at, name("stack 0x%08x" % at, v)))
        if c["stack"]:
            lines.append("  stack from 0x%08x: %s" % (c["stack"][0][0], " ".join("%08x" % v for _, v in c["stack"])))
    elif "crash" in j:
        lines.append("previous run: no crash record")
    recent = j.get("recent", "?")
    if recent is None:
        lines.append("previous run: no recent log records")
    elif recent == "other build":
        lines.append("previous run: its recent log records are from another build (not replayed)")
    elif isinstance(recent, int):
        lines.append("previous run: its latest %d log records%s" % (
            recent, ":" if j["past"] else " (not in this capture)"))
        lines += ["  " + as_text(r) for r in j["past"]]
    if j["marks"]:
        lines.append("init: " + ", ".join("%s %d us" % (m["what"], m["took_us"]) for m in j["marks"]))
    if j.get("done_us") is not None:
        lines.append("boot done %d us after the journal started" % j["done_us"])
    return "\n".join(lines)


def cmd_boot(a):
    journals = read_boot(read_records(a.input))
    funcs = elf_functions(a.binary) if a.binary else []
    if a.json:
        for j in journals:
            j["symbols"] = boot_symbols(j, funcs)
        print(json.dumps(journals, sort_keys=True))
    elif not journals:
        print("no boot journal in this session")
    else:
        print("\n\n".join(boot_text(j, funcs) for j in journals))


# --- the device's clock against the host's (LOG-20) -------------------------------------------------

def clock_samples(records):
    """(host send ns, device us, host receive ns) of each PING reply that carries the host's send
    time (capture's clock sync, firstrun's PINGs)."""
    return [(r["t_host_send_ns"], r["t_us"], r["t_host_ns"]) for r in records
            if r.get("kind") == "reply" and r.get("cmd") == "ping" and "t_host_send_ns" in r and "t_host_ns" in r]


def clock_fit(samples, window_s=1.0):
    """The device's µs clock against the host's monotonic clock: a fit per run of the device. A
    reset starts a new run: the device's clock goes back, or it went on by less than half the
    host's time since the last reply (a reset up longer than the run before it; 2026-11-11, the
    watchdog test after a soft one). The device read its clock between the PING's send and its
    reply's arrival; the middle is the estimate, best for the quickest round trips. So of each
    window_s of the run only the sample with the shortest round trip is used (a burst of PINGs
    counts once), and a line is fitted to those by least squares.
    Returns a list of fits:
      host_ns, device_us   a point on the line: the device's time at that host time
      rate                 device µs per host µs; drift_ppm = (rate - 1) * 1e6, None from one sample
      drift_se_ppm         the slope's standard error, None below three samples
      rtt_us               min, median, max round trip of all samples
      residual_us          rms and max of the used samples' distance from the line
      samples, used (one per window), span_s"""
    runs, cur = [], []
    for s in samples:
        if cur and (s[1] < cur[-1][1] or (s[1] - cur[-1][1]) * 1000 < (s[0] - cur[-1][2]) / 2 - 1e6):
            runs.append(cur)
            cur = []
        cur.append(s)
    if cur:
        runs.append(cur)
    return [_clock_fit_run(run, window_s) for run in runs]


def _clock_fit_run(run, window_s):
    rtts = sorted(r - s for s, _, r in run)
    best = {}
    for x in run:
        w = int((x[0] - run[0][0]) / (window_s * 1e9))
        if w not in best or x[2] - x[0] < best[w][2] - best[w][0]:
            best[w] = x
    used = [best[w] for w in sorted(best)]
    h0, d0 = (used[0][0] + used[0][2]) // 2, used[0][1]
    xs = [((s + r) / 2 - h0) / 1e3 for s, _, r in used]  # host µs after h0
    ys = [d - d0 for _, d, _ in used]                    # device µs after d0
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    rate = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx if n > 1 and sxx > 0 else 1.0
    a = my - rate * mx
    res = [y - (a + rate * x) for x, y in zip(xs, ys)]
    se = None
    if n > 2 and sxx > 0:
        se = (sum(e * e for e in res) / (n - 2) / sxx) ** 0.5 * 1e6
    return {"samples": len(run), "used": n, "span_s": (run[-1][0] - run[0][0]) / 1e9,
            "host_ns": h0, "device_us": d0 + a, "rate": rate,
            "drift_ppm": (rate - 1) * 1e6 if n > 1 and sxx > 0 else None, "drift_se_ppm": se,
            "rtt_us": {"min": rtts[0] / 1e3, "median": rtts[len(rtts) // 2] / 1e3, "max": rtts[-1] / 1e3},
            "residual_us": {"rms": (sum(e * e for e in res) / n) ** 0.5, "max": max(abs(e) for e in res)}}


def device_to_host_ns(fit, t_us):
    """The host time (monotonic ns) at which the device's clock read t_us, by a clock_fit() fit."""
    return fit["host_ns"] + (t_us - fit["device_us"]) / fit["rate"] * 1e3


def clock_text(fits):
    if not fits:
        return "no PING with the host's send time (capture --sync, or firstrun)"
    lines = []
    for i, f in enumerate(fits, 1):
        drift = "drift not known (one sample)" if f["drift_ppm"] is None else "device clock %+.1f%s ppm against the host" % (
            f["drift_ppm"], "" if f["drift_se_ppm"] is None else " ± %.1f" % f["drift_se_ppm"])
        lines.append("run %d: %d PINGs over %.1f s, %d used (the quickest of each second): %s; round trip min %.0f, "
                     "median %.0f, max %.0f us; distance from the line rms %.0f, max %.0f us" % (
                         i, f["samples"], f["span_s"], f["used"], drift, f["rtt_us"]["min"], f["rtt_us"]["median"],
                         f["rtt_us"]["max"], f["residual_us"]["rms"], f["residual_us"]["max"]))
    return "\n".join(lines)


def cmd_clock(a):
    fits = clock_fit(clock_samples(read_records(a.input)))
    print(json.dumps(fits, sort_keys=True) if a.json else clock_text(fits))


def read_records(path):
    """Decoded records from a session directory (device.jsonl) or a JSON-lines file."""
    if os.path.isdir(path):
        path = os.path.join(path, "device.jsonl")
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def cmd_metrics(a):
    records = read_records(a.input)
    s = summarize_capture_metrics(records) if getattr(a, "capture_window", False) else summarize_metrics(read_metrics(records))
    print(json.dumps(s, sort_keys=True) if a.json else metrics_text(s))
    if a.check and s["failed"]:
        sys.exit(1)


def cmd_cmd(a):
    dictionary = load_dictionary(a.dict, a.binary) if (a.dict or a.binary) else {"sites": {}}
    payload = command_payload(a, dictionary)
    if a.midi:
        # over SysEx on a MIDI port (USB MIDI or BLE MIDI): tools/hostmidi, imported only here
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "hostmidi"))
        import hostmidi
        port = hostmidi.open_port(a.midi)
        link, close = SysexLink(port), port.close
    else:
        fd = open_serial(a.port)
        link, close = Link(fd, Decoder(dictionary)), lambda: os.close(fd)
    try:
        t0 = time.monotonic_ns()
        r = link.call(CMDS[a.command], payload, timeout=a.timeout)
        rtt_us = (time.monotonic_ns() - t0) // 1000
    finally:
        close()
    if a.json:
        print(json.dumps(r, sort_keys=True))
    elif r["status"] != "ok":
        print("%s: %s" % (a.command, r["status"]))
    elif a.command == "ping":
        print("ping: device time %.6f s, round trip %d us" % (r["t_us"] / 1e6, rtt_us))
    elif a.command == "info":
        print("firmware %s, link %d, log format %d, variant %s, capabilities %s, ring %d bytes, timer %d Hz, build %s" % (
            r.get("fw_version") or "(no version)", r["link_version"], r["log_format"], r["variant"],
            ",".join(r["caps"]), r["ring_size"], r["timer_hz"], r["build_id"]))
        if dictionary.get("build_id") and dictionary["build_id"] != r["build_id"]:
            print("warning: the dictionary is for build %s" % dictionary["build_id"], file=sys.stderr)
    elif a.command == "levels":
        names = {s["module_index"]: s["module"] for s in dictionary["sites"].values() if "module_index" in s}
        print(" ".join("%s=%s" % (names.get(i, str(i)), LEVELS.get(v, "OFF" if v == 0 else str(v)))
                       for i, v in enumerate(r["levels"])))
    elif a.command == "inject":
        print("inject: %s, %d accepted, %d refused" % (r["inject"], r["accepted"], r["refused"]))
    elif a.command == "setup":
        print("setup: %s%s" % (r["setup"], " (setting %d)" % r["bad_pair"] if r["bad_pair"] else ""))
    elif a.command == "reset":
        print("reset: %s" % ("in about 100 ms" if r["reset"] == "ok" else r["reset"] + " (inject on first)"))
    elif a.command == "stock":
        print("stock: %s; sector 0 points at %#x%s" % (r["stock"], r["pointer"],
              ", written in %d tries; M-Vave's firmware starts in about 100 ms" % r["tries"] if r["tries"] else ""))
    else:
        print("%s: ok" % a.command)
    if r["status"] != "ok" or any(r.get(k, "ok") != "ok" for k in ("inject", "setup", "reset", "stock")):
        sys.exit(1)


def cmd_capture(a):
    dictionary = load_dictionary(a.dict, a.binary)
    os.makedirs(a.session, exist_ok=True)
    meta = {
        "format": FORMAT_VERSION,
        "tool": "smclog capture",
        "created": time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "host": platform.platform(),
        "port": a.port,
        "build_id": dictionary.get("build_id"),
        "dictionary": a.dict or os.path.basename(a.binary),
        "scenario": None,
        "t_host_start_ns": time.monotonic_ns(),
        "subscribe": not a.no_subscribe,
        "sync_s": a.sync,
    }
    with open(os.path.join(a.session, "meta.json"), "w") as f:
        json.dump(meta, f, indent=1, sort_keys=True)
    midi = None
    if a.midi:
        midi = subprocess.Popen([a.smcmidi, "listen", "--match", a.midi, "--log",
                                 os.path.join(a.session, "midi.jsonl")],
                                stdout=subprocess.DEVNULL)
    dec = Decoder(dictionary)
    fd = open_serial(a.port)
    raw = open(os.path.join(a.session, "device.bin"), "wb")
    recs = open(os.path.join(a.session, "device.jsonl"), "w")
    markers = open(os.path.join(a.session, "markers.jsonl"), "w")
    inputs = [fd] + ([sys.stdin.fileno()] if not a.no_markers else [])
    deadline = time.monotonic() + a.seconds if a.seconds else None
    n_recs = n_markers = 0
    link = Link(fd, dec)
    pending = {}  # seq -> (command name, when it was sent), replies not seen yet
    next_sync = time.monotonic() + a.sync if a.sync else None

    def send(cmd, payload=b""):
        pending[link.send(CMDS[cmd], payload)] = (cmd, time.monotonic())

    print("smclog capture: %s -> %s; Enter adds a marker, Ctrl-C stops" % (a.port, a.session), file=sys.stderr)
    if not a.no_subscribe:
        send("subscribe")
    try:
        while True:
            if next_sync is not None and time.monotonic() >= next_sync:
                send("ping", struct.pack("<Q", time.monotonic_ns()))
                next_sync += a.sync
            for seq, (cmd, sent) in list(pending.items()):
                if time.monotonic() - sent > 2.0:
                    del pending[seq]
                    print("smclog capture: no reply to %s (seq %d): not our firmware, or a link without commands"
                          % (cmd, seq), file=sys.stderr)
            timeout = 0.2 if deadline is None else max(0.0, min(0.2, deadline - time.monotonic()))
            ready, _, _ = select.select(inputs, [], [], timeout)
            now = time.monotonic_ns()
            if fd in ready:
                try:
                    data = os.read(fd, 4096)
                except BlockingIOError:
                    data = None
                if data == b"":
                    print("smclog capture: port closed", file=sys.stderr)
                    break
                if data:
                    raw.write(data)
                    for r in dec.feed(data, now):
                        if r["kind"] == "reply":
                            pending.pop(r["seq"], None)
                            if r["status"] != "ok":
                                print("smclog capture: %s refused: %s" % (r["cmd"], r["status"]), file=sys.stderr)
                        recs.write(json.dumps(r, sort_keys=True) + "\n")
                        n_recs += 1
                        if not a.quiet and not (r["kind"] == "reply" and r["cmd"] == "ping"):
                            print(as_text(r), file=sys.stderr)
            if sys.stdin.fileno() in ready:
                line = sys.stdin.readline()
                if not line:
                    inputs.remove(sys.stdin.fileno())
                else:
                    markers.write(json.dumps({"t_ns": now, "text": line.strip()}) + "\n")
                    markers.flush()
                    n_markers += 1
            if deadline is not None and time.monotonic() >= deadline:
                break
    except KeyboardInterrupt:
        pass
    finally:
        if not a.no_subscribe:
            try:
                link.send(CMDS["unsubscribe"])  # the device also stops when the port closes (DTR)
            except OSError:
                pass
        os.close(fd)
        for f in (raw, recs, markers):
            f.close()
        if midi:
            midi.terminate()
            midi.wait()
    print("smclog capture: %d records, %d markers" % (n_recs, n_markers), file=sys.stderr)


def main(argv=None):
    p = argparse.ArgumentParser(prog="smclog", description="Device log tools (docs/logging.md).")
    sub = p.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("dict", help="dictionary of a firmware binary")
    d.add_argument("binary")
    d.add_argument("-o", "--output")
    c = sub.add_parser("cmd", help="send one command over the command link (docs/link.md)")
    w = c.add_mutually_exclusive_group(required=True)
    w.add_argument("--port", help="serial device (USB CDC): /dev/cu.usbmodem*, /dev/ttyACM0, COM3, or auto (found by its USB IDs)")
    w.add_argument("--midi", help="a MIDI port (SysEx; tools/hostmidi): its name, fd:N or raw:PATH; "
                   "the log commands answer 'unavailable' there")
    g = c.add_mutually_exclusive_group()
    g.add_argument("--dict", help="dictionary JSON: module names, build ID check")
    g.add_argument("--binary", help="firmware ELF or host binary to take the dictionary from")
    c.add_argument("--json", action="store_true", help="print the reply as JSON")
    c.add_argument("--timeout", type=float, default=1.0, help="seconds to wait for each try (3 tries)")
    c.add_argument("command", choices=sorted(CMDS))
    c.add_argument("args", nargs="*")
    m = sub.add_parser("metrics", help="summary of a session's metrics heartbeats (LOG-06)")
    m.add_argument("input", help="session directory or JSON lines of decoded records")
    m.add_argument("--json", action="store_true", help="the summary as JSON")
    m.add_argument("--check", action="store_true", help="exit 1 if a check fails (NFR-01, stack margins)")
    m.add_argument("--capture-window", action="store_true",
                   help="only complete windows since subscription, excluding buffered history; needs clock-sync PINGs")
    k = sub.add_parser("clock", help="the device's clock against the host's, from a session's PINGs")
    k.add_argument("input", help="session directory or JSON lines of decoded records")
    k.add_argument("--json", action="store_true", help="the fits as JSON")
    b = sub.add_parser("boot", help="the boot journal of a session (LOG-11)")
    b.add_argument("input", help="session directory or JSON lines of decoded records")
    b.add_argument("--binary", help="firmware ELF: name the crash record's code addresses")
    b.add_argument("--json", action="store_true", help="the journals as JSON")
    for name in ("decode", "capture"):
        s = sub.add_parser(name)
        g = s.add_mutually_exclusive_group(required=True)
        g.add_argument("--dict", help="dictionary JSON from 'smclog dict'")
        g.add_argument("--binary", help="firmware ELF or host binary to take the dictionary from")
        if name == "decode":
            s.add_argument("input")
            s.add_argument("-o", "--output")
            s.add_argument("--text", action="store_true", help="one line of text per record")
        else:
            s.add_argument("--port", required=True, help="serial device (USB CDC): /dev/cu.usbmodem*, /dev/ttyACM0, COM3, or auto (found by its USB IDs)")
            s.add_argument("--session", required=True, help="session directory to create")
            s.add_argument("--seconds", type=float, help="stop after this long")
            s.add_argument("--midi", metavar="MATCH", help="also record MIDI ports matching MATCH (smcmidi)")
            s.add_argument("--smcmidi", default="build/smcmidi", help="smcmidi binary (default build/smcmidi)")
            s.add_argument("--no-markers", action="store_true", help="do not read markers from stdin")
            s.add_argument("--quiet", action="store_true", help="do not print records")
            s.add_argument("--no-subscribe", action="store_true",
                           help="only listen: send no commands (a stream that runs by itself)")
            s.add_argument("--sync", type=float, default=1.0, metavar="SECONDS",
                           help="PING period for clock synchronization (default 1; 0: none)")
    a = p.parse_args(argv)
    if a.cmd == "capture" and a.no_subscribe:
        a.sync = 0
    try:
        {"dict": cmd_dict, "decode": cmd_decode, "capture": cmd_capture, "cmd": cmd_cmd,
         "metrics": cmd_metrics, "boot": cmd_boot, "clock": cmd_clock}[a.cmd](a)
    except SmclogError as e:
        sys.exit("smclog: %s" % e)


if __name__ == "__main__":
    main()
