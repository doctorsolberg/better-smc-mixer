#!/usr/bin/env bash
# Every host test on Linux, in the host test image (docker/Dockerfile.host: the machine's own
# platform, no emulation), on any OS that runs Docker (docs/integration.md,
# "Any OS"; the owner put gcc in the image on 2026-11-14):
#
#   tools/env/test_linux.sh [quick]         make linux runs it (quick: make -C firmware quick only)
#
# The image's gcc builds the host programs (the C tests, smcsim, devhost, cfgtool) into its own
# build/host, from a copy of the sources inside the container (the repository is mounted read-only;
# build/, deps/, materials/ and logs/ are not copied), then runs what `make test` runs: the C
# tests, the scenarios, the Python tools' tests, the fuzzer's short run, the editor's contract
# tests and the editor live against devhost, all in the image's Node.js and Python. The macOS-only
# tools (jlrom, smcmidi) are not part of it. Without Docker it says SKIPPED. Nothing here talks to
# a device.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
skip() { echo "SKIPPED: the host tests on Linux did not run: $1" >&2; exit 0; }
command -v docker >/dev/null 2>&1 || skip "no docker command (docs/build.md)"
docker info >/dev/null 2>&1 || skip "Docker is not running"
IMAGE=$("$ROOT/tools/env/host_image.sh")

target=test
[ "${1:-}" = quick ] && target=quick

docker run --rm --user "$(id -u):$(id -g)" -e PYTHONDONTWRITEBYTECODE=1 \
    -e HOME=/tmp -v "$ROOT:/src:ro" -w /tmp "$IMAGE" sh -c '
    set -e
    command -v cc >/dev/null || { echo "test_linux.sh: the image has no gcc (docker/Dockerfile.host)"; exit 2; }
    echo "Linux $(uname -m): $(cc --version | head -1); $(python3 --version); Node $(node --version)"
    mkdir w
    tar -C /src --exclude=./build --exclude=./deps --exclude=./materials --exclude=./logs -cf - . | tar -C w -xf -
    cd w
    make -C firmware -j"$(nproc)" '"$target"'
    node --experimental-vm-modules --no-warnings tools/editortest/run.mjs
    node --test tools/editortest/cycle-observer.mjs tools/editortest/*.test.mjs tools/fwcycle/test_cdp.mjs tools/fwcycle/test_bridge.mjs tools/fwcycle/test_editor.mjs tools/fwcycle/test_compact.mjs
    SMC_NODE=node SMC_EDITOR_TESTS=required python3 -I tools/editortest/test_differential.py
    SMC_NODE=node SMC_EDITOR_TESTS=required python3 -I tools/editortest/differential.py --models 4 --mutations 64 --pairs 2
    if [ '"$target"' = test ]; then SMC_NODE=node SMC_EDITOR_TESTS=required python3 -I tools/editortest/live.py; fi
    echo "Linux: every host test passed"'
