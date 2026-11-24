#!/usr/bin/env bash
# Run a command in the build container with the dependencies mounted.
#
#   tools/env/run.sh CMD [ARGS...]     e.g. tools/env/run.sh /opt/jieli/pi32v2/bin/clang --version
#   tools/env/run.sh                   interactive shell
#
# Mounts (see docs/build.md):
#   deps/jieli-toolchain/<version>   -> /opt/jieli  (read-only)
#   deps/jieli-post-build/<version>  -> /opt/utils  (read-only)
#   the repository                   -> /src
# The container runs as the calling user, so files it writes in /src belong to them.
# EXTRA_DOCKER_ARGS adds docker run arguments (more mounts, -e VAR=...).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DEPS="${DEPS:-$ROOT/deps}"
# The tag follows the Dockerfile, so a changed Dockerfile builds a new image.
F="$ROOT/docker/Dockerfile.build"
sum=$( (command -v sha256sum >/dev/null && sha256sum "$F") || shasum -a 256 "$F")
IMAGE=better-smc-mixer-build:$(printf '%s' "$sum" | cut -c1-12)

one_dir() {  # the single versioned directory an archive extracted to
    local found
    found=$(find "$1" -mindepth 1 -maxdepth 1 -type d 2>/dev/null)
    [ -n "$found" ] && [ "$(printf '%s\n' "$found" | wc -l)" -eq 1 ] || {
        echo "run.sh: expected one directory in $1; run tools/env/fetch_deps.sh" >&2; exit 1; }
    printf '%s\n' "$found"
}
TOOLCHAIN=$(one_dir "$DEPS/jieli-toolchain")
POSTBUILD=$(one_dir "$DEPS/jieli-post-build")

docker image inspect "$IMAGE" >/dev/null 2>&1 || \
    docker build --platform linux/amd64 -t "$IMAGE" -f "$ROOT/docker/Dockerfile.build" "$ROOT/docker"

tty=""
[ -t 0 ] && [ -t 1 ] && tty=-it
[ $# -eq 0 ] && set -- bash
# shellcheck disable=SC2086
exec docker run --rm $tty --platform linux/amd64 \
    --user "$(id -u):$(id -g)" --ulimit nofile=8192:8192 \
    -v "$TOOLCHAIN:/opt/jieli:ro" -v "$POSTBUILD:/opt/utils:ro" -v "$ROOT:/src" \
    -w /src ${EXTRA_DOCKER_ARGS:-} "$IMAGE" "$@"
