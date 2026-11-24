"""Flash images of the SMC-Mixer's JieLi AC6966C (BR25): docs/image.md.

  flashimg.py info FILE          what a flash dump, a .fwsc or a UFW holds
  flashimg.py check DUMP FWSC    rebuild the stock application areas byte for byte (local files)
  flashimg.py build --app APP [--cfg CFG] [--tail TAIL] [--entry 0x1E00120] [--vm START:SIZE]
                    -o AREA [--physical PHYS] [--chipkey 0x980F]

`build` writes the application area as the CPU sees it at 0x1E00000 (AREA, plaintext, starts at
flash 0x1000) and, with --physical, as it is stored in flash (SFC-encrypted). Nothing here talks to
a device. Prints names, offsets, sizes and CRCs, never the contents of vendor files (SPEC C-7).
Standard library only.
"""
import hashlib
import struct
import sys
from dataclasses import dataclass, field

CHIPKEY = 0x980F       # the mixer's SFC chip key (docs/image.md, "SFC cipher")
AREA_AT = 0x1000       # flash offset of the application area; the CPU sees it at 0x1E00000
CPU_BASE = 0x1E00000
ENTRY = 0x1E00120      # stock entry point: the first file right after a 9-slot directory
SLOTS = 9
ENTRY_SIZE = 32
SECTOR = 0x1000
BLOCK = 32             # SFC block
S0_RESTART = 256       # sector 0 keystream restarts every 256 bytes
S0_KEY = 0xFFFF

# attr / res values of the stock directory
ATTR_HEAD, ATTR_FILE, ATTR_VM, ATTR_REGION = 0x83, 0x82, 0x12, 0x92
RES_FILE, RES_VM, RES_PRCT, RES_REGION = 0xFF, 0x80, 0x82, 0x81


# --- checksums and ciphers ------------------------------------------------------------------

def _crc_table():
    t = []
    for i in range(256):
        c = i << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
        t.append(c)
    return t


_CRC = _crc_table()


def crc16(data, crc=0):
    """CRC-16/XMODEM: polynomial 0x1021, initial value 0, not reflected."""
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ _CRC[(crc >> 8) ^ b]
    return crc


def keystream(key, n, skip=0):
    """JieLi ENC keystream: the low byte of a 16-bit LFSR (polynomial 0x1021) per byte."""
    key &= 0xFFFF
    out = bytearray(n)
    for i in range(skip + n):
        if i >= skip:
            out[i - skip] = key & 0xFF
        key = ((key << 1) ^ 0x1021) & 0xFFFF if key & 0x8000 else (key << 1) & 0xFFFF
    return out


def enc(data, key):
    """ENC cipher (XOR with the keystream from its start); symmetric."""
    ks = keystream(key, len(data))
    return bytes(a ^ b for a, b in zip(data, ks))


def sfc_key(addr, chipkey=CHIPKEY):
    """Key of the 32-byte SFC block holding flash address addr of the application area. The
    hardware keys on the CPU address (0x1E00000 + addr - 0x1000); only its bits 2..17 count."""
    return chipkey ^ (((CPU_BASE + addr - AREA_AT) >> 2) & 0xFFFF)


def sfc(data, flash_off, chipkey=CHIPKEY):
    """SFC cipher of data that sits at flash offset flash_off (any alignment); symmetric."""
    out = bytearray(data)
    pos = 0
    while pos < len(out):
        a = flash_off + pos
        start = a % BLOCK
        n = min(BLOCK - start, len(out) - pos)
        ks = keystream(sfc_key(a - start, chipkey), n, start)
        for i in range(n):
            out[pos + i] ^= ks[i]
        pos += n
    return bytes(out)


# --- JLFS entries -----------------------------------------------------------------------------

