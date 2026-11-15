#include "core/update/core_ffi.h"

#include <string>

#ifdef NEVR_UPDATE_CORE_LINKED
#include <memory>

#include "installer/include/nevr_install.h"
#endif

namespace nevr_update {

#ifdef NEVR_UPDATE_CORE_LINKED
// The core's C ABI, statically linked into the runtime (ADR 0024, section 3: one signed file, no extra DLL for the plugin gate to
// verify). One session per check. The session is used from one thread at a time: the check thread.
Core MakeFfiCore(const char* profile_json) {
  // An empty profile is the PC stage, NevrPcStage; any other profile goes to NevrStage (the Quest's quest-runtime profile).
  const std::string profile = profile_json != nullptr ? profile_json : "";
  auto session = std::make_shared<NevrSession*>(nullptr);
  Core core;
  core.open = [session](const std::string& config_json, std::string* error) {
    NevrSession* s = nullptr;
    if (NevrInstallOpen(config_json.c_str(), &s) != NEVR_OK || s == nullptr) {
      if (error != nullptr) *error = "NevrInstallOpen failed";
      return false;
    }
    *session = s;
    return true;
  };
  core.manifest_fetch = [session](std::string* error) {
    if (NevrManifestFetch(*session) == NEVR_OK) return true;
    if (error != nullptr) *error = NevrLastError(*session);
    return false;
  };
  core.stage = [session, profile](const std::string& root_dir, const std::string& current_json, std::string* result,
                                  std::string* error) {
    char* out = nullptr;
    const int status = profile.empty()
                           ? NevrPcStage(*session, root_dir.c_str(), current_json.c_str(), &out)
                           : NevrStage(*session, root_dir.c_str(), profile.c_str(), current_json.c_str(), &out);
    if (status != NEVR_OK) {
      if (error != nullptr) *error = NevrLastError(*session);
      if (out != nullptr) NevrStringFree(out);
      return false;
    }
    if (out != nullptr) {
      *result = out;
      NevrStringFree(out);
    }
    return true;
  };
  core.run_end = [session](const std::string& state, const std::string& reason) {
    if (*session != nullptr) NevrRunEnd(*session, state.c_str(), reason.c_str());
  };
  core.discard = [session](const std::string& root_dir, const std::string& version, const std::string& sha256, const std::string& reason,
                           std::string* error) {
    if (*session != nullptr && NevrPcDiscard(*session, root_dir.c_str(), version.c_str(), sha256.c_str(), reason.c_str()) == NEVR_OK) return true;
    if (error != nullptr && *session != nullptr) *error = NevrLastError(*session);
    return false;
  };
  core.close = [session] {
    if (*session != nullptr) NevrClose(*session);
    *session = nullptr;
  };
  return core;
}
#else
// A build configured without the core library has no core to call: the check ends in `update check action=failed reason=core_open_failed`.
Core MakeFfiCore(const char* /*profile_json*/) {
  Core core;
  core.open = [](const std::string&, std::string* error) {
    if (error != nullptr) *error = "this build does not link the core install method";
    return false;
  };
  return core;
}
#endif

}  // namespace nevr_update
