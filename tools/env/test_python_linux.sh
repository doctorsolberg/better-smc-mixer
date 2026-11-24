#!/usr/bin/env bash
# The Python tools' tests on Linux, in the host test image's Python (Debian's python3; docker/Dockerfile.host), on any OS that
# runs Docker (docs/integration.md, "Any OS"):
#
#   tools/env/test_python_linux.sh          make linux-py runs it
#
# Every suite that is plain Python runs. The suites that drive the host C programs (build/host:
# devhost, smcsim, cfgtool, the C tests) are left out: build/host holds this machine's builds, and
# tools/env/test_linux.sh (make linux) builds them in the image and runs every suite. Without Docker it says SKIPPED.
# Nothing here talks to a device.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
skip() { echo "SKIPPED: the Python tests on Linux did not run: $1" >&2; exit 0; }
command -v docker >/dev/null 2>&1 || skip "no docker command (docs/build.md)"
docker info >/dev/null 2>&1 || skip "Docker is not running"
IMAGE=$("$ROOT/tools/env/host_image.sh")

# plain Python: no program from build/host
SUITES="tools/smcconf/gen.py tools/hostmidi/test_hostmidi.py tools/hostserial/test_hostserial.py
tools/image/test_flashimg.py tools/image/test_installplan.py tools/jlrom/test_compare.py
tools/re/test_rampath.py tools/stage0/test_capture_report.py tools/usbkey/test_uf2.py
tools/release/test_check_public.py tools/release/test_check_docs.py tools/release/check_versions.py
tools/release/test_check_versions.py tools/release/test_release.py"

docker run --rm --user "$(id -u):$(id -g)" -e PYTHONDONTWRITEBYTECODE=1 \
    -v "$ROOT:/src:ro" -w /src "$IMAGE" sh -c '
    echo "Python on Linux: $(python3 --version)"
    failed=0
    for t in '"$(echo $SUITES)"'; do
        a=""; [ "$t" = tools/smcconf/gen.py ] && a=--check
        if out=$(python3 -I "$t" $a 2>&1); then echo "ok   $t"; else echo "FAIL $t"; echo "$out" | tail -20; failed=$((failed+1)); fi
    done
    echo "Python on Linux: $failed failed"
    exit $failed'
