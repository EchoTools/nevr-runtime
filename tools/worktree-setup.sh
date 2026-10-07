#!/usr/bin/env bash
# Make a fresh `git worktree add` checkout buildable.
#
# A new worktree has empty extern/{minhook,breakpad,lss} (submodules) and no gen/ (gitignored,
# generated), so `cmake --preset ...` fails on the first missing file. This copies those inputs, and
# the gitignored .env the build embeds the service endpoints from, out of the main checkout and
# leaves the main checkout unchanged. Never prints .env.
#
#   tools/worktree-setup.sh          copy the inputs into this worktree
#   tools/worktree-setup.sh --check  only report what is missing (used by `just configure`); exit 1 if any
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
here=$PWD

missing=()
for d in minhook breakpad lss; do
  [[ -n "$(ls -A "extern/$d" 2>/dev/null)" ]] || missing+=("extern/$d")
done
[[ -d gen/cpp ]] || missing+=("gen/")

if [[ "${1-}" == "--check" ]]; then
  if [[ ${#missing[@]} -gt 0 ]]; then
    echo "error: build inputs missing: ${missing[*]}" >&2
    echo "  in a git worktree, run: just worktree-setup (copies them from the main checkout)" >&2
    echo "  in the main checkout: git submodule update --init, and just proto for gen/" >&2
    exit 1
  fi
  exit 0
fi
[[ $# -eq 0 ]] || { echo "usage: worktree-setup.sh [--check]" >&2; exit 2; }

main=$(dirname "$(git rev-parse --path-format=absolute --git-common-dir)")
if [[ "$main" == "$here" ]]; then
  echo "error: this is the main checkout; initialise it with 'git submodule update --init' and 'just proto'" >&2
  exit 2
fi

for d in minhook breakpad lss; do
  [[ -n "$(ls -A "$main/extern/$d" 2>/dev/null)" ]] || { echo "error: $main/extern/$d is empty; initialise the submodules in the main checkout first" >&2; exit 1; }
done
[[ -d "$main/gen/cpp" ]] || { echo "error: $main/gen/cpp is missing; run 'just proto' in the main checkout first" >&2; exit 1; }

for d in minhook breakpad lss; do
  mkdir -p "extern/$d"
  tar -C "$main/extern/$d" --exclude=.git -cf - . | tar -C "extern/$d" -xf -
done
rm -rf gen
cp -r "$main/gen" gen
if [[ -f "$main/.env" ]]; then
  cp "$main/.env" .env
  echo "copied extern/{minhook,breakpad,lss}, gen/ and .env from $main"
else
  echo "copied extern/{minhook,breakpad,lss} and gen/ from $main"
  echo "warning: no .env in $main: the build will embed no service endpoints (launch-client.sh refuses such a DLL)" >&2
fi
