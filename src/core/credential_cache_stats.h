#pragma once
// What the credential cache did in this process (#562): every save, refused or written, and whether the cached
// credentials were used or the player had to sign in. The cache writer (core/auth_token.h SaveAuthToken) and the
// token-auth module note the outcomes here; the credential_cache self-check (runtime/compat/credential_cache_check.h)
// reads them on its flush. Only paths and outcomes are kept: never a token, its bytes or its length.
//
// Header only; thread safe. Placeholder, filled in by the next commit.

#include <cstdint>
#include <string>
#include <string_view>

namespace nevr_credential_stats {

struct Snapshot {
  std::uint64_t saved = 0;     // saves that wrote the cache
  std::uint64_t refused = 0;   // saves that wrote nothing
  std::uint64_t reused = 0;    // launches that used the cached credentials without a sign-in
  std::uint64_t signIn = 0;    // launches with no usable cache: the device-code sign-in follows
  std::string lastProtection;  // how the last written cache was protected
  std::string lastRefusal;     // why the last refused save wrote nothing
};

inline void NoteSaved(std::string_view /*protection*/) {}
inline void NoteRefused(std::string_view /*why*/) {}
inline void NoteCacheReused() {}
inline void NoteSignInRequired() {}
inline Snapshot Get() { return Snapshot(); }
inline void ResetForTest() {}

}  // namespace nevr_credential_stats
