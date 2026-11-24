"""Installation dry run for the SMC-Mixer: docs/image.md, "Installing" (SPEC §3.4 approach).

  installplan.py DUMP --app APP (--cfg CFG | --no-cfg) [--tail TAIL] [--at 0x41000]
                 [--vm START:SIZE] [--view auto|stock|raw] [--json] [--out DIR]

From an image of the mixer's 512 KB flash (DUMP: what the stock firmware's read returns, or a raw
read through the ROM loader) and our application (APP: the app.bin tools/env/build_app.sh cuts),
it works out the installation without writing anything and without talking to a device:

- the flash now: sector 0 and its boot pointer, the stock application area followed the way the
  uboot follows it, the sectors in use;
- our application area at --at (0x41000: on 512 KB the one place where the SFC keys are those of
  the stock position whichever way the chip counts them) with a directory of its own: our VM right
  after the area, up to the stock update loader at 0x6E000; PRCT from 0 up to our VM; BTIF, EXIF
  and USRFLASH as the stock directory declares them. The configuration's two flash slots (our
  firmware's own, firmware/store/cfgstore.h) lie below the area, at 0x25000..0x34FFF, in the free
  part of the stock VM's range (docs/ble.md, "Flash"; SPEC Q-23);
- the writes, in order, with sizes and CRC-16s (what `jlrom crc` computes on the chip): our area
  into erased flash, then the boot pointer in sector 0. They are applied to the image as flash
  takes them (an erase sets a sector to 0xFF, programming only clears bits);
- the flash after them, read the way the ROM and the uboot read it (sector 0, the boot pointer, the
  directory through the SFC under both key models, the entry point) and compared with the flash
  before: only sector 0's boot pointer entry and our sectors may change;
- what our firmware writes on its first run (its VM; BTIF only into an empty slot) and later (the
  configuration's slots);
- the rollback, applied to the flash after: it must give the flash before, byte for byte.

Prints names, offsets, sizes and CRC-16s, never flash contents (SPEC C-7). With --out it also
writes the images, which hold vendor code and Bluetooth data: --out must lie outside the
repository or under one of its ignored directories (materials/, logs/, build/). Exit status 0 when
every check holds, 1 when one fails, 2 on bad arguments. Standard library only.

--cfg is required: our SDK build reads its settings from cfg_tool.bin, and without it every start
ends in the SDK's first syscfg_read() and the chip resets (docs/bringup.md, "Step 4: boot marks",
runs 3 and 4; again on 2026-11-11/12 with controller 19). The SDK's default file is
deps/fw-AC63_BT_SDK/cpu/br25/tools/cfg_tool.bin. --no-cfg leaves it out on purpose.
"""
import argparse
import json
import os
import struct
import sys
from dataclasses import dataclass

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import flashimg as F  # noqa: E402

ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
PRIVATE = ("materials", "logs", "build")  # the repository's ignored directories (.gitignore)

FLASH_SIZE = 0x80000
SLOT = 0x41000         # our area: 0x1000 + 0x40000, keys as at 0x1000 (the key uses address bits 2..17)
LOADER_AT = 0x6E000    # LOADER.BIN and ble_app_ota.bin (hardware.md): kept while the stock firmware matters
# The configuration's flash slots (firmware/store/cfgstore.h: CFGSTORE_FLASH_AT, CFGSTORE_SLOTS *
# CFGSTORE_SLOT_SIZE) and the panel's state right after them (store/statestore.h: two sectors);
# test_installplan.py checks they agree: written by our firmware, not here.
CFG_AT = 0x25000
CFG_SIZE = 0x12000
VM_END = LOADER_AT     # our VM ends where the stock update loader starts
VM_LEAST = 0x2000      # CONFIG_VM_LEAST_SIZE of our board (firmware/app/board/..._global_build_cfg.h)
CHUNK = 0x100          # smcmidi sinco-write sends 0x22 one 256-byte page at a time
KEEP = ("BTIF", "EXIF", "USRFLASH")
# The SDK's BTIF items as our build registers them (the hid example's apps/hid/modules/user_cfg.c,
# btif_table, compiled into our image): (id, length, name). Each item sits at the sum of the
# previous items' 4 + length; its 4-byte header is length << 20 | id << 8 | CRC-16 low byte.
BTIF_ITEMS = ((102, 6, "CFG_BT_MAC_ADDR"), (110, 6, "CFG_BT_FRE_OFFSET"))
# Where the SFC counts its keys from (docs/image.md, "SFC cipher"): the CPU address, i.e. the
# area's own flash offset, or the flash offset relative to the stock area. The stock area cannot
# tell them apart; at --at both must give the same bytes.
KEY_MODELS = (("keyed on the CPU address", lambda at: at), ("keyed on the flash offset", lambda at: F.AREA_AT))


