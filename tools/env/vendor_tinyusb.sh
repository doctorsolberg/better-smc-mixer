#!/usr/bin/env bash
# Refresh or check the frozen TinyUSB copy in third_party/tinyusb (SPEC C-4).
#
#   tools/env/vendor_tinyusb.sh check    verify third_party/tinyusb against its MANIFEST (default)
#   tools/env/vendor_tinyusb.sh update   fetch the pinned commit and copy the needed files again
#
# Only the files the firmware needs are kept: the device core, the MIDI, CDC and DFU classes,
# the common code, the no-OS abstraction and the license. The BR25 device controller driver is our
# own code (firmware/platform), not part of this copy. The copy is not modified; MANIFEST holds the
# SHA-256 of every file, so a local change shows up in "check".
# "update" fetches with a partial, sparse clone (only src/ and LICENSE, about 5 MB) into
# deps/tinyusb-upstream; git checks the commit hash.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS="${DEPS:-$ROOT/deps}"
DEST="$ROOT/third_party/tinyusb"

TUSB_REPO=https://github.com/hathach/tinyusb.git
TUSB_TAG=0.21.0
TUSB_COMMIT=dae3f9a366bfcddbf9dcf1b48d7500286a849539

FILES="
LICENSE
src/tusb.c
src/tusb.h
src/tusb_option.h
src/common/tusb_common.h
src/common/tusb_compiler.h
src/common/tusb_debug.h
src/common/tusb_fifo.c
src/common/tusb_fifo.h
src/common/tusb_mcu.h
src/common/tusb_private.h
src/common/tusb_types.h
src/common/tusb_verify.h
src/osal/osal.h
src/osal/osal_none.h
src/device/dcd.h
src/device/usbd.c
src/device/usbd.h
src/device/usbd_pvt.h
src/class/audio/audio.h
src/class/midi/midi.h
src/class/midi/midi_device.c
src/class/midi/midi_device.h
src/class/cdc/cdc.h
src/class/cdc/cdc_device.c
src/class/cdc/cdc_device.h
src/class/dfu/dfu.h
src/class/dfu/dfu_device.c
src/class/dfu/dfu_device.h
src/class/dfu/dfu_rt_device.c
src/class/dfu/dfu_rt_device.h
"

log() { printf '%s vendor_tinyusb: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }
die() { log "ERROR: $*"; exit 1; }

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

fetch() {
    local up="$DEPS/tinyusb-upstream"
    if [ -d "$up/.git" ] && [ "$(git -C "$up" rev-parse HEAD 2>/dev/null)" = "$TUSB_COMMIT" ]; then
        log "upstream: present at $TUSB_COMMIT"
        return
    fi
    log "upstream: fetching $TUSB_REPO at $TUSB_COMMIT ($TUSB_TAG), src/ and LICENSE only"
    rm -rf "$up.tmp"
    git init --quiet "$up.tmp"
    git -C "$up.tmp" remote add origin "$TUSB_REPO"
    git -C "$up.tmp" config core.sparseCheckout true
    printf '/src/\n/LICENSE\n' > "$up.tmp/.git/info/sparse-checkout"
    git -C "$up.tmp" -c protocol.version=2 fetch --quiet --depth 1 --filter=blob:none origin "$TUSB_COMMIT"
    git -C "$up.tmp" -c advice.detachedHead=false checkout --quiet --detach FETCH_HEAD
    [ "$(git -C "$up.tmp" rev-parse HEAD)" = "$TUSB_COMMIT" ] || die "upstream: checked out the wrong commit"
    rm -rf "$up"
    mv "$up.tmp" "$up"
    log "upstream: $(du -sh "$up" | cut -f1) at $up"
}

update() {
    fetch
    local up="$DEPS/tinyusb-upstream" f
    rm -rf "$DEST.tmp"
    mkdir -p "$DEST.tmp"
    for f in $FILES; do
        [ -f "$up/$f" ] || die "missing upstream file $f"
        mkdir -p "$DEST.tmp/$(dirname "$f")"
        cp "$up/$f" "$DEST.tmp/$f"
    done
    {
        echo "# TinyUSB $TUSB_TAG ($TUSB_REPO, commit $TUSB_COMMIT), unmodified."
        echo "# Regenerate with tools/env/vendor_tinyusb.sh update; verify with tools/env/vendor_tinyusb.sh check."
        for f in $FILES; do printf '%s  %s\n' "$(sha256 "$DEST.tmp/$f")" "$f"; done
    } > "$DEST.tmp/MANIFEST"
    [ -f "$DEST/README.md" ] && cp "$DEST/README.md" "$DEST.tmp/README.md"
    rm -rf "$DEST"
    mv "$DEST.tmp" "$DEST"
    log "copied $(echo $FILES | wc -w | tr -d ' ') files into $DEST ($(du -sh "$DEST" | cut -f1))"
}

check() {
    [ -f "$DEST/MANIFEST" ] || die "no $DEST/MANIFEST (run: $0 update)"
    local bad=0 n=0 want f
    while read -r want f; do
        case "$want" in '#'*|'') continue ;; esac
        n=$((n + 1))
        if [ ! -f "$DEST/$f" ]; then log "missing: $f"; bad=1
        elif [ "$(sha256 "$DEST/$f")" != "$want" ]; then log "modified: $f"; bad=1; fi
    done < "$DEST/MANIFEST"
    local extra
    extra=$(cd "$DEST" && find . -type f ! -name MANIFEST ! -name README.md | sed 's#^\./##' | sort |
        while read -r f; do grep -q "  $f\$" MANIFEST || echo "$f"; done)
    [ -z "$extra" ] || { log "files not in MANIFEST: $extra"; bad=1; }
    [ "$bad" = 0 ] || die "third_party/tinyusb differs from TinyUSB $TUSB_TAG"
    log "third_party/tinyusb: $n files match TinyUSB $TUSB_TAG ($TUSB_COMMIT)"
}

case "${1:-check}" in
    check) check ;;
    update) update; check ;;
    *) die "unknown command '$1' (check, update)" ;;
esac
