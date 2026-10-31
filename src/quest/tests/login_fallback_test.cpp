// Host test for the login_host fallback rule (#1737, C7). The pure rules (quest/redirect/login_fallback.h) and
// the TString handler's fallback step (quest/redirect/tstring_thunks.cpp, HandleTString) are driven through the
// thunk entry, with fakes for the game's original and for the two decision functions. The redirector and the
// string pool are not linked here; their cases are in quest/redirect/tests/redirect_test.cpp.
//
// Built -fno-exceptions, as the sentinel builds the thunk unit (just test-quest-hooks).
// Run: login_fallback_test

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "quest/redirect/login_fallback.h"
#include "quest/redirect/tstring_thunks.h"
#include "quest/tests/test_check.h"

namespace {

using namespace nevr_quest::redirect;

using TStringFn = const char* (*)(const void*, const char*, const char*, std::uint32_t);

constexpr const char* kStockLogin = "wss://login.readyatdawn.com/rad/rad15_live";
constexpr const char* kPatchedLogin = "ws://fallback.example:777/login?evr_standalone-testBuildComment-v4.20.69-run";
constexpr const char* kRedirectTarget = "wss://nevr.example/ws";
constexpr const char* kStockComposed = "wss://login.readyatdawn.com/rad/rad15_live?nevr_quest-dev-1.2.3-abc";

BuildIdentity TestIdentity() { return {"nevr_quest", "dev", "1.2.3", "abc"}; }

// The game's original. The fake config has no key, so it returns the fallback it was given, and it records
// the pointer it received.
const char* g_originalGot = nullptr;
const char* FakeOriginal(const void*, const char*, const char* fallback, std::uint32_t) {
  g_originalGot = fallback;
  return fallback;
}

// The fallback decision, standing in for ServiceRedirector::ApplyFallback. It composes the rule's string into a
// static buffer (the test's stand-in for the pool).
const char* FakeFallback(const char* key, const char* fallback) noexcept {
  if (!NeedsVersion(key, fallback)) return fallback;
  static std::string buffer;
  buffer = Compose(fallback, TestIdentity());
  return buffer.c_str();
}

// The result decisions, standing in for ServiceRedirector::Apply. The first records what it was handed and
// returns it unchanged; the second redirects the two login reads to one target.
const char* g_applySawResult = nullptr;
const char* FakeApplyPassThrough(const char*, const char* result) noexcept {
  g_applySawResult = result;
  return result;
}
const char* FakeApplyRedirect(const char* key, const char* result) noexcept {
  if (std::strcmp(key, "login_host") == 0 || std::strcmp(key, "loginservice_host") == 0) return kRedirectTarget;
  return result;
}

TStringFn Entry(Slot slot) { return reinterpret_cast<TStringFn>(ThunkEntry(slot)); }

// Arms the libr15 slot's handler with the original above and no decisions; the tests set the decisions they need.
void Setup() {
  ResetThunk(Slot::kLibR15);
  ResetThunk(Slot::kMatchmaking);
  SetApply(nullptr);
  SetApplyFallback(nullptr);
  *ThunkOriginalOut(Slot::kLibR15) = reinterpret_cast<void*>(&FakeOriginal);
  *ThunkOriginalOut(Slot::kMatchmaking) = reinterpret_cast<void*>(&FakeOriginal);
  ArmThunk(Slot::kLibR15, true);
  g_originalGot = nullptr;
  g_applySawResult = nullptr;
}

void Teardown() {
  SetApply(nullptr);
  SetApplyFallback(nullptr);
  ArmThunk(Slot::kLibR15, false);
  ResetThunk(Slot::kLibR15);
  ResetThunk(Slot::kMatchmaking);
}

// The shape of BeginLogIn: the login_host read with the built-in fallback, then the loginservice_host read with
// the first result as its fallback. The second result is what Parse receives.
const char* g_parseArg = nullptr;
const char* ModelBeginLogIn(const char* fallback) {
  const char* const first = Entry(Slot::kLibR15)(nullptr, "login_host", fallback, 0U);
  const char* const second = Entry(Slot::kLibR15)(nullptr, "loginservice_host", first, 0U);
  g_parseArg = second;
  return second;
}

int CountQueries(const char* text) {
  int n = 0;
  for (const char* p = text; *p != '\0'; ++p) {
    if (*p == '?') ++n;
  }
  return n;
}

// ---- the pure rules ----------------------------------------------------------

void ComposePlacesOneQueryAndTheTupleInOrder() {
  QCHECK(Compose("wss://h.example/x", TestIdentity()) == "wss://h.example/x?nevr_quest-dev-1.2.3-abc");
  QCHECK(Compose(kStockLogin, TestIdentity()) == kStockComposed);
}

void ComposeSanitizesEachField() {
  QCHECK(SanitizeField("1.2+x y") == "1.2_x_y");
  const BuildIdentity id{"nevr_quest", "dev", "1.2+x y", "a/b"};
  QCHECK(Compose("wss://h/x", id) == "wss://h/x?nevr_quest-dev-1.2_x_y-a_b");
}

void ComposeReadsAnEmptyFieldAsUnknown() {
  const BuildIdentity empty{};
  QCHECK(SanitizeField("") == "unknown");
  QCHECK(Compose("wss://h/x", empty) == "wss://h/x?unknown-unknown-unknown-unknown");
  QCHECK(!empty.Valid());
  QCHECK(TestIdentity().Valid());
}

void NeedsVersionForAWsFallbackWithoutAQuery() {
  QCHECK(NeedsVersion("login_host", kStockLogin));
  QCHECK(NeedsVersion("login_host", "ws://h.example/x"));
  QCHECK(!NeedsVersion("login_host", kPatchedLogin));  // a query already: the rule does not apply
}

void NeedsVersionIsFalseOutsideTheRule() {
  QCHECK(!NeedsVersion("login_host", "https://x"));
  QCHECK(!NeedsVersion("login_host", ""));
  QCHECK(!NeedsVersion("login_host", nullptr));
  QCHECK(!NeedsVersion(nullptr, kStockLogin));
  QCHECK(!NeedsVersion("config_host", kStockLogin));
  QCHECK(!NeedsVersion("loginservice_host", kStockLogin));
  QCHECK(!NeedsVersion("matchmaker_host", kStockLogin));
  QCHECK(!NeedsVersion("login_hosts", kStockLogin));
}

// ---- the handler ------------------------------------------------------------

void HandlerPassesTheComposedFallbackToTheOriginal() {
  Setup();
  SetApplyFallback(&FakeFallback);
  const char* const caller = kStockLogin;
  const char* const got = Entry(Slot::kLibR15)(nullptr, "login_host", caller, 0U);
  QCHECK(g_originalGot != nullptr && g_originalGot != caller);
  QCHECK(g_originalGot != nullptr && std::strcmp(g_originalGot, kStockComposed) == 0);
  QCHECK(got == g_originalGot);
  Teardown();
}

void HandlerLeavesOtherKeysAlone() {
  Setup();
  SetApplyFallback(&FakeFallback);
  const char* const caller = kStockLogin;
  (void)Entry(Slot::kLibR15)(nullptr, "loginservice_host", caller, 0U);
  QCHECK(g_originalGot == caller);
  (void)Entry(Slot::kLibR15)(nullptr, "config_host", caller, 0U);
  QCHECK(g_originalGot == caller);
  Teardown();
}

void HandlerPassesTheOriginalResultToApply() {
  Setup();
  SetApply(&FakeApplyPassThrough);
  const char* const got = Entry(Slot::kLibR15)(nullptr, "login_host", kPatchedLogin, 0U);
  QCHECK(g_applySawResult == g_originalGot);
  QCHECK(got == kPatchedLogin);
  Teardown();
}

void NoFallbackFunctionIsAPassThrough() {
  Setup();  // no fallback function is set
  const char* const caller = kStockLogin;
  const char* const got = Entry(Slot::kLibR15)(nullptr, "login_host", caller, 0U);
  QCHECK(g_originalGot == caller);
  QCHECK(got == caller);
  Teardown();
}

// ---- BeginLogIn, modelled ----------------------------------------------------

void BeginLogInReachesParseWithOneQueryOnBothInputs() {
  for (const char* input : {kStockLogin, kPatchedLogin}) {
    Setup();
    SetApplyFallback(&FakeFallback);
    (void)ModelBeginLogIn(input);
    QCHECK(g_parseArg != nullptr && CountQueries(g_parseArg) == 1);
    if (std::strcmp(input, kStockLogin) == 0) {
      QCHECK(g_parseArg != nullptr && std::strcmp(g_parseArg, kStockComposed) == 0);
    } else {
      QCHECK(g_parseArg != nullptr && std::strcmp(g_parseArg, kPatchedLogin) == 0);
    }
    Teardown();
  }
}

void BeginLogInReachesParseWithOneTargetOnBothInputs() {
  for (const char* input : {kStockLogin, kPatchedLogin}) {
    Setup();
    SetApply(&FakeApplyRedirect);
    SetApplyFallback(&FakeFallback);
    (void)ModelBeginLogIn(input);
    QCHECK(g_parseArg != nullptr && std::strcmp(g_parseArg, kRedirectTarget) == 0);
    Teardown();
  }
}

}  // namespace

int main() {
  ComposePlacesOneQueryAndTheTupleInOrder();
  ComposeSanitizesEachField();
  ComposeReadsAnEmptyFieldAsUnknown();
  NeedsVersionForAWsFallbackWithoutAQuery();
  NeedsVersionIsFalseOutsideTheRule();
  HandlerPassesTheComposedFallbackToTheOriginal();
  HandlerLeavesOtherKeysAlone();
  HandlerPassesTheOriginalResultToApply();
  NoFallbackFunctionIsAPassThrough();
  BeginLogInReachesParseWithOneQueryOnBothInputs();
  BeginLogInReachesParseWithOneTargetOnBothInputs();

  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "login_fallback_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("login_fallback_test: all checks passed\n");
  return 0;
}
