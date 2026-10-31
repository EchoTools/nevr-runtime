#!/usr/bin/env bash
# Print the vcpkg root this checkout installs dependencies with, creating it first when absent.
#
# Every checkout gets its own root at build/vcpkg-root: a local clone of $VCPKG_ROOT (git hardlinks the
# objects) on the revision in .vcpkg-commit, with the vcpkg executable copied from $VCPKG_ROOT. vcpkg
# takes an exclusive filesystem lock on its root (`<root>/.vcpkg-root`) for a whole install, and
# keeps buildtrees/ and packages/ there, so two checkouts sharing $VCPKG_ROOT serialise on the lock (or
# fail with "Failed to take the filesystem lock") and race in the same buildtrees/.
#
# Ports are still built once: the binary cache (~/.cache/vcpkg/archives, vcpkg's default `files`
# provider) is shared by every root, and the install root stays per checkout
# (build/<preset>/vcpkg_installed).
#
#   tools/vcpkg_root.sh            print the root, creating it when missing or on another revision
#   tools/vcpkg_root.sh NAME       the same for build/NAME, a second root for an install that must not wait on
#                                  the default root's lock: vcpkg keeps flock(2) on <root>/.vcpkg-root while a
#                                  child it started can outlive it (#1408)
#
# The clone source is VCPKG_ROOT, the standard vcpkg variable; NEVR_VCPKG_SOURCE overrides it for one run.
set -euo pipefail
unset GIT_DIR GIT_WORK_TREE GIT_COMMON_DIR GIT_INDEX_FILE
cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/.."
here=$(pwd -P)
name="${1:-vcpkg-root}"
case "$name" in
  .|..|*/*) echo "vcpkg_root: $name is not a vcpkg root name (one path element, under build/)" >&2; exit 2 ;;
esac
root="$here/build/$name"
source_root="${NEVR_VCPKG_SOURCE:-${VCPKG_ROOT:-}}"
want=$(tr -d '[:space:]' < "$here/.vcpkg-commit")

have=""
if [[ -d "$root/.git" ]]; then
  have=$(git -C "$root" rev-parse HEAD 2>/dev/null || true)
fi

if [[ "$have" != "$want" ]]; then
  [[ -n "$source_root" ]] || { echo "vcpkg_root: VCPKG_ROOT is not set; point it at a vcpkg checkout (https://github.com/microsoft/vcpkg)" >&2; exit 1; }
  [[ -d "$source_root/.git" ]] || { echo "vcpkg_root: $source_root is not a vcpkg checkout (clone https://github.com/microsoft/vcpkg there)" >&2; exit 1; }
  mkdir -p "$here/build"
  # Build in a name nothing else uses, then rename: a half-made root is never visible at $root.
  tmp="$here/build/vcpkg-root.new.$$"
  trap 'rm -rf "$tmp"' EXIT
  if [[ -d "$root" ]]; then
    git -C "$root" fetch -q "$source_root" "$want" 2>/dev/null || true
    git -C "$root" checkout -q --detach "$want"
  else
    git clone -q --no-checkout "$source_root" "$tmp"
    git -C "$tmp" checkout -q --detach "$want"
    if [[ -x "$source_root/vcpkg" ]]; then
      cp "$source_root/vcpkg" "$tmp/vcpkg"
    else
      "$tmp/bootstrap-vcpkg.sh" -disableMetrics >&2
    fi
    mv "$tmp" "$root"
  fi
  have=$(git -C "$root" rev-parse HEAD)
  [[ "$have" == "$want" ]] || { echo "vcpkg_root: $root is at $have, want $want (.vcpkg-commit)" >&2; exit 1; }
fi
[[ -x "$root/vcpkg" ]] || { echo "vcpkg_root: $root/vcpkg is missing; delete $root and run again" >&2; exit 1; }
echo "$root"
