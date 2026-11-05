#!/usr/bin/env bash
# usage: no_emutls.sh <llvm-nm> <arm64 object or shared object> <name>
# Fails when the file defines an emulated thread-local variable (__emutls_v.<name>). The Quest build targets API 26, where the
# compiler emulates thread_local: the first use on a thread allocates, and a hooked call must not allocate. The only emulated
# variable allowed is libc++abi's own exception slot (__cxxabiv1), which a shared object that links libc++ carries.
# Fail-close: a missing file, or an nm that does not run, is a failure.
set -euo pipefail
nm="$1"; file="$2"; name="$3"
[ -f "$file" ] || { echo "FAIL: $name: no file at $file"; exit 1; }
symbols="$("$nm" "$file")" || { echo "FAIL: $name: $nm failed on $file"; exit 1; }
bad="$(awk '$3 ~ /^__emutls_v\./ && $3 !~ /__cxxabiv1/ { print $3 }' <<<"$symbols")"
if [ -n "$bad" ]; then
  echo "FAIL: $name has an emulated thread-local variable (thread_local allocates on first use per thread): $bad"
  exit 1
fi
echo "$name: no emulated thread-local storage of q15t's own"
