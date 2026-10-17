#pragma once
// Self-check "credential_cache" (#562, docs/engine/remote-log.md): what the credential cache did in this process,
// one result per change, read from core/credential_cache_stats.h on the self-check flush (never on a game call):
//   save ok protection=<host_mode_600|dacl|mode_0600> saves=<S> refused=<R>       pass
//   save refused why=<reason> saves=<S> refused=<R>                                fail  (nothing was written)
//   cache reused no_sign_in_needed                                                 pass
//   cache not reused sign_in_required                                              pass  (the device-code flow follows)
// Under Wine/Proton the cache is written only after its host mode was read back as 600 (core/credential_write.h);
// a refusal means that player signs in again on the next launch. When several changes are pending, a refusal is said
// first; the rest follow on the next flush. The text carries outcomes and short reasons only: no token, no length,
// no path.
//
// What it cannot see: the sign-in itself, or whether the next launch reuses the cache (that launch reports
// `cache reused`). Header only on top of the self-check unit (compat/self_check.h) and the stats header.

#include <cstdint>
#include <mutex>
#include <string>

#include "core/credential_cache_stats.h"
#include "runtime/compat/self_check.h"

namespace nevr_credential_cache_check {

namespace detail {
struct State {
  std::mutex mutex;
  nevr_credential_stats::Snapshot reported;  // the counts already said
};
inline State& S() {
  static State* const state = new State();  // never destroyed (the Quest sentinel registers no atexit)
  return *state;
}
}  // namespace detail

inline constexpr char kName[] = "credential_cache";
inline constexpr char kExpected[] =
    "a credential cache save writes the file (Wine: after the host mode was read back as 600), and a later launch reuses it";

/// The probe the unit calls on every Flush: true with the next change to say, false when there is none.
inline bool ProcessProbe(nevr_self_check::Observation* out) {
  const nevr_credential_stats::Snapshot now = nevr_credential_stats::Get();
  detail::State& s = detail::S();
  std::lock_guard<std::mutex> lock(s.mutex);
  nevr_credential_stats::Snapshot& said = s.reported;
  const std::string counts = " saves=" + std::to_string(now.saved) + " refused=" + std::to_string(now.refused);
  if (now.refused > said.refused) {
    said.refused = now.refused;
    out->observed = "save refused why=" + now.lastRefusal + counts;
    out->pass = false;
    return true;
  }
  if (now.saved > said.saved) {
    said.saved = now.saved;
    out->observed = "save ok protection=" + now.lastProtection + counts;
    out->pass = true;
    return true;
  }
  if (now.reused > said.reused) {
    said.reused = now.reused;
    out->observed = "cache reused no_sign_in_needed";
    out->pass = true;
    return true;
  }
  if (now.signInNoCache + now.signInUnusable > said.signInNoCache + said.signInUnusable) {
    said.signInNoCache = now.signInNoCache;
    said.signInUnusable = now.signInUnusable;
    out->observed = "cache not reused sign_in_required";
    out->pass = true;
    return true;
  }
  return false;
}

/// Registers the check with the unit; the unit calls `probe` on every Flush.
inline nevr_self_check::CheckId Register(nevr_self_check::Probe probe) {
  return nevr_self_check::Register({kName, kExpected, probe});
}

/// Registers the check(s) with the unit (placeholder: one check, filled in by the next commit).
inline void Register() { (void)Register(&ProcessProbe); }

/// Test hook: forget what has been said.
inline void ResetForTest() {
  detail::State& s = detail::S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.reported = nevr_credential_stats::Snapshot();
}

}  // namespace nevr_credential_cache_check
