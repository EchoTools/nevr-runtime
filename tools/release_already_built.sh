#!/usr/bin/env bash
# Does a release already carry the build's whole asset set? Prints `true` or `false`.
#
# usage: release_already_built.sh <vX.Y.Z> <owner/repo>
#
# build.yml's `guard` job calls this on every release event. A release whose draft was already built,
# signed and filled with the attested assets fires `published` when it is published; rebuilding it would
# replace the signed files. The signal is the release's asset NAMES: all five files this workflow
# publishes for version X.Y.Z are listed (the sealed zip, SHA256SUMS, RELEASE-NOTES.md and the dist
# zip). The release's pre-release flag is never read. A listing that fails (auth, the network, a bad
# tag) is an error (exit 1), not "false": a check that cannot run must not decide to rebuild.
set -euo pipefail
tag=${1:?usage: release_already_built.sh <vX.Y.Z> <owner/repo>}
repo=${2:?usage: release_already_built.sh <vX.Y.Z> <owner/repo>}
# The tag shape and the five names come from the one file tools/release_draft.sh also reads.
source "$(dirname "${BASH_SOURCE[0]}")/release_asset_names.sh"
if [[ ! "$tag" =~ $release_tag_regex ]]; then
  echo "release_already_built: tag '$tag' is not vX.Y.Z" >&2
  exit 1
fi
version=${BASH_REMATCH[1]}
# A failing name list must stop the script: a process substitution would swallow its exit status and an
# empty list would read as "every required file is present".
want_out=$(release_asset_names "$version") || { echo "release_already_built: the asset name list failed" >&2; exit 1; }
mapfile -t want <<<"$want_out"
if [ "${#want[@]}" -eq 0 ] || [ -z "${want[0]}" ]; then
  echo "release_already_built: the asset name list is empty; refusing to decide" >&2
  exit 1
fi
names=$(gh release view "$tag" --repo "$repo" --json assets --jq '.assets[].name')
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