@dataclass
class Entry:
    """One 32-byte JLFS entry: hdr_crc, data_crc, offset, size, attr, res, index, name[16]. The
    name field is the name, a NUL, 0xFF padding, and optionally a tag in its last bytes (the
    stock regions carry b"CODE", b"AUTO", ...)."""
    name: str
    offset: int
    size: int
    attr: int
    res: int = RES_FILE
    index: int = 0
    data_crc: int = 0xFFFF
    tag: bytes = b""

    def pack(self):
        field = self.name.encode() + b"\0"
        if len(field) + len(self.tag) > 16:
            raise ValueError(f"name and tag too long: {self.name}")
        field += b"\xff" * (16 - len(field) - len(self.tag)) + self.tag
        body = struct.pack("<HIIBBH", self.data_crc, self.offset, self.size, self.attr, self.res,
                           self.index) + field
        return struct.pack("<H", crc16(body)) + body

    @staticmethod
    def parse(b):
        """The entry in 32 bytes, or None when its header CRC does not match."""
        if len(b) < ENTRY_SIZE or crc16(b[2:ENTRY_SIZE]) != struct.unpack_from("<H", b)[0]:
            return None
        _, dc, off, size, attr, res, idx = struct.unpack_from("<HHIIBBH", b)
        name, _, rest = bytes(b[16:32]).partition(b"\0")
        return Entry(name.decode("latin-1"), off, size, attr, res, idx, dc, rest.lstrip(b"\xff"))


def read_entries(buf, at=0, decode=None, limit=64):
    """Entries from buf[at] on, up to and including the one with index != 0. decode, if given,
    is applied to each 32-byte entry first (sector 0 entries are ENC-ciphered one by one)."""
    out = []
    for i in range(limit):
        raw = buf[at + i * ENTRY_SIZE:at + (i + 1) * ENTRY_SIZE]
        e = Entry.parse(decode(raw) if decode else raw)
        if e is None:
            raise ValueError(f"entry {i} at {at + i * ENTRY_SIZE:#x}: bad header CRC")
        out.append(e)
        if e.index:
            return out
    raise ValueError(f"no last entry within {limit}")


# --- sector 0 ---------------------------------------------------------------------------------

def sector0_view(sector):
    """Physical sector 0 <-> the view the stock firmware's flash read (SINCO 0x23) returns: XOR
    with the ENC keystream of 0xFFFF restarting every 256 bytes. Symmetric."""
    ks = keystream(S0_KEY, S0_RESTART)
    return bytes(b ^ ks[i % S0_RESTART] for i, b in enumerate(sector))


def sector0_entries(physical):
    """Flash header and file list of a physical sector 0 (each entry ENC 0xFFFF on its own).
    Entry 0 is the flash header (a different layout that shares the CRC); the list follows."""
    head = Entry.parse(enc(physical[:ENTRY_SIZE], S0_KEY))
    if head is None:
        raise ValueError("sector 0: bad flash header CRC")
    return [head] + read_entries(physical, ENTRY_SIZE, lambda r: enc(r, S0_KEY), limit=16)


def set_boot_pointer(physical, dir_offset, name="app_dir_head"):
    """A copy of a physical sector 0 whose `app_dir_head` points at dir_offset: only that 32-byte
    entry changes (its offset and header CRC)."""
    out = bytearray(physical)
    for i, e in enumerate(sector0_entries(physical)):
        if i and e.name == name:
            e.offset = dir_offset
            at = i * ENTRY_SIZE
            out[at:at + ENTRY_SIZE] = enc(e.pack(), S0_KEY)
            return bytes(out)
    raise ValueError(f"no {name} entry in sector 0")


# --- application area -------------------------------------------------------------------------

@dataclass
class Region:
    """A flash region the directory declares (absolute flash offset)."""
    name: str
    offset: int
    size: int
    attr: int = ATTR_REGION
    res: int = RES_REGION
    tag: bytes = b""


