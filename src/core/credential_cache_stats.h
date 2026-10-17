#pragma once
// What the credential cache did in this process (#562): every save, refused or written, and whether the cached
// credentials were used or the player had to sign in. The cache writer (core/auth_token.h SaveAuthToken) and the
// token-auth module note the outcomes here; the credential_cache self-check (runtime/compat/credential_cache_check.h)
// reads them on its flush. Only outcomes and short reasons are kept: never a token, its bytes or its length, and
// no path.
//
// Header only; thread safe (a mutex: these are called from the auth threads, never from a hooked game function).

#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace nevr_credential_stats {

struct Snapshot {
  std::uint64_t saved = 0;     // saves that wrote the cache
  std::uint64_t refused = 0;   // saves that wrote nothing
  std::uint64_t reused = 0;    // launches that used the cached credentials without a sign-in
  std::uint64_t signIn = 0;    // launches with no usable cache: the device-code sign-in follows
  std::string lastProtection;  // how the last written cache was protected: host_mode_600 (Wine), dacl, mode_0600
  std::string lastRefusal;     // why the last refused save wrote nothing
};

namespace detail {
struct State {
  std::mutex mutex;
  Snapshot snap;
};
inline State& S() {
  static State* const state = new State();  // never destroyed: the auth threads can outlive static destruction
  return *state;
}
}  // namespace detail

inline void NoteSaved(std::string_view protection) {
  detail::State& s = detail::S();
  std::lock_guard<std::mutex> lock(s.mutex);
  ++s.snap.saved;
  s.snap.lastProtection.assign(protection);
}
inline void NoteRefused(std::string_view why) {
  detail::State& s = detail::S();
  std::lock_guard<std::mutex> lock(s.mutex);
  ++s.snap.refused;
  s.snap.lastRefusal.assign(why.substr(0, 120));
}
inline void NoteCacheReused() {
  detail::State& s = detail::S();
  std::lock_guard<std::mutex> lock(s.mutex);
  ++s.snap.reused;
}
inline void NoteSignInRequired() {
  detail::State& s = detail::S();
  std::lock_guard<std::mutex> lock(s.mutex);
  ++s.snap.signIn;
}
inline Snapshot Get() {
  detail::State& s = detail::S();
  std::lock_guard<std::mutex> lock(s.mutex);
  return s.snap;
}
inline void ResetForTest() {
  detail::State& s = detail::S();
  std::lock_guard<std::mutex> lock(s.mutex);
  s.snap = Snapshot();
}

}  // namespace nevr_credential_stats
