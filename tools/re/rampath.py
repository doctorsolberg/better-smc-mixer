"""Check that an interrupt's code path never needs flash (docs/hwtest.md, "While flash is written").

Usage: python3 -I tools/re/rampath.py LISTING --root FUNC [--root FUNC ...]
                                     [--indirect FUNC=TARGET[,TARGET...] ...] [--quiet]
                                     [--rom-ok NAME[,NAME...]] [--forbid PATTERN ...]
                                     [--explore]

While the SDK erases or programs flash, the flash cannot be read: code and constant data in flash
stall or fault. An interrupt that must keep running then (the scan, at the priority the SDK's
flash code leaves unmasked) may reach only code in RAM (.data_code) or in the chip's mask ROM, and
may load no address in flash. This tool follows a pi32v2 objdump listing of our image (app.lst,
from tools/env/build_app.sh) from each root through every direct call, tail call and
fall-through, and fails on:

- a function reached that lies in flash;
- an instruction of a reached function that loads a flash address (a constant table, a string, a
  jump table, a function pointer to flash code);
- an indirect call or jump in a reached function, unless --indirect names its targets (they are
  then followed too);
- with --rom-ok, a call into the ROM to a function not named there (the ROM's RTOS, for one: an
  interrupt above the SDK's mask may have interrupted the RTOS in its own critical section);
- a reached function whose name matches a --forbid pattern (fnmatch; the SDK's RTOS and lock
  functions that run from RAM, for example).

--explore also follows the calls of functions found in flash, to list everything a path needs
before it is moved. ROM functions are trusted and not followed (the listing has no ROM code). Addresses are masked to
25 bits as the CPU does (objdump shows some far branch targets above 0x2000000). Exit 1 on a
failure, 2 on bad arguments. Standard library only.
"""
import argparse
import fnmatch
import re
import sys

MASK = 0x1FFFFFF
RAM = (0x10000, 0x40000)       # RAM, where .data_code runs
ROM = (0x100000, 0x200000)     # the mask ROM
FLASH = (0x1E00000, 0x2000000)  # code0 and everything mapped from flash

LABEL = re.compile(r"^([A-Za-z_$.][\w.$]*):\s*$")
INSN = re.compile(r"^\s*([0-9a-f]+):\s+((?:[0-9a-f]{2} )+)\s*\t(.*)$")
CONT = re.compile(r"^\s+\t(.*)$")
# objdump puts the target's symbol after the branch, or on the block's closing line ("} <f : a >")
TARGET = re.compile(r"\b(?:call|goto)\s+-?(?:0x)?[0-9a-f]+[\s{}a-z]*<([^<>]*?)\s*:\s*([0-9a-f]+)\s*>")
INDIRECT = re.compile(r"\b(?:call|goto)\s+r\d+\b|\bpc\s*=\s*r\d+\b")
CONST = re.compile(r"=\s*(\d+)\b")  # decimal immediates; hex ones are small
TERMINAL = re.compile(r"^(?:rts|rti|rte|goto\b|pc\s*=|\{pc\b)")


def region(addr):
    for name, (lo, hi) in (("RAM", RAM), ("ROM", ROM), ("flash", FLASH)):
        if lo <= addr < hi:
            return name
    return "elsewhere"


class Func:
    def __init__(self, name, start):
        self.name, self.start = name, start
        self.insns = []  # (address, text)

    def __repr__(self):
        return "%s@%#x" % (self.name, self.start)


def parse(text):
    """The listing's labelled blocks, in address order. A continuation line (the second line of
    a conditional block, where objdump puts the symbol of a branch target) joins its instruction."""
    funcs, cur, pending = [], None, None
    for line in text.splitlines():
        m = LABEL.match(line)
        if m:
            pending = m.group(1)
            continue
        m = INSN.match(line)
        if m:
            addr = int(m.group(1), 16)
            if pending is not None:
                cur = Func(pending, addr)
                funcs.append(cur)
                pending = None
            if cur is not None:
                cur.insns.append([addr, m.group(3).strip()])
            continue
        m = CONT.match(line)
        if m and cur is not None and cur.insns:
            cur.insns[-1][1] += " " + m.group(1).strip()
    funcs.sort(key=lambda f: f.start)
    return funcs


def targets(text):
    """Branch and call targets named in an instruction's text (masked)."""
    return [int(a, 16) & MASK for _, a in TARGET.findall(text)]


def target_names(text):
    """(address, symbol) of each target; the symbol without an offset."""
    return [(int(a, 16) & MASK, re.sub(r"\+0x[0-9a-fA-F]+$", "", n.strip()))
            for n, a in TARGET.findall(text)]


class Index:
    def __init__(self, funcs):
        self.funcs = funcs
        self.by_name = {}
        for f in funcs:
            self.by_name.setdefault(f.name, []).append(f)

    def containing(self, addr):
        """The block whose code holds addr (None for ROM or anything not listed)."""
        lo, hi = 0, len(self.funcs)
        while lo < hi:
            mid = (lo + hi) // 2
            if self.funcs[mid].start <= addr:
                lo = mid + 1
            else:
                hi = mid
        if lo == 0:
            return None
        f = self.funcs[lo - 1]
        last = f.insns[-1][0] if f.insns else f.start
        return f if f.start <= addr <= last + 8 else None

    def after(self, f):
        i = self.funcs.index(f)
        return self.funcs[i + 1] if i + 1 < len(self.funcs) else None


