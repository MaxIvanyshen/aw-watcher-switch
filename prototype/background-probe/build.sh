#!/bin/sh
# Builds dist/background-probe/sdcard/atmosphere/contents/4200000000415755/{exefs.nsp,flags/boot2.flag} in the pinned devkitPro image.
set -eu
IMAGE=devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282
TITLE=4200000000415755
if [ "${1:-}" != "--inside" ]; then
  root=$(cd "$(dirname "$0")/../.." && pwd)
  mkdir -p "$root/dist"
  exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$root":/src:ro -v "$root/dist":/dist "$IMAGE" sh /src/prototype/background-probe/build.sh --inside
fi
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"
rm -rf /tmp/w; cp -R /src/prototype/background-probe /tmp/w; cd /tmp/w
make background-probe.nsp > /tmp/make.log 2>&1 || { cat /tmp/make.log; exit 1; }
grep 'warning:' /tmp/make.log && exit 1
sh check-perms.sh probe.json background-probe.elf background-probe.nsp
out=/dist/background-probe/sdcard/atmosphere/contents/$TITLE
rm -rf /dist/background-probe; mkdir -p "$out/flags"
cp background-probe.nsp "$out/exefs.nsp"; : > "$out/flags/boot2.flag"
cp background-probe.elf /dist/background-probe/
aarch64-none-elf-size background-probe.elf
(cd /dist/background-probe && sha256sum sdcard/atmosphere/contents/$TITLE/exefs.nsp && wc -c < sdcard/atmosphere/contents/$TITLE/exefs.nsp)
