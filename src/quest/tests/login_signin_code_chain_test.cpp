// Host test for #581, the whole chain with no saved credentials: token auth is awaiting the player, so the identity
// is not ready and does not hold; the four login answers are delivered on synthetic handles; the not-ready reset
// withdraws the stand-ins; the game's own UpdateInternal fails the login with the "prerequisites are missing" text;
// the sign-in prompt hook takes that text for a local failure and writes the code onto the login-failed screen.
// The real prerequisite handlers (login_prerequisites.cpp) and the real prompt hook and board
// (login_prompt_hook.cpp, prompt_board.cpp) run together, built without exceptions as the sentinel builds them,
// against a fake CR15NetGame and a fake game state. Only the game's own UpdateInternal, the callbacks and the
// Platform SDK are stand-ins. The held case (token auth working on its own) is the control: the game never
// fails and the code is never written.
//
// Built and run by `just test-quest-hooks`.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "hook_log.h"
#include "login_prompt_hook.h"
#include "quest/auth/prompt_board.h"
#include "quest/game_login_failures.h"
#include "quest/login/login_prerequisites.h"
#include "quest/login/login_standin.h"
#include "quest/tests/test_check.h"

namespace {

namespace board = nevr::quest_auth::prompt_board;
namespace lp = nevr_quest::login_prompt;
namespace layout = sentinel::pinned::game_layout;
using nevr_quest_login::Prerequisite;
using sentinel::pinned::CR15NetGameOpaque;

// ---- the fake CR15NetGame, as login_prompt_hook_test.cpp lays it out -----------------------------

struct alignas(16) FakeGame {
  unsigned char bytes[layout::kErrorBlockOffset + layout::kErrorBlockBytes + 64];
};
FakeGame g_net;

CR15NetGameOpaque* Obj(FakeGame& g) { return reinterpret_cast<CR15NetGameOpaque*>(&g); }
void SetState(FakeGame& g, std::int32_t s) { std::memcpy(g.bytes + layout::kStateOffset, &s, sizeof(s)); }
std::string Line(FakeGame& g, int n) {
  const char* p = reinterpret_cast<const char*>(g.bytes + layout::kErrorBlockOffset + 1 + n * layout::kErrorLineBytes);
  return std::string(p, strnlen(p, layout::kErrorLineBytes));
}

void FakeSetDelimitedErrorMessage(CR15NetGameOpaque* self, const char* message) {
  unsigned char* block = reinterpret_cast<unsigned char*>(self) + layout::kErrorBlockOffset;
  block[0] = 0;
  std::size_t i = 0;
  for (; i + 1 < layout::kErrorLineBytes && message[i] != '\0'; ++i) block[1 + i] = static_cast<unsigned char>(message[i]);
  for (; i < layout::kErrorLineBytes; ++i) block[1 + i] = 0;
}
void FakeUpdate(CR15NetGameOpaque*, std::uint64_t) {}
void FakeEnableOriginal(void*, const void*) {}

lp::ErrorThunk::Fn ErrorEntry() { return reinterpret_cast<lp::ErrorThunk::Fn>(lp::ErrorThunk::EntryAddress()); }

// ---- the game's login state and the identity source's two answers --------------------------------

struct Globals {
  std::uint64_t org = 0;
  std::string name;
  std::string token;
};
Globals g_globals;
int g_sdk_calls = 0;  // a real Platform SDK function was reached with a synthetic handle

bool g_ready = false;
bool g_hold = false;
bool ReadyNow() noexcept { return g_ready; }
bool HoldNow() noexcept { return g_hold; }

// LoginResetHeldStandIns (login_hook.cpp) on the fake globals.
void ResetHeld() noexcept {
  if (g_globals.org == nevr_quest_login::StandIn::OrgId()) g_globals.org = ~std::uint64_t{0};
  if (g_globals.name == nevr_quest_login::StandIn::OculusId()) g_globals.name = "?";
}

// The Platform SDK as the handlers see it for a handle they must not pass on: reaching any of these is a failure.
bool RealIsError(const void*) { ++g_sdk_calls; return false; }
const char* RealGetString(const void*) { ++g_sdk_calls; return nullptr; }
const void* RealGetHandle(const void*) { ++g_sdk_calls; return nullptr; }
std::uint64_t RealGetId(const void*) { ++g_sdk_calls; return 0; }
const char* RealGetOculusId(const void*) { ++g_sdk_calls; return nullptr; }
const void* RealPop() { return nullptr; }  // the game's queue holds nothing else
void RealFree(void*) {}

bool IsError(const void* m) { return nevr_quest_login::OnMessageIsError(&RealIsError, m); }

// The game's four callbacks, as libpnsovr's do (login_prerequisites_test.cpp states the model).
void OrgCallback(void*, void* message) {
  if (IsError(message)) {
    g_globals.org = ~std::uint64_t{0};
    return;
  }
  g_globals.org = nevr_quest_login::OnOrgScopedIdGetId(
      &RealGetId, nevr_quest_login::OnMessageGetOrgScopedId(&RealGetHandle, message));
}
void UserCallback(void*, void* message) {
  if (IsError(message)) return;
  const char* id = nevr_quest_login::OnUserGetOculusId(&RealGetOculusId,
                                                       nevr_quest_login::OnMessageGetUser(&RealGetHandle, message));
  g_globals.name = id != nullptr ? id : "";
}
void TokenCallback(void*, void* message) {
  if (IsError(message)) {
    g_globals.token = "?";
    return;
  }
  const char* text = nevr_quest_login::OnMessageGetString(&RealGetString, message);
  g_globals.token = text != nullptr ? text : "";
}
void ProofCallback(void*, void*) {}

nevr_quest_login::GameCallback CallbackFor(Prerequisite which) {
  switch (which) {
    case Prerequisite::OrgScopedId: return &OrgCallback;
    case Prerequisite::LoggedInUser: return &UserCallback;
    case Prerequisite::AccessToken: return &TokenCallback;
    default: return &ProofCallback;
  }
}

// One pump of the game's message queue for the three requests the login makes, then the game's UpdateInternal:
// with the org id at -1 or the token "?" it fails the login with its own text (libpnsovr 0x1eda08-0x1eda14,
// 0x1edb4c) through SetDelimitedErrorMessage, and the instance moves to "login failed".
void RunLoginAttempt() {
  namespace local = nevr_quest_login::local;
  g_globals = Globals{};
  g_sdk_calls = 0;
  SetState(g_net, layout::kStateLoggingIn);
  const Prerequisite order[] = {Prerequisite::OrgScopedId, Prerequisite::LoggedInUser, Prerequisite::AccessToken};
  for (Prerequisite which : order) static_cast<void>(local::Request(which));
  for (Prerequisite which : order) {
    void* handle = const_cast<void*>(local::OnPopMessage(&RealPop));
    if (handle == nullptr) continue;  // held: the game keeps waiting
    nevr_quest_login::OnPrerequisiteCallback(which, CallbackFor(which), nullptr, handle);
    local::OnFreeMessage(&RealFree, handle);
  }
  if (g_globals.org == ~std::uint64_t{0} || g_globals.token == "?") {
    ErrorEntry()(Obj(g_net), nevr_quest::game_login_failures::kPrerequisitesMissing);
    SetState(g_net, layout::kStateLoginFailed);
  }
}

std::string Prompt(const char* code) {
  return std::string("Sign in to play: on a phone or computer, open\necho.test/login/device\nand enter the code ") +
         code + "\nEach code lasts 5 minutes, then a new one is issued.";
}

void ConfigureWiring(bool ready, bool hold) {
  nevr_quest_login::ResetPrerequisitesForTest();
  nevr_quest_login::local::ResetForTest();
  g_ready = ready;
  g_hold = hold;
  const nevr_quest_login::OvrErrorApi api{&RealIsError, nullptr, nullptr, nullptr, nullptr};
  nevr_quest_login::ConfigurePrerequisites(api, true, &ReadyNow, &ResetHeld, &HoldNow);
  nevr_quest_login::local::SetEnabled(true);
}

// No saved credentials, token auth awaiting the player: the code reaches the login-failed screen.
void TestTheCodeIsWrittenWhenTokenAuthAwaitsThePlayer() {
  board::Withdraw();
  const std::string text = Prompt("CHAIN-ABCD");
  QCHECK(board::Publish(text.data(), text.size(), board::Mode::kPrompt));  // token auth published the code
  ConfigureWiring(/*ready=*/false, /*hold=*/false);
  const lp::Counts before = lp::CurrentCounts();
  RunLoginAttempt();
  QCHECK(g_sdk_calls == 0);
  QCHECK(nevr_quest_login::local::Delivered() == 3);
  QCHECK(g_globals.org == ~std::uint64_t{0} && g_globals.name == "?");  // withdrawn by the not-ready reset
  QCHECK(lp::CurrentCounts().shown == before.shown + 1);                // login_prompt_text_shown
  QCHECK(Line(g_net, 0) == "Sign in to play: on a phone or computer, open");
  QCHECK(Line(g_net, 2) == "and enter the code CHAIN-ABCD");
  board::Withdraw();
}

// Control: token auth is producing a token on its own (starting, refreshing): nothing is delivered, the game keeps
// waiting, and no code is written because there is no failure to write it onto.
void TestNothingIsWrittenWhileTokenAuthWorksOnItsOwn() {
  board::Withdraw();
  const std::string text = Prompt("CHAIN-HELD");
  QCHECK(board::Publish(text.data(), text.size(), board::Mode::kPrompt));
  ConfigureWiring(/*ready=*/false, /*hold=*/true);
  const lp::Counts before = lp::CurrentCounts();
  std::memset(g_net.bytes, 0, sizeof(g_net.bytes));
  RunLoginAttempt();
  QCHECK(nevr_quest_login::local::Delivered() == 0 && nevr_quest_login::local::Pending() == 3);
  QCHECK(lp::CurrentCounts().shown == before.shown);
  QCHECK(Line(g_net, 2).empty());
  board::Withdraw();
}

// Control: the identity is ready: the answers are delivered, the stand-ins stay and the game does not fail.
void TestAReadyIdentityIsNotFailed() {
  board::Withdraw();
  ConfigureWiring(/*ready=*/true, /*hold=*/false);
  const lp::Counts before = lp::CurrentCounts();
  std::memset(g_net.bytes, 0, sizeof(g_net.bytes));
  RunLoginAttempt();
  QCHECK(nevr_quest_login::local::Delivered() == 3);
  QCHECK(g_globals.org == nevr_quest_login::StandIn::OrgId());
  QCHECK(lp::CurrentCounts().shown == before.shown && lp::CurrentCounts().kept == before.kept);
}

}  // namespace

int main() {
  lp::ErrorThunk::Reset();
  lp::UpdateThunk::Reset();
  lp::EnablePageThunk::Reset();
  *lp::ErrorThunk::OriginalOut() = reinterpret_cast<void*>(&FakeSetDelimitedErrorMessage);
  *lp::UpdateThunk::OriginalOut() = reinterpret_cast<void*>(&FakeUpdate);
  *lp::EnablePageThunk::OriginalOut() = reinterpret_cast<void*>(&FakeEnableOriginal);
  lp::ArmForTest();
  TestTheCodeIsWrittenWhenTokenAuthAwaitsThePlayer();
  TestNothingIsWrittenWhileTokenAuthWorksOnItsOwn();
  TestAReadyIdentityIsNotFailed();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "login_signin_code_chain_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("login_signin_code_chain_test: with token auth awaiting the player the code reaches the login-failed screen\n");
  return 0;
}
