#!/usr/bin/env bash
# Keeps one ccache config for every build (#1736): copies tools/ccache.conf into the cache directory and prints that path.
#
# CCACHE_CONFIGPATH used to be <worktree>/tools/ccache.conf. A process started from a worktree that was later removed keeps the dead
# path, and ccache then reads no config at all and silently caps the shared cache at its 5 GiB default. A file inside the cache
# directory goes with nothing but the cache. It is also the file ccache reads when only CCACHE_DIR is set.
#
#   tools/ccache_config.sh <cache dir>
#
# Prints the cache directory's ccache.conf, or tools/ccache.conf itself when ccache is not installed (CI and release builds do
# not use it) or the cache directory cannot be written. Never fails: the justfile calls it on every invocation.
set -u
here=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd -P)
src="$here/tools/ccache.conf"
dir=${1:-}
command -v ccache >/dev/null 2>&1 || { echo "$src"; exit 0; }
[ -n "$dir" ] || { echo "$src"; exit 0; }
dest="$dir/ccache.conf"
if cmp -s "$src" "$dest" 2>/dev/null; then
  echo "$dest"
  exit 0
fi
tmp="$dest.$$.tmp"
if mkdir -p "$dir" 2>/dev/null && cp "$src" "$tmp" 2>/dev/null && mv -f "$tmp" "$dest" 2>/dev/null; then
  echo "$dest"
else
  rm -f "$tmp" 2>/dev/null
  echo "$src"
fi
