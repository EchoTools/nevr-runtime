#!/usr/bin/env bash
# usage: dronemuzzle_thunks.sh <llvm-objdump> <dronemuzzle arm64 object>
# The two naked thunks (q15t_dm_tp_thunk, q15t_dm_fire_thunk) are in the object: instruction words and relocations, at least 38 lines.
set -euo pipefail
objdump="$1"; object="$2"
dump="$("$objdump" -dr "$object")"
thunk_lines=$(awk '/<q15t_dm_(tp|fire)_thunk>:/{p=1} p&&/^$/{p=0} p' <<<"$dump" \
  | sed -E 's/^ *[0-9a-f]+:\s+//; s/<[^>]*>//g; s/0x[0-9a-f]+//g; s/[[:space:]]+/ /g; s/^ ?[0-9a-f]{16}:? ?//' | grep -v '^$' | wc -l)
[ "$thunk_lines" -ge 38 ] || { echo "dronemuzzle: the thunks were not found in the arm64 object"; exit 1; }
echo "dronemuzzle: arm64 thunks present ($thunk_lines lines of instruction words and relocations)"
