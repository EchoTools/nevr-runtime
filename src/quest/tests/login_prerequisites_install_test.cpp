// Host test for the login-prerequisite install and its gating
// (src/quest/login/login_prerequisites_install.cpp, SubstitutionAllowed).
//
// The install's slot resolution against the real libpnsovr.so is covered by tests/got_pinned_test.cpp
// (just test-quest-hooks-pinned). Here, with no libpnsovr.so loaded, every slot resolution fails, so
// the install is fully partial; this checks it stays fail-safe and idempotent, and that the gate
// that decides whether substitution turns on (SubstitutionAllowed, and a configured-but-off handler)
// behaves as its comment says. Built -fno-exceptions like the install.

#include <cstdio>

#include "quest/login/login_prerequisite_thunks.h"
#include "quest/login/login_prerequisites.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/test_check.h"

namespace {

bool Ready() noexcept { return true; }

int g_lines = 0;
bool g_sawPartial = false;
bool g_sawResidual = false;
bool Contains(const char* line, const char* needle) {
  for (const char* s = line; *s != '\0'; ++s) {
    const char* a = s;
    const char* b = needle;
    while (*b != '\0' && *a == *b) {
      ++a;
      ++b;
    }
    if (*b == '\0') return true;
  }
  return false;
}
void Sink(sentinel::LogLevel, const char* line) {
  ++g_lines;
  if (Contains(line, "\"partial\"")) g_sawPartial = true;
  if (Contains(line, "quest_login_prerequisites_residual")) g_sawResidual = true;
}

// A real "is-error" the configured-but-off handler can call; it is never reached when substitution
// is off (the handler passes through), so its body only has to exist.
bool IsError(const void*) { return true; }
const char* ErrMsg(const void*) { return ""; }

int g_entitlementOriginalCalls = 0;
std::uint64_t FakeEntitlementOriginal() {
  ++g_entitlementOriginalCalls;
  return 0xBEEF;  // a request id the SDK would have returned
}
NEVR_HOOK_RECORD(kTestEntitlementHook, nevr_quest_login::EntitlementRequestThunk, &nevr_quest_login::OnEntitlementRequest);

// The org-id request: the real handler behind a recorder that keeps the caller the thunk handed it, so the test
// can place a fabricated load bias under the one call instruction it uses.
const void* g_orgCaller = nullptr;
int g_orgOriginalCalls = 0;
std::uint64_t g_orgOriginalUser = 0;
std::uint64_t FakeOrgOriginal(std::uint64_t user) {
  ++g_orgOriginalCalls;
  g_orgOriginalUser = user;
  return 0x1234;  // a request id the SDK would have returned
}
std::uint64_t RecordingOrgHandler(nevr_quest_login::OrgRequestThunk::Fn original, const void* caller,
                                  std::uint64_t user) noexcept {
  g_orgCaller = caller;
  return nevr_quest_login::OnOrgScopedIdRequest(original, caller, user);
}
NEVR_HOOK_RECORD(kTestOrgHook, nevr_quest_login::OrgRequestThunk, &RecordingOrgHandler);

// The "game": one fixed call instruction into the thunk entry, so its return address is the same every time.
__attribute__((noinline)) std::uint64_t GameCallsOrgRequest(std::uint64_t (*entry)(std::uint64_t), std::uint64_t user) {
  const std::uint64_t id = entry(user);
  __asm__ volatile("" ::: "memory");  // keeps the call from becoming a tail call
  return id;
}

}  // namespace

