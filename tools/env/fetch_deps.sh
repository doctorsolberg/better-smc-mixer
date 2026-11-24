#!/usr/bin/env bash
# Fetch the third-party build dependencies into deps/ (kept out of git, never redistributed).
#
#   tools/env/fetch_deps.sh            fetch whatever is missing, verify everything
#   DEPS=/path tools/env/fetch_deps.sh  use another directory
#
# What it fetches (pinned; see docs/build.md):
#   deps/jieli-toolchain/   JieLi Linux toolchain (clang for pi32v2), SHA-256 checked
#   deps/jieli-post-build/  JieLi Linux post-build tools (image packing), SHA-256 checked
#   deps/fw-AC63_BT_SDK/    JieLi AC63 SDK, one pinned commit, shallow
#
# The JieLi archives are licensed for use, not for redistribution: they stay in deps/.
# Archives are listed before extraction and refused if an entry is absolute or contains "..".
# Each step is idempotent: a dependency whose stamp matches its pin is not fetched again.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS="${DEPS:-$ROOT/deps}"

TOOLCHAIN_URL=https://jl-update.oss-cn-shenzhen.aliyuncs.com/jieli-linux-toolchains-20260730.1.tar.xz
TOOLCHAIN_SHA256=a1545cddbc451a06bac43eb5cd96d831470c42a97c5c0b1f98879a20fc7647bd
POSTBUILD_URL=https://jl-update.oss-cn-shenzhen.aliyuncs.com/jieli-linux-post-build-tools-20260923.1.tar.xz
POSTBUILD_SHA256=6949d48c4cd82e274d7943d4ca67f214593fdb4f699ef9f51c2ecc78125ab739
SDK_REPO=https://github.com/Jieli-Tech/fw-AC63_BT_SDK.git
SDK_COMMIT=fdc018de81629e21e594d677cf05f87d288be0f0

log() { printf '%s fetch_deps: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*"; }
die() { log "ERROR: $*"; exit 1; }

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1
    else shasum -a 256 "$1" | cut -d' ' -f1; fi
}

# fetch_archive NAME URL SHA256 -> $DEPS/NAME holds the extracted archive
fetch_archive() {
    local name=$1 url=$2 want=$3
    local dest="$DEPS/$name" file="$DEPS/downloads/$(basename "$url")"
    if [ -f "$dest/.fetched" ] && [ "$(cat "$dest/.fetched")" = "$want" ]; then
        log "$name: present ($(basename "$url"))"
        return
    fi
    mkdir -p "$DEPS/downloads"
    if [ ! -f "$file" ] || [ "$(sha256 "$file")" != "$want" ]; then
        log "$name: downloading $url"
        rm -f "$file.part"
        curl --fail --location --proto '=https' --tlsv1.2 --retry 3 --silent --show-error \
            --output "$file.part" "$url"
        mv "$file.part" "$file"
    fi
    local got size
    got=$(sha256 "$file")
    size=$(wc -c < "$file" | tr -d ' ')
    [ "$got" = "$want" ] || die "$name: SHA-256 mismatch: got $got, want $want ($file kept for inspection)"
    log "$name: $size bytes, SHA-256 ok"
    if tar -tJf "$file" | grep -Eq '^/|(^|/)\.\.(/|$)'; then
        die "$name: archive has absolute or .. entries, refusing to extract"
    fi
    rm -rf "$dest.tmp" "$dest"
    mkdir -p "$dest.tmp"
    tar -xJf "$file" -C "$dest.tmp" --no-same-owner
    printf '%s\n' "$want" > "$dest.tmp/.fetched"
    mv "$dest.tmp" "$dest"
    log "$name: extracted to $dest ($(find "$dest" -type f | wc -l | tr -d ' ') files)"
}

fetch_sdk() {
    local dest="$DEPS/fw-AC63_BT_SDK"
    if [ -d "$dest/.git" ] && [ "$(git -C "$dest" rev-parse HEAD 2>/dev/null)" = "$SDK_COMMIT" ]; then
        if [ -z "$(git -C "$dest" status --porcelain --untracked-files=no)" ]; then
            log "sdk: present at $SDK_COMMIT"
        else
            log "sdk: present at $SDK_COMMIT, WARNING: tracked files are modified"
        fi
        return
    fi
    log "sdk: cloning $SDK_REPO at $SDK_COMMIT (shallow)"
    rm -rf "$dest.tmp"
    git init --quiet "$dest.tmp"
    git -C "$dest.tmp" remote add origin "$SDK_REPO"
    git -C "$dest.tmp" -c protocol.version=2 fetch --quiet --depth 1 origin "$SDK_COMMIT"
    git -C "$dest.tmp" -c advice.detachedHead=false checkout --quiet --detach FETCH_HEAD
    [ "$(git -C "$dest.tmp" rev-parse HEAD)" = "$SDK_COMMIT" ] || die "sdk: checked out the wrong commit"
    rm -rf "$dest"
    mv "$dest.tmp" "$dest"
    log "sdk: $(du -sh "$dest" | cut -f1) at $dest"
}

mkdir -p "$DEPS"
log "deps directory: $DEPS"
case "${1:-all}" in
    all) fetch_archive jieli-toolchain "$TOOLCHAIN_URL" "$TOOLCHAIN_SHA256"
         fetch_archive jieli-post-build "$POSTBUILD_URL" "$POSTBUILD_SHA256"
         fetch_sdk ;;
    jieli) fetch_archive jieli-toolchain "$TOOLCHAIN_URL" "$TOOLCHAIN_SHA256"
           fetch_archive jieli-post-build "$POSTBUILD_URL" "$POSTBUILD_SHA256" ;;
    sdk) fetch_sdk ;;
    *) die "unknown target '$1' (all, jieli, sdk)" ;;
esac
log "done"
