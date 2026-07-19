#!/usr/bin/env bash
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
image=${GNW_HW_IMAGE:-gnw-hw-harness:1}

if [[ ${1:-} == --build ]]; then
    docker build -f "$here/Dockerfile" -t "$image" "$repo"
    shift
fi

exec docker run --rm --network none \
    --user "$(id -u):$(id -g)" \
    -v "$repo:/opt/workdir" \
    -w /opt/workdir \
    "$image" "$@"