def stock_regions(vm_start=0x24000, vm_size=0x57000):
    """The regions of the stock directory; PRCT (the protected range) ends where the VM starts."""
    return [Region("VM", vm_start, vm_size, ATTR_VM, RES_VM, b"\x00\x10\x00\x00"),
            Region("PRCT", 0, vm_start, ATTR_REGION, RES_PRCT, b"CODE"),
            Region("BTIF", 0x7B000, 0x1000, tag=b"AUTO"), Region("EXIF", 0x7C000, 0x1000, tag=b"AUTO"),
            Region("USRFLASH", 0x7D000, 0x2000)]


@dataclass
class Area:
    entry: int
    files: list            # [(name, bytes)] in order
    regions: list          # [Region]
    tail: bytes = b""      # what follows the last file (stock: the resource entry and the MD5)
    entries: list = field(default_factory=list)

    @property
    def size(self):
        """app_area_head.size: the end of the last file, relative to the directory."""
        return SLOTS * ENTRY_SIZE + sum(len(d) for _, d in self.files)

    def file(self, name):
        return next(d for n, d in self.files if n == name)


def build_area(area, at=AREA_AT):
    """The application area as the CPU sees it (from 0x1E00000), from its parts. at is the flash
    offset it goes to (the stock 0x1000, or another slot: installplan.py); its VM must start
    after its end."""
    n = 1 + len(area.files) + len(area.regions)
    if n > SLOTS:
        raise ValueError(f"{n} entries do not fit the {SLOTS}-slot directory")
    entries, body, off = [], b"", SLOTS * ENTRY_SIZE
    for name, data in area.files:
        entries.append(Entry(name, off, len(data), ATTR_FILE, RES_FILE, 0, crc16(data)))
        body += data
        off += len(data)
    for i, r in enumerate(area.regions):
        entries.append(Entry(r.name, r.offset, r.size, r.attr, r.res, int(i == len(area.regions) - 1),
                             tag=r.tag))
    rest = b"".join(e.pack() for e in entries).ljust((SLOTS - 1) * ENTRY_SIZE, b"\xff") + body
    head = Entry("app_area_head", area.entry, area.size, ATTR_HEAD, RES_FILE, 0, crc16(rest))
    plain = head.pack() + rest + area.tail
    vm = next((r for r in area.regions if r.name == "VM"), None)
    if vm and at + len(plain) > vm.offset:
        raise ValueError(f"area ends at {at + len(plain):#x}, past the VM start {vm.offset:#x}")
    return plain


def parse_area(plain):
    """Split a plaintext application area into its parts; raises on any CRC mismatch."""
    ents = read_entries(plain, 0, limit=SLOTS)
    head = ents[0]
    if head.name != "app_area_head":
        raise ValueError(f"first entry is {head.name!r}, not app_area_head")
    if head.size > len(plain) or crc16(plain[ENTRY_SIZE:head.size]) != head.data_crc:
        raise ValueError("app_area_head: area CRC does not match")
    files, regions = [], []
    for e in ents[1:]:
        if e.attr == ATTR_FILE:
            data = bytes(plain[e.offset:e.offset + e.size])
            if crc16(data) != e.data_crc:
                raise ValueError(f"{e.name}: data CRC does not match")
            files.append((e.name, data))
        else:
            regions.append(Region(e.name, e.offset, e.size, e.attr, e.res, e.tag))
    return Area(head.offset, files, regions, b"", ents)


def stock_tail(config_dat, config_entry, app):
    """The stock resource tail: the `config.dat` entry, its data, then the MD5 of app.bin as 32
    hex digits. The entry is the stock one as read (its data CRC covers only the first 0x160
    bytes, docs/image.md); only its header CRC is recomputed."""
    return config_entry.pack() + config_dat + hashlib.md5(app).hexdigest().encode()


