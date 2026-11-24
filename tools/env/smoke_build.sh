#!/usr/bin/env bash
# Build-environment check: compile and link an unmodified AC63 SDK example for BR25 (ac636n_hid)
# and cut app.bin from it, using the toolchain and post-build tools from deps/.
#
#   tools/env/smoke_build.sh       log: logs/env/smoke-build.log, output: build/sdk-smoke/
#
# The SDK builds in its own tree (objs/, generated sdk.ld, cpu/br25/tools/sdk.elf), so the build
# runs in a copy (APFS clone on macOS, cheap) and deps/fw-AC63_BT_SDK stays pristine.
# The SDK's own Linux post-build (cpu/br25/tools/download.sh) ends in strip-ini and host-client,
# which the published post-build tools do not include; app.bin is cut here with the same steps
# instead. Nothing is packed into a flashable image and nothing talks to a device.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS="${DEPS:-$ROOT/deps}"
WORK="$ROOT/build/sdk-smoke"
LOG="$ROOT/logs/env/smoke-build.log"
mkdir -p "$ROOT/build" "$(dirname "$LOG")"

[ -d "$DEPS/fw-AC63_BT_SDK/apps" ] || { echo "smoke_build: no SDK; run tools/env/fetch_deps.sh" >&2; exit 1; }
rm -rf "$WORK"
cp -cR "$DEPS/fw-AC63_BT_SDK" "$WORK" 2>/dev/null || cp -a "$DEPS/fw-AC63_BT_SDK" "$WORK"
rm -rf "$WORK/.git"

{
    echo "smoke_build: $(date -u +%Y-%m-%dT%H:%M:%SZ), SDK $(git -C "$DEPS/fw-AC63_BT_SDK" rev-parse HEAD)"
    start=$(date +%s)
    "$ROOT/tools/env/run.sh" bash -c '
        set -euo pipefail
        T=/opt/jieli/pi32v2/bin
        cd /src/build/sdk-smoke/apps/hid/board/br25
        echo "+ make pre_build"
        make pre_build
        # The default target runs the post-build step, which races the link under -j
        # and needs tools that are not published: link sdk.elf on its own.
        echo "+ make -j$(nproc) sdk.elf"
        make -j"$(nproc)" ../../../../cpu/br25/tools/sdk.elf 2>&1 | grep -v "stack size limit" || true
        cd ../../../../cpu/br25/tools
        test -s sdk.elf
        $T/objdump -section-headers -address-mask=0x1ffffff sdk.elf
        # app.bin as cpu/br25/tools/download.sh builds it: .text, .data, .data_code, then each
        # overlay with its trailing zero bytes removed (an overlay starts with an FF FF FF FF
        # marker, then zero-filled space; -mark ff keeps the marker).
        W=$(mktemp -d)
        for s in text data data_code; do $T/objcopy -O binary -j .$s sdk.elf $W/$s.bin; done
        parts="$W/text.bin $W/data.bin $W/data_code.bin"
        for s in aec wav ape flac m4a amr dts fm mp3 wma; do
            $T/objcopy -O binary -j .overlay_$s sdk.elf $W/${s}o.bin
            /opt/utils/remove_tailing_zeros -i $W/${s}o.bin -o $W/$s.bin -mark ff >/dev/null
            parts="$parts $W/$s.bin"
        done
        cat $parts > app.bin
        ls -l $W | sed "s/^/  /"
        echo "sdk.elf $(stat -c %s sdk.elf) bytes, app.bin $(stat -c %s app.bin) bytes"
        $T/nm sdk.elf | grep -E " _start$" || true
    '
    echo "smoke_build: ok in $(( $(date +%s) - start )) s"
} 2>&1 | tee "$LOG"
