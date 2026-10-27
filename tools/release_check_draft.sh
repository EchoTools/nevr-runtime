#!/usr/bin/env bash
# The checks of RELEASING.md "Check the draft" (unsigned) and "After the signer has replaced assets" (--signed), run on a
# release draft. Usage: tools/release_check_draft.sh [--signed] vX.Y.Z
#
# Every check fails the script (exit 1) with the check's name; nothing is skipped and no failure is swallowed. The
# attestation loop is not run with --signed: the signer replaced the zip, so its attestation no longer matches (the
# signer's decision record is the evidence for those bytes, RELEASING.md "Sign").
set -euo pipefail

signed=0
if [ "${1:-}" = "--signed" ]; then
  signed=1
  shift
fi
tag="${1:?usage: release_check_draft.sh [--signed] vX.Y.Z}"
version="${tag#v}"
repo="${GITHUB_REPOSITORY:-EchoTools/nevr-runtime}"
zip="nevr-runtime-v${version}-windows.zip"
quest="nevr-runtime-v${version}-quest-libovrplatformloader.so"

fail() { echo "release_check_draft: FAILED: $1" >&2; exit 1; }
step() { echo "== $1"; }

step "the draft carries exactly the two names"
names=$(gh release view "$tag" --repo "$repo" --json assets --jq '.assets[].name' | sort)
want=$(printf '%s\n%s\n' "$quest" "$zip" | sort)
[ "$names" = "$want" ] || fail "assets are [$names], expected [$want]"
echo "$names"

work=$(mktemp -d)
gh release download "$tag" --repo "$repo" -D "$work"

if [ "$signed" -eq 0 ]; then
  step "attestations verify (each file exits 0)"
  for f in "$work"/*; do
    gh attestation verify "$f" -R "$repo" >/dev/null || fail "attestation of $(basename "$f")"
    echo "ok $(basename "$f")"
  done
fi

unz=$(mktemp -d)
unzip -q "$work/$zip" -d "$unz"
step "SHA256SUMS inside the zip"
(cd "$unz" && sha256sum -c SHA256SUMS) || fail "SHA256SUMS does not match the files in the zip"

step "NEVR-BUILD literal: two identical lines, bound to the tag's commit"
git fetch --force --quiet origin "refs/tags/$tag:refs/tags/$tag" || fail "fetch of tag $tag"
commit=$(git rev-parse "$tag^{commit}") || fail "tag $tag does not resolve to a commit"
lines=$(cd "$unz" && strings -a BugSplat64.dll nevr-runtime.dll | grep -a '^NEVR-BUILD' | sort -u) \
  || fail "no NEVR-BUILD literal in the DLLs"
echo "$lines"
[ "$(echo "$lines" | wc -l)" -eq 1 ] || fail "NEVR-BUILD literals differ between the two DLLs"
[ "$lines" = "NEVR-BUILD $version $commit" ] || fail "NEVR-BUILD line [$lines] is not 'NEVR-BUILD $version $commit'"

step "VERSIONINFO of the three files"
python3 tools/pe_version_info.py check --dll "$unz/BugSplat64.dll" --runtime-dll "$unz/nevr-runtime.dll" \
  --exe "$unz/echovr_server.exe" || fail "VERSIONINFO check"

echo "release_check_draft: OK $tag (signed=$signed)"
