// token_auth.h
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include "auth_snapshot.h"

#ifdef NEVR_TEST_HOOKS
#include <chrono>
#include <functional>
#include "abi/echovr.h"
#include "device_poll_response.h"
#include "core/device_auth_flow.h"
struct CachedAuthToken;
#endif  // NEVR_TEST_HOOKS

namespace nevr_token_auth {
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

// One immutable generation for callers that need token and identity fields to
// agree. The returned snapshot remains valid across later publication.
std::shared_ptr<const AuthSnapshot> GetAuthSnapshot();

#ifdef NEVR_TEST_HOOKS
// In-memory DeviceAuth observations for the token-auth unit test target.  These
// hooks intentionally construct a short-lived DeviceAuth instance: they do not
// consult the credential cache, start the refresh thread, or issue HTTP calls.
namespace test_hook {
struct DeviceAuthState {
    bool authenticated = false;
    std::string token;
    uint64_t token_expiry = 0;
    std::string refresh_token;
    uint64_t refresh_token_expiry = 0;
    std::string user_id;
    uint64_t discord_id = 0;
    std::string username;
};

struct DeviceAuthFlowOps {
    using Clock = std::chrono::steady_clock;
    std::function<Clock::time_point()> now;
    std::function<std::string()> request_device_code;
    std::function<intptr_t(const std::string&)> open_browser;
    std::function<int(const std::string&, const std::string&, intptr_t)> show_open_failure;
    std::function<DevicePollResponse(const std::string&)> poll;
    std::function<void(Clock::duration)> sleep;
    std::function<bool()> save;
    std::function<void(EchoVR::LogLevel, const std::string&)> log;
    std::function<bool()> cancelled;  // optional
    std::function<void(const std::string& code, const std::string& login_url)> on_code_issued;  // optional
    std::function<void(nevr::auth::FlowEnd)> on_end;                                             // optional
};

struct DeviceAuthFlowResult {
    bool success = false;
    DeviceAuthState state;
};

DeviceAuthFlowResult RunDeviceAuthFlow(bool is_server, const DeviceAuthState& initial,
                                       const DeviceAuthFlowOps& ops);

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
}  // namespace test_hook
#endif  // NEVR_TEST_HOOKS
}
