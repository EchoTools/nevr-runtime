#pragma once

#include <cstdint>

namespace nevr::lifecycle {

enum class LoginRedirectOverrideAction {
  KeepResult,
  UseOverride,
};

struct LoginRedirectOverrideInput {
  const char* result = nullptr;
  const char* defaultValue = nullptr;
  const char* overrideValue = nullptr;
  const char* keyName = nullptr;
  const char* socketUri = nullptr;
  uint16_t bridgePort = 0;
  bool earlyConfigPresent = false;
  bool rootIsEarlyConfig = false;
  bool redirectsArmed = false;
  bool bridgeActive = false;
};

struct LoginRedirectOverrideOutcome {
  LoginRedirectOverrideAction action = LoginRedirectOverrideAction::KeepResult;
  const char* value = nullptr;
  bool rerouted = false;
};

using LoginRedirectResolver = const char* (*)(void* context, uint16_t bridgePort);

// True when a socket target lookup may be useful. The actual target lookup is
// deferred until the existing early-config override guard has selected a URL.
bool ShouldResolveLoginRedirectOverride(const LoginRedirectOverrideInput& input);

// KeepResult always preserves the engine's exact result pointer. UseOverride
// preserves the exact config pointer unless a stale websocket override can be
// replaced with the bridge's canonical endpoint.
LoginRedirectOverrideOutcome ApplyLoginRedirectOverride(
    const LoginRedirectOverrideInput& input, LoginRedirectResolver resolver, void* context);

}  // namespace nevr::lifecycle
