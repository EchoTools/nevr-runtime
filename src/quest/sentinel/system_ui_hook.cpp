#include "system_ui_hook.h"

#include "hook_install.h"
#include "hook_log.h"

namespace nevr_quest::system_ui {

namespace {

using sentinel::pinned::LibR15ShowSystemUIThunk;
using sentinel::pinned::OvrJavaOpaque;

sentinel::GotHook g_hook;

// vrapi_ShowSystemUI. The game's quit path asks for the global menu; the player is then sent to the library. Ask for the
// confirm-quit menu instead, so the player confirms and the game exits. Every other type passes through untouched.
// Never logs, allocates or throws: it is live across the call into the runtime (callback_thunk.h rule 1).
int Handler(LibR15ShowSystemUIThunk::Fn original, const OvrJavaOpaque* java, int type) noexcept {
  if (type == kGlobalMenu) return original(java, kConfirmQuitMenu);
  return original(java, type);
}

NEVR_HOOK_RECORD(kHook, LibR15ShowSystemUIThunk, &Handler);

}  // namespace

bool Install() noexcept {
  LibR15ShowSystemUIThunk::Arm(kHook);
  const sentinel::GotStatus status =
      sentinel::InstallThunk<LibR15ShowSystemUIThunk>(g_hook, sentinel::pinned::LibR15ShowSystemUI());
  if (status != sentinel::GotStatus::kOk) {
    sentinel::LogFields(sentinel::LogLevel::kError, "system_ui",
                        {{"result", "refused"}, {"from", kGlobalMenu}, {"to", kConfirmQuitMenu},
                         {"slot", sentinel::pinned::kShowSystemUISymbol}, {"status", sentinel::GotStatusName(status)}});
    return false;
  }
  sentinel::LogFields(sentinel::LogLevel::kInfo, "system_ui",
                      {{"result", "installed"}, {"from", kGlobalMenu}, {"to", kConfirmQuitMenu},
                       {"slot", sentinel::pinned::kShowSystemUISymbol}, {"status", sentinel::GotStatusName(status)}});
  return true;
}

}  // namespace nevr_quest::system_ui
