#!/usr/bin/env bash
# What kind of release does a tag have? For a build started by hand (workflow_dispatch) on a vX.Y.Z tag ref:
# it attaches its files to the tag's DRAFT release, and only to one that does not carry them yet.
#
# usage: release_draft.sh <vX.Y.Z>   with the tag's releases on stdin
#   stdin is a JSON array with one {"draft": bool, "assets": [names]} per release of that tag (the `drafts`
#   job of build.yml makes it: GitHub's "get a release by tag name" returns published releases only, and a
#   listing shows drafts only to a token with push access, so the listing is made in a job of its own with
#   the write token and no checkout; this script holds no token and makes no call).
#   prints `draft` when the tag has exactly one DRAFT release, no published one, and that draft carries none
#   of the build's five files; prints `none` (a dry run: nothing is uploaded) when the tag has no release or
#   has a published one.
#
# Exit 1: two drafts for the tag (ambiguous); a draft beside a published release (ambiguous); a draft that
# already carries ANY of the five names (the signer replaced them, or someone attached them: a second dispatch
# would put UNSIGNED bytes over them, and a partial draft needs a human); stdin that is not a JSON array, which
# is never read as "none". Exit 2: a tag that is not vX.Y.Z. One decision line goes to stderr either way.
set -euo pipefail
tag=${1:?usage: release_draft.sh <vX.Y.Z> (releases JSON on stdin)}

# The tag shape and the five names come from the one file tools/release_already_built.sh (the published
# guard) also reads.
source "$(dirname "${BASH_SOURCE[0]}")/release_asset_names.sh"
if ! [[ "$tag" =~ $release_tag_regex ]]; then
  echo "release_draft: '$tag' is not a vX.Y.Z tag" >&2
  exit 2
fi
version=${BASH_REMATCH[1]}

listing=$(cat)
if ! jq -e 'type == "array" and all(.[]; (.draft | type) == "boolean")' >/dev/null 2>&1 <<<"$listing"; then
  echo "release_draft: the release listing for $tag is not a JSON array of {draft, assets}; refusing to decide" >&2
  exit 1
fi
drafts=$(jq '[.[] | select(.draft)] | length' <<<"$listing")
published=$(jq '[.[] | select(.draft | not)] | length' <<<"$listing")
# A failing name list must stop the script: a process substitution would swallow its exit status and an
# empty list would read as "none of the built files is on the draft", so it would upload over signed files.
want_out=$(release_asset_names "$version") || { echo "release_draft: the asset name list failed; refusing to decide" >&2; exit 1; }
mapfile -t want <<<"$want_out"
if [ "${#want[@]}" -eq 0 ] || [ -z "${want[0]}" ]; then
  echo "release_draft: the asset name list is empty; refusing to decide" >&2
  exit 1
fi
mapfile -t on_drafts < <(jq -r '.[] | select(.draft) | .assets[]?' <<<"$listing")
present=()
for name in "${want[@]}"; do
  for have in "${on_drafts[@]}"; do
    [ "$have" = "$name" ] && { present+=("$name"); break; }
  done
done
decision() { echo "release_draft: $tag: $drafts draft(s), $published published, ${#present[@]} of ${#want[@]} built names on the draft(s): $1" >&2; }

if [ "$drafts" -eq 0 ]; then
  decision "none (a dry run: nothing is attached)"
  echo none
  exit 0
fi
if [ "$published" -ne 0 ]; then
  decision "refused (both a draft and a published release; refusing to guess which one)"
  exit 1
fi
if [ "$drafts" -ne 1 ]; then
  decision "refused ($drafts draft releases, expected one)"
  exit 1
fi
if [ "${#present[@]}" -ne 0 ]; then
  decision "refused (the draft already carries built assets: ${present[*]}; refusing to overwrite)"
  exit 1
fi
decision draft
echo draft