def parse_stock_tail(plain, at):
    """(entry, config.dat, md5 hex) of the stock resource tail at plain[at]."""
    e = Entry.parse(plain[at:at + ENTRY_SIZE])
    if e is None:
        raise ValueError(f"no resource entry at {at:#x}")
    data = bytes(plain[at + e.offset:at + e.offset + e.size])
    md5 = bytes(plain[at + e.offset + e.size:at + e.offset + e.size + 32])
    return e, data, md5


def physical(plain, flash_off=AREA_AT, chipkey=CHIPKEY, upto=None):
    """The area as stored in flash: SFC-encrypted where the image has data, erased (0xFF) from
    there up to flash offset upto."""
    out = sfc(plain, flash_off, chipkey)
    if upto is not None:
        out += b"\xff" * (upto - flash_off - len(plain))
    return out


# --- containers -------------------------------------------------------------------------------

FWSC_BLOCK, FWSC_DATA = 0x30, 0x2F


def fwsc_unwrap(raw):
    """(name, version, UFW) of a vendor .fwsc file: the first N 0x30-byte blocks each carry one
    metadata byte after 0x2F UFW bytes. Metadata byte i (1-based) is char + i: the name, '_', a
    3-digit version, then a pad byte repeated to the end. N is 20 (the mixer) or 36 (other
    M-Vave files)."""
    for n in (36, 20):
        if len(raw) < n * FWSC_BLOCK:
            continue
        meta = [raw[i * FWSC_BLOCK + FWSC_DATA] for i in range(n)]
        text = "".join(chr((m - i - 1) & 0xFF) for i, m in enumerate(meta))
        name, sep, rest = text.partition("_")
        k = len(name) + 1 + 3  # the version has 3 digits; a pad value can equal its last one
        if not sep or not name.isprintable() or not rest[:3].isdigit() or k >= n:
            continue
        if len(set(meta[k:])) != 1:
            continue
        ufw = b"".join(raw[i * FWSC_BLOCK:i * FWSC_BLOCK + FWSC_DATA] for i in range(n))
        return name, rest[:3], ufw + raw[n * FWSC_BLOCK:]
    raise ValueError("not a .fwsc file")


def fwsc_wrap(name, version, ufw, n=20, pad=0x40):
    """The inverse of fwsc_unwrap (for tests and for packing our own update files)."""
    text = f"{name}_{version}"
    if len(text) > n or len(ufw) < n * FWSC_DATA:
        raise ValueError("text or UFW too short for the metadata blocks")
    out = bytearray()
    for i in range(n):
        m = (ord(text[i]) + i + 1) & 0xFF if i < len(text) else pad
        out += ufw[i * FWSC_DATA:(i + 1) * FWSC_DATA] + bytes([m])
    return bytes(out + ufw[n * FWSC_DATA:])


UFW_HEADER, UFW_RECORD = 0x40, 0xA0


def ufw_header(ufw):
    """(chip, total length) of a UFW; its 0x40-byte header is ENC 0xFFFF with a CRC16."""
    h = enc(ufw[:UFW_HEADER], 0xFFFF)
    if crc16(h[2:UFW_HEADER]) != struct.unpack_from("<H", h)[0]:
        raise ValueError("UFW header CRC does not match")
    return h[16:32].split(b"\0")[0].decode("latin-1"), struct.unpack_from("<I", h, 4)[0]


def ufw_files(ufw):
    """{name: (offset, size, data CRC ok, flash address or None)} of a UFW. Records follow the
    header, 0xA0 bytes each, ENC 0xFFFF each on its own; a record's name is at +0x40."""
    files = {}
    for i in range(16):
        at = UFW_HEADER + i * UFW_RECORD
        r = enc(ufw[at:at + UFW_RECORD], 0xFFFF)
        name = r[0x40:0x50].split(b"\0")[0]
        if not name or not all(0x20 < c < 0x7F for c in name):
            break
        dcrc, off, size = struct.unpack_from("<III", r, 4)
        if off + size > len(ufw):
            break
        addr = struct.unpack_from("<I", r, 0x1C)[0] or None
        files[name.decode()] = (off, size, crc16(ufw[off:off + size]) == dcrc, addr)
    return files


