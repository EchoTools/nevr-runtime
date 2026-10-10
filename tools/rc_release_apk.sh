#!/usr/bin/env bash
# Fetch the Quest APK attached to a release candidate's GitHub release into <dir>.
#
# usage: rc_release_apk.sh <tag> <owner/repo> <dir>   (prints the downloaded path)
#
# A release candidate ships both artifacts, so this never degrades silently: it lists the release's
# assets first (`gh release view`), fails when the listing fails (auth, a draft the token cannot see, a
# bad tag, the network), fails when no *-quest.apk is listed, fails when more than one is, and fails when
# the download of the listed one fails. gh's own error output is left on stderr.
set -euo pipefail
tag=$1
repo=$2
dir=$3

names=$(gh release view "$tag" --repo "$repo" --json assets --jq '.assets[].name')
apks=$(printf '%s\n' "$names" | grep -E -- '-quest\.apk$' || true)
count=$(printf '%s' "$apks" | grep -c . || true)
if [ "$count" -eq 0 ]; then
  echo "rc_release_apk: release $tag lists no *-quest.apk; a release candidate ships both the Windows zip and the Quest APK" >&2
  exit 1
fi
if [ "$count" -gt 1 ]; then
  echo "rc_release_apk: release $tag lists $count *-quest.apk assets, expected one" >&2
  exit 1
fi
mkdir -p "$dir"
gh release download "$tag" --repo "$repo" --pattern "$apks" --dir "$dir" --clobber >&2
if [ ! -s "$dir/$apks" ]; then
  echo "rc_release_apk: $dir/$apks is missing or empty after the download" >&2
  exit 1
fi
printf '%s\n' "$dir/$apks"
