// Not run. tests/quest TestLoginHookFramesCarryNoPersonality reads this executable's frames. It
// links the whole nevr_quest_login archive and references the install entry point, so the login
// handler (reachable from TryInstallLoginHook) survives the linker's garbage collection and is
// present at its final address together with everything it calls.
#include "quest/login/login_hook.h"

int main(int argc, char**) {
  if (argc > 1000) {  // never true; keeps the install path, and the handler it arms, linked
    const nevr_quest_login::BuildInfo build;
    return static_cast<int>(nevr_quest_login::TryInstallLoginHook(nullptr, build, nullptr));
  }
  return 0;
}