# --- the stock rebuild ------------------------------------------------------------------------

def rebuild_check(plain, label):
    """Rebuild a stock plaintext area from its parsed parts: (report lines, rebuilt area, equal)."""
    area = parse_area(plain)
    e, cfg, md5 = parse_stock_tail(plain, area.size)
    app = area.file("app.bin")
    area.tail = stock_tail(cfg, e, app)
    rebuilt = build_area(area)
    ok = rebuilt == bytes(plain[:len(rebuilt)])
    md5_ok = md5 == hashlib.md5(app).hexdigest().encode()
    return [f"{label}: directory {len(area.entries)} entries, files "
            + ", ".join(f"{n} {len(d):#x}" for n, d in area.files)
            + f"; resource {e.name} {e.size:#x}; MD5 of app.bin " + ("matches" if md5_ok else "DIFFERS"),
            f"{label}: area {AREA_AT:#x}..{AREA_AT + len(rebuilt):#x} rebuilt "
            + ("byte for byte" if ok else "WITH DIFFERENCES")], rebuilt, ok and md5_ok


def check(dump, fwsc):
    """The local checks against the V012 dump (as the stock firmware reads it) and the V013 .fwsc."""
    lines, ok = [], True
    name, version, ufw = fwsc_unwrap(fwsc)
    files = ufw_files(ufw)
    off, size, crc_ok, _ = files["flash.bin"]
    fb = ufw[off:off + size]
    lines.append(f"{name} {version}: flash.bin {size:#x} bytes (flash 0..{size:#x}), record CRC "
                 + ("ok" if crc_ok else "BAD"))
    vm = parse_area(sfc(fb[AREA_AT:], AREA_AT)).regions[0]
    plain13 = sfc(fb[AREA_AT:vm.offset], AREA_AT)
    l13, area13, ok13 = rebuild_check(plain13, f"V{version}")
    phys_ok = physical(area13, AREA_AT, CHIPKEY, vm.offset) == fb[AREA_AT:vm.offset]
    lines += l13 + [f"V{version}: SFC-encrypted with chip key {CHIPKEY:#06x}, erased to the VM at "
                    f"{vm.offset:#x}: " + ("equal to flash.bin" if phys_ok else "DIFFERENT")]
    s0 = sector0_entries(fb[:SECTOR])
    boot = next(e for e in s0[1:] if e.name == "app_dir_head")
    lines.append(f"V{version}: sector 0 lists " + ", ".join(e.name for e in s0[1:])
                 + f"; app_dir_head -> {boot.offset:#x}")
    ok &= ok13 and phys_ok

    vm12 = parse_area(dump[AREA_AT:]).regions[0]
    l12, area12, ok12 = rebuild_check(dump[AREA_AT:vm12.offset], "V012 dump")
    end = AREA_AT + len(area12)
    tail_ok = sfc(b"\xff" * (vm12.offset - end), end) == dump[end:vm12.offset]
    lines += l12 + ["V012 dump: " + ("erased flash (reads as keystream) from there to the VM"
                                     if tail_ok else "the rest up to the VM is NOT erased flash")]
    same_s0 = sector0_view(dump[:SECTOR]) == fb[:SECTOR]
    lines.append("V012 dump: sector 0 through the 256-byte keystream "
                 + ("equals V013's physical sector 0" if same_s0 else "differs from V013's"))
    ok &= ok12 and tail_ok
    return ok, lines


# --- command line -----------------------------------------------------------------------------

def describe_area(area, at):
    lines = [f"application area at {at:#x}: entry {area.entry:#x}, size {area.size:#x}, CRCs ok"]
    for e in area.entries[1:]:
        lines.append(f"  {e.name:14} {e.offset:#08x} size {e.size:#08x} attr {e.attr:#04x} "
                     f"res {e.res:#04x}{' last' if e.index else ''}")
    return lines


