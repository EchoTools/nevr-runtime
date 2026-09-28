#include "runtime/lifecycle/login_redirect_override.h"

#include <cstring>
#include <string>
#include <string_view>

namespace nevr::lifecycle {
namespace {

bool IsLoginServiceKey(const char* keyName) {
  if (keyName == nullptr) return false;
  constexpr const char* kKeys[] = {
      "config_host", "configservice_host", "login_host", "loginservice_host",
  };
  for (const char* key : kKeys) {
    if (std::strcmp(keyName, key) == 0) return true;
  }
  return false;
}

bool IsWebSocketUrl(const char* value) {
  if (value == nullptr) return false;
  return std::strncmp(value, "ws://", 5) == 0 || std::strncmp(value, "wss://", 6) == 0;
}

bool IsEligibleOverride(const LoginRedirectOverrideInput& input) {
  return input.earlyConfigPresent && !input.rootIsEarlyConfig &&
         input.result == input.defaultValue && input.overrideValue != nullptr &&
         input.overrideValue[0] != '\0';
}

bool HasCanonicalBridgeUrl(const char* url, uint16_t bridgePort) {
  if (url == nullptr || bridgePort == 0) return false;
  const std::string canonical = "ws://127.0.0.1:" + std::to_string(bridgePort);
  return std::string_view(url) == canonical;
}

}  // namespace

bool ShouldResolveLoginRedirectOverride(const LoginRedirectOverrideInput& input) {
  return IsEligibleOverride(input) && input.redirectsArmed && input.bridgeActive &&
         input.bridgePort != 0 && IsLoginServiceKey(input.keyName) &&
         IsWebSocketUrl(input.overrideValue) &&
         !HasCanonicalBridgeUrl(input.overrideValue, input.bridgePort);
}

LoginRedirectOverrideOutcome ApplyLoginRedirectOverride(
    const LoginRedirectOverrideInput& input, LoginRedirectResolver resolver, void* context) {
  LoginRedirectOverrideOutcome outcome{LoginRedirectOverrideAction::KeepResult, input.result, false};
  if (!IsEligibleOverride(input)) return outcome;

  outcome.action = LoginRedirectOverrideAction::UseOverride;
  outcome.value = input.overrideValue;
  if (!ShouldResolveLoginRedirectOverride(input) || input.socketUri == nullptr ||
      input.socketUri[0] == '\0' || resolver == nullptr) {
    return outcome;
  }

  const char* resolved = resolver(context, input.bridgePort);
  if (resolved == nullptr || resolved[0] == '\0') return outcome;

  outcome.value = resolved;
  outcome.rerouted = true;
  return outcome;
}

}  // namespace nevr::lifecycle