def sfc_from(data, flash_off, base):
    """SFC (de)cipher of data at flash offset flash_off, with flash offset base seen by the CPU
    at 0x1E00000 (the keys count from there). Symmetric."""
    return F.sfc(data, flash_off - base + F.AREA_AT)


def align(x, a=F.SECTOR):
    return (x + a - 1) // a * a


def sectors(lo, hi):
    return list(range(lo, hi, F.SECTOR))


def rng(lo, hi):
    return f"{lo:#x}..{hi - 1:#x}"


# --- reading flash ----------------------------------------------------------------------------

def _sector0_ok(s):
    try:
        F.sector0_entries(s)
        return True
    except ValueError:
        return False


def flash_view(data, view="auto"):
    """(flash as stored, view). A raw read is that already. A stock read (SINCO 0x23) shows sector
    0 through the 256-byte keystream and the stock area decrypted up to the stock VM start; above
    that flash reads as stored."""
    if len(data) != FLASH_SIZE:
        raise ValueError(f"{len(data):#x} bytes: not a {FLASH_SIZE // 1024} KB flash image")
    if view == "auto":
        view = ("raw" if _sector0_ok(data[:F.SECTOR]) else
                "stock" if _sector0_ok(F.sector0_view(data[:F.SECTOR])) else None)
        if view is None:
            raise ValueError("sector 0 parses neither as stored nor as the stock firmware reads it")
    if view == "raw":
        return bytes(data), view
    vm = region(F.parse_area(data[F.AREA_AT:]).regions, "VM")
    return (F.sector0_view(data[:F.SECTOR]) + sfc_from(data[F.AREA_AT:vm.offset], F.AREA_AT, F.AREA_AT)
            + bytes(data[vm.offset:]), view)


def region(regions, name):
    r = next((r for r in regions if r.name == name), None)
    if r is None:
        raise ValueError(f"the directory declares no {name}")
    return r


@dataclass
class Chain:
    """What the ROM and the uboot find, in their order."""
    s0: list            # sector 0: the flash header, then its file list
    pointer: int        # app_dir_head: the directory's flash offset
    area: F.Area        # the directory there, its files and regions (all CRCs checked)
    entry_flash: int    # where the entry point lies in flash
    entry_file: str     # the file it lands in
    entry_at: int       # its offset in that file


def boot_chain(image, base_of=KEY_MODELS[0][1]):
    """Follow the boot as the ROM and the uboot do (docs/image.md, "How the uboot starts the
    application"): sector 0's file list and its boot pointer, the directory there through the SFC
    (base_of(pointer): where the keys count from), app_area_head's entry point onto the file it
    lands in. Raises ValueError where the chain breaks."""
    s0 = F.sector0_entries(image[:F.SECTOR])
    names = [e.name for e in s0[1:]]
    if "uboot.boot" not in names:
        raise ValueError("sector 0 lists no uboot.boot")
    if "app_dir_head" not in names:
        raise ValueError("sector 0 lists no app_dir_head")
    ptr = next(e for e in s0[1:] if e.name == "app_dir_head").offset
    if ptr % F.SECTOR or not F.SECTOR <= ptr < len(image):
        raise ValueError(f"the boot pointer {ptr:#x} is not a sector inside flash")
    base = base_of(ptr)
    head = F.Entry.parse(sfc_from(image[ptr:ptr + F.ENTRY_SIZE], ptr, base))
    if head is None or head.name != "app_area_head":
        raise ValueError(f"no app_area_head at {ptr:#x}")
    if ptr + head.size > len(image):
        raise ValueError(f"the area at {ptr:#x} would end past the flash")
    area = F.parse_area(sfc_from(image[ptr:ptr + head.size], ptr, base))
    if area.entry & ~0xFFFF != F.CPU_BASE:
        raise ValueError(f"the entry point {area.entry:#x} is outside the window the uboot maps")
    rel = area.entry - F.CPU_BASE
    hit = next((e for e in area.entries[1:]
                if e.attr == F.ATTR_FILE and e.offset <= rel < e.offset + e.size), None)
    if hit is None:
        raise ValueError(f"the entry point {area.entry:#x} lands in no file")
    return Chain(s0, ptr, area, ptr + rel, hit.name, rel - hit.offset)


