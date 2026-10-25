#include "core/build_identity.h"

// CMake sets these as compile definitions (root CMakeLists.txt:38-45).
// Fallbacks match the pattern in src/core/globals.h — a build without
// CMake (manual compiler invocation) gets safe defaults.

#ifndef NEVR_PROJECT_VERSION
#define NEVR_PROJECT_VERSION "0.0.0"
#endif

#ifndef NEVR_GIT_COMMIT_HASH
#define NEVR_GIT_COMMIT_HASH "unknown"
#endif

#ifndef NEVR_GIT_COMMIT_FULL
#define NEVR_GIT_COMMIT_FULL "0000000000000000000000000000000000000000"
#endif

#ifndef NEVR_GIT_DESCRIBE
#define NEVR_GIT_DESCRIBE "unknown"
#endif

// CMake sets CMAKE_BUILD_TYPE; plain Makefile/IDE builds do not, and
// a command-line -DCMAKE_BUILD_TYPE= overrides it. Fall back to a
// compile-time marker that makes the undefined case obvious.
#ifndef CMAKE_BUILD_TYPE
#define CMAKE_BUILD_TYPE "unknown-build-type"
#endif

// Stringify helper so the raw define survives the preprocessor pass.
#define NEVR_STR_(x) #x
#define NEVR_STR(x)  NEVR_STR_(x)

namespace nevr_build_identity {

// The identity literal: a leading NUL in the array so the string is NUL-bounded in the file whatever
// the linker places before it; the pointer handed out skips it. The version and the commit come from the
// same compile definitions as Info above (one code path).
static const char kIdentityArray[] = "\0NEVR-BUILD " NEVR_PROJECT_VERSION " " NEVR_GIT_COMMIT_FULL;

const char* IdentityLiteral() { return kIdentityArray + 1; }

const Info& Get() {
  static const Info info = []() {
    Info i;
    // NEVR_PROJECT_VERSION et al. are already string literals (defined by CMake's
    // add_compile_definitions with quoted values).  Assigning them directly
    // avoids NEVR_STR() double-stringification, which produced strings
    // containing literal double-quote characters that broke JSON encoding
    // in BuildLoginRequest (N146).
    i.project_version = NEVR_PROJECT_VERSION;
    i.git_commit      = NEVR_GIT_COMMIT_HASH;
    i.git_describe    = NEVR_GIT_DESCRIBE;
    i.build_type      = CMAKE_BUILD_TYPE;
    // git describe --dirty appends "-dirty" when the working tree has
    // uncommitted changes. Derive the flag from the string the build
    // system already computed rather than adding a second source of truth.
    i.is_dirty = (i.git_describe.find("-dirty") != std::string::npos);
    return i;
  }();
  return info;
}

}  // namespace nevr_build_identity
