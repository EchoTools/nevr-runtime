#!/usr/bin/env bash
# usage: no_emutls.sh <llvm-nm> <arm64 object or shared object> <name>
# Fails when the file defines an emulated thread-local variable (__emutls_v.<name>). The Quest build targets API 26, where the
# compiler emulates thread_local: the first use on a thread allocates, and a hooked call must not allocate. The one emulated
# variable allowed is libc++abi's exception-globals slot, by its exact symbol (a shared object that links libc++ carries it); any
# other variable, libc++abi's included, fails.
# Fail-close: a missing file, an nm that does not run, and an nm that lists no symbol at all (a stripped file would pass
# unchecked) are failures.
set -euo pipefail
nm="$1"; file="$2"; name="$3"
allowed="__emutls_v._ZZN10__cxxabiv112_GLOBAL__N_19__globalsEvE10eh_globals"
[ -f "$file" ] || { echo "FAIL: $name: no file at $file"; exit 1; }
symbols="$("$nm" "$file")" || { echo "FAIL: $name: $nm failed on $file"; exit 1; }
listed="$(awk 'NF == 3 && $2 ~ /^[A-Za-z?-]$/ { n++ } NF == 2 && $1 ~ /^[A-Za-z?-]$/ { n++ } END { print n + 0 }' <<<"$symbols")"
if [ "$listed" -eq 0 ]; then
  echo "FAIL: $name: $nm lists no symbols in $file (stripped?), so nothing was checked"
  exit 1
fi
bad="$(awk -v ok="$allowed" '$3 ~ /^__emutls_v\./ && $3 != ok { print $3 }' <<<"$symbols")"
if [ -n "$bad" ]; then
  echo "FAIL: $name has an emulated thread-local variable (thread_local allocates on first use per thread): $bad"
  exit 1
fi
echo "$name: no emulated thread-local storage of q15t's own"