def used(image, lo=0, hi=None):
    """[(start, end)] of the runs of sectors that are not erased."""
    out = []
    for s in sectors(lo, hi or len(image)):
        if image[s:s + F.SECTOR].count(0xFF) != F.SECTOR:
            if out and out[-1][1] == s:
                out[-1] = (out[-1][0], s + F.SECTOR)
            else:
                out.append((s, s + F.SECTOR))
    return out


def overlaps(a, b):
    return a[0] < b[1] and b[0] < a[1]


def minus(runs, cut):
    """The runs [(start, end)] without the range cut."""
    out = []
    for a, b in runs:
        out += [(lo, hi) for lo, hi in ((a, min(b, cut[0])), (max(a, cut[1]), b)) if lo < hi]
    return out


def erased(image, s):
    return image[s:s + F.SECTOR].count(0xFF) == F.SECTOR


def earlier_area(image, at):
    """Whether the directory of an earlier installation of ours is at `at`: its first entry,
    through the SFC, an app_area_head whose header CRC checks. Our firmware points sector 0 back at
    the stock area to be updated (docs/image.md, "Installing again"); its area stays."""
    head = F.Entry.parse(sfc_from(image[at:at + F.ENTRY_SIZE], at, at))
    return head is not None and head.name == "app_area_head"


def btif_item(btif, offset, length):
    """'empty', 'valid' or 'other': the SDK's reading of a BTIF item (__btif_item_read in our
    image: an all-ones header is empty; the length and the CRC's low byte must match)."""
    h = struct.unpack_from("<I", btif, offset)[0]
    if h == 0xFFFFFFFF:
        return "empty"
    data = btif[offset + 4:offset + 4 + length]
    return "valid" if h >> 20 == length and F.crc16(data) & 0xFF == h & 0xFF else "other"


# --- writes as flash takes them ---------------------------------------------------------------

@dataclass
class Write:
    what: str
    offset: int
    data: bytes
    erase: tuple = ()   # sectors erased first
    file: str = ""      # where --out saves data


def apply(image, writes):
    """The image after the writes: an erase sets its sector to 0xFF, programming ANDs (NOR flash
    only clears bits)."""
    out = bytearray(image)
    for w in writes:
        for s in w.erase:
            out[s:s + F.SECTOR] = b"\xff" * F.SECTOR
        for i, b in enumerate(w.data):
            out[w.offset + i] &= b
    return bytes(out)


def needs_erase(old, new):
    """How many bits programming alone cannot make (0 -> 1)."""
    return sum(bin(n & ~o & 0xFF).count("1") for o, n in zip(old, new))


# --- the plan -----------------------------------------------------------------------------------

class Report:
    def __init__(self):
        self.items = []

    def add(self, section, status, text):
        self.items.append({"section": section, "status": status, "text": text})

    def check(self, section, ok, text, bad=None):
        self.add(section, "pass" if ok else "fail", text if ok else (bad or text))
        return ok

    @property
    def failed(self):
        return sum(1 for i in self.items if i["status"] == "fail")


@dataclass
class Plan:
    report: Report
    view: str = ""
    before: bytes = b""
    after: bytes = b""
    phys: bytes = b""
    writes: list = None
    rollback: list = None
    found: str = ""          # what of ours the flash holds: "" none, "earlier" an installation, "stopped" this one's pages
    steps: list = None       # the writes and checks in words, in order
    undo: list = None        # the rollback in words


