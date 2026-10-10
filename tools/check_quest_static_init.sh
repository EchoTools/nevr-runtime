#!/usr/bin/env bash
# Fails when the Quest sentinel runs more at load time than its one ELF constructor.
#
# A namespace-scope object with a constructor (a std::string, a config struct) or an inline/template
# variable with a dynamic initializer gets an init_array entry that runs after the sentinel's ELF
# constructor and can reset what that constructor produced. Use a function-local static instead.
#
# Two layers, both fail-closed (a tool that cannot run is a failure, never a pass):
#   1. The final shared object: every .init_array entry, resolved to a symbol, must be on the list
#      below. This covers every source file and every initializer naming scheme.
#   2. Every object file given: no _GLOBAL__sub_I_* and no __cxx_global_var_init* symbol, so a new
#      initializer is named at the object that introduced it.
#
# usage: tools/check_quest_static_init.sh LLVM_BIN_DIR SHARED_OBJECT OBJECT...
set -euo pipefail

die() { echo "check_quest_static_init: $*" >&2; exit 1; }

bin=${1:?usage: check_quest_static_init.sh LLVM_BIN_DIR SHARED_OBJECT OBJECT...}
so=${2:?usage: check_quest_static_init.sh LLVM_BIN_DIR SHARED_OBJECT OBJECT...}
shift 2
[ "$#" -gt 0 ] || die "no object files given"

nm="$bin/llvm-nm"
readelf="$bin/llvm-readelf"
for tool in "$nm" "$readelf"; do
    "$tool" --version >/dev/null 2>&1 || die "tool does not run: $tool"
done
[ -f "$so" ] || die "missing shared object $so"

# The two compiler-rt entries every arm64 NDK library carries, and the sentinel's own constructor.
expected=" init_have_lse_atomics init_cpu_features _ZL18nevr_sentinel_ctorv "

sections=$("$readelf" -S -W "$so") || die "llvm-readelf -S failed on $so"
init_line=$(printf '%s\n' "$sections" | sed -E 's/^ *\[ *[0-9]+\] *//' | awk '$1 == ".init_array" { print; exit }')
[ -n "$init_line" ] || die "no .init_array section in $so"
read -r _ _ init_addr _ init_size _ <<<"$init_line"
start=$((16#$init_addr))
size=$((16#$init_size))
end=$((start + size))

relocs=$("$readelf" -r -W "$so") || die "llvm-readelf -r failed on $so"
symbols=$("$nm" --defined-only "$so") || die "llvm-nm failed on $so"

entries=()
while read -r off _ type rest; do
    [[ "$off" =~ ^[0-9a-f]+$ ]] || continue
    offset=$((16#$off))
    if [ "$offset" -ge "$start" ] && [ "$offset" -lt "$end" ]; then
        [ "$type" = "R_AARCH64_RELATIVE" ] || die "unexpected relocation $type in .init_array of $so"
        entries+=("${rest##* }")
    fi
done <<<"$relocs"
[ $(( ${#entries[@]} * 8 )) -eq "$size" ] || die ".init_array of $so holds $((size / 8)) slots but ${#entries[@]} relocations were read"

status=0
seen_ctor=0
for addend in "${entries[@]}"; do
    hex=$(printf '%016x' "$((16#$addend))")
    names=$(printf '%s\n' "$symbols" | awk -v a="$hex" '$1 == a { print $3 }')
    [ -n "$names" ] || { echo "check_quest_static_init: $so: .init_array entry 0x$addend has no symbol" >&2; status=1; continue; }
    match=""
    for name in $names; do
        case "$expected" in *" $name "*) match=$name ;; esac
    done
    if [ -z "$match" ]; then
        echo "check_quest_static_init: $so: .init_array entry 0x$addend ($(echo $names)) is not an expected initializer; use a function-local static" >&2
        status=1
    elif [ "$match" = "_ZL18nevr_sentinel_ctorv" ]; then
        seen_ctor=1
    fi
done
[ "$seen_ctor" -eq 1 ] || { echo "check_quest_static_init: $so: nevr_sentinel_ctor is not in .init_array" >&2; status=1; }

for obj in "$@"; do
    [ -f "$obj" ] || die "missing object $obj"
    out=$("$nm" "$obj") || die "llvm-nm failed on $obj"
    bad=$(printf '%s\n' "$out" | awk '$3 ~ /^(_GLOBAL__sub_I_|__cxx_global_var_init)/ { print $3 }')
    if [ -n "$bad" ]; then
        echo "check_quest_static_init: $obj has a dynamic initializer ($(echo $bad)); use a function-local static" >&2
        status=1
    fi
done

if [ "$status" -eq 0 ]; then
    echo "check_quest_static_init: OK (${#entries[@]} .init_array entries in $so, $# object files)"
fi
exit "$status"
