#!/usr/bin/env bash
# Device toolchain checks: build small images with the JieLi toolchain (never run, never written
# to the device) and check what the compiler and linker did with our code.
#
#   tools/env/check_device.sh        log: logs/env/check-device.log
#
# smclog: with LTO, -Oz and the SDK's code generation flags, the call sites must land in the
# smclog section, which is not loaded (INFO); the IDs the code passes must be the offsets the
# dictionary reports; the build-id note must be in flash.
#
# core: the control core (docs/core.md) with the factory configuration, the input layer (scan,
# event queue, injection; docs/hwtest.md) and smclog builds with -Werror and links; their call
# sites are in the dictionary; the size report gives their code size (-Oz, no LTO) and the RAM of
# struct core, struct core_config and struct scan. The configuration codec (config/cfgbin.c,
# docs/config.md), the configuration protocol (link/cfglink.c, docs/link.md) and its flash slots
# (store/cfgstore.c, on a stand-in flash) are built and linked with them the same way. The BLE-MIDI
# packets (midi/blemidi.c, docs/ble.md) and the BLE queue (midi/bleq.c) are compiled the same way
# and their size reported.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT=build/device-check
LOG="$ROOT/logs/env/check-device.log"
mkdir -p "$ROOT/$OUT" "$(dirname "$LOG")"

