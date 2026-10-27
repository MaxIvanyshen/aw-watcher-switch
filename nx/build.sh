#!/bin/sh
# Builds the sysmodule in the pinned devkitPro image. Output: dist/ (never touches the repo tree).
set -eu
IMAGE=devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282
TITLE=4200000000415753

if [ "${1:-}" != "--inside" ]; then
  root=$(cd "$(dirname "$0")/.." && pwd)
  mkdir -p "$root/dist"
  exec docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e IMAGE="$IMAGE" \
    -v "$root":/src:ro -v "$root/dist":/dist "$IMAGE" sh /src/nx/build.sh --inside
fi

export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"
w=/tmp/w
rm -rf "$w"; mkdir -p "$w"
cp -R /src/nx /src/core "$w"/
cd "$w/nx"
make nsp > /tmp/make.log 2>&1 && rc=0 || rc=$?
cat /tmp/make.log
[ "$rc" = 0 ] || exit "$rc"
grep -q 'warning:' /tmp/make.log && { echo "build: warnings present" >&2; exit 1; } || true

out=/dist/sdcard/atmosphere/contents/$TITLE
rm -rf /dist/sdcard /dist/aw-watcher.elf /dist/build-info.txt
mkdir -p "$out/flags"
cp aw-watcher.nsp "$out/exefs.nsp"
: > "$out/flags/boot2.flag"
cp aw-watcher.elf /dist/aw-watcher.elf
cp aw-watcher.npdm /dist/aw-watcher.npdm

{
  echo "image: $IMAGE"
  aarch64-none-elf-gcc --version | head -1
  echo "--- sizes (bytes)"
  for f in aw-watcher.elf aw-watcher.nso aw-watcher.npdm aw-watcher.nsp; do printf '%s %s\n' "$f" "$(wc -c < "$f")"; done
  echo "--- elf sections (bss includes the static inner heap)"
  aarch64-none-elf-size aw-watcher.elf
  echo "--- sha256"
  (cd /dist && sha256sum sdcard/atmosphere/contents/$TITLE/exefs.nsp)
  echo "--- kernel access (hactool decode of the packed NPDM)"
  hactool -t npdm aw-watcher.npdm 2>/dev/null | sed -n '/Kernel Access Control:/,/Service Access Control:/p'
  echo "--- no-net check"
  sh check-no-net.sh watcher.json aw-watcher.elf aw-watcher.nsp
} | tee /dist/build-info.txt
