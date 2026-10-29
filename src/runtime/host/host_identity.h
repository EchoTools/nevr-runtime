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

/// The NUL-bounded literal this binary embeds, "NEVR-HOST-BUILD <release|dev>": whether this host is the plugin
/// developers' build (NEVR_ALLOW_UNSIGNED_PLUGINS), which loads an unsigned nevr-runtime.dll. The host carries no
/// NEVR-PLUGIN-TRUST literal (that one is the runtime's), so this is what tools/package_release.py reads to refuse a
/// dev host in a package (#1122).
const char* BuildLiteral() noexcept;

/// The word alone: "release" or "dev".
const char* BuildWord() noexcept;

}  // namespace nevr_host
