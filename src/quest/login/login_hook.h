#pragma once
// Quest login rewrite: the Android half. Installs the interception and adapts the live
// CNSOVRUser / NRadEngine::CJson of libpnsovr.so to the portable rewrite in login_rewrite.h.
//
// Interception point (measured on the pinned store APK; addresses are ELF vaddrs):
//   libpnsovr.so  CNSOVRUser::SendLogInRequest(CJson&) @0x1ec584 tail-calls
//   CNSUser::SendLogInRequest(CJson&) through a PLT stub whose BIND_NOW
//   R_AARCH64_JUMP_SLOT at GOT 0x6dd1b8 names
//   _ZN10NRadEngine7CNSUser16SendLogInRequestERNS_5CJsonE. Hooking that slot with the GOT
//   backend (sentinel::GotHook) sees the CNSOVRUser (x0) and the Oculus login CJson (x1)
//   before anything is serialized.
//
// What reaches the wire, and what the hook therefore has to change:
//   JSON         the CJson argument
//   platform     [CNSUser+0x90] & 0xf; CNSOVRUser's constructor (0x1edd68-0x1edd74) already
//                stores 4, so the hook only checks it
//   account id   CNSUser::SendLogInRequest calls this->AccountID() through vtable+0x70
//                (0x382b90/0x382b9c). CNSOVRUser overrides it (vtable slot 0x6a1300) with
//                CNSOVRUser::AccountID() @0x1ede14: `adrp x8,0x70e000; ldr x0,[x8,#0x3e0]; ret`,
//                a process global that GotLoggedInUserOrgIdCb fills from
//                ovr_OrgScopedID_GetID (0x1ecf0c). That virtual slot is a data relocation the
//                GOT backend cannot hook, so the hook changes the global instead, after
//                checking the three instructions and that the address is writable, and then
//                proves the result by making the same virtual call.
//
// Hook frequency: once per login attempt, on the game's login path. Nothing here sleeps,
// yields or blocks.

#include "quest/login/login_rewrite.h"

namespace QuestLogin {

enum class InstallState {
  Installed,
  AlreadyInstalled,
  ModuleNotLoaded,  // libpnsovr.so is not mapped yet; retry after the game loads it
  BuildMismatch,    // the loaded library is not the pinned build; nothing was touched
  SlotInvalid,      // the account-id global could not be proven; nothing was touched
  SymbolMissing,    // a CJson export of libpnsovr.so is absent; nothing was touched
  HookFailed,       // the GOT backend refused the slot; nothing was touched
};

const char* InstallStateName(InstallState state);

// Thread-safe and idempotent. Returns ModuleNotLoaded until libpnsovr.so has been dlopen'd by
// the game (CNSProvider::Create loads it), so the caller retries after that point. `source`
// must outlive the process. `log` defaults to the sentinel's structured logcat sink.
InstallState TryInstallLoginHook(IdentitySource* source, const BuildInfo& build, LogFn log = nullptr);

// LogFn that forwards a record to sentinel::LogFields: one JSON line, to logcat by default or
// to whatever sink is installed with sentinel::SetLogSink, the same place the backend's lines go.
void SentinelLog(Level level, const char* event, const LogKv* fields, std::size_t count);

}  // namespace QuestLogin
