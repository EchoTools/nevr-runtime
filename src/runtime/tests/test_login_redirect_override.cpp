#include "runtime/lifecycle/login_redirect_override.h"

#include <gtest/gtest.h>

#include <array>
#include <string>

namespace {

using nevr::lifecycle::ApplyLoginRedirectOverride;
using nevr::lifecycle::LoginRedirectOverrideAction;
using nevr::lifecycle::LoginRedirectOverrideInput;
using nevr::lifecycle::LoginRedirectOverrideOutcome;
using nevr::lifecycle::ShouldResolveLoginRedirectOverride;

struct ResolverState {
  int calls = 0;
  uint16_t port = 0;
  const char* result = nullptr;
};

const char* Resolve(void* context, uint16_t port) {
  auto* state = static_cast<ResolverState*>(context);
  ++state->calls;
  state->port = port;
  return state->result;
}

LoginRedirectOverrideInput EligibleInput(const char* key, const char* value) {
  LoginRedirectOverrideInput input;
  input.result = "engine-default";
  input.defaultValue = input.result;
  input.overrideValue = value;
  input.keyName = key;
  input.socketUri = "ws://service.example/socket";
  input.bridgePort = 54321;
  input.earlyConfigPresent = true;
  input.redirectsArmed = true;
  input.bridgeActive = true;
  return input;
}

void ExpectKept(const LoginRedirectOverrideOutcome& outcome, const char* expected,
                ResolverState& resolver) {
  EXPECT_EQ(outcome.action, LoginRedirectOverrideAction::UseOverride);
  EXPECT_EQ(outcome.value, expected);
  EXPECT_FALSE(outcome.rerouted);
  EXPECT_EQ(resolver.calls, 0);
}

TEST(LoginRedirectOverride, ReroutesStaleWsAndWssOverridesForAllLoginKeys) {
  constexpr std::array<const char*, 4> kKeys = {
      "config_host", "configservice_host", "login_host", "loginservice_host",
  };
  constexpr std::array<const char*, 2> kSchemes = {"ws://", "wss://"};
  static constexpr char kResolved[] = "ws://127.0.0.1:54321";

  for (const char* key : kKeys) {
    for (const char* scheme : kSchemes) {
      const std::string stale = std::string(scheme) + "login.readyatdawn.com/rad/rad15_live";
      LoginRedirectOverrideInput input = EligibleInput(key, stale.c_str());
      ResolverState resolver{0, 0, kResolved};
      ASSERT_TRUE(ShouldResolveLoginRedirectOverride(input));
      const LoginRedirectOverrideOutcome outcome = ApplyLoginRedirectOverride(input, Resolve, &resolver);
      EXPECT_EQ(outcome.action, LoginRedirectOverrideAction::UseOverride);
      EXPECT_EQ(outcome.value, kResolved);
      EXPECT_TRUE(outcome.rerouted);
      EXPECT_EQ(resolver.calls, 1);
      EXPECT_EQ(resolver.port, input.bridgePort);
    }
  }
}

TEST(LoginRedirectOverride, PointerIdentityControlsTheExistingDefaultGate) {
  const std::string resultText = "same text";
  const std::string differentDefaultText = "same text";
  LoginRedirectOverrideInput input = EligibleInput("loginservice_host", "wss://stale.example/path");
  input.result = resultText.c_str();
  input.defaultValue = differentDefaultText.c_str();
  ResolverState resolver{0, 0, "ws://127.0.0.1:54321"};

  const LoginRedirectOverrideOutcome outcome = ApplyLoginRedirectOverride(input, Resolve, &resolver);
  EXPECT_EQ(outcome.action, LoginRedirectOverrideAction::KeepResult);
  EXPECT_EQ(outcome.value, input.result);
  EXPECT_EQ(resolver.calls, 0);
}

TEST(LoginRedirectOverride, EarlyConfigAndNonDefaultResultGatesKeepExactResultPointer) {
  const char* const engineResult = "engine-result";
  const char* const defaultValue = engineResult;
  const char* const overrideValue = "wss://stale.example/path";
  for (const auto& mutate : std::array<void (*)(LoginRedirectOverrideInput&), 3>{
           [](auto& input) { input.earlyConfigPresent = false; },
           [](auto& input) { input.rootIsEarlyConfig = true; },
           [](auto& input) { input.result = "other-result"; },
       }) {
    LoginRedirectOverrideInput input = EligibleInput("loginservice_host", overrideValue);
    input.result = engineResult;
    input.defaultValue = defaultValue;
    mutate(input);
    ResolverState resolver{0, 0, "ws://127.0.0.1:54321"};
    const LoginRedirectOverrideOutcome outcome = ApplyLoginRedirectOverride(input, Resolve, &resolver);
    EXPECT_EQ(outcome.action, LoginRedirectOverrideAction::KeepResult);
    EXPECT_EQ(outcome.value, input.result);
    EXPECT_EQ(resolver.calls, 0);
  }
}

TEST(LoginRedirectOverride, NullAndEmptyOverridesKeepResult) {
  for (const char* value : std::array<const char*, 2>{nullptr, ""}) {
    LoginRedirectOverrideInput input = EligibleInput("loginservice_host", value);
    ResolverState resolver{0, 0, "ws://127.0.0.1:54321"};
    const LoginRedirectOverrideOutcome outcome = ApplyLoginRedirectOverride(input, Resolve, &resolver);
    EXPECT_EQ(outcome.action, LoginRedirectOverrideAction::KeepResult);
    EXPECT_EQ(outcome.value, input.result);
    EXPECT_EQ(resolver.calls, 0);
  }
}

TEST(LoginRedirectOverride, NonReroutableContextPreservesOverridePointer) {
  struct Case {
    const char* name;
    void (*mutate)(LoginRedirectOverrideInput&);
  };
  const std::array<Case, 7> cases = {{
      {"unarmed", [](auto& input) { input.redirectsArmed = false; }},
      {"inactive", [](auto& input) { input.bridgeActive = false; }},
      {"no port", [](auto& input) { input.bridgePort = 0; }},
      {"missing target", [](auto& input) { input.socketUri = nullptr; }},
      {"empty target", [](auto& input) { input.socketUri = ""; }},
      {"https", [](auto& input) { input.overrideValue = "https://login.example/path"; }},
      {"unrelated key", [](auto& input) { input.keyName = "serverdb_host"; }},
  }};

  for (const Case& testCase : cases) {
    LoginRedirectOverrideInput input = EligibleInput("loginservice_host", "wss://stale.example/path");
    testCase.mutate(input);
    ResolverState resolver{0, 0, "ws://127.0.0.1:54321"};
    const LoginRedirectOverrideOutcome outcome = ApplyLoginRedirectOverride(input, Resolve, &resolver);
    SCOPED_TRACE(testCase.name);
    ExpectKept(outcome, input.overrideValue, resolver);
  }
}

TEST(LoginRedirectOverride, CanonicalSamePortPointerIsPreservedButWrongPortIsRerouted) {
  const std::string canonical = "ws://127.0.0.1:54321";
  LoginRedirectOverrideInput samePort = EligibleInput("configservice_host", canonical.c_str());
  ResolverState resolver{0, 0, "ws://127.0.0.1:54321"};
  EXPECT_FALSE(ShouldResolveLoginRedirectOverride(samePort));
  const LoginRedirectOverrideOutcome samePortOutcome = ApplyLoginRedirectOverride(samePort, Resolve, &resolver);
  EXPECT_EQ(samePortOutcome.value, samePort.overrideValue);
  EXPECT_FALSE(samePortOutcome.rerouted);
  EXPECT_EQ(resolver.calls, 0);

  const std::string wrongPort = "ws://127.0.0.1:54320";
  LoginRedirectOverrideInput wrongPortInput = EligibleInput("configservice_host", wrongPort.c_str());
  const LoginRedirectOverrideOutcome wrongPortOutcome =
      ApplyLoginRedirectOverride(wrongPortInput, Resolve, &resolver);
  EXPECT_EQ(wrongPortOutcome.value, resolver.result);
  EXPECT_TRUE(wrongPortOutcome.rerouted);
  EXPECT_EQ(resolver.calls, 1);
}

TEST(LoginRedirectOverride, NullOrEmptyResolutionFallsBackToExactOverridePointer) {
  for (const char* resolved : std::array<const char*, 2>{nullptr, ""}) {
    const std::string overrideValue = "wss://login.readyatdawn.com/rad/rad15_live";
    LoginRedirectOverrideInput input = EligibleInput("login_host", overrideValue.c_str());
    ResolverState resolver{0, 0, resolved};
    const LoginRedirectOverrideOutcome outcome = ApplyLoginRedirectOverride(input, Resolve, &resolver);
    EXPECT_EQ(outcome.action, LoginRedirectOverrideAction::UseOverride);
    EXPECT_EQ(outcome.value, input.overrideValue);
    EXPECT_FALSE(outcome.rerouted);
    EXPECT_EQ(resolver.calls, 1);
  }
}

}  // namespace
