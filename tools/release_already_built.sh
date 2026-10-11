#!/usr/bin/env bash
# Does a release already carry the build's whole asset set? Prints `true` or `false`.
#
# usage: release_already_built.sh <vX.Y.Z> <owner/repo>
#
# build.yml's `guard` job calls this on every release event. A release whose draft was already built,
# signed and filled with the attested assets fires `published` when it is published; rebuilding it would
# replace the signed files. The signal is the release's asset NAMES: all five files this workflow
# publishes for version X.Y.Z are listed (the sealed zip, SHA256SUMS, RELEASE-NOTES.md and the two dist
# zips). The release's pre-release flag is never read. A listing that fails (auth, the network, a bad
# tag) is an error (exit 1), not "false": a check that cannot run must not decide to rebuild.
set -euo pipefail
tag=${1:?usage: release_already_built.sh <vX.Y.Z> <owner/repo>}
repo=${2:?usage: release_already_built.sh <vX.Y.Z> <owner/repo>}
if [[ ! "$tag" =~ ^v([0-9]+\.[0-9]+\.[0-9]+)$ ]]; then
  echo "release_already_built: tag '$tag' is not vX.Y.Z" >&2
  exit 1
fi
version=${BASH_REMATCH[1]}
names=$(gh release view "$tag" --repo "$repo" --json assets --jq '.assets[].name')
want=("nevr-runtime-v$version-windows.zip" SHA256SUMS RELEASE-NOTES.md
      "nevr-runtime-v$version.zip" "nevr-runtime-v$version-lite.zip")
missing=()
for name in "${want[@]}"; do
  grep -qxF -- "$name" <<<"$names" || missing+=("$name")
done
if [ "${#missing[@]}" -eq 0 ]; then
  echo true
else
  echo "release_already_built: $tag lacks: ${missing[*]}" >&2
  echo false
fi
