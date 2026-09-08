// token_auth.h
#pragma once

#include <cstdint>
#include <string>

#ifdef NEVR_TEST_HOOKS
struct CachedAuthToken;
#endif  // NEVR_TEST_HOOKS

namespace TokenAuth {
void Init(uintptr_t base_addr, bool is_server);
void Shutdown();

// Returns the current valid Bearer token, or empty string if not authenticated.
// Thread-safe — called from WS bridge connection handlers.
std::string GetToken();

// Returns the discord ID from the current JWT, or 0 if not authenticated.
uint64_t GetDiscordId();

// Returns the account's username, or empty if unknown. Survives restarts — it is
// persisted to the credential cache alongside the refresh token. Callers SHALL
// treat empty as "no honest answer" and must not substitute a placeholder that
// looks like a real name (N123).
std::string GetUsername();

#ifdef NEVR_TEST_HOOKS
// In-memory DeviceAuth observations for the token-auth unit test target.  These
// hooks intentionally construct a short-lived DeviceAuth instance: they do not
// consult the credential cache, start the refresh thread, or issue HTTP calls.
namespace TestHook {
struct DeviceAuthState {
    bool authenticated = false;
    std::string token;
    uint64_t discord_id = 0;
    std::string username;
};

DeviceAuthState InspectInitialDeviceAuth();
DeviceAuthState InspectDeviceAuthAfterRefresh(const ::CachedAuthToken& auth);
// Exercises the production executable-relative credential-cache lookup. The
// caller owns fixture setup and must ensure _local/.credentials.json beside the
// test executable is restored when this returns.
DeviceAuthState InspectDeviceAuthFromCache();

// Drives the background refresh thread's expiry guard, in-process and without
// the 60-second sleep, against a DeviceAuth holding `live` in memory. The
// credential cache is left in whatever state the caller's fixture put on disk,
// so the two sources can be made to disagree and it is observable which one the
// guard consults. Returns the guard's decision: true = refresh now.
bool InspectRefreshDecision(const ::CachedAuthToken& live, uint64_t now);
}  // namespace TestHook
#endif  // NEVR_TEST_HOOKS
}
