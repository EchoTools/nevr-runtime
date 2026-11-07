#!/usr/bin/env bash
# The mutation run of one combat module: `mutants.sh <module> [name-regex]` applies each mutant of `mutants.d/<module>.sh` to the module's
# source in this checkout, builds the module's host test (the `combat` group, build/quest-host-combat) and runs it. A mutant is killed when
# the build succeeds and the test fails. A mutant that does not compile is reported as NOT A KILL (the build, not the test, said no), and a
# mutant the test passes SURVIVED. The file is put back from a copy taken before the run (and its modification time moved, so ninja
# rebuilds it) after every mutant and when the script ends, however it ends. Exit status: 0 when every mutant was killed and at least one ran;
# 1 when one survived, did not compile or the final rebuild failed; 2 when none ran (a filter that matches nothing, or an empty list) or when
# a program the run judges does not build or fails its tests on the unmutated tree (it is built and run first, and the run stops there).
#
# A line of the list is `runm <name> <file> <sed> [<build dir under build/> <test target>]`. The two last words name another host-test
# build and program when the mutated file is shared (combat_install.cpp, quest_config.cpp: `quest-host-shared quest_config_test`); the
# default is this group's build and `<module>_test`.
#
# Build jobs. Every build the script starts is capped at CMAKE_BUILD_PARALLEL_LEVEL jobs (default 4; ninja's own default is the core count
# plus two, and a full-tree compile at that width was oom-killed in the 6 GiB build slice). The final rebuild covers only the targets the
# run built (not the whole tree), and each build prints its peak memory (`peak_mib`, the build's own cgroup `memory.peak`, or `n/a` when
# the host exposes none), so a run shows what it cost. A failed rebuild prints the first error lines of its log. The rebuild is of those
# targets alone because a rebuild of the whole group also builds every other program in it, and one of those can fail to link for a reason
# that has nothing to do with the module under test (another unit's row not yet in that program's source list).
#
# Run it in the build slice, from the repository root's checkout, after `just test-quest-combat` has configured the tree:
#   systemd-run --user --scope --slice=nevr-builds.slice -p MemoryMax=8G src/quest/combat/mutants.sh sendguard
set -uo pipefail
module="${1:?usage: mutants.sh <module> [name-regex]}"
only="${2:-}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../../.." && pwd)"
list="$here/mutants.d/$module.sh"
[ -f "$list" ] || { echo "mutants.sh: no mutant list for '$module' ($list)" >&2; exit 2; }
build="$root/build/quest-host-combat"
[ -f "$build/build.ninja" ] || { echo "mutants.sh: $build is not configured; run 'just test-quest-combat $module' first" >&2; exit 2; }
backup="$(mktemp -d "${NEVR_WORK_ROOT:-$HOME/src/nevr-runtime-work}/combat-mutants.XXXXXX")" || exit 2
current=""
jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
case "$jobs" in ''|*[!0-9]*|0) jobs=4 ;; esac
touched=""  # the "<dir> <target>" pairs this run built, one per line
# build_target <dir under build/> <target> <log>: a capped build in a scope of its own, so its memory is measured on its own. Prints the
# build's peak memory on stdout as `peak_mib=<n>` (`n/a` without a memory.peak file) and returns the build's status.
build_target() {
  local dir="$1" target="$2" log="$3"
  local inner='cmake --build "$1" --target "$2" -j "$3" >"$4" 2>&1; rc=$?
cg=/sys/fs/cgroup$(sed -n "s/^0:://p" /proc/self/cgroup)/memory.peak
if [ -r "$cg" ]; then echo "peak_mib=$(( $(cat "$cg") / 1048576 ))" >>"$4"; else echo "peak_mib=n/a" >>"$4"; fi
exit $rc'
  if command -v systemd-run >/dev/null 2>&1 && systemd-run --user --scope --quiet -p MemoryAccounting=yes -- true 2>/dev/null; then
    systemd-run --user --scope --quiet --slice=nevr-builds.slice -p MemoryAccounting=yes -- bash -c "$inner" _ "$root/build/$dir" "$target" "$jobs" "$log"
  else
    bash -c "$inner" _ "$root/build/$dir" "$target" "$jobs" "$log"
  fi
}
# The lines of a build log that are errors: a compiler's `error:`, ninja's `FAILED:`, the linker's. Not a compile line that carries -Werror.
errpat='(^|[^[:alnum:]_-])error:|^FAILED:|undefined reference|ld returned|multiple definition'
maxpeak=0
note_peak() {
  local v
  v="$(grep -o 'peak_mib=[0-9]*' "$1" | tail -n 1 | cut -d= -f2)"
  [ -n "$v" ] && [ "$v" -gt "$maxpeak" ] && maxpeak="$v"
  return 0
}
note_touched() { case "$touched" in *"|$1 $2|"*) ;; *) touched="$touched|$1 $2|" ;; esac; }
restore() {
  if [ -n "$current" ] && [ -f "$backup/file" ]; then
    cp -p -- "$backup/file" "$root/$current" && touch -- "$root/$current"
  fi
  current=""
}
# The files the run writes in $backup, each removed by its name; then the directory, which must be empty by then. Anything else found
# there is left in place with the directory, and one line on stderr names it (#2270: no recursive removal).
backup_files="file build.log test.log baseline.log baseline.test.log rebuild.log"
cleanup() {
  local f
  restore
  for f in $backup_files; do
    if [ -e "$backup/$f" ]; then rm -f -- "${backup:?}/$f"; fi
  done
  rmdir -- "$backup" 2>/dev/null || echo "mutants.sh: $backup holds files the run did not write; left in place" >&2
}
trap cleanup EXIT
survived=0
notkill=0
killed=0
phase=collect
baseline=""
# runm <name> <file relative to the repository root> <sed expression>
runm() {
  local name="$1" file="$2" expr="$3" dir="${4:-quest-host-combat}" target="${5:-${module}_test}"
  local bdir="$root/build/$dir"
  if [ -n "$only" ] && ! [[ "$name" =~ $only ]]; then return 0; fi
  if [ "$phase" = collect ]; then  # the first pass only names the programs the run will judge
    case "$baseline" in *"|$dir $target|"*) ;; *) baseline="$baseline|$dir $target|" ;; esac
    return 0
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
  note_touched "$dir" "$target"
  build_target "$dir" "$target" "$backup/build.log"; build_rc=$?
  note_peak "$backup/build.log"
  if [ "$build_rc" -ne 0 ]; then
    echo "MUTANT $name: NOT A KILL, it does not compile"
    notkill=$((notkill + 1))
  elif ctest --test-dir "$bdir" -R "^${target}\$" >"$backup/test.log" 2>&1; then
    echo "MUTANT $name: SURVIVED"
    survived=$((survived + 1))
  else
    echo "MUTANT $name: killed"
    killed=$((killed + 1))
  fi
  restore
}
# The unmutated tree first: every program the run will judge is built and run as it is. A program that fails here fails every mutant for
# a reason that is not the mutant, so the run stops (exit 2) before any is counted.
# shellcheck source=/dev/null
source "$list"
for pair in $(printf '%s' "$baseline" | tr '|' '\n' | tr ' ' ',' | grep ,); do
  d="${pair%,*}"; t="${pair#*,}"
  build_target "$d" "$t" "$backup/baseline.log"; brc=$?
  note_peak "$backup/baseline.log"
  if [ "$brc" -ne 0 ]; then
    echo "mutants.sh $module: the unmutated tree does not build: $d $t; no mutant was judged" >&2
    grep -E "$errpat" "$backup/baseline.log" | head -n 8 >&2
    exit 2
  fi
  if ! ctest --test-dir "$root/build/$d" -R "^${t}\$" >"$backup/baseline.test.log" 2>&1; then
    echo "mutants.sh $module: $t fails on the unmutated tree; no mutant was judged" >&2
    tail -n 8 "$backup/baseline.test.log" >&2
    exit 2
  fi
  echo "mutants.sh $module: baseline $d $t passes"
