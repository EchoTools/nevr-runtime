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

// The one line a server with no password prints for its host (#1532): the code and the page that takes it, as a
// JSON object on one line so a supervisor can read it too. The code lives five minutes; the log carries it in full too,
// because a headless server's log may be the only place its host can read it. Tokens are never logged.
std::string ServerSignInPromptLine(const std::string& code, const std::string& loginUrl);

// The Info line a server with auth.password writes: it says what the server does, not that device codes are off.
std::string ServerPasswordSignInLine();
// The Info text of the flow's own guard, written when a caller runs the device flow in server mode
// without enabling server sign-in in the flow's ops (only a caller that builds its own ops can reach it).
std::string ServerFlowRefusedLine();
// The `auth=` value of the module's readiness line: "password" for a server that signs in with auth.password (its
// sign-in follows, so it is not "n/a"), "n/a" for a server with no token auth, else ok or failed.
const char* TokenAuthReadiness(bool is_server, bool has_device_auth, bool server_password_sign_in, bool auth_ok);
// The module's readiness Info line, whole: `[NEVR.MODULE] token_auth initialized mode=<server|client> auth=<value>`.
std::string TokenAuthReadinessLine(bool is_server, bool has_device_auth, bool server_password_sign_in, bool auth_ok);

struct ConsoleWrite {
    bool ok = false;
    size_t bytes = 0;
    unsigned long error = 0;  // GetLastError() when !ok
};
// Writes ServerSignInPromptLine plus a newline to standard error.
ConsoleWrite WriteServerSignInPrompt(const std::string& code, const std::string& loginUrl);

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

// True once the sign-in has ended without a token (the readiness is Failed): no token comes in this run, so a login has
// nothing to present (#1446). False before that, and when token auth never ran.
bool SignInEndedWithoutToken();

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
    bool server_sign_in = false;  // a server with no password: the flow runs in server mode (#1532)
};

struct DeviceAuthFlowResult {
    bool success = false;
    DeviceAuthState state;
};

DeviceAuthFlowResult RunDeviceAuthFlow(bool is_server, const DeviceAuthState& initial,
                                       const DeviceAuthFlowOps& ops);

DeviceAuthState InspectInitialDeviceAuth();
DeviceAuthState InspectDeviceAuthAfterRefresh(const ::CachedAuthToken& auth);
// Exercises the production credential-cache lookup. The caller owns fixture
// setup and must ensure the cache the lookup selects (the nevr root's
// .credentials.json, the only place it reads) is restored when this returns.
DeviceAuthState InspectDeviceAuthFromCache();

// Drives the background refresh thread's expiry guard, in-process and without
// the 60-second sleep, against a DeviceAuth holding `live` in memory. The
// credential cache is left in whatever state the caller's fixture put on disk,
// so the two sources can be made to disagree and it is observable which one the
// guard consults. Returns the guard's decision: true = refresh now.
bool InspectRefreshDecision(const ::CachedAuthToken& live, uint64_t now);

// Publishes `readiness` as the module's current snapshot, with no token, so SignInEndedWithoutToken can be driven through
// each readiness. The snapshot is process-wide: a caller publishes Disabled again when it returns (#1446).
void PublishReadinessForTest(AuthReadiness readiness);
}  // namespace test_hook
#endif  // NEVR_TEST_HOOKS
}
