#!/usr/bin/env bash
# Print the vcpkg root this checkout installs dependencies with, creating it first when absent.
#
# Every checkout gets its own root at build/vcpkg-root: a local clone of ~/.vcpkg (git hardlinks the
# objects) on the revision in .vcpkg-commit, with the vcpkg executable copied from ~/.vcpkg, or bootstrapped
# when that copy is not the tool release the revision names (scripts/vcpkg-tool-metadata.txt). vcpkg
# takes an exclusive filesystem lock on its root (`<root>/.vcpkg-root`) for a whole install, and
# keeps buildtrees/ and packages/ there, so two checkouts sharing ~/.vcpkg serialise on the lock (or
# fail with "Failed to take the filesystem lock") and race in the same buildtrees/.
#
# Ports are still built once: the binary cache (~/.cache/vcpkg/archives, vcpkg's default `files`
# provider) is shared by every root, and the install root stays per checkout
# (build/<preset>/vcpkg_installed).
#
#   tools/vcpkg_root.sh            print the root, creating it when missing or on another revision
#
# NEVR_VCPKG_SOURCE overrides the clone source (default ~/.vcpkg).
set -euo pipefail
unset GIT_DIR GIT_WORK_TREE GIT_COMMON_DIR GIT_INDEX_FILE
cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")/.."
here=$(pwd -P)
root="$here/build/vcpkg-root"
source_root="${NEVR_VCPKG_SOURCE:-$HOME/.vcpkg}"
want=$(tr -d '[:space:]' < "$here/.vcpkg-commit")

have=""
if [[ -d "$root/.git" ]]; then
  have=$(git -C "$root" rev-parse HEAD 2>/dev/null || true)
fi

if [[ "$have" != "$want" ]]; then
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

# The executable is the one the revision names (scripts/vcpkg-tool-metadata.txt), not whatever ~/.vcpkg holds:
# a revision's ports can need a newer tool than the copied one. A mismatch bootstraps the named release.
metadata="$root/scripts/vcpkg-tool-metadata.txt"
if [[ -f "$metadata" ]]; then
  want_tool=$(sed -n 's/^VCPKG_TOOL_RELEASE_TAG=//p' "$metadata" | tr -d '[:space:]')
  tool_version() { "$root/vcpkg" version 2>/dev/null | grep -oE '[0-9]{4}-[0-9]{2}-[0-9]{2}' | sed -n 1p; }
  if [[ -n "$want_tool" && "$(tool_version)" != "$want_tool" ]]; then
    "$root/bootstrap-vcpkg.sh" -disableMetrics >&2
    have_tool=$(tool_version)
    [[ "$have_tool" == "$want_tool" ]] || { echo "vcpkg_root: $root/vcpkg is version '$have_tool' after bootstrap, want $want_tool ($metadata)" >&2; exit 1; }
  fi
fi
echo "$root"