{
echo "check_device: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
"$ROOT/tools/env/run.sh" bash -c "set -e; cd /src; T=/opt/jieli/pi32v2/bin; L=/opt/jieli/pi32v2/lib/r3
    CF='-target pi32v2 -mcpu=r3 -integrated-as -flto -Oz -g -fno-common -fallow-pointer-null
        -fprefer-gnu-section -std=gnu11 -Wall -Wextra -Werror -I/opt/jieli/pi32v2/include
        -Ifirmware/lib/smclog'
    \$T/clang \$CF -c firmware/lib/smclog/smclog.c -o $OUT/smclog.o
    \$T/clang \$CF -c firmware/tests/device/smclog_check.c -o $OUT/smclog_check.o
    \$T/lto-wrapper -o $OUT/smclog_check.elf $OUT/smclog.o $OUT/smclog_check.o \
        --plugin-opt=mcpu=r3 --plugin-opt=-enable-ipra=false --gc-sections --build-id=sha1 \
        -e main -Lfirmware/lib/smclog -T firmware/tests/device/check.ld \$L/libc.a \$L/libcompiler-rt.a
    \$T/objdump -h $OUT/smclog_check.elf > $OUT/smclog_check.sections
    \$T/objdump -d $OUT/smclog_check.elf > $OUT/smclog_check.lst
    I='-Ifirmware/core -Ifirmware/input -Ifirmware/config -Ifirmware/link -Ifirmware/store -Ifirmware/midi'
    for f in core/core core/core_names core/core_config_factory input/scan input/evq input/inject config/cfgbin link/cfglink store/cfgstore store/statestore store/bootsel midi/blemidi midi/bleq; do
        b=\$(basename \$f)
        \$T/clang \$CF \$I -c firmware/\$f.c -o $OUT/\$b.o
        \$T/clang \${CF/-flto /} \$I -c firmware/\$f.c -o $OUT/\$b.nolto.o
        \$T/objdump -h $OUT/\$b.nolto.o > $OUT/\$b.nolto.sections
    done
    \$T/clang \$CF \$I -c firmware/tests/device/core_check.c -o $OUT/core_check.o
    \$T/lto-wrapper -o $OUT/core_check.elf $OUT/smclog.o $OUT/core.o $OUT/core_names.o \
        $OUT/core_config_factory.o $OUT/scan.o $OUT/evq.o $OUT/inject.o $OUT/cfgbin.o $OUT/cfglink.o $OUT/cfgstore.o $OUT/statestore.o \
        $OUT/core_check.o \
        --plugin-opt=mcpu=r3 --plugin-opt=-enable-ipra=false --gc-sections --build-id=sha1 \
        -e main -Lfirmware/lib/smclog -T firmware/tests/device/check.ld \$L/libc.a \$L/libcompiler-rt.a
    \$T/objdump -h $OUT/core_check.elf > $OUT/core_check.sections
    \$T/objdump -d $OUT/core_check.elf > $OUT/core_check.lst
    \$T/nm -S $OUT/core_check.elf > $OUT/core_check.syms"
python3 -I - "$ROOT/tools/smclog" "$ROOT/$OUT" <<'EOF'
import re, sys
sys.path.insert(0, sys.argv[1])
import smclog
out = sys.argv[2]
d = smclog.make_dictionary(out + "/smclog_check.elf")
ok = True
def check(cond, what):
    global ok
    print("  %s %s" % ("ok  " if cond else "FAIL", what))
    ok = ok and cond
sites = {int(k) for k in d["sites"]}
check(len(sites) == 3, "3 call sites in the dictionary (%d)" % len(sites))
check(d["section"] == "not loaded", "smclog section is not loaded (%s)" % d["section"])
check(d["build_id"] is not None and len(d["build_id"]) == 40, "SHA-1 build ID %s" % d["build_id"])
lst = open(out + "/smclog_check.lst").read()
passed = {int(v) for v in re.findall(r"r0 = (\d+) <\w+\.smclog_site_(?:\.\d+)? :", lst)}
check(passed == {s * 4 for s in sites}, "call sites pass the dictionary offsets %s" % sorted(passed))
sec = open(out + "/smclog_check.sections").read()
m = re.search(r"\.note\.gnu\.build-id\s+[0-9a-f]+\s+([0-9a-f]+)", sec)
check(m is not None and 0x1E00000 <= int(m.group(1), 16) < 0x1F00000, "build-id note in flash")

# the control core
d = smclog.make_dictionary(out + "/core_check.elf")
sites = {int(k) for k in d["sites"]}
for src in ("core/core.c", "input/scan.c", "input/inject.c", "config/cfgbin.c", "link/cfglink.c",
            "store/cfgstore.c", "store/statestore.c"):
    file_sites = {k for k, v in d["sites"].items() if v["file"].endswith(src)}
    in_source = len(re.findall(r"\bLOGC?_(?:ERROR|WARN|INFO|DEBUG|TRACE)\(",
                               open(out + "/../../firmware/" + src).read()))
    check(len(file_sites) == in_source, "%s: all %d call sites in the dictionary (%d)" % (
        src, in_source, len(file_sites)))
lst = open(out + "/core_check.lst").read()
passed = {int(v) for v, a in re.findall(r"= (\d+) <\w+\.smclog_site_(?:\.\d+)? : ([0-9a-f]+) >", lst)
          if int(v) == int(a, 16)}  # other constants get symbolized too
check(passed and passed <= {s * 4 for s in sites},
      "core + input + cfgbin + cfglink + cfgstore: %d call sites passed from code, all in the dictionary" % len(passed))
def section_sizes(path):
    sizes = {}
    for m in re.finditer(r"^\s*\d+\s+(\S+)\s+([0-9a-f]{8})\s", open(path).read(), re.M):
        sizes[m.group(1)] = sizes.get(m.group(1), 0) + int(m.group(2), 16)
    return sizes
for f in ("core", "core_names", "core_config_factory", "scan", "evq", "inject", "cfgbin", "cfglink",
          "cfgstore", "statestore", "bootsel", "blemidi", "bleq"):
    z = section_sizes("%s/%s.nolto.sections" % (out, f))
    code = sum(v for k, v in z.items() if k.startswith((".text", ".rodata")))
    print("  size %-17s code+rodata %6d bytes (-Oz, no LTO), data %d, bss %d" % (
        f, code, sum(v for k, v in z.items() if k.startswith(".data")),
        sum(v for k, v in z.items() if k.startswith(".bss"))))
syms = {}
for line in open(out + "/core_check.syms"):
    p = line.split()
    if len(p) == 4:
        syms[p[3]] = int(p[1], 16)
check("core" in syms and "config" in syms, "core: struct core %s bytes, struct core_config %s bytes (RAM)" % (
    syms.get("core"), syms.get("config")))
check("scan" in syms and "evq" in syms, "input: struct scan %s bytes, struct evq %s bytes (RAM)" % (
    syms.get("scan"), syms.get("evq")))
z = section_sizes(out + "/core_check.sections")
print("  image core_check.elf: .text %d, .data %d, .bss %d bytes (core + input + cfgbin + cfglink + cfgstore + smclog + factory configuration)" % (
    z.get(".text", 0), z.get(".data", 0), z.get(".bss", 0)))
print("check_device: %s" % ("all ok" if ok else "FAILED"))
sys.exit(0 if ok else 1)
EOF
} 2>&1 | tee "$LOG"
exit "${PIPESTATUS[0]}"
