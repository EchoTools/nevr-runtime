#include "runtime/host/host_identity.h"

// CMake defines these on this file alone (nevr_build_identity_define in src/runtime/CMakeLists.txt). The fallbacks
// are the ones src/core/build_identity.cpp uses, for a build without CMake.
#ifndef NEVR_PROJECT_VERSION
#define NEVR_PROJECT_VERSION "0.0.0"
#endif

#ifndef NEVR_GIT_COMMIT_FULL
#define NEVR_GIT_COMMIT_FULL "0000000000000000000000000000000000000000"
#endif

namespace nevr_host {
namespace {

// A leading NUL in the array so the string is NUL-bounded in the file whatever the linker places before it; the
// pointer handed out skips it. The same shape as nevr_build_identity::IdentityLiteral.
const char kIdentityArray[] = "\0NEVR-BUILD " NEVR_PROJECT_VERSION " " NEVR_GIT_COMMIT_FULL;

}  // namespace

const char* IdentityLiteral() noexcept { return kIdentityArray + 1; }

const char* Version() noexcept { return NEVR_PROJECT_VERSION; }

}  // namespace nevr_host
