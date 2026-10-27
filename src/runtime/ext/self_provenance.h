// self_provenance.h — whether this runtime is the signed release or a self-built one (#842, ADR 0010 section 1).
//
// The runtime checks its own Authenticode signature with the same verifier that vets plugins (plugin_signature.h,
// the production policy of plugin_trust.h). The decision is pure (Decide); CheckOwnModule only reads the file this
// code was loaded from and passes the verdict to Decide. Nothing here refuses to run: the result is a label, logged at
// boot, carried in the server registration (nevr_build_identity::Provenance) and shown to players.
//
// Windows only (plugin_signature.h).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "runtime/ext/plugin_signature.h"

namespace nevr_self_provenance {

struct Result {
  // The word the registration and the boot log carry: "signed-release", "self-built" or "dev".
  const char* word = "unchecked";
  std::string version;  // the build's own version string (nevr_build_identity project_version)
  std::string reason;   // empty for a signed release; else why it is not one (a Verdict's Describe(), or "dev version")
};

// Pure: the label for a build with this version, whether it is a dev build (plugin_trust IsDevBuild), and the verdict of
// the verifier on its own file. A dev version or a dev build is "dev" whatever the signature says. A release version whose
// file the verifier accepts is "signed-release". Anything else is "self-built", with the verdict as the reason (Unsigned,
// SubjectMismatch, MissingEku, ChainStatus, ...).
Result Decide(std::string_view projectVersion, bool devBuild, const nevr_plugin_signature::Verdict& verdict);

// Verifies the module this function lives in (its own file, opened read-only with FILE_SHARE_READ) under `policy`, then
// returns Decide over the verdict. A file that cannot be opened is "self-built" with reason "Unreadable".
Result CheckOwnModule(std::string_view projectVersion, bool devBuild, const nevr_plugin_signature::Policy& policy,
                      int64_t nowUnix);

}  // namespace nevr_self_provenance
