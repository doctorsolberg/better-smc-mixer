"""Decode the stock control tables in USRFLASH (flash 0x7D000, stored as is).

Usage: python3 -I tools/stage0/usrflash.py DUMP.bin
Layout as decoded on V012/V013 (docs/SPEC.md §3.3):
  0x0000  43 DAW button records, 0x47 bytes each
  0x0BED  0xFF, then 8 DAW encoder records (6 bytes) and 8 DAW fader records (5 bytes)
  0x0C45  43 User button records, 0x47 bytes each
  0x1832  0xFF, then 8 User encoder records (6 bytes) and 8 User fader records (5 bytes)
"""
import sys

BUTTONS = [f"{k}{n + 1}" for n in range(8) for k in ("M", "S", "R", "SEL")] + \
    ["Play", "Stop", "Rec", "Rew", "FF", "Bank<", "Bank>", "Up", "Down", "Left", "Right"]
TYPES = {0x00: "CC?", 0x02: "CC", 0x03: "enc", 0x05: "note"}


def buttons(u, base):
    out = []
    for i, name in enumerate(BUTTONS):
        r = u[base + i * 0x47: base + (i + 1) * 0x47]
        ln = r[5]
        out.append((name, r[0], r[1], r[2], r[3], r[4], r[6:6 + ln].hex(" "), r[-1]))
    return out


def main():
    d = open(sys.argv[1], "rb").read()
    u = d[0x7D000:0x7F000] if len(d) == 0x80000 else d
    for title, base, tail in (("DAW", 0x0000, 0x0BED), ("User", 0x0C45, 0x1832)):
        print(f"== {title} buttons: name type b1 num min max | sysex field | last")
        for name, t, b1, num, mn, mx, sx, last in buttons(u, base):
            print(f"  {name:6} {TYPES.get(t, hex(t)):5} {b1:#04x} {num:3} {mn:3} {mx:3} | {sx} | {last}")
        print(f"== {title} tail @{tail:#x}: {u[tail:tail + 0x58].hex(' ')}")


if __name__ == "__main__":
    main()
