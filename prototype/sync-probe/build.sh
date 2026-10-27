#!/bin/sh
# Builds dist/sync-probe.nro in the same pinned devkitPro image as nx/build.sh.
set -eu
IMAGE=devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282
root=$(cd "$(dirname "$0")/../.." && pwd)
mkdir -p "$root/dist"
exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$root":/src:ro -v "$root/dist":/dist "$IMAGE" sh -c '
set -eu
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"
rm -rf /tmp/w; cp -R /src/prototype/sync-probe /tmp/w; cd /tmp/w
make sync-probe.nro
cp sync-probe.nro /dist/sync-probe.nro
cp sync-probe.elf /dist/sync-probe.elf
aarch64-none-elf-nm sync-probe.elf | grep -E " (T|U) (bsd|socket|connect|poll|send|recv)" || true
aarch64-none-elf-nm sync-probe.elf | grep -E "bsdInitialize|socketInitialize"
'
