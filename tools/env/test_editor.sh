#!/usr/bin/env bash
# The editor's contract tests (docs/integration.md, T-1) in the host test image's Node.js (docker/Dockerfile.host), on any OS
# that runs Docker: the editor's own modules (app/editor/js) against the configuration files
# and the shared vectors the firmware's side wrote (tools/editortest/vectors.py).
#
#   tools/env/test_editor.sh [-v]        make quick and make test run it
#
# Without Docker, or with an image that has no Node yet, it says so and exits 0 with SKIPPED:
# the editor's side did not run. SMC_EDITOR_TESTS=required makes that an error (exit 2).
# Only the repository is mounted (no deps/): the tests need nothing from JieLi.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
skip() {
    echo "SKIPPED: the editor's contract tests did not run: $1" >&2
    [ "${SMC_EDITOR_TESTS:-}" = required ] && exit 2
    exit 0
}

command -v docker >/dev/null 2>&1 || skip "no docker command (docs/build.md)"
docker info >/dev/null 2>&1 || skip "Docker is not running"

# The host test image (no emulation): the tag follows its Dockerfile.
IMAGE=$("$ROOT/tools/env/host_image.sh") || skip "the host test image did not build (docker/Dockerfile.host)"

run() {
    docker run --rm --user "$(id -u):$(id -g)" -v "$ROOT:/src:ro" -w /src \
        "$IMAGE" "$@"
}
run sh -c 'command -v node >/dev/null' || skip "the image has no Node.js (docker/Dockerfile.host)"
# --experimental-vm-modules: the syntax check of every module in one process (one node start)
run node --experimental-vm-modules --no-warnings tools/editortest/run.mjs "$@"
# Every host-only editor regression (*.test.mjs); the glob is expanded against the repository root.
UNIT_TESTS=()
for t in "$ROOT"/tools/editortest/*.test.mjs; do UNIT_TESTS+=("tools/editortest/${t##*/}"); done
run node --test tools/editortest/cycle-observer.mjs "${UNIT_TESTS[@]}" tools/fwcycle/test_cdp.mjs tools/fwcycle/test_bridge.mjs tools/fwcycle/test_editor.mjs tools/fwcycle/test_compact.mjs
# Seeded agreement includes malformed binaries and live edits in C, Python and the editor.
python3 -I "$ROOT/tools/editortest/test_differential.py"
python3 -I "$ROOT/tools/editortest/differential.py" --models 4 --mutations 64 --pairs 2
