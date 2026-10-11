#!/usr/bin/env bash
# End-to-end: generate the definitions from a registry, then type-check the
# sample script (must be clean) and each bad sample (must fail, on its line).
#   check_test.sh <script_stubs_test binary> <nevr_script_check binary> <out dir>
set -euo pipefail
stubs=$1 check=$2 out=$3
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$out"
"$stubs" --emit "$out/nevr.d.luau"
fail=0
if "$check" "$out/nevr.d.luau" "$here/../samples/low_gravity.lua" >"$out/good.txt" 2>&1; then
  echo "PASS samples/low_gravity.lua checks clean"
else
  echo "FAIL samples/low_gravity.lua:"; cat "$out/good.txt"; fail=1
fi
expect() {  # <sample> <line> — the checker must exit 1 and report an error on that line of that file
  local f="$here/samples/$1" rc=0
  "$check" "$out/nevr.d.luau" "$f" >"$out/$1.txt" 2>&1 || rc=$?
  if [ "$rc" -eq 1 ] && grep -q "^$f:$2:[0-9]*: TypeError: " "$out/$1.txt"; then
    echo "PASS $1 rejected: $(grep -m1 "^$f:$2:" "$out/$1.txt" | sed "s#^$here/##")"
  else
    echo "FAIL $1 (exit $rc), expected a TypeError on line $2:"; cat "$out/$1.txt"; fail=1
  fi
}
expect bad_key.lua 1
expect bad_value.lua 1
expect bad_read_only.lua 3
expect bad_skip_in_post.lua 3
expect bad_hook.lua 1
exit $fail
