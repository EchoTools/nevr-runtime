#!/usr/bin/env bash
# The files a release's build publishes, by name: the one list that tools/release_already_built.sh (the
# published guard) and tools/release_draft.sh (the draft attach) both read, so they cannot disagree about
# what "already built" means. Sourced, not run.
#
#   release_asset_names <X.Y.Z>   prints the three names, one per line (RELEASING.md, "Assets"):
#     nevr-runtime-vX.Y.Z-windows.zip   the Windows package, sealed (BugSplat64.dll, nevr-runtime.dll, echovr_server.exe, install-nevr-runtime.exe, install.ps1,
#                                       uninstall.ps1, quest-diagnose.ps1, README.txt, SIGNING.txt, SHA256SUMS)
#     nevr-runtime-vX.Y.Z-quest-libovrplatformloader.so   the Quest library (a loose ELF: attested and not
#                                       signed; declared in the signer's config/sources.yml)
#     nevr-runtime-vX.Y.Z-quest-runtime.so   the Quest runtime library, libnevr-runtime.so under the release's name (a
#                                       loose ELF: attested and not signed; the kind quest-runtime in the manifest, DESIGN.md piece 17)
# There is no top-level SHA256SUMS and no RELEASE-NOTES.md: the release's body is a header, a player line and one line
# per file, written after signing (tools/package_release.py notes); it holds no hash. The zip's own SHA256SUMS stays: install.ps1
# and uninstall.ps1 check both DLLs against it.
# The Quest names have ONE source each, release_quest_asset_name and release_quest_runtime_asset_name: build.yml's quest job
# names its outputs with them, so the files it uploads are the ones this list (and so the published guard) expects.
# The .tar.zst archives are a workflow artifact and never a release asset.
# The tag a release has: a plain semver vX.Y.Z (no leading zeros); BASH_REMATCH[1] is X.Y.Z.
release_tag_regex='^v((0|[1-9][0-9]*)\.(0|[1-9][0-9]*)\.(0|[1-9][0-9]*))$'

release_quest_asset_name() {
  local version=${1:?usage: release_quest_asset_name <X.Y.Z>}
  printf '%s\n' "nevr-runtime-v$version-quest-libovrplatformloader.so"
}

release_quest_runtime_asset_name() {
  local version=${1:?usage: release_quest_runtime_asset_name <X.Y.Z>}
  printf '%s\n' "nevr-runtime-v$version-quest-runtime.so"
}

release_asset_names() {
  local version=${1:?usage: release_asset_names <X.Y.Z>}
  printf '%s\n' "nevr-runtime-v$version-windows.zip"
  release_quest_asset_name "$version"
  release_quest_runtime_asset_name "$version"
}
