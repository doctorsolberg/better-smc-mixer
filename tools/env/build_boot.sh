#!/usr/bin/env bash
# Build the DFU bootloader (firmware/boot) in the container: bare metal, the JieLi toolchain and
# TinyUSB only (SPEC C-4).
#
#   tools/env/build_boot.sh [make args]     output: build/boot/, log: logs/build/boot.log
#
# Outputs: boot.elf, boot.bin (.text then .data, as the image would sit in flash), the log
# dictionary boot.smclog.json and boot.map. Then the image is checked: nothing but our objects,
# TinyUSB, libc and compiler-rt is linked (no SDK); _start opens the image; only known sections;
# RAM stays below the ROM's area; the build-id note is in flash; the call sites pass the offsets
# the dictionary has. Nothing is packed into a flashable image and nothing talks to a device.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT=build/boot
LOG="$ROOT/logs/build/boot.log"
mkdir -p "$ROOT/$OUT" "$(dirname "$LOG")"

{
echo "build_boot: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
start=$(date +%s)
"$ROOT/tools/env/vendor_tinyusb.sh" check
"$ROOT/tools/env/run.sh" bash -c "
    set -euo pipefail
    T=/opt/jieli/pi32v2/bin
    make -C /src/firmware/boot -j\$(nproc) OUT=/src/$OUT $*
    cd /src/$OUT
    \$T/objdump -section-headers boot.elf | tee boot.sections
    W=\$(mktemp -d)
    for s in text data; do \$T/objcopy -O binary -j .\$s boot.elf \$W/\$s.bin; done
    cat \$W/text.bin \$W/data.bin > boot.bin
    echo \"boot.elf \$(stat -c %s boot.elf) bytes, boot.bin \$(stat -c %s boot.bin) bytes\"
    \$T/nm -n boot.elf > boot.syms
    \$T/objdump -d boot.elf | grep -E 'smclog_site_' > boot.sites || true
"
python3 -I "$ROOT/tools/smclog/smclog.py" dict "$ROOT/$OUT/boot.elf" -o "$ROOT/$OUT/boot.smclog.json"
python3 -I - "$ROOT/$OUT" <<'PY'
import json, re, sys
out = sys.argv[1]
def read(name):
    with open(out + "/" + name) as f:
        return f.read()
ok = True
def check(cond, what):
    global ok
    print("  %s %s" % ("ok  " if cond else "FAIL", what))
    ok = ok and cond

# Linked inputs: our objects and TinyUSB (all built into OUT/obj), libc, compiler-rt; the LTO
# output object is the compiled form of those.
inputs = set(re.findall(r"(/[^\s():]+\.(?:a|o))", read("boot.map")))
foreign = sorted(p for p in inputs if not (
    p.startswith("/src/build/boot/obj/") or p.startswith("/tmp/lto-llvm-")
    or p in ("/opt/jieli/pi32v2/bin/../lib/r3/libc.a", "/opt/jieli/pi32v2/bin/../lib/r3/libcompiler-rt.a")))
check(not foreign, "only our objects, TinyUSB, libc and compiler-rt linked" + (" %s" % foreign if foreign else ""))
members = sorted(set(re.findall(r"lib/r3/(lib[\w-]+\.a\([\w.-]+\))", read("boot.map"))))
print("       from the toolchain: %s" % ", ".join(members))

syms = {}
for line in read("boot.syms").splitlines():
    f = line.split()
    if len(f) == 3:
        syms[f[2]] = int(f[0], 16)
sections = {}
for m in re.finditer(r"^\s*\d+ (\S+)\s+([0-9a-f]{8}) ([0-9a-f]{16})", read("boot.sections"), re.M):
    sections[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
check(syms.get("_start") == sections[".text"][1] == 0x1E00120, "_start opens .text at 0x1E00120")
known = {".text", ".data", ".bss", "smclog", ".shstrtab", ".symtab", ".strtab"}
unknown = sorted(s for s in sections if s not in known and not s.startswith(".debug_"))
check(not unknown, "no sections besides .text, .data, .bss and smclog" + (" %s" % unknown if unknown else ""))
check(syms["__stack_top"] <= 0x31AE0, "RAM ends at 0x%X, below the ROM's area at 0x31AE0" % syms["__stack_top"])
stack = syms["__stack_top"] - syms["__bss_end"]
print("       flash %d bytes (.text %d, .data %d), RAM %d bytes (.data + .bss), stack %d bytes" % (
    sections[".text"][0] + sections[".data"][0], sections[".text"][0], sections[".data"][0],
    syms["__bss_end"] - sections[".data"][1], stack))

with open(out + "/boot.smclog.json") as f:
    d = json.load(f)
check(d["build_id"] is not None and len(d["build_id"]) == 40, "SHA-1 build ID %s" % d["build_id"])
note = syms.get("__build_id_note", 0)
check(0x1E00000 <= note < 0x1F00000, "build-id note in flash at 0x%X" % note)
sites = {int(k) * 4 for k in d["sites"]}
passed = {int(v) for v, a in re.findall(r"= (\d+) <\w+\.smclog_site_(?:\.\d+)? : ([0-9a-f]+) >", read("boot.sites"))
          if int(v) == int(a, 16)}  # other constants get symbolized too
check(bool(passed) and passed <= sites, "%d call sites, %d referenced from code, all in the dictionary" % (
    len(sites), len(passed)))
sys.exit(0 if ok else "build_boot: checks FAILED")
PY
echo "build_boot: ok in $(( $(date +%s) - start )) s"
} 2>&1 | tee "$LOG"
exit "${PIPESTATUS[0]}"
