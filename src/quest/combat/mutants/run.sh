#!/usr/bin/env bash
# Mutation run for one combat module (src/quest/combat/README.md rule 10): usage `run.sh <module> [name-regex]`.
# Each line of mutants/<module>.sh is `runm <module> <name> <file> <sed>`, a file relative to src/quest/combat. The sed is applied to the file in place, the
# module's host test (`<module>_test`, preset quest-host-combat) is rebuilt and run, and the file is put back from a saved copy with a new mtime, so
# ninja rebuilds it. A mutant is KILLED when the test fails, SURVIVED when it passes or the sed changed nothing, and NOT A KILL when the build fails.
# Exit status: the number of mutants that were not killed.
set -uo pipefail
# Every build and test runs in the shared capped slice: re-exec inside it when not there already.
if ! grep -q 'nevr-builds.slice' /proc/self/cgroup 2>/dev/null; then
  exec systemd-run --user --scope --quiet --slice=nevr-builds.slice -p MemoryMax=8G -- bash "$0" "$@"
fi
module="${1:?usage: run.sh <module> [name-regex]}"
filter="${2:-}"
here="$(cd "$(dirname "$0")" && pwd)"
combat="$(cd "$here/.." && pwd)"
quest="$(cd "$combat/.." && pwd)"
list="$here/$module.sh"
[ -f "$list" ] || { echo "run.sh: no mutant list $list" >&2; exit 64; }
test_name="${module}_test"
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
bad=0
saved=""
mutated=""
restore() {
  if [ -n "$saved" ] && [ -n "$mutated" ]; then
    cp -- "$saved" "$mutated" && touch -- "$mutated"
    rm -f -- "$saved"
  fi
  saved=""
  mutated=""
}
trap restore EXIT
(cd "$quest" && cmake --preset quest-host-combat >/dev/null) || { echo "run.sh: configure failed" >&2; exit 65; }
# The tree must pass before any mutant is judged.
(cd "$quest" && cmake --build --preset quest-host-combat -j "$jobs" --target "$test_name" >/dev/null && ctest --preset quest-host-combat -R "^${test_name}\$" >/dev/null) \
  || { echo "run.sh: $test_name does not pass on the unmutated tree" >&2; exit 66; }
runm() {
  local mod="$1" name="$2" file="$3" expr="$4"
  [ "$mod" = "$module" ] || return 0
  if [ -n "$filter" ] && ! [[ "$name" =~ $filter ]]; then return 0; fi
  mutated="$combat/$file"
  saved="$(mktemp)"
  cp -p -- "$mutated" "$saved"
  sed -i -- "$expr" "$mutated"
  if cmp -s -- "$mutated" "$saved"; then
    echo "MUTANT $name: sed changed nothing"
    bad=$((bad + 1))
    restore
    return 0
  fi
  touch -- "$mutated"
  if ! (cd "$quest" && cmake --build --preset quest-host-combat -j "$jobs" --target "$test_name" >/dev/null 2>&1); then
    echo "MUTANT $name: NOT A KILL, it does not build"
    bad=$((bad + 1))
  elif (cd "$quest" && ctest --preset quest-host-combat -R "^${test_name}\$" >/dev/null 2>&1); then
    echo "MUTANT $name: SURVIVED"
    bad=$((bad + 1))
  else
    echo "MUTANT $name: killed"
  fi
  restore
}
# shellcheck source=/dev/null
. "$list"
echo "run.sh: $module: $bad mutant(s) not killed"
exit "$bad"