def plan(dump, app, cfg=None, tail=b"", at=SLOT, vm=None, view="auto"):
    """The dry run (module docstring). Returns a Plan; plan.report.failed counts what broke. The
    images are set only as far as the checks got."""
    rep = Report()
    p = Plan(rep, writes=[], rollback=[], steps=[], undo=[])

    sec = "now"
    try:
        before, p.view = flash_view(dump, view)
    except ValueError as e:
        rep.add(sec, "fail", f"the image: {e}")
        return p
    p.before = before
    try:
        stock = boot_chain(before)
    except ValueError as e:
        rep.add(sec, "fail", f"the boot chain of the flash now: {e}")
        return p
    s0names = ", ".join(e.name for e in stock.s0[1:])
    if not rep.check(sec, stock.pointer == F.AREA_AT,
                     f"sector 0 ({s0names}): the boot pointer is the stock {F.AREA_AT:#x}",
                     f"the boot pointer is {stock.pointer:#x}, not the stock {F.AREA_AT:#x}: "
                     "not the stock flash (already installed?)"):
        return p
    svm = region(stock.area.regions, "VM")
    rep.add(sec, "pass", f"the stock area at {stock.pointer:#x}, as the uboot reads it: CRCs ok, entry "
            f"{stock.area.entry:#x} = {stock.entry_file}+{stock.entry_at:#x} (flash {stock.entry_flash:#x}); "
            + ", ".join(f"{n} {len(d):#x}" for n, d in stock.area.files)
            + f"; VM {svm.offset:#x}+{svm.size:#x}")
    in_use = used(before)
    rep.add(sec, "info", "sectors in use: " + ", ".join(rng(a, b) for a, b in in_use))

    sec = "ours"
    files = [("app.bin", app)] + ([("cfg_tool.bin", cfg)] if cfg else [])
    size = F.SLOTS * F.ENTRY_SIZE + sum(len(d) for _, d in files) + len(tail)
    end = at + size
    vm_start, vm_size = vm if vm else (align(end), VM_END - align(end))
    cfg_rng = (CFG_AT, CFG_AT + CFG_SIZE)
    keep = [region(stock.area.regions, n) for n in KEEP]
    vm_r = F.stock_regions(vm_start, vm_size)[0]
    prct = F.Region("PRCT", 0, vm_start, F.ATTR_REGION, F.RES_PRCT, b"CODE")
    area = F.Area(F.ENTRY, files, [vm_r, prct] + keep, tail)
    ours = (at, align(end))
    vm_rng = (vm_start, vm_start + vm_size)
    ok = rep.check(sec, at % F.SECTOR == 0 and F.AREA_AT < at and end <= FLASH_SIZE,
                   f"area {rng(at, end)} ({size:#x} bytes)", f"the area {rng(at, end)} does not fit")
    ok &= rep.check(sec, vm_start >= align(end) and vm_start % F.SECTOR == 0 and vm_size % F.SECTOR == 0
                    and vm_size >= VM_LEAST and vm_rng[1] <= VM_END,
                    f"our VM {rng(*vm_rng)} ({vm_size // 1024} KB; the SDK needs {VM_LEAST // 1024} KB), "
                    f"after the area, sector-aligned, below the stock update loader ({VM_END:#x})",
                    f"our VM {vm_start:#x}+{vm_size:#x} must be sector-aligned, at least {VM_LEAST:#x} bytes, "
                    f"start after the area's last sector ({align(end):#x}) and end by the stock update "
                    f"loader ({VM_END:#x})")
    ok &= rep.check(sec, svm.offset + F.SECTOR <= cfg_rng[0] and cfg_rng[1] <= at,
                    f"the configuration's slots {rng(*cfg_rng)} below the area, above the stock VM's "
                    f"first sector ({svm.offset:#x})",
                    f"the configuration's slots {rng(*cfg_rng)} must lie between the stock VM's first "
                    f"sector ({svm.offset:#x}) and the area ({at:#x})")
    if not ok:
        return p
    try:
        plain = F.build_area(area, at)
    except ValueError as e:
        rep.add(sec, "fail", f"the directory: {e}")
        return p
    rep.add(sec, "pass", "directory: " + ", ".join(
        f"{e.name} {e.offset:#x}+{e.size:#x}" for e in F.parse_area(plain).entries[1:])
        + f"; entry {F.ENTRY:#x}")
    ciphers = [sfc_from(plain, at, base(at)) for _, base in KEY_MODELS]
    if not rep.check(sec, ciphers[0] == ciphers[1],
                     f"both SFC key models encrypt it the same at {at:#x}",
                     f"at {at:#x} the two SFC key models encrypt it differently: only "
                     f"{F.AREA_AT:#x} + k * 0x40000 is safe"):
        return p
    p.phys = phys = ciphers[0]
    for r in keep:
        clash = [n for n, x in (("the area", ours), ("our VM", vm_rng), ("the configuration's slots", cfg_rng))
                 if overlaps(x, (r.offset, r.offset + r.size))]
        rep.check(sec, not clash, f"{r.name} {r.offset:#x}+{r.size:#x} kept as the stock directory has it",
                  f"{' and '.join(clash)} overlap{'s' if len(clash) == 1 else ''} {r.name}")
    loader = used(before, LOADER_AT, keep[0].offset)
    rep.check(sec, not any(overlaps(vm_rng, u) for u in loader),
              f"the update loader's sectors ({', '.join(rng(*u) for u in loader) or 'none in this flash'}) "
              "stay outside our VM", "our VM overlaps the stock update loader")
    rep.check(sec, not any(overlaps(cfg_rng, u) for u in loader) and cfg_rng[1] <= LOADER_AT,
              "and outside the configuration's slots", "the configuration's slots overlap the stock update loader")
    # What of ours is there already. An earlier installation (the update: our firmware pointed
    # sector 0 back at the stock area, ours stayed): its sectors are erased and written again, the
    # configuration's slots kept. Else every sector of ours must be erased or hold, page by page,
    # this area's bytes or none: a run of this installation that stopped (the installer writes our
    # area's first sector, the directory, last).
    want = phys + b"\xff" * (ours[1] - at - len(phys))
    page_ok = lambda q: before[q:q + CHUNK] in (want[q - at:q - at + CHUNK], b"\xff" * CHUNK)  # noqa: E731
    mine = [s for s in sectors(*ours) if not erased(before, s) and all(page_ok(q) for q in range(s, s + F.SECTOR, CHUNK))]
    if earlier_area(before, at):
        p.found = "earlier"
        rep.add(sec, "info", f"our sectors {rng(at, VM_END)} hold an earlier installation of ours (its directory "
                f"at {at:#x}): erased and written again; the configuration's slots and the panel's state "
                f"{rng(*cfg_rng)} are kept")
    else:
        p.found = "stopped" if mine else ""
        busy = [s for s in sectors(at, vm_rng[1]) + (sectors(*cfg_rng) if not mine else [])
                if not erased(before, s) and s not in mine]
        runs = []
        for s in busy:
            if runs and runs[-1][1] == s:
                runs[-1] = (runs[-1][0], s + F.SECTOR)
            else:
                runs.append((s, s + F.SECTOR))
        rep.check(sec, not busy, f"our sectors {rng(*cfg_rng)} and {rng(at, vm_rng[1])} are erased in this flash"
                  + (f", but {len(mine)} of the area's that hold its bytes already: a run of this installation "
                     "that stopped; the configuration's slots are kept" if mine else ""),
                  "our sectors are in use: " + ", ".join(rng(*u) for u in sorted(runs)))
    if rep.failed:
        return p

    # The writes.
    sec = "writes"
    s0_old = before[:F.SECTOR]
    s0_new = F.set_boot_pointer(s0_old, at)
    idx = next(i for i, e in enumerate(stock.s0) if i and e.name == "app_dir_head")
    ent = (idx * F.ENTRY_SIZE, (idx + 1) * F.ENTRY_SIZE)
    bits = needs_erase(s0_old[ent[0]:ent[1]], s0_new[ent[0]:ent[1]])
    erased_crc = lambda lo, hi: F.crc16(b"\xff" * (hi - lo))  # noqa: E731
    stale = tuple(s for s in sectors(ours[1], VM_END) if not erased(before, s)) if p.found == "earlier" else ()
    p.writes = ([Write("our earlier sectors", stale[0], b"", stale)] if stale else []) + [
        Write("our area", at, phys, tuple(sectors(*ours)), "area-phys.bin"),
        Write("the boot pointer", 0, s0_new, (0,), "sector0-after.bin")]
    p.steps = [
        f"1. Check that the device holds this flash: CRC-16 of sector 0 0x0+0x1000 = {F.crc16(s0_old):#06x}, "
        f"of the stock area {F.AREA_AT:#x}+{svm.offset - F.AREA_AT:#x} = {F.crc16(before[F.AREA_AT:svm.offset]):#06x}, "
        f"of our sectors {at:#x}+{vm_rng[1] - at:#x} = {F.crc16(before[at:vm_rng[1]]):#06x} "
        + {"": "(erased).", "earlier": "(an earlier installation of ours).",
           "stopped": "(part of this area, the rest erased)."}[p.found],
        (f"2. Our area: erase the earlier installation's {len(stale)} sectors after it ({', '.join(f'{x:#x}' for x in stale)}), then "
         if stale else "2. Our area: ")
        + f"erase {len(sectors(*ours))} sectors {rng(*ours)} ({'already erased here' if not p.found else 'where they hold anything'}; "
        "the first, with the directory, last), write "
        f"{len(phys):#x} bytes (area-phys.bin; {-(-len(phys) // CHUNK)} writes of 256 bytes with 0x22, smcmidi sinco-write). "
        f"Then CRC-16 of {at:#x}+{len(phys):#x} = {F.crc16(phys):#06x}; the stock read (0x23) returns "
        "these bytes as stored, being above the stock VM start.",
        f"3. The boot pointer: sector 0's entry {idx} (app_dir_head, at {ent[0]:#x}) from {F.AREA_AT:#x} to "
        f"{at:#x}, the other {F.SECTOR - F.ENTRY_SIZE} bytes as they are: erase sector 0, write "
        f"sector0-after.bin. Then CRC-16 of 0x0+0x1000 = {F.crc16(s0_new):#06x}.",
        f"4. Reset: the uboot reads the boot pointer {at:#x} and jumps to {F.ENTRY:#x} = flash "
        f"{at + F.ENTRY - F.CPU_BASE:#x}, the first byte of our app.bin.",
    ]
    rep.add(sec, "info", "the boot pointer entry alone could be changed by programming, without erasing "
            "sector 0 (its bits only go from 1 to 0)" if bits == 0 else
            f"the boot pointer entry needs sector 0 erased: {bits} of its bits go from 0 to 1")
    rep.add(sec, "info", f"the stock write (0x22) stores sector 0 as sent (bringup.md, Step 2b): CRC-16 "
            f"{F.crc16(s0_new):#06x}; its driver checks only the end of the flash, not PRCT (static analysis)")

    # The flash after, read as the ROM and the uboot read it.
    sec = "after"
    p.after = after = apply(before, p.writes)
    for name, base in KEY_MODELS:
        try:
            c = boot_chain(after, base)
        except ValueError as e:
            rep.add(sec, "fail", f"{name}: {e}")
            continue
        rep.check(sec, c.pointer == at and c.area.entry == F.ENTRY and c.entry_file == "app.bin"
                  and c.entry_at == 0 and c.area.file("app.bin") == app,
                  f"{name}: sector 0 -> {c.pointer:#x}, directory CRCs ok, entry {c.area.entry:#x} = flash "
                  f"{c.entry_flash:#x}, the first byte of app.bin, which is ours byte for byte",
                  f"{name}: the chain leads to {c.pointer:#x}, entry {c.area.entry:#x} in "
                  f"{c.entry_file}+{c.entry_at:#x}, not to our app.bin's first byte")
    changed = [s for s in sectors(0, FLASH_SIZE) if before[s:s + F.SECTOR] != after[s:s + F.SECTOR]]
    diff0 = [i for i in range(F.SECTOR) if before[i] != after[i]]
    rep.check(sec, set(changed) <= {0} | set(sectors(*ours)) | set(stale) and diff0 and ent[0] <= diff0[0]
              and diff0[-1] < ent[1],
              f"what changes: sector 0 bytes {diff0[0]:#x}..{diff0[-1]:#x} (app_dir_head) and "
              f"{len(changed) - 1} of our sectors; nothing else" if diff0 else "",
              "the writes change more than the boot pointer entry and our sectors: "
              + ", ".join(f"{s:#x}" for s in changed))
    kept = [("the stock area", F.AREA_AT, svm.offset)] + [(r.name, r.offset, r.offset + r.size) for r in keep] \
        + [(f"in use {rng(max(a, F.SECTOR), b)}", max(a, F.SECTOR), b)
           for a, b in minus(in_use, (at, VM_END)) if b > F.SECTOR]
    same = [n for n, a, b in kept if before[a:b] == after[a:b]]
    rep.check(sec, len(same) == len(kept), "unchanged byte for byte: the stock area, "
              + ", ".join(KEEP) + ", every other sector in use",
              "changed: " + ", ".join(n for n, a, b in kept if before[a:b] != after[a:b]))

    # The first run of our firmware.
    sec = "first run"
    rep.add(sec, "info", f"the SDK's settings store (VM) is ours at {rng(*vm_rng)}; its writes stay there "
            "(⚠️ the SDK's VM code has not run on the device)")
    rep.add(sec, "info", f"the configuration's two slots and the panel's state {rng(*cfg_rng)} "
            "(firmware/store/cfgstore.h, statestore.h): "
            + ("kept, so the first start runs the configuration saved there, if our firmware reads it; "
               if p.found and not all(erased(before, s) for s in sectors(*cfg_rng)) else
               "erased now, so the first start runs the factory configuration with Bluetooth on, preset 1 page 1; ")
            + "our firmware writes one slot a few seconds "
            "after a configuration is received (✅ at this place 2026-11-12, controller 21). A later "
            "installation of our area leaves them; the rollback (R2) erases them")
    btif = region(stock.area.regions, "BTIF")
    data = before[btif.offset:btif.offset + btif.size]
    at_item = 0
    for ident, n, name in BTIF_ITEMS:
        st = btif_item(data, at_item, n)
        if ident == 102:
            text = {"valid": "valid: our firmware reads the stock MAC address and writes nothing to BTIF",
                    "empty": "empty: our firmware writes a random MAC address into it (programming an "
                             "erased slot, no erase)",
                    "other": "holds something else: our firmware refuses to write there and makes a new "
                             "random MAC address on every boot"}[st]
            rep.add(sec, "pass" if st == "valid" else "info", f"BTIF item {ident} ({name}) at "
                    f"{btif.offset + at_item:#x}: {text}")
        else:
            rep.add(sec, "info", f"BTIF item {ident} ({name}) at {btif.offset + at_item:#x}: {st}; our "
                    "firmware only reads it")
        at_item += 4 + n

    # The rollback, applied to the flash after.
    sec = "rollback"
    p.rollback = [Write("sector 0 back", 0, s0_old, (0,), "sector0-before.bin"),
                  Write("our sectors erased", at, b"", tuple(sectors(*cfg_rng)) + tuple(sectors(at, vm_rng[1])))]
    # the flash before, but for what of ours it held (nothing on a first installation)
    bare = apply(before, [Write("", 0, b"", tuple(sectors(*cfg_rng)) + tuple(sectors(at, VM_END)))])
    as_before = "the flash before" + (" with our sectors erased" if bare != before else "")
    p.undo = [
        f"R1. Sector 0 back: erase it and write sector0-before.bin; CRC-16 of 0x0+0x1000 = "
        f"{F.crc16(s0_old):#06x}. The stock firmware starts again: its area was never touched.",
        f"R2. Erase our sectors {rng(*cfg_rng)} and {rng(at, vm_rng[1])} (the configuration's slots and "
        f"the panel's state, the area and the VM our firmware wrote), so the stock VM range holds nothing of ours; CRC-16 = "
        f"{erased_crc(*cfg_rng):#06x} and {erased_crc(at, vm_rng[1]):#06x}.",
        f"R3. CRC-16 of the whole flash 0x0+{FLASH_SIZE:#x} = {F.crc16(bare):#06x}, {as_before}, "
        "unless the stock firmware's own VM or BTIF were written in between (then compare range by range). "
        "The last line of defence stays the raw read of Rehearsal 1 (recovery.md).",
    ]
    rep.check(sec, apply(after, p.rollback) == bare,
              f"applied to the flash after, the rollback gives {as_before} byte for byte",
              "the rollback does NOT give the flash before")

    sec = "open"
    for text in ("the uboot's way through the directory is open-smc-pad's reading on the SMC-PAD; not "
                 "seen on the mixer",
                 "no cfg_tool.bin (--no-cfg): our SDK build resets in its first syscfg_read() without "
                 "one (docs/bringup.md, \"Step 4: boot marks\", run 3)" if not cfg else "",
                 "this flash is a model until a raw ROM read confirms it (recovery.md, Rehearsal 1)"
                 if p.view == "stock" else ""):
        if text:
            rep.add(sec, "open", text)
    return p


