#include "runtime/host/host_identity.h"

#include <cstddef>

#include "core/build_stamp.h"

namespace nevr_host {
namespace {

// NEVR_ALLOW_UNSIGNED_PLUGINS is defined on this file by src/runtime/CMakeLists.txt, as it is for plugin_trust.cpp.
#ifdef NEVR_ALLOW_UNSIGNED_PLUGINS
const char kBuildArray[] = "\0NEVR-HOST-BUILD dev";
#else
const char kBuildArray[] = "\0NEVR-HOST-BUILD release";
#endif
constexpr std::size_t kBuildWordOffset = sizeof("\0NEVR-HOST-BUILD ") - 1;

}  // namespace

// The identity literal is the build's one stamp unit's (src/core/build_stamp.h): NUL-bounded, the same text as the runtime's.
const char* IdentityLiteral() noexcept { return nevr_build_stamp::IdentityLiteral(); }

const char* BuildLiteral() noexcept { return kBuildArray + 1; }

const char* BuildWord() noexcept { return kBuildArray + kBuildWordOffset; }

const char* Version() noexcept { return nevr_build_stamp::ProjectVersion(); }

}  // namespace nevr_host
