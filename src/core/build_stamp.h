#pragma once

// The build's version and commit (#1935). cmake/nevr_build_stamp.cmake writes the one generated translation unit that
// defines these functions, at build time, from the committed VERSION file and from git. Nothing else takes the version
// or the commit as a compile definition: a source calls these, and a new commit or an uncommitted edit recompiles that
// one unit.
namespace nevr_build_stamp {

/// X.Y.Z, from the committed VERSION file.
const char* Version() noexcept;

/// The stamped version: X.Y.Z for a CI build on the tag vX.Y.Z, X.Y.Z-dev.<distance>+<short commit> otherwise.
const char* ProjectVersion() noexcept;

/// The short commit (git rev-parse --short HEAD) and the full commit (40 hex), at build time.
const char* CommitShort() noexcept;
const char* CommitFull() noexcept;

/// git describe --always --dirty --broken --abbrev=12, at build time. Ends in "-dirty" when the tree had uncommitted
/// changes.
const char* Describe() noexcept;

/// True when Describe() ends in "-dirty".
bool IsDirty() noexcept;

/// True for a CI build exactly on the tag of VERSION (the stamp is X.Y.Z).
bool IsRelease() noexcept;

/// The Quest versionCode of this build (tools/quest_apk_version.py), or 0 when the build did not compute one.
int ApkVersionCode() noexcept;

/// The identity literal embedded in the binary, without its leading NUL: "NEVR-BUILD <version> <commit40>".
const char* IdentityLiteral() noexcept;

}  // namespace nevr_build_stamp