class Result:
    def __init__(self):
        self.reached = {}  # start -> (Func or None, how it was reached)
        self.rom = {}      # ROM address -> (its symbol, the function that calls it)
        self.failures = []

    def fail(self, msg):
        self.failures.append(msg)


def walk(funcs, roots, indirect, explore=False, rom_ok=None, forbid=()):
    """Follow the roots (names) through the listing; indirect maps a block name to the names of
    the functions its indirect calls may reach. rom_ok: the ROM functions allowed (None: any);
    forbid: fnmatch patterns no reached function may match."""
    idx = Index(funcs)
    res = Result()
    todo = []
    for name in roots:
        if name not in idx.by_name:
            res.fail("root %s is not in the listing" % name)
            continue
        for f in idx.by_name[name]:
            todo.append((f, "root"))
    while todo:
        f, how = todo.pop()
        if f.start in res.reached:
            continue
        res.reached[f.start] = (f, how)
        for pat in forbid:
            if fnmatch.fnmatchcase(f.name, pat):
                res.fail("%s is reached (from %s), and matches --forbid %s" % (f.name, how, pat))
        where = region(f.start)
        if where != "RAM":
            res.fail("%s (%#x) is in %s, reached from %s" % (f.name, f.start, where, how))
            if not explore:
                continue  # its own calls would only repeat the cause
        for addr, text in f.insns:
            for v in CONST.findall(text):
                v = int(v)
                if FLASH[0] <= v < FLASH[1]:
                    res.fail("%s loads %#x (flash) at %#x: %s" % (f.name, v, addr, text))
            if INDIRECT.search(text):
                names = indirect.get(f.name)
                if names is None:
                    res.fail("%s has an indirect call or jump at %#x (%s); name its targets with "
                             "--indirect %s=..." % (f.name, addr, text, f.name))
                for n in names or ():
                    if n not in idx.by_name:
                        res.fail("%s: indirect target %s is not in the listing" % (f.name, n))
                    for g in idx.by_name.get(n, ()):
                        todo.append((g, "%s (indirect)" % f.name))
            for t, sym in target_names(text):
                if f.start <= t <= f.insns[-1][0]:
                    continue  # a branch inside the block
                g = idx.containing(t)
                if g is None:
                    if region(t) == "ROM":
                        if t not in res.rom and rom_ok is not None and sym not in rom_ok:
                            res.fail("%s calls the ROM's %s (%#x) at %#x, not in --rom-ok"
                                     % (f.name, sym, t, addr))
                        res.rom.setdefault(t, (sym, f.name))
                    else:
                        res.fail("%s calls %#x (%s) at %#x, not a listed function"
                                 % (f.name, t, region(t), addr))
                    continue
                todo.append((g, f.name))
        if f.insns and not TERMINAL.match(f.insns[-1][1]):
            g = idx.after(f)
            if g is not None:
                todo.append((g, "%s (falls through)" % f.name))
    return res


def parse_indirect(items):
    out = {}
    for item in items or ():
        func, sep, names = item.partition("=")
        if not sep or not func or not names:
            raise ValueError("--indirect wants FUNC=TARGET[,TARGET...], not %r" % item)
        out.setdefault(func, []).extend(n for n in names.split(",") if n)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("listing")
    ap.add_argument("--root", action="append", required=True)
    ap.add_argument("--indirect", action="append")
    ap.add_argument("--quiet", action="store_true", help="only the summary and the failures")
    ap.add_argument("--explore", action="store_true", help="follow functions in flash too")
    ap.add_argument("--rom-ok", help="the ROM functions the path may call, comma-separated")
    ap.add_argument("--forbid", action="append", default=[], help="fnmatch pattern of a name")
    a = ap.parse_args(argv)
    try:
        indirect = parse_indirect(a.indirect)
    except ValueError as e:
        print("rampath: %s" % e, file=sys.stderr)
        return 2
    try:
        with open(a.listing) as f:
            funcs = parse(f.read())
    except OSError as e:
        print("rampath: %s" % e, file=sys.stderr)
        return 2
    rom_ok = None if a.rom_ok is None else {n for n in a.rom_ok.split(",") if n}
    res = walk(funcs, a.root, indirect, a.explore, rom_ok, a.forbid)
    if not a.quiet:
        for start in sorted(res.reached):
            func, how = res.reached[start]
            print("rampath:   %-32s %#9x %-5s from %s" % (func.name, start, region(start), how))
        for t in sorted(res.rom):
            print("rampath:   %-32s %#9x ROM   from %s" % (res.rom[t][0], t, res.rom[t][1]))
    for msg in res.failures:
        print("rampath: FAIL %s" % msg)
    print("rampath: %s reach %d functions in RAM and %d in ROM: %s" % (
        ", ".join(a.root), sum(1 for s in res.reached if region(s) == "RAM"), len(res.rom),
        "%d failures" % len(res.failures) if res.failures else "no flash"))
    return 1 if res.failures else 0


if __name__ == "__main__":
    sys.exit(main())