# --- command line -------------------------------------------------------------------------------

SECTIONS = (("now", "the flash now"), ("ours", "our area"), ("writes", "the writes"),
            ("after", "the flash after, as the ROM and the uboot read it"), ("first run", "our firmware's first run"),
            ("rollback", "the rollback"), ("open", "open"))


def text(p, title):
    out = [title]
    for key, head in SECTIONS:
        items = [i for i in p.report.items if i["section"] == key]
        steps = p.steps if key == "writes" else p.undo if key == "rollback" else []
        if not items and not steps:
            continue
        out.append(f"-- {head}")
        out += [f"   {s}" for s in steps]
        out += [f"{i['status'].upper():4} {i['text']}" for i in items]
    n = sum(1 for i in p.report.items if i["status"] == "pass")
    out.append(f"result: {n} checks hold, {p.report.failed} failed")
    return "\n".join(out)


def as_json(p):
    def w(x):
        return {"what": x.what, "offset": x.offset, "size": len(x.data), "erase": list(x.erase),
                "crc16": F.crc16(x.data) if x.data else None, "file": x.file or None}
    return {"view": p.view, "found": p.found, "items": p.report.items, "failed": p.report.failed, "steps": p.steps,
            "writes": [w(x) for x in p.writes], "rollback_steps": p.undo, "rollback": [w(x) for x in p.rollback],
            "crc16": {"before": F.crc16(p.before) if p.before else None,
                      "after": F.crc16(p.after) if p.after else None}}


