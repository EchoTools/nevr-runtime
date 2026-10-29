#!/usr/bin/env bash
# The files a release's build publishes, by name: the one list that tools/release_already_built.sh (the
# published guard) and tools/release_draft.sh (the draft attach) both read, so they cannot disagree about
# what "already built" means. Sourced, not run.
#
#   release_asset_names <X.Y.Z>   prints the two names, one per line (RELEASING.md, "Assets"):
#     nevr-runtime-vX.Y.Z-windows.zip   the Windows package, sealed (BugSplat64.dll, nevr-runtime.dll, echovr_server.exe, nevr-install.exe, install.ps1,
#                                       uninstall.ps1, quest-diagnose.ps1, README.txt, SIGNING.txt, SHA256SUMS)
#     nevr-runtime-vX.Y.Z-quest-libovrplatformloader.so   the Quest library (a loose ELF: attested and not
#                                       signed; declared in the signer's config/sources.yml)
# There is no top-level SHA256SUMS and no RELEASE-NOTES.md: the notes and a SHA-256 table of both assets are the
# release's body, written after signing (tools/package_release.py notes). The zip's own SHA256SUMS stays: install.ps1
# and uninstall.ps1 check both DLLs against it.
# The Quest library's name has ONE source, release_quest_asset_name: build.yml's quest job names its output
# with it, so the file it uploads is the one this list (and so the published guard) expects.
# The .tar.zst archives are a workflow artifact and never a release asset.
# The tag a release has: a plain semver vX.Y.Z (no leading zeros), or an rc vX.Y.Z-rc.N (#714); BASH_REMATCH[1] is the
# version, X.Y.Z or X.Y.Z-rc.N.
release_tag_regex='^v((0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*)(-rc\.[1-9][0-9]*)?)$'

release_quest_asset_name() {
  local version=${1:?usage: release_quest_asset_name <X.Y.Z>}
  printf '%s\n' "nevr-runtime-v$version-quest-libovrplatformloader.so"
}

release_asset_names() {
  local version=${1:?usage: release_asset_names <X.Y.Z>}
  printf '%s\n' "nevr-runtime-v$version-windows.zip"
  release_quest_asset_name "$version"
}
