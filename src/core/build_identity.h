#pragma once

#include <cstdint>
#include <string>

/// Build identity — compile-time constants, never measured.
///
/// This exists because the login payload carried no NEVR version information.
/// The server registration already sends GIT_DESCRIBE in its version field;
/// the client login sent nothing. A version number a peer cannot verify is a
/// courtesy, not an identity — if two different binaries can present the same
/// string, the string is not an identity and must not be treated as one (N112).
///
/// Every field here comes from a CMake compile definition set at build time.
/// Nothing is measured at runtime; nothing is guessed.
namespace BuildIdentity {

struct Info {
  /// Semver from git tags, e.g. "1.2.3". Set by CMake from
  /// set_project_version_from_git.cmake.
  std::string project_version;

  /// Short commit hash, e.g. "abc1234". Set by CMake from git rev-parse.
  std::string git_commit;

  /// git describe --tags --always --dirty, e.g. "v1.2.3-5-gabc1234-dirty".
  /// This is the single highest-value identity string — a tagged release
  /// reports "v1.2.3", a build from a dirty tree reports "-dirty". Already
  /// sent in the server registration; now also sent in the client login.
  std::string git_describe;

  /// CMake build type: Debug, Release, RelWithDebInfo, MinSizeRel.
  std::string build_type;

  /// True when git describe reported -dirty (uncommitted changes at build
  /// time). Derived from git_describe, not a separate CMake variable.
  bool is_dirty = false;
};

/// Compile-time constants, cached on first call. These values are baked into
/// the binary and never change during the process lifetime.
const Info& Get();

}  // namespace BuildIdentity
