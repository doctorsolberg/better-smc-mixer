#!/usr/bin/env bash
# Build the RP2040 USB key firmware (tools/usbkey) in the container and check it; run its host
# tests on this machine.
#
#   tools/env/build_usbkey.sh        output: build/usbkey/usbkey.uf2, log: logs/build/usbkey.log
#
# Checks: nothing but our objects, TinyUSB and libgcc is linked; the second-stage boot fills the
# first 256 bytes of flash and its CRC is right; the vector table follows at 0x10000100 with the
# stack top and the reset handler; only the BOOTSEL reader runs from RAM; the UF2 file holds the
# whole image. Nothing talks to a board: copying the UF2 onto it is done by hand
# (docs/recovery.md).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT=build/usbkey
LOG="$ROOT/logs/build/usbkey.log"
mkdir -p "$ROOT/$OUT" "$(dirname "$LOG")"

{
echo "build_usbkey: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
start=$(date +%s)
"$ROOT/tools/env/vendor_tinyusb.sh" check
"$ROOT/tools/env/run.sh" bash -c "
    set -euo pipefail
    make -C /src/tools/usbkey clean >/dev/null
    make -C /src/tools/usbkey -j\$(nproc) OUT=/src/$OUT
    cd /src/$OUT
    arm-none-eabi-gcc --version | head -1
    arm-none-eabi-objdump -h usbkey.elf > usbkey.sections
    arm-none-eabi-nm -n usbkey.elf > usbkey.syms
    arm-none-eabi-readelf -sW usbkey.elf > usbkey.symtab
    arm-none-eabi-size usbkey.elf
"
python3 -I - "$ROOT/$OUT" <<'PY'
import re, struct, sys
sys.path.insert(0, sys.argv[1].rsplit("/build/", 1)[0] + "/tools/usbkey")
import uf2

out = sys.argv[1]
fails = []
def check(ok, msg):
    print(("  ok   " if ok else "  FAIL ") + msg)
    if not ok:
        fails.append(msg)

mp = open(f"{out}/usbkey.map").read()
inputs = set(re.findall(r"^LOAD (.+)$", mp, re.M))
allowed = re.compile(r"(/src/build/usbkey/[\w/]+\.o|.*/libgcc\.a|linker stubs)$")
check(all(allowed.match(i) for i in inputs), "linked: our objects, TinyUSB, libgcc only")

sections = {}
for line in open(f"{out}/usbkey.sections"):
    m = re.match(r"\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", line)
    if m:
        sections[m.group(1)] = tuple(int(m.group(k), 16) for k in (2, 3, 4))
alloc = {n for n in sections if not n.startswith(".debug") and n != ".comment"}
check(alloc == {".boot2", ".text", ".data", ".bss"}, "sections: " + " ".join(sorted(alloc)))
check(sections[".boot2"][:2] == (256, 0x10000000), ".boot2: 256 bytes at 0x10000000")

syms = {}
for line in open(f"{out}/usbkey.syms"):
    a, t, n = line.split()
    syms[n] = int(a, 16)
# Functions by their ELF type (nm calls everything in .data code, since .ram_func is in it)
code = {f[7] for f in (l.split() for l in open(f"{out}/usbkey.symtab")) if len(f) == 8 and f[3] == "FUNC"}
check(sections[".text"][1] == 0x10000100 and syms.get("vectors") == 0x10000100, "vector table at 0x10000100")

uf = open(f"{out}/usbkey.uf2", "rb").read()
image = bytearray()
for i in range(0, len(uf), 512):
    hdr = struct.unpack("<8I", uf[i:i + 32])
    assert hdr[3] == 0x10000000 + len(image), "UF2 blocks out of order"
    image += uf[i + 32:i + 32 + hdr[4]]
sp, reset = struct.unpack("<II", image[256:264])
check(sp == syms["__stack_top"] == 0x20040000, f"initial stack pointer {sp:#x}")
check(reset == syms["reset_handler"] | 1, f"reset vector {reset:#x} -> reset_handler (Thumb)")
check(struct.unpack("<I", image[252:256])[0] == uf2.crc32_boot2(image[:252]), "boot2 CRC-32")
data_end = sections[".data"][2] + sections[".data"][0]
check(len(image) >= data_end - 0x10000000, f"UF2 holds the image up to .data's end ({len(image)} bytes)")
ram_code = [n for n in code if 0x20000000 <= syms[n] < 0x20000000 + sections[".data"][0]
            and n not in ("__data_start", "__data_end")]
check(ram_code == ["bootsel_pressed"], "code in RAM: " + ", ".join(ram_code))
print(f"usbkey.uf2: {len(image)} bytes of flash, RAM {sections['.data'][0] + sections['.bss'][0]} bytes")
sys.exit(1 if fails else 0)
PY
cc -std=c11 -Wall -Wextra -Werror -o "$ROOT/$OUT/test_key" "$ROOT/tools/usbkey/test_key.c" "$ROOT/tools/usbkey/key.c"
"$ROOT/$OUT/test_key"
python3 -I "$ROOT/tools/usbkey/test_uf2.py" 2>&1 | tail -1
echo "build_usbkey: ok in $(( $(date +%s) - start )) s"
} 2>&1 | tee "$LOG"
exit "${PIPESTATUS[0]}"
