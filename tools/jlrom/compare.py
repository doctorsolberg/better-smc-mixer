"""Compare a flash read through the ROM loader with a dump read by the stock firmware.

Usage: python3 -I tools/jlrom/compare.py ROM_READ STOCK_DUMP [--chipkey 0x980F] [--plain-from 0x24000]
                                               [--vm-end 0x6E000] [--strict]
       python3 -I tools/jlrom/compare.py --raw ROM_READ_1 ROM_READ_2 [--plain-from …] [--vm-end …] [--strict]

ROM_READ is what `jlrom read` saved: flash as stored. STOCK_DUMP is what `smcmidi sinco-read`
saved: flash as the stock firmware sees it, through the SFC cipher. So:

- from --plain-from up (the VM area and everything above it) flash is stored as is: the two
  must be equal byte for byte;
- from 0x1000 up to --plain-from the SFC cipher applies: the ROM read decrypted with the chip key
  must equal the dump. The cipher keys each 32-byte block with chipkey ^ ((address - base) >> 2).
  The vendor update (V013) says chip key 0x980F with base 0x1000, i.e. keyed on the CPU address
  (docs/image.md); the candidates are all tried, so the report shows what the chip really does;
- sector 0: the stock firmware reads it XORed with the ENC keystream of 0xFFFF restarting every
  256 bytes (docs/image.md), so the ROM read must equal the dump's sector 0 through that stream.

The stock firmware's VM (its settings: --plain-from up to --vm-end, the stock update loader's
start; docs/hardware.md) is rewritten by the stock firmware itself, for one when the DAW/User mode
is switched. So the two files can differ there whenever the stock firmware ran between the two
reads, and that says nothing against the read: such sectors are listed and marked, and fail the
comparison only with --strict. A difference anywhere else always fails.

--raw compares two ROM reads with each other (both as stored): equal, or different only in the
stock firmware's VM.

Prints a report of offsets and counts, never flash contents (the dumps hold Bluetooth keys).
Exit status 0 when the plain area matches (the VM aside) and one base explains the SFC area.
Standard library only.
"""
import sys

SECTOR = 0x1000
BLOCK = 32
BASES = (0x1000, 0x0, 0x1120)
VM_END = 0x6E000  # the stock firmware's VM ends where its update loader starts (docs/hardware.md)
S0_RESTART = 256


def enc_cipher(buf, off, size, key):
    """JieLi ENC keystream (16-bit LFSR, polynomial 0x1021) XORed in place."""
    key &= 0xFFFF
    for i in range(off, off + size):
        buf[i] ^= key & 0xFF
        key = ((key << 1) ^ 0x1021) & 0xFFFF if key & 0x8000 else (key << 1) & 0xFFFF


def sfc(data, start, end, chipkey, base):
    """SFC (de)cipher of data[start:end] (flash offsets; start and end 32-byte aligned).
    Symmetric: the same call encrypts and decrypts. Returns a new bytearray of the range."""
    out = bytearray(data[start:end])
    for a in range(start, end, BLOCK):
        enc_cipher(out, a - start, min(BLOCK, end - a), chipkey ^ ((a - base) >> 2))
    return out


def diff_ranges(a, b, start):
    """Mismatching 4 KB sectors of two equal-length buffers that begin at flash offset start."""
    bad = []
    for o in range(0, len(a), SECTOR):
        x, y = a[o:o + SECTOR], b[o:o + SECTOR]
        if x != y:
            bad.append((start + o, sum(1 for p, q in zip(x, y) if p != q)))
    return bad