def info(data):
    """What a file holds: a plaintext area as `build` writes it, a .fwsc, a UFW or a whole flash
    image (physical, or as the stock firmware reads it)."""
    try:
        return ["plaintext application area (flash 0x1000 up)"] + describe_area(parse_area(data), AREA_AT)
    except ValueError:
        pass
    lines = []
    try:
        name, version, data = fwsc_unwrap(data)
        lines.append(f".fwsc: {name} {version}, UFW {len(data):#x} bytes")
    except ValueError:
        pass
    try:
        chip, total = ufw_header(data)
        lines.append(f"UFW: chip {chip}, length {total:#x}")
        for n, (off, size, ok, addr) in ufw_files(data).items():
            where = f", flash {addr:#x}" if addr is not None else ""
            lines.append(f"  {n:12} at {off:#08x} size {size:#08x}{where}, CRC {'ok' if ok else 'BAD'}")
        off, size, _, _ = ufw_files(data)["flash.bin"]
        data, view = data[off:off + size], "physical"
    except (ValueError, KeyError):
        view = "as read by the stock firmware"
    s0 = data[:SECTOR] if view == "physical" else sector0_view(data[:SECTOR])
    try:
        lines.append(f"sector 0 ({view}): " + ", ".join(
            f"{e.name} {e.offset:#x}+{e.size:#x}" for e in sector0_entries(s0)[1:]))
    except ValueError as ex:
        lines.append(f"sector 0: {ex}")
    plain = sfc(data[AREA_AT:], AREA_AT) if view == "physical" else data[AREA_AT:]
    try:
        lines += describe_area(parse_area(plain), AREA_AT)
    except ValueError as ex:
        lines.append(f"application area: {ex}")
    return lines


def main(argv):
    def opt(name, default=None, conv=str):
        if name in argv:
            i = argv.index(name)
            v = argv[i + 1]
            del argv[i:i + 2]
            return conv(v)
        return default

    if len(argv) >= 2 and argv[0] == "info":
        with open(argv[1], "rb") as f:
            print("\n".join(info(f.read())))
        return 0
    if len(argv) == 3 and argv[0] == "check":
        with open(argv[1], "rb") as f:
            dump = f.read()
        with open(argv[2], "rb") as f:
            fwsc = f.read()
        ok, lines = check(dump, fwsc)
        print("\n".join(lines))
        print("check: ok" if ok else "check: FAILED")
        return 0 if ok else 1
    if argv and argv[0] == "build":
        app, cfg, tail = opt("--app"), opt("--cfg"), opt("--tail")
        out, phys = opt("-o"), opt("--physical")
        entry = opt("--entry", ENTRY, lambda s: int(s, 0))
        chipkey = opt("--chipkey", CHIPKEY, lambda s: int(s, 0))
        vm = opt("--vm", "0x24000:0x57000")
        if not app or not out or len(argv) != 1:
            print(__doc__, file=sys.stderr)
            return 2
        start, size = (int(x, 0) for x in vm.split(":"))
        files = []
        for name, path in (("app.bin", app), ("cfg_tool.bin", cfg)):
            if path:
                with open(path, "rb") as f:
                    files.append((name, f.read()))
        area = Area(entry, files, stock_regions(start, size))
        if tail:
            with open(tail, "rb") as f:
                area.tail = f.read()
        plain = build_area(area)
        with open(out, "wb") as f:
            f.write(plain)
        print(f"build: {out}: area {AREA_AT:#x}..{AREA_AT + len(plain):#x}, entry {entry:#x}, "
              f"VM at {start:#x}")
        if phys:
            with open(phys, "wb") as f:
                f.write(physical(plain, AREA_AT, chipkey, start))
            print(f"build: {phys}: SFC-encrypted with chip key {chipkey:#06x}, erased to {start:#x}")
        return 0
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
