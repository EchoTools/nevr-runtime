#!/usr/bin/env bash
# The files a release's build publishes, by name: the one list that tools/release_already_built.sh (the
# published guard) and tools/release_draft.sh (the draft attach) both read, so they cannot disagree about
# what "already built" means. Sourced, not run.
#
#   release_asset_names <X.Y.Z>   prints the five names, one per line:
#     nevr-runtime-vX.Y.Z-windows.zip   the sealed zip
#     SHA256SUMS, RELEASE-NOTES.md      its checksums and notes
#     nevr-runtime-vX.Y.Z.zip           the dist archive
#     nevr-runtime-vX.Y.Z-lite.zip      the stripped dist archive
# The .tar.zst archives are a workflow artifact and never a release asset.
# The tag a release has: a plain semver vX.Y.Z (no leading zeros); BASH_REMATCH[1] is X.Y.Z.
release_tag_regex='^v((0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*))$'

release_asset_names() {
  local version=${1:?usage: release_asset_names <X.Y.Z>}
  printf '%s\n' "nevr-runtime-v$version-windows.zip" SHA256SUMS RELEASE-NOTES.md \
    "nevr-runtime-v$version.zip" "nevr-runtime-v$version-lite.zip"
}
