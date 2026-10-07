#!/usr/bin/env bash
# Make a fresh `git worktree add` checkout buildable.
#
# A new worktree has empty extern/{minhook,breakpad,lss} (submodules) and no gen/ (gitignored,
# generated), so `cmake --preset ...` fails on the first missing file. This fills those, and the
# gitignored .env the build embeds the service endpoints from, from the main checkout.
#
# What it will and will not do:
#   - It runs only inside a LINKED worktree (never the main checkout), whatever env or arguments say.
#   - It writes only where the destination is absent or an empty directory. It never deletes or
#     replaces anything that has content: an initialised submodule, an existing gen/ or .env is kept
#     (to refresh gen/, remove it and run this again). A gen/ that has other content but no
#     generated source is refused, not overwritten.
#   - It copies the main checkout's submodule CONTENT: a worktree on a branch that pins different
#     submodule commits should run `git submodule update --init` instead.
#   - It never touches the main checkout and never prints .env. Copies are made in a private
#     directory inside this worktree's git dir and moved into place, under a lock.
#
#   tools/worktree-setup.sh          fill in the missing inputs
#   tools/worktree-setup.sh --check  report what a root-preset build is missing (used by `just
#                                    configure`): extern/minhook and a generated gen/cpp source
#
# NEVR_MAIN_CHECKOUT names the main checkout when it cannot be derived (a bare repository).
set -euo pipefail
# The script may only ever act on the checkout it lives in: ignore any git env pointing elsewhere.
unset GIT_DIR GIT_WORK_TREE GIT_COMMON_DIR GIT_INDEX_FILE
self=$(readlink -f "${BASH_SOURCE[0]}")
cd "$(dirname "$self")/.."
here=$(pwd -P)

# Any regular file counts as content, except a submodule's `.git` pointer (file or directory).
has_files() { [[ -n "$(find "$1" -name .git -prune -o -type f -print -quit 2>/dev/null)" ]]; }
has_gen() { [[ -n "$(find "$1/gen/cpp" -type f \( -name '*.pb.cc' -o -name '*.pb.h' \) -print -quit 2>/dev/null)" ]]; }
# Absent, or a directory with nothing in it (not even dotfiles): safe to fill.
is_fillable() { [[ ! -e "$1" && ! -L "$1" ]] || { [[ -d "$1" && ! -L "$1" && -z "$(ls -A "$1")" ]]; }; }

if [[ "${1-}" == "--check" && $# -eq 1 ]]; then
  missing=()
  has_files extern/minhook || missing+=(extern/minhook)
  has_gen "$here" || missing+=(gen/cpp)
  if [[ ${#missing[@]} -gt 0 ]]; then
    echo "error: build inputs missing: ${missing[*]}" >&2
    echo "  in a git worktree, run: just worktree-setup (copies them from the main checkout)" >&2
    echo "  in the main checkout: git submodule update --init, and just proto for gen/" >&2
    exit 1
  fi
  exit 0
fi
[[ $# -eq 0 ]] || { echo "usage: worktree-setup.sh [--check]" >&2; exit 2; }

gitdir=$(git rev-parse --path-format=absolute --git-dir 2>/dev/null) || { echo "error: not inside a git checkout" >&2; exit 2; }
common=$(git rev-parse --path-format=absolute --git-common-dir)
if [[ "$gitdir" == "$common" ]]; then
  echo "error: this is not a linked worktree (it is the main checkout); initialise it with 'git submodule update --init' and 'just proto'" >&2
  exit 2
fi
if [[ -n "${NEVR_MAIN_CHECKOUT:-}" ]]; then
  main=$(cd "$NEVR_MAIN_CHECKOUT" && pwd -P) || { echo "error: NEVR_MAIN_CHECKOUT is not a directory" >&2; exit 2; }
else
  main=$(cd "$common/.." && pwd -P)
fi
if [[ "$main" == "$here" || ! -d "$main/extern" ]]; then
  echo "error: no separate main checkout with an extern/ directory at $main (bare repository?)" >&2
  echo "  set NEVR_MAIN_CHECKOUT to the checkout that has the initialised submodules" >&2
  exit 2
fi

# Destinations must be real directories inside this worktree (a symlink, even a dangling one, would
# send a copy into another tree).
for p in extern gen .env extern/minhook extern/breakpad extern/lss; do
  if [[ -L "$p" ]]; then echo "error: $p is a symlink; remove it first" >&2; exit 2; fi
done
mkdir -p extern

for d in minhook breakpad lss; do
  has_files "$main/extern/$d" || { echo "error: $main/extern/$d is not initialised; run 'git submodule update --init' in the main checkout first" >&2; exit 1; }
done
has_gen "$main" || { echo "error: $main/gen/cpp holds no generated source; run 'just proto' in the main checkout first" >&2; exit 1; }

# One run at a time, and a private scratch directory that only this script uses.
scratch="$gitdir/worktree-setup"
mkdir -p "$scratch"
exec 9>"$scratch/lock"
flock -n 9 || { echo "error: another worktree-setup is running in this worktree" >&2; exit 1; }
rm -rf "$scratch/tmp"
mkdir "$scratch/tmp"
trap 'rm -rf "$scratch/tmp"' EXIT

filled=()
kept=()
refused=()
for d in minhook breakpad lss; do
  if has_files "extern/$d"; then kept+=("extern/$d"); continue; fi
  if ! is_fillable "extern/$d"; then refused+=("extern/$d (has content but no files)"); continue; fi
  mkdir "$scratch/tmp/$d"
  tar -C "$main/extern/$d" --exclude=.git -cf - . | tar -C "$scratch/tmp/$d" -xf -
  [[ ! -d "extern/$d" ]] || rmdir "extern/$d"
  mv "$scratch/tmp/$d" "extern/$d"
  filled+=("extern/$d")
done
if has_gen "$here"; then
  kept+=(gen/)
elif is_fillable gen; then
  mkdir "$scratch/tmp/gen"
  tar -C "$main/gen" -cf - . | tar -C "$scratch/tmp/gen" -xf -
  [[ ! -d gen ]] || rmdir gen
  mv "$scratch/tmp/gen" gen
  filled+=(gen/)
else
  refused+=("gen/ (has other content but no generated source: move it away, then run again)")
fi
if [[ -e .env ]]; then
  kept+=(.env)
elif [[ -f "$main/.env" ]]; then
  (umask 077; cp "$main/.env" .env)
  filled+=(.env)
else
  echo "warning: no .env in $main: the build will embed no service endpoints (launch-client.sh refuses such a DLL)" >&2
fi
echo "filled from $main: ${filled[*]:-nothing}"
[[ ${#kept[@]} -eq 0 ]] || echo "kept what this worktree already has: ${kept[*]}"
if [[ ${#refused[@]} -gt 0 ]]; then
  printf 'error: not touched: %s\n' "${refused[@]}" >&2
  exit 1
fi
