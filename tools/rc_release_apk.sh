#!/usr/bin/env bash
# Fetch the Quest APK attached to a release candidate's GitHub release into <dir>.
#
# usage: rc_release_apk.sh <tag> <owner/repo> <dir>   (prints the downloaded path, or nothing)
#
# A release candidate is the Windows zip; a Quest APK is optional (the Quest build is not part of the
# public set). So a release that lists no *-quest.apk is a zip-only candidate: nothing is printed and
# the script exits 0. Nothing else degrades silently: it lists the release's assets first
# (`gh release view`) and fails when the listing fails (auth, a draft the token cannot see, a bad tag,
# the network), fails when more than one *-quest.apk is listed, and fails when the download of the listed
# one fails. Whether an attached APK is really this candidate's is package_rc.py seal's check, not this
# script's. gh's own error output is left on stderr.
set -euo pipefail
tag=$1
repo=$2
dir=$3

names=$(gh release view "$tag" --repo "$repo" --json assets --jq '.assets[].name')
apks=$(printf '%s\n' "$names" | grep -E -- '-quest\.apk$' || true)
count=$(printf '%s' "$apks" | grep -c . || true)
if [ "$count" -eq 0 ]; then
  echo "rc_release_apk: release $tag lists no *-quest.apk: a zip-only release candidate" >&2
  exit 0
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
