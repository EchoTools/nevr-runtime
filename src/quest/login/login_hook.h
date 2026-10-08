#pragma once
// Quest login rewrite: the Android half. Installs the interception and adapts the live
// CNSUser / NRadEngine::CJson to the portable rewrite in login_rewrite.h.
//
// Interception point (measured on the pinned store APK, see docs/adr/0003 and the issue
// comment on #158 for the evidence):
//   libpnsovr.so  CNSOVRUser::SendLogInRequest(CJson&) @0x1ec584 tail-calls
//   CNSUser::SendLogInRequest(CJson&) through a PLT stub whose R_AARCH64_JUMP_SLOT in
//   libpnsovr.so names _ZN10NRadEngine7CNSUser16SendLogInRequestERNS_5CJsonE (GOT 0x6dd1b8).
//   That slot is a named import, so the existing HookImport backend can take it. At that call
//   the CNSUser (x0) still holds the Oculus identity and the CJson (x1) holds the Oculus
//   login payload, and nothing has been serialized or sent yet.
//
// Hook frequency: once per login attempt, on the game's login path. Nothing here sleeps,
// yields or blocks.

#include "quest/login/login_rewrite.h"

namespace QuestLogin {

// Signature of sentinel::HookImport (src/quest/sentinel/got_hook.h). Passed in so this file
// does not depend on the hook backend's location and a test can substitute it.
using ImportHookFn = bool (*)(const char* moduleSoName, const char* symbolName, void* hookFn,
                              void** originalOut);

enum class InstallState {
  Installed,
  AlreadyInstalled,
  ModuleNotLoaded,   // libpnsovr.so or libr15.so is not mapped yet; retry after it loads
  BuildMismatch,     // the loaded library is not the pinned build; nothing was touched
  SymbolMissing,     // a CJson export the rewrite needs is absent; nothing was touched
  HookFailed,        // the backend refused the slot; nothing was touched
};

const char* InstallStateName(InstallState state);

// Idempotent. Returns ModuleNotLoaded until libpnsovr.so has been dlopen'd by the game
// (CNSProvider::Create loads it), so the caller retries after that point. `source` and `log`
// must outlive the process; the hook keeps the pointers.
InstallState TryInstallLoginHook(IdentitySource* source, const BuildInfo& build,
                                 ImportHookFn hookImport, LogFn log);

// LogFn that writes to logcat under the tag "NEVR-Login".
void AndroidLog(Level level, const char* line);

}  // namespace QuestLogin