int main() {
  sentinel::SetLogSink(&Sink);

  // The gate: substitution turns on only with all eight accessor hooks AND ovr_Message_IsError.
  for (int n = 0; n <= 8; ++n) {
    QCHECK(nevr_quest_login::SubstitutionAllowed(n, true) == (n == 8));
  }
  QCHECK(!nevr_quest_login::SubstitutionAllowed(8, false));  // no is-error -> cannot substitute

  // The install with no libpnsovr.so loaded: nothing patched, substitution off, no residual line,
  // and idempotent (second call re-logs nothing).
  nevr_quest_login::ResetPrerequisitesForTest();
  sentinel::ElfImage dummy{};  // base 0, no program headers: every slot resolution fails
  const nevr_quest_login::PrerequisiteInstall r = nevr_quest_login::InstallLoginPrerequisites(dummy, &Ready, nullptr);
  QCHECK(r.callbacks == 0 && r.accessors == 0 && r.requests == 0 && !r.substitute);
  QCHECK(g_sawPartial);
  QCHECK(!g_sawResidual);  // only a substituting install warns about residuals
  const int linesAfterFirst = g_lines;
  const nevr_quest_login::PrerequisiteInstall r2 = nevr_quest_login::InstallLoginPrerequisites(dummy, &Ready, nullptr);
  QCHECK(r2.accessors == 0 && !r2.substitute);
  QCHECK(g_lines == linesAfterFirst);

  // Configured with substitution OFF (what the install computes when fewer than eight accessors
  // hooked): a delivered error is measured and the game keeps its own answer, reason
  // "substitution_unavailable" -- the gate, not the unconfigured pass-through.
  nevr_quest_login::ResetPrerequisitesForTest();
  nevr_quest_login::OvrErrorApi api{&IsError, nullptr, nullptr, nullptr, &ErrMsg};
  nevr_quest_login::ConfigurePrerequisites(api, /*substitute=*/false, &Ready, nullptr);
  g_sawPartial = false;
  int seen_is_error = 0;
  struct Ctx {
    int* seen;
  } ctx{&seen_is_error};
  const auto game = +[](void* self, void* message) noexcept {
    // The game asks IsError; with substitution off the hook returns the real answer (true here).
    if (nevr_quest_login::OnMessageIsError(&IsError, message)) ++*static_cast<Ctx*>(self)->seen;
  };
  const int before = g_lines;
  nevr_quest_login::OnPrerequisiteCallback(nevr_quest_login::Prerequisite::AccessToken, game, &ctx,
                                     reinterpret_cast<void*>(0x1));
  QCHECK(seen_is_error == 1);         // the game saw the real error (not forced false)
  QCHECK(g_lines == before + 1);      // one record was logged

  // #411: the entitlement request is answered with request id 0 and the SDK function is never called; nothing is
  // logged on the game's call path. Driven through the real thunk entry the slot would point at.
  {
    using Thunk = nevr_quest_login::EntitlementRequestThunk;
    Thunk::Reset();
    *Thunk::OriginalOut() = reinterpret_cast<void*>(&FakeEntitlementOriginal);
    Thunk::Arm(kTestEntitlementHook);
    using Entry = std::uint64_t (*)();
    Entry entry = nullptr;
    void* address = Thunk::EntryAddress();
    __builtin_memcpy(&entry, &address, sizeof(entry));
    const int linesBefore = g_lines;
    QCHECK(entry() == 0);
    QCHECK(entry() == 0);
    QCHECK(g_entitlementOriginalCalls == 0);  // no request reached the Platform SDK
    QCHECK(Thunk::Calls() == 2);              // each one counted (prereq_entitlement_local_calls)
    QCHECK(g_lines == linesBefore);           // the handler never logs
    Thunk::Disarm();
    Thunk::Reset();
  }

  // #431: ovr_User_GetOrgScopedID is called by the login (three sites) and by CNSOVRSocial (nine). Driven through
  // the real thunk entry from one call instruction; the load bias is fabricated so that return address lands on a
  // login site, then on a Social one. Local answers are on (as the install sets them when every hook is in).
  {
    using Thunk = nevr_quest_login::OrgRequestThunk;
    namespace L = nevr_quest_login::local;
    nevr_quest_login::ResetPrerequisitesForTest();
    Thunk::Reset();
    *Thunk::OriginalOut() = reinterpret_cast<void*>(&FakeOrgOriginal);
    Thunk::Arm(kTestOrgHook);
    using Entry = std::uint64_t (*)(std::uint64_t);
    Entry entry = nullptr;
    void* address = Thunk::EntryAddress();
    __builtin_memcpy(&entry, &address, sizeof(entry));
    L::SetEnabled(true);

    nevr_quest_login::SetPnsovrBase(0);  // no base known: not a login site, so forwarded
    QCHECK(GameCallsOrgRequest(entry, 77) == 0x1234);
    QCHECK(g_orgOriginalCalls == 1 && g_orgOriginalUser == 77);
    QCHECK(g_orgCaller != nullptr);
    const std::uintptr_t caller = reinterpret_cast<std::uintptr_t>(g_orgCaller);

    // The caller is the login's RadPluginMain site (0x2069c0): answered locally, the SDK untouched.
    nevr_quest_login::SetPnsovrBase(caller - 0x2069c0);
    const std::uint64_t local_id = GameCallsOrgRequest(entry, 78);
    QCHECK(local_id >= L::kRequestIdBase);
    QCHECK(g_orgOriginalCalls == 1);
    QCHECK(L::Requested() == 1);

    // The caller is CNSOVRSocial::GotFriendOrgIdCB's site (0x1fcb90 + ...): forwarded with the user it asked
    // about, no slot taken.
    nevr_quest_login::SetPnsovrBase(caller - 0x1fcb90);
    QCHECK(GameCallsOrgRequest(entry, 79) == 0x1234);
    QCHECK(g_orgOriginalCalls == 2 && g_orgOriginalUser == 79);
    QCHECK(L::Requested() == 1);

    // The social facade is selected: CNSOVRSocial is not driven, so the same Social site is refused locally. A local
    // id from the refused space, the SDK untouched, no handle, counted; a login site is still answered as before.
    static bool selected = true;
    L::SetSocialSelectedProbe([]() noexcept { return selected; });
    nevr_quest_login::SetPnsovrBase(caller - 0x1fcb90);
    const auto linesBeforeRefusal = g_lines;
    const std::uint64_t refused_id = GameCallsOrgRequest(entry, 81);
    QCHECK(g_lines == linesBeforeRefusal + 1);  // the first refusal from a call site names it
    QCHECK(refused_id >= L::kRequestIdBase);
    QCHECK((refused_id & L::kRefusedIdBit) != 0);
    QCHECK(g_orgOriginalCalls == 2);
    QCHECK(L::Refused() == 1);
    QCHECK(L::Requested() == 1);               // not a delivered request: no answer is queued behind it
    const std::uint64_t second = GameCallsOrgRequest(entry, 82);
    QCHECK(second != refused_id && (second & L::kRefusedIdBit) != 0);  // ids are distinct
    QCHECK(g_lines == linesBeforeRefusal + 1);  // the same site is not logged again
    nevr_quest_login::SetPnsovrBase(caller - 0x1f8770);
    static_cast<void>(GameCallsOrgRequest(entry, 85));  // another Social site
    QCHECK(g_lines == linesBeforeRefusal + 2);
    nevr_quest_login::SetPnsovrBase(caller - 0x1fcb90);
    QCHECK(L::Refused() == 3);
    QCHECK(L::Refused() == 3 && g_orgOriginalCalls == 2);
    nevr_quest_login::SetPnsovrBase(caller - 0x2069c0);
    QCHECK((GameCallsOrgRequest(entry, 83) & L::kRefusedIdBit) == 0);   // the login's own call
    QCHECK(L::Refused() == 3 && L::Requested() == 2);
    // The facade not selected (stock Meta social): forwarded as before.
    selected = false;
    nevr_quest_login::SetPnsovrBase(caller - 0x1fcb90);
    QCHECK(GameCallsOrgRequest(entry, 84) == 0x1234);
    QCHECK(g_orgOriginalCalls == 3 && g_orgOriginalUser == 84 && L::Refused() == 3);
    // Not a known Social site (an unknown caller: base unknown, or an offset the build does not list): forwarded
    // even with the facade selected. The refusal is for the nine CNSOVRSocial sites only.
    selected = true;
    nevr_quest_login::SetPnsovrBase(0);
    QCHECK(GameCallsOrgRequest(entry, 86) == 0x1234);
    QCHECK(g_orgOriginalCalls == 4 && g_orgOriginalUser == 86 && L::Refused() == 3);
    nevr_quest_login::SetPnsovrBase(caller - 0x1fcb94);  // 4 past a listed site's return address
    QCHECK(GameCallsOrgRequest(entry, 87) == 0x1234);
    QCHECK(g_orgOriginalCalls == 5 && L::Refused() == 3);
    // The login's own site with local answers off and the facade selected: the SDK, as before local answers
    // (the refusal must not capture it).
    L::SetEnabled(false);
    nevr_quest_login::SetPnsovrBase(caller - 0x2069c0);
    QCHECK(GameCallsOrgRequest(entry, 88) == 0x1234);
    QCHECK(g_orgOriginalCalls == 6 && g_orgOriginalUser == 88 && L::Refused() == 3 && L::Requested() == 2);
    // A Social site with local answers off and the facade selected is still refused (it is not the login's).
    nevr_quest_login::SetPnsovrBase(caller - 0x1fcb90);
    QCHECK((GameCallsOrgRequest(entry, 89) & L::kRefusedIdBit) != 0);
    QCHECK(g_orgOriginalCalls == 6 && L::Refused() == 4);
    L::SetEnabled(true);
    selected = false;
    L::SetSocialSelectedProbe(nullptr);

    // Local answers off (an incomplete install): even a login site goes to the SDK.
    L::SetEnabled(false);
    nevr_quest_login::SetPnsovrBase(caller - 0x1ecf84);
    QCHECK(GameCallsOrgRequest(entry, 80) == 0x1234);
    QCHECK(g_orgOriginalCalls == 7 && L::Requested() == 2);

    Thunk::Disarm();
    Thunk::Reset();
    nevr_quest_login::SetPnsovrBase(0);
    nevr_quest_login::ResetPrerequisitesForTest();
  }

  sentinel::SetLogSink(nullptr);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "login_prerequisites_install_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("login_prerequisites_install_test: all checks passed\n");
  return 0;
}
