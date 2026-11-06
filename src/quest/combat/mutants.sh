#!/usr/bin/env bash
# The mutation run of one combat module: `mutants.sh <module> [name-regex]` applies each mutant of `mutants.d/<module>.sh` to the module's
# source in this checkout, builds the module's host test (the `combat` group, build/quest-host-combat) and runs it. A mutant is killed when
# the build succeeds and the test fails. A mutant that does not compile is reported as NOT A KILL (the build, not the test, said no), and a
# mutant the test passes SURVIVED. The file is put back from a copy taken before the run (and its modification time moved, so ninja
# rebuilds it) after every mutant and when the script ends, however it ends. Exit status: 0 when every mutant was killed and at least one ran;
# 1 when one survived, did not compile or the final rebuild failed; 2 when none ran (a filter that matches nothing, or an empty list).
#
# A line of the list is `runm <name> <file> <sed> [<build dir under build/> <test target> [<command>]]`. The two words after the sed name another
# host-test build and program when the mutated file is shared (combat_install.cpp, quest_config.cpp: `quest-host-shared quest_config_test`); the
# default is this group's build and `<module>_test`. A last word is a command (run by bash from the repository root, exit status 0 = the mutant
# survived) that judges the mutant instead of ctest: a program that takes arguments (`got_pinned_test`) or a source check (`go test`, a Python test);
# a test target of `-` builds nothing.
#
# Before the first mutant of a (build, target) pair is judged, the unmutated tree is built and judged by the same test; a tree that does not pass
# stops the run (exit 2), so a mutant is never "killed" by a test that was already failing. The script runs itself inside the shared build slice when
# it was not started in it, so it can be started from a plain shell:
#   src/quest/combat/mutants.sh sendguard
set -uo pipefail
# Every build and test runs in the shared capped slice: re-exec inside it when not there already.
if ! grep -q 'nevr-builds.slice' /proc/self/cgroup 2>/dev/null; then
  exec systemd-run --user --scope --quiet --slice=nevr-builds.slice -p MemoryMax=8G -- bash "$0" "$@"
fi
module="${1:?usage: mutants.sh <module> [name-regex]}"
only="${2:-}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
list="$here/mutants.d/$module.sh"
[ -f "$list" ] || { echo "mutants.sh: no mutant list for '$module' ($list)" >&2; exit 2; }
build="$root/build/quest-host-combat"
[ -f "$build/build.ninja" ] || { echo "mutants.sh: $build is not configured; run 'just test-quest-combat $module' first" >&2; exit 2; }
backup="$(mktemp -d "${NEVR_WORK_ROOT:-$HOME/src/nevr-runtime-work}/combat-mutants.XXXXXX")" || exit 2
restored=1
current=""
restore() {
  if [ -n "$current" ] && [ -f "$backup/file" ]; then
    cp -p -- "$backup/file" "$root/$current" && touch -- "$root/$current"
  fi
  current=""
}
cleanup() { restore; rm -rf -- "${backup:?}"; }
trap cleanup EXIT
survived=0
notkill=0
killed=0
declare -A baseline=()
# judge <build dir> <target> <command>: build the target (unless it is `-`), then run the command or ctest. 0: the test passes. 99: it does not build.
judge() {
  local bdir="$root/build/$1" target="$2" cmd="$3"
  if [ "$target" != "-" ] && ! cmake --build "$bdir" --target "$target" >"$backup/build.log" 2>&1; then return 99; fi
  if [ -n "$cmd" ]; then (cd "$root" && bash -c "$cmd") >"$backup/test.log" 2>&1; return $?; fi
  ctest --test-dir "$bdir" -R "^${target}\$" >"$backup/test.log" 2>&1
}
# runm <name> <file relative to the repository root> <sed expression> [<build dir> <test target> [<command>]]
runm() {
  local name="$1" file="$2" expr="$3" dir="${4:-quest-host-combat}" target="${5:-${module}_test}" cmd="${6:-}"
  local bdir="$root/build/$dir"
  if [ -n "$only" ] && ! [[ "$name" =~ $only ]]; then return 0; fi
  if [ -z "${baseline[$dir/$target/$cmd]:-}" ]; then
    if [ "$target" != "-" ] && [ ! -f "$bdir/build.ninja" ]; then echo "mutants.sh: $bdir is not configured" >&2; exit 2; fi
    judge "$dir" "$target" "$cmd"
    local rc=$?
    if [ "$rc" -ne 0 ]; then
      echo "mutants.sh: the unmutated tree does not pass ($dir $target${cmd:+ : $cmd}, status $rc): no mutant is judged" >&2
      exit 2
    fi
    baseline[$dir/$target/$cmd]=1
  fi
  cp -p -- "$root/$file" "$backup/file"
  current="$file"
  sed -i -e "$expr" "$root/$file"
  if cmp -s "$backup/file" "$root/$file"; then
    echo "MUTANT $name: the sed changed nothing"
    survived=$((survived + 1))
    restore
    return 0
  fi
  touch -- "$root/$file"
  judge "$dir" "$target" "$cmd"
  case $? in
    99) echo "MUTANT $name: NOT A KILL, it does not compile"; notkill=$((notkill + 1)) ;;
    0) echo "MUTANT $name: SURVIVED"; survived=$((survived + 1)) ;;
    *) echo "MUTANT $name: killed"; killed=$((killed + 1)) ;;
  esac
  restore
}
# shellcheck source=/dev/null
source "$list"
# Rebuild the restored tree once, so the build directories are left as the checkout has it; a failure here is a failure of the run.
rebuilt=0
for d in quest-host-combat quest-host-shared quest-host-hooks-pinned; do
  [ -f "$root/build/$d/build.ninja" ] || continue
  cmake --build "$root/build/$d" >/dev/null 2>&1 || { echo "mutants.sh $module: the rebuild of $d after the run failed" >&2; rebuilt=1; }
done
echo "mutants.sh $module: $killed killed, $survived survived, $notkill not a kill"
if [ $((killed + survived + notkill)) -eq 0 ]; then
  echo "mutants.sh $module: no mutant ran (the filter '${only}' matches none, or the list is empty)" >&2
  exit 2
fi
[ "$survived" -eq 0 ] && [ "$notkill" -eq 0 ] && [ "$rebuilt" -eq 0 ]
