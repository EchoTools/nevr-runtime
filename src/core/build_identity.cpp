#include "core/build_identity.h"

#include <atomic>

#include "core/build_stamp.h"

// CMake sets CMAKE_BUILD_TYPE; plain Makefile/IDE builds do not, and
// a command-line -DCMAKE_BUILD_TYPE= overrides it. Fall back to a
// compile-time marker that makes the undefined case obvious.
#ifndef CMAKE_BUILD_TYPE
#define CMAKE_BUILD_TYPE "unknown-build-type"
#endif

namespace nevr_build_identity {

// The identity literal is written by the build's one stamp unit (src/core/build_stamp.h): NUL-bounded, one per binary.
const char* IdentityLiteral() { return nevr_build_stamp::IdentityLiteral(); }

// Written once by the runtime's self-check at boot and read by the registration; atomic so a read on another thread is never torn.
static std::atomic<const char*> g_provenance{"unchecked"};

const char* Provenance() { return g_provenance.load(); }

void SetProvenance(const char* word) { g_provenance.store(word); }

const Info& Get() {
  static const Info info = []() {
    Info i;
    // The version and the commit come from the build's stamp unit (src/core/build_stamp.h), read at build time.
    i.project_version = nevr_build_stamp::ProjectVersion();
    i.git_commit      = nevr_build_stamp::CommitShort();
    i.git_describe    = nevr_build_stamp::Describe();
    i.build_type      = CMAKE_BUILD_TYPE;
    // git describe --dirty appends "-dirty" when the working tree has uncommitted changes; the stamp unit says so.
    i.is_dirty = nevr_build_stamp::IsDirty();
    return i;
  }();
  return info;
}

std::string MenuVersionLabel() { return std::string("nEVR ") + Get().project_version; }

}  // namespace nevr_build_identity
