#!/usr/bin/env bash
# What kind of release does a tag have? For a build started by hand (workflow_dispatch) on a vX.Y.Z tag ref:
# it attaches its files to the tag's DRAFT release, and only to one that does not carry them yet.
#
# usage: release_draft.sh <vX.Y.Z> <owner/repo>
#   prints `draft` when the tag has exactly one DRAFT release, no published one, and that draft carries none
#   of the build's five files; prints `none` (a dry run: nothing is uploaded) when the tag has no release or
#   has a published one.
#
# GitHub's "get a release by tag name" returns published releases only, so the draft is found by listing the
# repository's releases (a listing shows drafts only to a token with push access: contents: write). Exit 1:
# two drafts for the tag (ambiguous); a draft beside a published release (ambiguous); a draft that already
# carries ANY of the five names (the signer replaced them, or someone attached them: a second dispatch would
# put UNSIGNED bytes over them, and a partial draft needs a human); a failed listing, which is never read as
# "none". Exit 2: a tag that is not vX.Y.Z, before any call (the tag is interpolated into the jq filter).
set -euo pipefail
tag=${1:?usage: release_draft.sh <vX.Y.Z> <owner/repo>}
repo=${2:?usage: release_draft.sh <vX.Y.Z> <owner/repo>}

# The tag shape and the five names come from the one file tools/release_already_built.sh (the published
# guard) also reads.
source "$(dirname "${BASH_SOURCE[0]}")/release_asset_names.sh"
if ! [[ "$tag" =~ $release_tag_regex ]]; then
  echo "release_draft: '$tag' is not a vX.Y.Z tag" >&2
  exit 2
fi
version=${BASH_REMATCH[1]}

# One line per release of the tag (`draft` or `published`) and, for a draft, one `asset:<name>` line per asset.
lines=$(gh api --paginate "repos/$repo/releases" --jq \
  ".[] | select(.tag_name == \"$tag\") | (if .draft then \"draft\" else \"published\" end), (if .draft then (.assets[]?.name | \"asset:\" + .) else empty end)")
drafts=$(printf '%s\n' "$lines" | grep -c '^draft$' || true)
published=$(printf '%s\n' "$lines" | grep -c '^published$' || true)

if [ "$drafts" -eq 0 ]; then
  if [ "$published" -ne 0 ]; then
    echo "release_draft: $tag has a published release; this build is a dry run and attaches nothing" >&2
  else
    echo "release_draft: no release for $tag; this build is a dry run and attaches nothing" >&2
  fi
  echo none
  exit 0
fi
if [ "$published" -ne 0 ]; then
  echo "release_draft: $tag has both a draft and a published release; refusing to guess which one" >&2
  exit 1
fi
if [ "$drafts" -ne 1 ]; then
  echo "release_draft: $drafts draft releases for $tag, expected one" >&2
  exit 1
fi
mapfile -t want < <(release_asset_names "$version")
present=()
for name in "${want[@]}"; do
  grep -qxF -- "asset:$name" <<<"$lines" && present+=("$name")
done
if [ "${#present[@]}" -ne 0 ]; then
  echo "release_draft: draft $tag already carries built assets; refusing to overwrite (${present[*]})" >&2
  exit 1
fi
echo draft
