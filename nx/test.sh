#!/bin/sh
# Host tests for the platform policy unit (no libnx, no core, no Docker).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=${TMPDIR:-/tmp}/aw-watcher-nx-tests
cc=${CC:-cc}

rm -rf "$out"; mkdir -p "$out"

"$cc" -std=c11 -Wall -Wextra -Wpedantic -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer \
  -g -O1 -I"$here/include" \
  "$here/tests/test_policy.c" "$here/source/policy.c" -o "$out/test_policy"

"$out/test_policy"
