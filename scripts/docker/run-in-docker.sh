#!/usr/bin/env bash
# Build the test image (idempotent; cached after the first run) and run the
# project's test suite against the working tree inside it. The tree is mounted
# read-only and copied to a writable scratch dir inside the container, so the
# host repo is never modified (no stray node_modules/, build/, out/).

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
IMAGE_TAG="pprof-nodejs-test:latest"

if ! command -v docker >/dev/null 2>&1; then
    echo "docker not found in PATH; install Docker Desktop / colima / podman-with-docker-alias" >&2
    exit 1
fi

if ! docker info >/dev/null 2>&1; then
    echo "docker daemon not reachable; is it running?" >&2
    exit 1
fi

echo "==> building $IMAGE_TAG (cached after first run)"
docker build -q -t "$IMAGE_TAG" "$SCRIPT_DIR" >/dev/null

echo "==> running tests"
exec docker run --rm \
    -v "$REPO_DIR":/work:ro \
    "$IMAGE_TAG" \
    bash -c '
        set -euo pipefail
        cp -R /work/. /tmp/work/
        # Drop any host-built artifacts so we get a clean build inside.
        rm -rf /tmp/work/node_modules /tmp/work/build /tmp/work/out
        npm install --no-audit --no-fund
        npm test
    '
