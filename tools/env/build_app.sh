#!/usr/bin/env bash
# Build the SMC-Mixer application (firmware/app) against the pinned AC63 SDK in the container.
#
#   tools/env/build_app.sh [make args]                 the controller: build/app/,
#                                                      log: logs/build/app.log
#   VARIANT=hwtest tools/env/build_app.sh [make args]  the hardware test (docs/hwtest.md):
#                                                      build/hwtest/, logs/build/hwtest.log
#   BOOTMARK=1 ...                                     either, with the boot marks of a diagnosis
#                                                      (firmware/platform/br25/bootmark_br25.h):
#                                                      build/<variant>-bootmark/. It writes flash
#                                                      at every start: for a diagnosis only
#   BLE=1 tools/env/build_app.sh [make args]           the controller with BLE-MIDI (docs/ble.md):
#                                                      build/app-ble/
#
# The SDK tree in deps/ is only read: generated files (sdk.ld, sdk_used_list.used), objects and
# outputs go to build/app/. Outputs: app.elf, app.bin (cut as the SDK's download.sh does), the
# log dictionary app.smclog.json (LOG-22, kept with the binary), app.map and the disassembly
# app.lst. Nothing is packed into a flashable image and nothing talks to a device.
#
# The scan timer check: TIMER3 (docs/hwtest.md) must be ours alone: in both variants only our scan
# code may load its register address (the scan interrupt, its start and the µs clock that reads
# it). A register reached by index from another timer's address would escape this check.
#
# The scan interrupt's path (docs/hwtest.md, "While flash is written"): it runs while the SDK writes
# flash, so tools/re/rampath.py follows it on app.lst: only code in RAM and the ROM functions named
# below, no flash address loaded, no RTOS or SDK lock function.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SDK=deps/fw-AC63_BT_SDK
VARIANT="${VARIANT:-app}"
case "$VARIANT" in app|hwtest) ;; *) echo "build_app: VARIANT must be app or hwtest" >&2; exit 2;; esac
BOOTMARK="${BOOTMARK:-}"
case "$BOOTMARK" in ""|1) ;; *) echo "build_app: BOOTMARK must be 1 or unset" >&2; exit 2;; esac
BLE="${BLE:-}"
case "$BLE" in ""|1) ;; *) echo "build_app: BLE must be 1 or unset" >&2; exit 2;; esac
NAME=$VARIANT${BOOTMARK:+-bootmark}${BLE:+-ble}
OUT=build/$NAME
LOG="$ROOT/logs/build/$NAME.log"
mkdir -p "$ROOT/$OUT" "$(dirname "$LOG")"
[ -d "$ROOT/$SDK/apps" ] || { echo "build_app: no SDK; run tools/env/fetch_deps.sh" >&2; exit 1; }

