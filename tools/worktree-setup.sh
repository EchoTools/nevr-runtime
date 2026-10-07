#!/usr/bin/env bash
# Make a fresh `git worktree add` checkout buildable.
#
# A new worktree has empty extern/{minhook,breakpad,lss} (submodules) and no gen/ (gitignored,
# generated), so `cmake --preset ...` fails on the first missing file. This copies those inputs, and
# the gitignored .env the build embeds the service endpoints from, out of the main checkout and
# leaves the main checkout unchanged. Never prints .env, never overwrites an existing .env, and
# replaces extern/<d> and gen/ only after the copy has succeeded.
#
#   tools/worktree-setup.sh          copy the inputs into this worktree
#   tools/worktree-setup.sh --check  only report what a root-preset build is missing (used by
#                                    `just configure`): extern/minhook and gen/cpp; exit 1 if any
#
# NEVR_MAIN_CHECKOUT names the main checkout when it cannot be derived (a bare repository or a
# separate git dir).
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
here=$(pwd -P)

# A submodule directory is populated when it holds anything besides the `.git` pointer file.
populated() { [[ -n "$(find "$1" -mindepth 1 -not -name .git -print -quit 2>/dev/null)" ]]; }
has_gen() { [[ -n "$(find "$1/gen/cpp" -type f -print -quit 2>/dev/null)" ]]; }

if [[ "${1-}" == "--check" && $# -eq 1 ]]; then
  missing=()
  populated extern/minhook || missing+=(extern/minhook)
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

if [[ -n "${NEVR_MAIN_CHECKOUT:-}" ]]; then
  main=$(cd "$NEVR_MAIN_CHECKOUT" && pwd -P) || { echo "error: NEVR_MAIN_CHECKOUT is not a directory" >&2; exit 2; }
else
  common=$(git rev-parse --path-format=absolute --git-common-dir) \
    || { echo "error: not inside a git checkout" >&2; exit 2; }
  main=$(cd "$common/.." && pwd -P)
fi
if [[ "$main" == "$here" ]]; then
  echo "error: this is the main checkout; initialise it with 'git submodule update --init' and 'just proto'" >&2
  exit 2
fi
if [[ ! -d "$main/extern" ]]; then
  echo "error: no main checkout with an extern/ directory at $main (bare repository or separate git dir?)" >&2
  echo "  set NEVR_MAIN_CHECKOUT to the checkout that has the initialised submodules" >&2
  exit 2
fi

for d in minhook breakpad lss; do
  populated "$main/extern/$d" || { echo "error: $main/extern/$d is not initialised; run 'git submodule update --init' in the main checkout first" >&2; exit 1; }
done
has_gen "$main" || { echo "error: $main/gen/cpp is missing or empty; run 'just proto' in the main checkout first" >&2; exit 1; }

# Copy next to the destination, then swap, so a failed copy leaves what was there.
tmps=()
cleanup() { for t in "${tmps[@]}"; do rm -rf "$t"; done; }
trap cleanup EXIT
mkdir -p extern
for d in minhook breakpad lss; do
  tmp=$(mktemp -d "extern/.$d.XXXXXX"); tmps+=("$tmp")
  tar -C "$main/extern/$d" --exclude=.git -cf - . | tar -C "$tmp" -xf -
  rm -rf "extern/$d"
  mv "$tmp" "extern/$d"
done
tmp=$(mktemp -d ".gen.XXXXXX"); tmps+=("$tmp")
cp -r "$main/gen/." "$tmp"
rm -rf gen
mv "$tmp" gen

if [[ -f "$main/.env" ]]; then
  if [[ -e .env ]]; then
    echo "copied extern/{minhook,breakpad,lss} and gen/ from $main; kept this worktree's own .env"
  else
    (umask 077; cp "$main/.env" .env)
    echo "copied extern/{minhook,breakpad,lss}, gen/ and .env from $main"
  fi
else
  echo "copied extern/{minhook,breakpad,lss} and gen/ from $main"
  [[ -e .env ]] || echo "warning: no .env in $main: the build will embed no service endpoints (launch-client.sh refuses such a DLL)" >&2
fi