done
phase=run
source "$list"
# Rebuild the restored tree once, so the build directories are left as the checkout has it; a failure here is a failure of the run.
rebuilt=0
for pair in $(printf '%s' "$touched" | tr '|' '\n' | tr ' ' ',' | grep ,); do
  d="${pair%,*}"; t="${pair#*,}"
  build_target "$d" "$t" "$backup/rebuild.log"; rb=$?
  note_peak "$backup/rebuild.log"
  if [ "$rb" -eq 0 ]; then
    echo "mutants.sh $module: rebuilt $d $t, $(grep -o 'peak_mib=.*' "$backup/rebuild.log" | tail -n 1)"
  else
    echo "mutants.sh $module: the rebuild of $d $t after the run failed, $(grep -o 'peak_mib=.*' "$backup/rebuild.log" | tail -n 1)" >&2
    grep -E "$errpat" "$backup/rebuild.log" | head -n 8 >&2
    rebuilt=1
  fi
done
echo "mutants.sh $module: $killed killed, $survived survived, $notkill not a kill; build jobs $jobs, largest build peak ${maxpeak} MiB"
if [ $((killed + survived + notkill)) -eq 0 ]; then
  echo "mutants.sh $module: no mutant ran (the filter '${only}' matches none, or the list is empty)" >&2
  exit 2
fi
[ "$survived" -eq 0 ] && [ "$notkill" -eq 0 ] && [ "$rebuilt" -eq 0 ]