def private(path):
    """True when path lies outside the repository or under one of its ignored directories."""
    p, root = os.path.realpath(path), os.path.realpath(ROOT)
    if os.path.commonpath([p, root]) != root:
        return True
    rel = os.path.relpath(p, root).split(os.sep)
    return rel[0] in PRIVATE


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("dump")
    ap.add_argument("--app", required=True)
    cfg = ap.add_mutually_exclusive_group(required=True)
    cfg.add_argument("--cfg", help="cfg_tool.bin, after app.bin (the SDK's: "
                     "deps/fw-AC63_BT_SDK/cpu/br25/tools/cfg_tool.bin)")
    cfg.add_argument("--no-cfg", action="store_true",
                     help="no cfg_tool.bin: our SDK build does not start without one")
    ap.add_argument("--tail")
    ap.add_argument("--at", type=lambda s: int(s, 0), default=SLOT)
    ap.add_argument("--vm", help="START:SIZE (default: after the area, up to the loader at 0x6E000)")
    ap.add_argument("--view", choices=("auto", "stock", "raw"), default="auto")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--out")
    a = ap.parse_args(argv)
    if a.out and not private(a.out):
        print(f"installplan: --out {a.out}: the images hold vendor code and Bluetooth data; use a directory "
              f"outside the repository or under {', '.join(d + '/' for d in PRIVATE)} (SPEC C-7)", file=sys.stderr)
        return 2
    vm = tuple(int(x, 0) for x in a.vm.split(":")) if a.vm else None

    def read(path):
        with open(path, "rb") as f:
            return f.read()
    p = plan(read(a.dump), read(a.app), read(a.cfg) if a.cfg else None, read(a.tail) if a.tail else b"",
             a.at, vm, a.view)
    report = text(p, f"installation dry run: {os.path.basename(a.dump)} ({p.view or '?'} read), "
                     f"{os.path.basename(a.app)} {len(read(a.app))} bytes, at {a.at:#x}; nothing is written")
    print(json.dumps(as_json(p), indent=1) if a.json else report)
    if a.out and p.after:
        os.makedirs(a.out, exist_ok=True)
        files = {"flash-before.bin": p.before, "flash-after.bin": p.after}
        files.update({x.file: x.data for x in p.writes + p.rollback if x.file})
        for name, data in files.items():
            with open(os.path.join(a.out, name), "wb") as f:
                f.write(data)
        with open(os.path.join(a.out, "plan.json"), "w") as f:
            json.dump(as_json(p), f, indent=1)
        with open(os.path.join(a.out, "plan.txt"), "w") as f:
            f.write(report + "\n")
        print(f"installplan: wrote {', '.join(sorted(files))}, plan.json, plan.txt to {a.out}", file=sys.stderr)
    return 1 if p.report.failed else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