def stored_area(what, a, b, start, end, vm, strict):
    """Compares a[start:end] with b[start:end], both as stored. vm: the (start, end) of the stock
    firmware's VM, where a difference is expected if the stock firmware ran in between.
    Returns (ok, report lines)."""
    bad = diff_ranges(a[start:end], b[start:end], start)
    in_vm = [(x, k) for x, k in bad if vm[0] <= x < vm[1]]
    outside = len(bad) - len(in_vm)
    if not bad:
        return True, [f"{what} {start:#07x}..{end:#07x}: equal"]
    lines = [f"{what} {start:#07x}..{end:#07x}: {len(bad)} sectors differ, {outside} of them outside "
             f"the stock firmware's VM ({vm[0]:#07x}..{vm[1]:#07x})"]
    lines += [f"  sector {x:#07x}: {k} bytes differ" + (" (the stock firmware's VM)" if (x, k) in in_vm else "")
              for x, k in bad]
    if not outside:
        lines.append("  -> only the stock firmware's VM differs: expected if the stock firmware ran between "
                     "the two reads" + (" (--strict: counted as a mismatch)" if strict else ""))
    return not outside and not (strict and in_vm), lines


def compare_raw(a, b, vm, strict=False):
    """Two ROM reads, both as stored. Returns (ok, report lines)."""
    lines = []
    n = min(len(a), len(b))
    if len(a) != len(b):
        lines.append(f"sizes differ: {len(a):#x} and {len(b):#x}; comparing {n:#x}")
    ok, more = stored_area("raw reads", a, b, 0, n, vm, strict)
    return ok and len(a) == len(b), lines + more


def compare(rom, dump, chipkey, plain_from, vm_end=VM_END, strict=False):
    """Returns (ok, report lines)."""
    lines = []
    n = min(len(rom), len(dump))
    if len(rom) != len(dump):
        lines.append(f"sizes differ: ROM read {len(rom):#x}, dump {len(dump):#x}; comparing {n:#x}")
    ok = True

    if n > plain_from:
        same, more = stored_area("plain area", rom, dump, plain_from, n, (plain_from, vm_end), strict)
        lines += more
        ok &= same

    end = min(plain_from, n)
    if end > SECTOR:
        best = None
        for base in BASES:
            dec = sfc(rom, SECTOR, end, chipkey, base)
            blocks = (end - SECTOR) // BLOCK
            good = sum(1 for o in range(0, end - SECTOR, BLOCK)
                       if dec[o:o + BLOCK] == dump[SECTOR + o:SECTOR + o + BLOCK])
            lines.append(f"SFC area {SECTOR:#07x}..{end:#07x}, chipkey {chipkey:#06x}, base {base:#07x}: "
                         f"{good} of {blocks} blocks match")
            if best is None or good > best[1]:
                best = (base, good, blocks, dec)
        base, good, blocks, dec = best
        if good == blocks:
            lines.append(f"  -> base {base:#x} explains the whole SFC area")
        else:
            ok = False
            bad = diff_ranges(dec, dump[SECTOR:end], SECTOR)
            lines.append(f"  -> no base explains it; best {base:#x}, sectors that differ:")
            lines += [f"  sector {a:#07x}: {k} bytes differ" for a, k in bad]

    if n >= SECTOR:
        ks = bytearray(S0_RESTART)
        enc_cipher(ks, 0, S0_RESTART, 0xFFFF)
        k = sum(1 for i in range(SECTOR) if rom[i] != dump[i] ^ ks[i % S0_RESTART])
        lines.append("sector 0 through the 256-byte keystream: "
                     + ("equal" if not k else f"{k} of {SECTOR} bytes differ"))
        ok &= not k
    return ok, lines


def main(argv):
    args = list(argv)

    def opt(name, default):
        if name in args:
            i = args.index(name)
            v = int(args[i + 1], 0)
            del args[i:i + 2]
            return v
        return default

    def flag(name):
        if name in args:
            args.remove(name)
            return True
        return False

    chipkey = opt("--chipkey", 0x980F)
    plain_from = opt("--plain-from", 0x24000)
    vm_end = opt("--vm-end", VM_END)
    strict, raw = flag("--strict"), flag("--raw")
    if len(args) != 2 or plain_from % BLOCK or vm_end < plain_from:
        sys.exit(__doc__.strip())
    rom, dump = (open(p, "rb").read() for p in args)
    if raw:
        ok, lines = compare_raw(rom, dump, (plain_from, vm_end), strict)
    else:
        ok, lines = compare(rom, dump, chipkey, plain_from, vm_end, strict)
    print("\n".join(lines))
    print("result:", "ok" if ok else "MISMATCH")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
