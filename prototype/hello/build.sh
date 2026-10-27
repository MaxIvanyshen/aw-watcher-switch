#!/bin/sh
# Builds dist/hello-probe.nro in the same pinned devkitPro image as nx/build.sh.
set -eu
IMAGE=devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282
root=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$root/dist"
exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$root":/src:ro -v "$root/dist":/dist "$IMAGE" sh -c '
set -eu
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"
rm -rf /tmp/w; cp -R /src/prototype/hello /tmp/w; cd /tmp/w
make hello-probe.nro
cp hello-probe.nro /dist/hello-probe.nro
cp hello-probe.elf /dist/hello-probe.elf
'
