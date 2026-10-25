#pragma once
// Self-checks "credential_cache" and "credential_cache_refused" (#562, docs/engine/remote-log.md): what the credential
// cache did in this process, read from core/credential_cache_stats.h on the self-check flush (never on a game call).
// `credential_cache`, one result per change:
//   save ok protection=<host_mode_600|dacl|mode_0600> saves=<S> refused=<R>     pass   the first save, then only when the
//                                                                                protection changes (not every refresh)
//   cache reused no_sign_in_needed                                              pass
//   cache not reused sign_in_required reason=no_cache                            pass   the device-code flow follows
//   cache not reused sign_in_required reason=cache_present_unusable              pass   a cache was there but its refresh
//                                                                                failed or both tokens had expired
// `credential_cache_refused` (its own check, so its per-check cap is never used up by saves or reuse):
//   save refused why=<reason> saves=<S> refused=<R>                              fail   nothing was written; under
//                                                                                Wine/Proton the host mode was not read back
//                                                                                as 600, so that player signs in again
// Read `observed`, not `pass`: a sign-in that was needed passes. The text carries outcomes and short reasons only: no
// token, no length, no digest, no path.
//
// What it cannot see: the sign-in itself, or whether the next launch reuses the cache (that launch reports `cache reused`).
// Header only on top of the self-check unit (compat/self_check.h) and the stats header.

#include <cstdint>
#include <mutex>
#include <string>

#include "core/credential_cache_stats.h"
#include "runtime/compat/self_check.h"

namespace nevr_credential_cache_check {

namespace detail {
struct State {
  std::mutex mutex;
  nevr_credential_stats::Snapshot reported;  // the counts already said (saves, reuse, sign-in)
  std::uint64_t reportedRefused = 0;         // the refusals already said, by the refusal check
};
inline State& S() {
  static State* const state = new State();  // never destroyed (the Quest sentinel registers no atexit)
  return *state;
}
}  // namespace detail

inline constexpr char kName[] = "credential_cache";
inline constexpr char kRefusedName[] = "credential_cache_refused";
inline constexpr char kExpected[] =
    "a credential cache save writes the file (Wine: after the host mode was read back as 600), and a later launch reuses it";
inline constexpr char kRefusedExpected[] = "no credential cache save is refused";

/// The probe of `credential_cache`, called by the unit on every Flush: true with the next change to say. A save is said
/// once, then only when how the file is protected changes (a refresh saves about six times in a long session, and each
/// result counts against the unit's per-check cap); the counts it names are the running ones.
inline bool ProcessProbe(nevr_self_check::Observation* out) {
  const nevr_credential_stats::Snapshot now = nevr_credential_stats::Get();
  detail::State& s = detail::S();
  std::lock_guard<std::mutex> lock(s.mutex);
  nevr_credential_stats::Snapshot& said = s.reported;
  if (now.saved > said.saved) {
    const bool say = said.saved == 0 || now.lastProtection != said.lastProtection;
    said.saved = now.saved;
    said.lastProtection = now.lastProtection;
    if (say) {
      out->observed = "save ok protection=" + now.lastProtection + " saves=" + std::to_string(now.saved) +
                      " refused=" + std::to_string(now.refused);
      out->pass = true;
      return true;
    }
  }
  if (now.reused > said.reused) {
    said.reused = now.reused;
    out->observed = "cache reused no_sign_in_needed";
    out->pass = true;
    return true;
  }
  if (now.signInNoCache > said.signInNoCache) {
    said.signInNoCache = now.signInNoCache;
    out->observed = "cache not reused sign_in_required reason=no_cache";
    out->pass = true;
    return true;
  }
  if (now.signInUnusable > said.signInUnusable) {
    said.signInUnusable = now.signInUnusable;
    out->observed = "cache not reused sign_in_required reason=cache_present_unusable";
    out->pass = true;
    return true;
  }
  return false;
}

/// The probe of `credential_cache_refused`: a refusal is its own check, with its own per-check cap, so a long
/// session's saves and reuse can never use up the slots a refusal needs.
inline bool RefusalProbe(nevr_self_check::Observation* out) {
  const nevr_credential_stats::Snapshot now = nevr_credential_stats::Get();
  detail::State& s = detail::S();
  std::lock_guard<std::mutex> lock(s.mutex);
  if (now.refused <= s.reportedRefused) return false;
  s.reportedRefused = now.refused;
  out->observed = "save refused why=" + now.lastRefusal + " saves=" + std::to_string(now.saved) +
                  " refused=" + std::to_string(now.refused);
  out->pass = false;
  return true;
}

/// Registers both checks with the unit; it calls their probes on every Flush.
inline void Register() {
  (void)nevr_self_check::Register({kName, kExpected, &ProcessProbe});
  (void)nevr_self_check::Register({kRefusedName, kRefusedExpected, &RefusalProbe});
}

/// Test hook: forget what has been said.
inline void ResetForTest() {
  detail::State& s = detail::S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.reported = nevr_credential_stats::Snapshot();
  s.reportedRefused = 0;
}

}  // namespace nevr_credential_cache_check
