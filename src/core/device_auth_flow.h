#pragma once
// The device-code login loop (request a code, send the player to a login URL,
// poll until verified / expired / error / deadline / cancelled), with every
// side effect behind an injected operation so it runs under a fake clock and a
// fake server. Shared by the Windows token-auth module and the Quest shim.
//
// The flow itself never blocks on anything but the injected `sleep` and `poll`;
// callers decide which thread runs it. It does not apply or persist the result.

#include "core/auth_types.h"
#include "core/device_poll_response.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

namespace nevr::auth {

inline constexpr std::chrono::steady_clock::duration kDeviceAuthLifetime = std::chrono::minutes(5);
inline constexpr std::chrono::steady_clock::duration kDeviceAuthPollInterval = std::chrono::seconds(3);

// open_browser returns a platform status; values above this mean "accepted"
// (the ShellExecute convention: Windows returns it directly, Quest's link presenter
// returns a value above it on success and 0 on failure).
inline constexpr intptr_t kBrowserOpenAcceptedAbove = 32;

struct DeviceFlowOps {
  using Clock = std::chrono::steady_clock;
  std::function<Clock::time_point()> now;
  std::function<std::string()> request_device_code;  // empty = failed
  std::function<intptr_t(const std::string&)> open_browser;  // arg: login URL carrying the code
  // Called when open_browser reports failure; returns 0 to abort the flow.
  std::function<int(const std::string&, const std::string&, intptr_t)> show_open_failure;
  std::function<nevr_token_auth::DevicePollResponse(const std::string&)> poll;
  std::function<void(Clock::duration)> sleep;
  std::function<void(LogLevel, const std::string&)> log;
  // Optional: true once the flow should stop (the game is closing while it waits, #37).
  std::function<bool()> cancelled;
  // True when the caller answers a code that runs out with a new one (the Quest session does):
  // the expiry and deadline lines then say so, at Info, instead of asking the player to restart.
  bool renews_expired_codes = false;
  // Consecutive failed polls (timeout, transport error) the wait tolerates before it gives up; an
  // answered poll resets the run. The Windows module sets 5 (#202); 1 ends the wait on the first error.
  unsigned max_consecutive_poll_errors = 1;
};

struct DeviceFlowResult {
  bool verified = false;
  nevr_token_auth::DevicePollResponse response;  // meaningful only when verified
};

// `login_url` is the page the player opens, without the code ("?code=<code>" is
// appended). Returns verified=false for every non-success ending, having logged
// which one. A poll that answers "verified" is honoured even when it returns after the
// five-minute deadline: the server hands the tokens out once and then deletes the code. Requires now, request_device_code, open_browser, show_open_failure,
// poll, sleep and log; returns unverified (and logs if it can) when one is missing.
DeviceFlowResult RunDeviceCodeFlow(const DeviceFlowOps& ops, const std::string& login_url);

}  // namespace nevr::auth
