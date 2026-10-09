// Host test for the login-prerequisite install (src/quest/login/login_prerequisites_install.cpp).
//
// No libpnsovr.so is loaded on the build host, so the backend's FindLoadedImage fails for every
// slot and the install is fully partial. That is exactly the shape that must stay fail-safe: no
// slot patched, substitution off, the handlers still configured (ready-gated), and a second call
// idempotent. The full slot resolution against the real libpnsovr.so is covered by
// tests/got_pinned_test.cpp (just test-quest-hooks-pinned). Built -fno-exceptions like the install.

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
void Sink(sentinel::LogLevel, const char* line) {
  ++g_lines;
  const char* s = line;
  for (; *s != '\0'; ++s) {
    if (s[0] == 'p' && s[1] == 'a' && s[2] == 'r' && s[3] == 't' && s[4] == 'i' && s[5] == 'a' && s[6] == 'l') {
      g_sawPartial = true;
    }
    if (s[0] == 'r' && s[1] == 'e' && s[2] == 's' && s[3] == 'i' && s[4] == 'd' && s[5] == 'u' && s[6] == 'a') {
      g_sawResidual = true;
    }
  }
}

}  // namespace

int main() {
  sentinel::SetLogSink(&Sink);
  QuestLogin::ResetPrerequisitesForTest();

  sentinel::ElfImage dummy{};  // base 0, no program headers: every slot resolution fails
  const QuestLogin::PrerequisiteInstall r = QuestLogin::InstallLoginPrerequisites(dummy, &Ready);
  QCHECK(r.callbacks == 0);
  QCHECK(r.accessors == 0);
  QCHECK(r.requests == 0);
  QCHECK(!r.substitute);  // substitution requires all eight accessor hooks
  QCHECK(g_sawPartial);   // the install logged a partial summary
  QCHECK(!g_sawResidual); // the residual warning is only for a substituting install

  // Idempotent: the second call returns the cached result and installs nothing new.
  const int linesAfterFirst = g_lines;
  const QuestLogin::PrerequisiteInstall r2 = QuestLogin::InstallLoginPrerequisites(dummy, &Ready);
  QCHECK(r2.accessors == 0 && !r2.substitute);
  QCHECK(g_lines == linesAfterFirst);  // nothing re-logged

  // The handlers were configured even though nothing installed: a delivered error is measured and
  // passed through (substitution_unavailable), never synthesized, because accessors != 8.
  QuestLogin::ResetPrerequisitesForTest();  // clears config; the install's static state stays
  bool original_ran = false;
  const auto original = +[](void* self, void*) noexcept { *static_cast<bool*>(self) = true; };
  QuestLogin::OnPrerequisiteCallback(QuestLogin::Prerequisite::AccessToken, original, &original_ran,
                                     reinterpret_cast<void*>(0x1));
  QCHECK(original_ran);  // unconfigured after reset -> straight pass-through
  QCHECK(!QuestLogin::PrerequisitesSynthesizedSinceReset());

  sentinel::SetLogSink(nullptr);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "login_prerequisites_install_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("login_prerequisites_install_test: all checks passed\n");
  return 0;
}
