// Host test for the login-prerequisite install and its gating
// (src/quest/login/login_prerequisites_install.cpp, SubstitutionAllowed).
//
// The install's slot resolution against the real libpnsovr.so is covered by tests/got_pinned_test.cpp
// (just test-quest-hooks-pinned). Here, with no libpnsovr.so loaded, every slot resolution fails, so
// the install is fully partial; this checks it stays fail-safe and idempotent, and that the gate
// that decides whether substitution turns on (SubstitutionAllowed, and a configured-but-off handler)
// behaves as its comment says. Built -fno-exceptions like the install.

#include <cstdio>

#include "quest/login/login_prerequisites.h"
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

  sentinel::SetLogSink(nullptr);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "login_prerequisites_install_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("login_prerequisites_install_test: all checks passed\n");
  return 0;
}
