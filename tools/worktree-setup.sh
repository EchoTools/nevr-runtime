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
#   - It never touches the main checkout and never prints .env. Copies are made in
#     `.nevr-worktree-setup/` at the worktree root (the same filesystem as the destinations, so a
#     move is an atomic rename, and a name nothing else uses) and moved into place, under a lock.
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

# Content is a regular, non-dot file (a `.git` pointer, a `.keep` or an empty directory tree is not).
has_files() { [[ -d "$1" && -n "$(find "$1" -name .git -prune -o -type f ! -name '.*' -print -quit 2>/dev/null)" ]]; }
has_gen() { [[ -n "$(find "$1/gen/cpp" -type f \( -name '*.pb.cc' -o -name '*.pb.h' \) -size +0 -print -quit 2>/dev/null)" ]]; }
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
top=$(git rev-parse --show-toplevel)
if [[ "$(cd "$top" && pwd -P)" != "$here" ]]; then
  echo "error: this script is not at the root of its worktree ($top); it only runs from tools/ at the worktree root" >&2
  exit 2
fi
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

# One run at a time. The lock lives in the git dir; the copies are made in a directory at the worktree
# root with a fixed name (so cleaning it never matches anything else).
mkdir -p "$gitdir/worktree-setup"
exec 9>"$gitdir/worktree-setup/lock"
flock -n 9 || { echo "error: another worktree-setup is running in this worktree" >&2; exit 1; }
scratch="$here/.nevr-worktree-setup"
[[ ! -L "$scratch" ]] || { echo "error: $scratch is a symlink; remove it first" >&2; exit 2; }
# Only a directory this script made (it holds our marker) or an empty one may be cleared: the name is
# git-ignored, so anything else there is the user's and must not be deleted.
if [[ -e "$scratch" ]]; then
  if [[ -f "$scratch/.created-by-worktree-setup" || ( -d "$scratch" && -z "$(ls -A "$scratch")" ) ]]; then
    rm -rf "$scratch"
  else
    echo "error: $scratch exists and was not created by this script; move it away first" >&2; exit 2
  fi
fi
mkdir "$scratch" "$scratch/tmp"
: > "$scratch/.created-by-worktree-setup"
trap 'rm -rf "$scratch"' EXIT
# A rename only stays atomic within one filesystem: refuse a destination on another (a mount point
# for extern/ or the worktree root would turn the move into a copy that a kill can leave truncated).
for p in extern .; do
  [[ "$(stat -c %d "$p")" == "$(stat -c %d "$scratch/tmp")" ]] || { echo "error: $p is on a different filesystem than $scratch; setup would copy instead of rename" >&2; exit 2; }
done

filled=()
kept=()
refused=()
for d in minhook breakpad lss; do
  if has_files "extern/$d"; then kept+=("extern/$d"); continue; fi
  if ! is_fillable "extern/$d"; then refused+=("extern/$d (has content but no files)"); continue; fi
  mkdir "$scratch/tmp/$d"
  tar -C "$main/extern/$d" --exclude=.git -cf - . | tar -C "$scratch/tmp/$d" -xf -
  [[ ! -d "extern/$d" ]] || rmdir "extern/$d"
  mv -T "$scratch/tmp/$d" "extern/$d"
  filled+=("extern/$d")
done
if has_gen "$here"; then
  kept+=(gen/)
elif is_fillable gen; then
  mkdir "$scratch/tmp/gen"
  tar -C "$main/gen" -cf - . | tar -C "$scratch/tmp/gen" -xf -
  [[ ! -d gen ]] || rmdir gen
  mv -T "$scratch/tmp/gen" gen
  filled+=(gen/)
else
  refused+=("gen/ (has other content but no generated source: move it away, then run again)")
fi
if [[ -e .env ]]; then
  kept+=(.env)
elif [[ -f "$main/.env" ]]; then
  (umask 077; cp "$main/.env" "$scratch/tmp/env")
  mv -T -n "$scratch/tmp/env" .env
  # `mv -n` skips silently when .env appeared meanwhile: believe the filesystem, not the exit status.
  if [[ -e "$scratch/tmp/env" ]]; then kept+=(.env); else filled+=(.env); fi
else
  echo "warning: no .env in $main: the build will embed no service endpoints (launch-client.sh refuses such a DLL)" >&2
fi
# Warn when what was just copied is not the commit this branch pins (only for what this run filled).
for f in "${filled[@]:-}"; do
  [[ "$f" == extern/* ]] || continue
  d=${f#extern/}
  mine=$(git ls-tree HEAD "extern/$d" 2>/dev/null | awk '{print $3}')
  theirs=$(git -C "$main/extern/$d" rev-parse HEAD 2>/dev/null || true)
  if [[ -n "$mine" && -n "$theirs" && "$mine" != "$theirs" ]]; then
    echo "warning: this branch pins extern/$d at ${mine:0:12} but the main checkout has ${theirs:0:12}: remove extern/$d and run 'git submodule update --init extern/$d' for this branch's version" >&2
  fi
done
echo "filled from $main: ${filled[*]:-nothing}"
[[ ${#kept[@]} -eq 0 ]] || echo "kept what this worktree already has: ${kept[*]}"
if [[ ${#refused[@]} -gt 0 ]]; then
  printf 'error: not touched: %s\n' "${refused[@]}" >&2
  exit 1
fi
