#!/usr/bin/env bash
# usage: powerboost_thunk.sh <llvm-objdump> <powerboost arm64 object> <expected listing>
# The thunk as compiled into the plugin: exactly the expected instructions, in this order (no other register, no other branch).
set -euo pipefail
objdump="$1"; object="$2"; want="$3"
got="$(mktemp)"
trap 'rm -f -- "$got"' EXIT
"$objdump" -dr --disassemble-symbols=q15t_pb_resolve_thunk "$object" \
  | sed -n -E -e 's/^ *[0-9a-f]+: +[0-9a-f]{8} +\t//p' -e 's/^\t+[0-9a-f]+: +(R_AARCH64_[A-Z0-9_]+)\t(.*)$/    reloc \1 \2/p' | tr -s ' \t' ' ' > "$got"
if ! diff -u "$want" "$got"; then
  echo "FAIL: the compiled thunk differs from the expected instruction sequence"
  exit 1
fi
echo "arm64 thunk disassembly: matches the expected 15 instructions and 5 relocations"
