#!/usr/bin/env bash
# Make a fresh `git worktree add` checkout buildable.
#
# A new worktree has empty extern/{minhook,breakpad,lss} (submodules) and no gen/ (gitignored,
# generated), so `cmake --preset ...` fails on the first missing file. This fills those from the main
# checkout, and the git-ignored config/public-defaults.env (the public client defaults the build embeds).
# It also fills extern/luau (read by `just test-scripting`, so by `just verify`) on a branch that pins it:
# from the main checkout when that has it, else by `git submodule update --init extern/luau` here.
# .env is runtime-only and is never copied here.
#
# It then seeds build/$PRESET (default mingw-release, as in the justfile) from the main checkout's build/$PRESET
# with tools/seed_build_tree.py, so the first build in this worktree compiles only what differs from the main
# checkout's last build. That is done only when build/$PRESET is absent or empty, and says why when it cannot be.
#
# What it will and will not do:
#   - It runs only inside a LINKED worktree (never the main checkout), whatever env or arguments say.
#   - It writes only where the destination is absent or an empty directory. It never deletes or
#     replaces anything that has content: an initialised submodule is kept. An existing gen/ is kept too, but a
#     gen/ that is not the generator's output for the justfile's pin is regenerated in place by tools/proto_stamp.py,
#     which removes only the files its own stamp lists. A gen/ that has other content but no generated source is
#     refused, not overwritten.
#   - It copies the main checkout's submodule CONTENT: a worktree on a branch that pins different
#     submodule commits should run `git submodule update --init` instead.
#   - It never touches the main checkout and never reads or copies .env. Copies are made in
#     `.nevr-worktree-setup/` at the worktree root (the same filesystem as the destinations, so a
#     move is an atomic rename, and a name nothing else uses) and moved into place, under a lock.
#
#   tools/worktree-setup.sh          fill in the missing inputs
#   tools/worktree-setup.sh --check  report what a root-preset build is missing (used by `just
#                                    configure`): extern/minhook and a gen/ that passes tools/proto_stamp.py check
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
  # gen/ must be the generator's output for the pin the justfile names (tools/proto_stamp.py), not merely present.
  if ! gen_reason=$(python3 -I tools/proto_stamp.py --root "$here" check 2>&1 >/dev/null); then
    missing+=("gen/ ($gen_reason)")
  fi
  if [[ ${#missing[@]} -gt 0 ]]; then
    echo "error: build inputs missing: ${missing[*]}" >&2
    echo "  in a git worktree, run: just worktree-setup (copies them from the main checkout, regenerates a stale gen/)" >&2
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
for p in extern gen config/public-defaults.env extern/minhook extern/breakpad extern/lss; do
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
# extern/luau (the scripting VM; only `just test-scripting` reads it) is filled only on a branch that pins it:
# copied from the main checkout when that has it, else initialised in this worktree (it needs the network then).
# A failure is refused below, so the exit status is non-zero and the line names it.
if [[ -n "$(git ls-tree HEAD extern/luau 2>/dev/null)" ]]; then
  if has_files extern/luau; then
    kept+=(extern/luau)
  elif ! is_fillable extern/luau; then
    refused+=("extern/luau (has content but no files)")
  elif has_files "$main/extern/luau"; then
    mkdir "$scratch/tmp/luau"
    tar -C "$main/extern/luau" --exclude=.git -cf - . | tar -C "$scratch/tmp/luau" -xf -
    [[ ! -d extern/luau ]] || rmdir extern/luau
    mv -T "$scratch/tmp/luau" extern/luau
    filled+=(extern/luau)
  elif git submodule update --init extern/luau >"$scratch/luau-init.log" 2>&1 && has_files extern/luau; then
    filled+=(extern/luau)
  else
    refused+=("extern/luau (the main checkout has none and 'git submodule update --init extern/luau' failed: $(tail -n 1 "$scratch/luau-init.log" 2>/dev/null))")
  fi
fi
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
# The public client defaults the build embeds (git-ignored): copied when this worktree has none.
defaults=config/public-defaults.env
if [[ -e "$defaults" ]]; then
  kept+=("$defaults")
elif [[ -f "$main/$defaults" ]]; then
  mkdir -p config
  cp -p "$main/$defaults" "$scratch/tmp/public-defaults.env"
  mv -T -n "$scratch/tmp/public-defaults.env" "$defaults"
  # `mv -n` skips silently when the file appeared meanwhile: believe the filesystem, not the exit status.
  if [[ -e "$scratch/tmp/public-defaults.env" ]]; then kept+=("$defaults"); else filled+=("$defaults"); fi
else
  echo "warning: no $defaults in $main: copy config/public-defaults.env.example to it and fill the four values; the build fails without it" >&2
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
# The build tree. The seeder prints what it did (or why it did not) and appends a record to its log; it only
# reads the main checkout and writes nothing in this worktree unless build/$preset is absent or empty.
preset=${PRESET:-mingw-release}
python3 -I tools/seed_build_tree.py --main "$main" --worktree "$here" --preset "$preset" \
  || refused+=("build/$preset (seeding it from the main checkout failed: see above)")
# gen/ must be the generator's output for the pin the justfile names. A copy from the main checkout can be stale
# (#1053): regenerate it here with the protoc of this worktree's build tree, which the seeded tree may have just
# supplied. With no such protoc, leave it and say so: `just configure` regenerates it after vcpkg has installed protoc.
protoc_dir="$here/build/$preset/vcpkg_installed/x64-linux/tools/protobuf"
if ! gen_reason=$(python3 -I tools/proto_stamp.py --root "$here" check 2>&1 >/dev/null); then
  if [[ -x "$protoc_dir/protoc" ]]; then
    python3 -I tools/proto_stamp.py --root "$here" ensure --protoc-dir "$protoc_dir" \
      || refused+=("gen/ (regenerating it at the justfile's pin failed: see above)")
  else
    echo "warning: gen/ is not current ($gen_reason); just configure regenerates it once vcpkg has installed protoc" >&2
  fi
fi
if [[ ${#refused[@]} -gt 0 ]]; then
  printf 'error: not touched: %s\n' "${refused[@]}" >&2
  exit 1
fi
