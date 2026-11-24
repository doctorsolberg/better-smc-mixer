#!/usr/bin/env bash
# The host test image's tag (docker/Dockerfile.host: gcc, Python, Node.js on the machine's own
# platform, no emulation; docs/build.md, "The host test image"), built first if it is missing:
#
#   IMAGE=$(tools/env/host_image.sh)          [--no-build: print the tag only if it exists]
#
# The tag carries a hash of the Dockerfile, so a changed Dockerfile builds a new image. Used by
# test_linux.sh, test_editor.sh, test_python_linux.sh and tools/editortest/live.py (which names
# the tag the same way). Exit 1 without Docker or when the image is missing with --no-build.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
F="$ROOT/docker/Dockerfile.host"
sum=$( (command -v sha256sum >/dev/null && sha256sum "$F") || shasum -a 256 "$F")
IMAGE=better-smc-mixer-host:$(printf '%s' "$sum" | cut -c1-12)
if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    [ "${1:-}" = --no-build ] && exit 1
    docker build -t "$IMAGE" -f "$F" "$ROOT/docker" >&2
fi
echo "$IMAGE"
