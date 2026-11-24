"""Map a 512 KB flash dump: per-4KB-sector contents, JLFS directory entries, strings.

Usage: python3 -I tools/stage0/flashmap.py DUMP.bin
Read-only analysis; prints a report to stdout.
"""
import math
import re
import struct
import sys


def crc16(data, crc=0):
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def entropy(b):
    if not b:
        return 0.0
    counts = [0] * 256
    for x in b:
        counts[x] += 1
    return -sum(c / len(b) * math.log2(c / len(b)) for c in counts if c)


def sector_kind(s):
    if s.count(0xFF) == len(s):
        return "FF"
    if s.count(0x00) == len(s):
        return "00"
    return f"{entropy(s):.1f}"


def jlfs_entries(d):
    """Valid 32-byte JLFS entries anywhere in the dump (header CRC checked)."""
    out = []
    for o in range(0, len(d) - 32, 16):
        e = d[o:o + 32]
        hc, dc, off, size, attr, res, idx = struct.unpack("<HHIIBBH", e[:16])
        name = e[16:32].split(b"\0")[0]
        if not name or not all(32 <= c < 127 for c in name):
            continue
        if crc16(e[2:32]) == hc:
            out.append((o, name.decode(), off, size, attr, idx, dc))
    return out


def main():
    d = open(sys.argv[1], "rb").read()
    print(f"size {len(d):#x}")
    print("\n4 KB sectors (FF = erased, 00 = zeros, number = entropy bits/byte):")
    row = []
    for s in range(0, len(d), 0x1000):
        row.append(f"{s >> 12:02X}:{sector_kind(d[s:s + 0x1000]):>3}")
        if len(row) == 8:
            print("  " + "  ".join(row))
            row = []
    print("\nJLFS entries (offset, name, data offset, size, attr, index):")
    for o, name, off, size, attr, idx, dc in jlfs_entries(d):
        print(f"  {o:#07x}  {name:16} off={off:#08x} size={size:#08x} attr={attr:#04x} idx={idx} dcrc={dc:#06x}")
    print("\nStrings (>= 6 chars, with offsets), first 120:")
    n = 0
    for m in re.finditer(rb"[\x20-\x7e]{6,}", d):
        s = m.group().decode()
        if re.search(r"[A-Za-z]{3}", s):
            print(f"  {m.start():#07x}  {s[:70]}")
            n += 1
            if n >= 120:
                break


if __name__ == "__main__":
    main()
