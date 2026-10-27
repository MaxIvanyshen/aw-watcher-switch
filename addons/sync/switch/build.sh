#!/bin/sh
# Builds dist/addon-sync/sdcard/atmosphere/contents/4200000000415755/{exefs.nsp,flags/boot2.flag} in the pinned devkitPro image.
set -eu
IMAGE=devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282
TITLE=4200000000415755
if [ "${1:-}" != "--inside" ]; then
  root=$(cd "$(dirname "$0")/../../.." && pwd)
  mkdir -p "$root/dist"
  exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$root":/src:ro -v "$root/dist":/dist "$IMAGE" sh /src/addons/sync/switch/build.sh --inside
fi
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"
rm -rf /tmp/w; mkdir -p /tmp/w/addons /tmp/w/core
cp -R /src/addons/sync /tmp/w/addons/sync; cp -R /src/core/src /tmp/w/core/src
cd /tmp/w/addons/sync/switch
make aw-sync.nsp > /tmp/make.log 2>&1 || { cat /tmp/make.log; exit 1; }
if grep 'warning:' /tmp/make.log; then exit 1; fi
sh check-perms.sh sync.json aw-sync.elf aw-sync.nsp
out=/dist/addon-sync/sdcard/atmosphere/contents/$TITLE
rm -rf /dist/addon-sync; mkdir -p "$out/flags"
cp aw-sync.nsp "$out/exefs.nsp"; : > "$out/flags/boot2.flag"
cp aw-sync.elf /dist/addon-sync/
aarch64-none-elf-size aw-sync.elf
(cd /dist/addon-sync && sha256sum sdcard/atmosphere/contents/$TITLE/exefs.nsp && wc -c < sdcard/atmosphere/contents/$TITLE/exefs.nsp)
