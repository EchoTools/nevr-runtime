#!/usr/bin/env bash
# Make a fresh `git worktree add` checkout buildable.
#
# A new worktree has empty extern/{minhook,breakpad,lss} (submodules) and no gen/ (gitignored,
# generated), so `cmake --preset ...` fails on the first missing file. This fills in what is
# MISSING from the main checkout: extern/<d> that is empty, gen/ when it holds no generated source,
# and the gitignored .env the build embeds the service endpoints from. It never replaces anything
# that is already there (an initialised submodule with local work, an existing gen/ or .env), never
# touches the main checkout, never prints .env, and swaps each copy in only after it has succeeded.
# To refresh gen/ after `just proto` changed it, delete the worktree's gen/ and run this again.
#
#   tools/worktree-setup.sh          fill in the missing inputs
#   tools/worktree-setup.sh --check  only report what a root-preset build is missing (used by
#                                    `just configure`): extern/minhook and gen/cpp; exit 1 if any
#
# NEVR_MAIN_CHECKOUT names the main checkout when it cannot be derived (a bare repository or a
# separate git dir). It never lets the script run inside the main checkout itself.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
here=$(pwd -P)

# A submodule directory is populated when it holds anything besides its `.git` pointer (a file in a
# submodule checkout, a directory in a plain clone).
populated() { [[ -n "$(find "$1" -mindepth 1 -name .git -prune -o -print -quit 2>/dev/null)" ]]; }
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

# The main checkout is always derived from git when possible, so no override can aim the script at it.
derived=""
if common=$(git rev-parse --path-format=absolute --git-common-dir 2>/dev/null); then
  derived=$(cd "$common/.." && pwd -P)
fi
if [[ -n "${NEVR_MAIN_CHECKOUT:-}" ]]; then
  main=$(cd "$NEVR_MAIN_CHECKOUT" && pwd -P) || { echo "error: NEVR_MAIN_CHECKOUT is not a directory" >&2; exit 2; }
elif [[ -n "$derived" ]]; then
  main=$derived
else
  echo "error: not inside a git checkout" >&2; exit 2
fi
if [[ "$here" == "$main" || "$here" == "$derived" ]]; then
  echo "error: this is the main checkout; initialise it with 'git submodule update --init' and 'just proto'" >&2
  exit 2
fi
if [[ ! -d "$main/extern" ]]; then
  echo "error: no main checkout with an extern/ directory at $main (bare repository or separate git dir?)" >&2
  echo "  set NEVR_MAIN_CHECKOUT to the checkout that has the initialised submodules" >&2
  exit 2
fi

# The destinations must really be inside this worktree: a symlinked extern/ or gen/ would send the
# swap into another checkout.
for p in extern gen; do
  if [[ -e "$p" && "$(realpath "$p")" != "$here/$p" ]]; then
    echo "error: $p is a symlink out of this worktree ($(realpath "$p")); remove it first" >&2; exit 2
  fi
done
mkdir -p extern
for d in minhook breakpad lss; do
  if [[ -e "extern/$d" && "$(realpath "extern/$d")" != "$here/extern/$d" ]]; then
    echo "error: extern/$d is a symlink out of this worktree; remove it first" >&2; exit 2
  fi
done

for d in minhook breakpad lss; do
  populated "$main/extern/$d" || { echo "error: $main/extern/$d is not initialised; run 'git submodule update --init' in the main checkout first" >&2; exit 1; }
done
has_gen "$main" || { echo "error: $main/gen/cpp is missing or empty; run 'just proto' in the main checkout first" >&2; exit 1; }

# Temporaries are named for this script; remove any a killed run left behind, then copy next to the
# destination and swap, so a failed copy leaves what was there.
rm -rf .gen.?????? extern/.minhook.?????? extern/.breakpad.?????? extern/.lss.??????
tmps=()
cleanup() { for t in "${tmps[@]}"; do rm -rf "$t"; done; }
trap cleanup EXIT
filled=()
kept=()
for d in minhook breakpad lss; do
  if populated "extern/$d"; then kept+=("extern/$d"); continue; fi
  tmp=$(mktemp -d "extern/.$d.XXXXXX"); tmps+=("$tmp")
  tar -C "$main/extern/$d" --exclude=.git -cf - . | tar -C "$tmp" -xf -
  rm -rf "extern/$d"
  mv "$tmp" "extern/$d"
  filled+=("extern/$d")
done
if has_gen "$here"; then
  kept+=(gen/)
else
  tmp=$(mktemp -d ".gen.XXXXXX"); tmps+=("$tmp")
  tar -C "$main/gen" -cf - . | tar -C "$tmp" -xf -
  rm -rf gen
  mv "$tmp" gen
  filled+=(gen/)
fi
if [[ -e .env || -L .env ]]; then
  kept+=(.env)
elif [[ -f "$main/.env" ]]; then
  (umask 077; cp "$main/.env" .env)
  filled+=(.env)
else
  echo "warning: no .env in $main: the build will embed no service endpoints (launch-client.sh refuses such a DLL)" >&2
fi
echo "filled from $main: ${filled[*]:-nothing}"
[[ ${#kept[@]} -eq 0 ]] || echo "kept what this worktree already has: ${kept[*]}"
