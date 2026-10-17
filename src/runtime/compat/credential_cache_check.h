#pragma once
// Self-check "credential_cache" (#562, docs/engine/remote-log.md): placeholder, filled in by the next commit.

#include "runtime/compat/self_check.h"

namespace nevr_credential_cache_check {

inline bool ProcessProbe(nevr_self_check::Observation*) { return false; }
inline void ResetForTest() {}
inline nevr_self_check::CheckId Register(nevr_self_check::Probe probe) {
  return nevr_self_check::Register({"credential_cache", "placeholder", probe});
}

}  // namespace nevr_credential_cache_check
