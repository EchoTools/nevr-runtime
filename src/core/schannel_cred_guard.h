#pragma once

// Which Schannel credential struct a caller handed to AcquireCredentialsHandle.
//
// SCHANNEL_CRED (dwVersion 4) and SCH_CREDENTIALS (dwVersion 5) both begin with dwVersion but differ
// after it, so a hook that edits protocol bits and flags by SCHANNEL_CRED's layout is only valid
// for version 4. libcurl's Schannel backend passes version 5 (measured: dwVersion=5 from the
// runtime's own curl), and writing version-4 fields into it corrupts the credentials.
//
// PURE: std only.

#include <cstdint>
#include <cstring>

namespace nevr {

constexpr std::uint32_t kSchannelCredVersion = 4;    // SCHANNEL_CRED_VERSION
constexpr std::uint32_t kSchCredentialsVersion = 5;  // SCH_CREDENTIALS_VERSION

/// The dwVersion at the start of the struct, or 0 for a null pointer.
inline std::uint32_t SchannelAuthVersion(const void* authData) {
  std::uint32_t version = 0;
  if (authData != nullptr) std::memcpy(&version, authData, sizeof(version));
  return version;
}

/// True only for a SCHANNEL_CRED, the one layout the TLS modernisation hook may edit.
inline bool IsLegacySchannelCred(const void* authData) {
  return SchannelAuthVersion(authData) == kSchannelCredVersion;
}

}  // namespace nevr
