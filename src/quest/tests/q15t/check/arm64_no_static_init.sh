#!/usr/bin/env bash
# usage: arm64_no_static_init.sh <llvm-readelf> <object> <module>
# Fails unless the arm64 object has no static initializer section: the repack refuses a plugin with one.
set -euo pipefail
readelf="$1"; object="$2"; module="$3"
[ -f "$object" ] || { echo "FAIL: $module: no arm64 object at $object"; exit 1; }
sections="$("$readelf" -S "$object")"
if grep -q -E '\.init_array|\.ctors' <<<"$sections"; then
  echo "FAIL: $module.arm64.o has a static initializer section"
  exit 1
fi
echo "arm64 build of $module: ok (NDK r26d clang++, no .init_array/.ctors)"