{
echo "build_app: $(date -u +%Y-%m-%dT%H:%M:%SZ), variant $VARIANT, SDK $(git -C "$ROOT/$SDK" rev-parse HEAD)"
start=$(date +%s)
"$ROOT/tools/env/run.sh" bash -c "
    set -euo pipefail
    T=/opt/jieli/pi32v2/bin
    make -C /src/firmware/app -j\$(nproc) SDK=/src/$SDK VARIANT=$VARIANT BOOTMARK=$BOOTMARK BLE=$BLE OUT=/src/$OUT $*
    cd /src/$OUT
    \$T/objdump -section-headers -address-mask=0x1ffffff app.elf | tee app.sections
    # app.bin as cpu/br25/tools/download.sh builds it (see tools/env/smoke_build.sh)
    W=\$(mktemp -d)
    for s in text data data_code; do \$T/objcopy -O binary -j .\$s app.elf \$W/\$s.bin; done
    parts=\"\$W/text.bin \$W/data.bin \$W/data_code.bin\"
    for s in aec wav ape flac m4a amr dts fm mp3 wma; do
        \$T/objcopy -O binary -j .overlay_\$s app.elf \$W/\${s}o.bin
        /opt/utils/remove_tailing_zeros -i \$W/\${s}o.bin -o \$W/\$s.bin -mark ff >/dev/null 2>&1
        parts=\"\$parts \$W/\$s.bin\"
    done
    cat \$parts > app.bin
    echo \"app.elf \$(stat -c %s app.elf) bytes, app.bin \$(stat -c %s app.bin) bytes\"
    \$T/nm app.elf > app.nm
    grep -E ' (_start|__build_id_note|__start_smclog|__stop_smclog)\$' app.nm || true
    \$T/objdump -d app.elf > app.lst
    grep -E 'smclog_site_' app.lst > app.sites || true
"
python3 -I "$ROOT/tools/smclog/smclog.py" dict "$ROOT/$OUT/app.elf" -o "$ROOT/$OUT/app.smclog.json"
# The call sites must pass the offsets the dictionary has (as in tools/env/check_device.sh).
python3 -I - "$ROOT/$OUT" <<'PY'
import json, re, sys
out = sys.argv[1]
with open(out + "/app.smclog.json") as f:
    sites = {int(k) * 4 for k in json.load(f)["sites"]}
with open(out + "/app.sites") as f:
    passed = {int(v) for v, a in re.findall(r"= (\d+) <\w+\.smclog_site_(?:\.\d+)? : ([0-9a-f]+) >", f.read())
              if int(v) == int(a, 16)}  # other constants get symbolized too
if not passed or not passed <= sites:
    sys.exit("build_app: call sites pass %s, the dictionary has %s" % (sorted(passed), sorted(sites)))
print("build_app: %d call sites, %d referenced from code, all in the dictionary" % (len(sites), len(passed)))
PY
# The RAM a reset keeps (.smc_keep, firmware/platform/br25/diag_br25.c): after .bss and the RAM
# code, before the heap.
python3 -I - "$ROOT/$OUT/app.nm" <<'PY'
import sys
syms = {}
with open(sys.argv[1]) as f:
    for line in f:
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16)
k0, k1 = syms["smc_keep_begin"], syms["smc_keep_end"]
if not (syms["bss_end"] <= syms["data_code_limit_end"] <= k0 < k1 <= syms["_HEAP_BEGIN"] < syms["_HEAP_END"]):
    sys.exit("build_app: .smc_keep 0x%x-0x%x is not between the RAM code (to 0x%x) and the heap (from 0x%x)"
             % (k0, k1, syms["data_code_limit_end"], syms["_HEAP_BEGIN"]))
print("build_app: kept across a reset 0x%x-0x%x (%d bytes); heap 0x%x-0x%x (%d bytes)" % (
    k0, k1, k1 - k0, syms["_HEAP_BEGIN"], syms["_HEAP_END"], syms["_HEAP_END"] - syms["_HEAP_BEGIN"]))
PY
# TIMER3, the scan timer: who loads its register block's address (0x1E0700).
python3 -I - "$ROOT/$OUT/app.lst" "$VARIANT" <<'PY'
import re, sys
lst, variant = sys.argv[1], sys.argv[2]
func, users = None, set()
with open(lst) as f:
    for line in f:
        m = re.match(r"^([A-Za-z_$][\w.$]*):$", line)  # not .local labels inside a function
        if m:
            func = m.group(1)
        elif re.search(r"= 1967872 <", line):
            users.add(func)
ours = {"scan_isr", "input_br25_clock_start", "input_br25_start", "input_br25_time_us"}
if not users or not users <= ours:
    sys.exit("build_app: TIMER3 loaded by %s, expected only %s (%s)" % (sorted(users), sorted(ours), variant))
print("build_app: TIMER3 loaded by %s" % (", ".join(sorted(users)) or "nothing"))
PY
# The scan's sink is called through a pointer (scan.c's send()): the variant's.
case "$VARIANT" in app) SINK=sink;; hwtest) SINK=evq_sink;; esac
python3 -I "$ROOT/tools/re/rampath.py" "$ROOT/$OUT/app.lst" --root scan_isr --indirect "send=$SINK" \
    --rom-ok memcpy,memset,xTaskGetCurrentTaskHandle,chip_reset --forbid 'os_*' --forbid 'local_irq_*' --quiet
if [ -n "$(git -C "$ROOT/$SDK" status --porcelain)" ]; then
    echo "build_app: WARNING: the SDK tree in $SDK was modified"
fi
echo "build_app: ok in $(( $(date +%s) - start )) s"
} 2>&1 | tee "$LOG"
exit "${PIPESTATUS[0]}"
