#pragma once

// The host's own build identity (docs/adr/0012-static-install-root.md). BugSplat64.dll links no nevr_core, so it
// cannot use src/core/build_identity.cpp; it carries the same literal from the same compile definitions, in a file
// of its own. The runtime and the host of one build state one version and one commit, and
// tools/package_release.py refuses a pair that disagrees.

namespace nevr_host {

/// The NUL-bounded literal this binary embeds, "NEVR-BUILD <version> <commit40>" (the format of
/// src/core/build_identity.h). The version and commit are the build's own compile definitions.
const char* IdentityLiteral() noexcept;

/// The version alone, as the build stamped it (X.Y.Z or X.Y.Z-dev.N+sha).
const char* Version() noexcept;

}  // namespace nevr_host
