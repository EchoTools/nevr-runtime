#!/usr/bin/env bash
# Fails when a Quest sentinel object file has a dynamic initializer that is not on the allow list.
# A namespace-scope object with a constructor (a std::string, a std::vector, a config struct)
# gets a _GLOBAL__sub_I_<file> initializer that runs after the sentinel's ELF constructor and can
# reset what that constructor produced. Use a function-local static instead.
# usage: tools/check_quest_static_init.sh LLVM_NM OBJECT...
set -euo pipefail
nm=${1:?usage: check_quest_static_init.sh LLVM_NM OBJECT...}
shift
[ "$#" -gt 0 ] || { echo "check_quest_static_init: no object files given" >&2; exit 2; }
# sentinel_log.cpp: std::mutex registers its destructor with __cxa_atexit; it holds no state that the
# constructor produces (see the comment on g_mutex in sentinel_log.cpp).
allowed=" sentinel_log.cpp "
status=0
for obj in "$@"; do
    [ -f "$obj" ] || { echo "check_quest_static_init: missing object $obj" >&2; exit 2; }
    while read -r _ _ sym; do
        file=${sym#_GLOBAL__sub_I_}
        case "$allowed" in
            *" $file "*) ;;
            *) echo "check_quest_static_init: $obj has a dynamic initializer ($sym); use a function-local static" >&2
               status=1 ;;
        esac
    done < <("$nm" "$obj" | grep ' _GLOBAL__sub_I_' || true)
done
if [ "$status" -eq 0 ]; then echo "check_quest_static_init: OK ($# object files)"; fi
exit "$status"
