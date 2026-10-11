#pragma once
// The libpnsovr.so slots the login-prerequisite hooks take (login_prerequisites.h), pinned to the
// store build. Each slot is a link-time address checked against the ELF's own relocation table
// (`readelf -rW libpnsovr.so`); tests/got_pinned_test.cpp resolves every one of them in the real
// library (just test-quest-hooks-pinned). No thunks here, so the pinned test includes it freely.

#include <cstdint>

#include "quest/sentinel/got_hook.h"

namespace nevr_quest_login::PrerequisiteTargets {

inline constexpr const char* kPnsovr = "libpnsovr.so";
inline constexpr const char* kPnsovrBuildId = "ca47bb8d03e6f43c1825133bbb9c15f174705c51";

// Non-GOT byte facts the login send path pins in libpnsovr.so, shared by login_hook.cpp (which acts
// on them) and tests/got_pinned_test.cpp (which checks them against the real library), so the two
// never drift. Each is a function prologue (code) or a global address.
//   CNSUser::DeferredLogInFailed @0x382e44: str w1,[x0,#0xa0]; str x2,[x0,#0xa8]; ret
inline constexpr std::uint64_t kDeferredFailedVaddr = 0x382e44ULL;
inline constexpr std::uint32_t kDeferredFailedCode[3] = {0xb900a001u, 0xf9005402u, 0xd65f03c0u};
inline constexpr const char* kDeferredFailedSymbol = "_ZN10NRadEngine7CNSUser19DeferredLogInFailedENS_15ENSResponseCodeEPKc";
//   GotLoggedInUserCb @0x1ed0fc/0x1ed100: adrp x8,0x70e000; add x8,x8,#0x470 -> the 36-byte name buffer
inline constexpr std::uint64_t kUserNameCodeVaddr = 0x1ed0fcULL;
inline constexpr std::uint32_t kUserNameCode[2] = {0xb0002908u, 0x9111c108u};
inline constexpr std::uint64_t kUserNameVaddr = 0x70e470ULL;
inline constexpr std::size_t kUserNameBytes = 0x24;
//   CNSOVRUser::OfflineID @0x1ede20: adrp x0,0x70e000; add x0,x0,#0x458; ret -> the 21-byte decimal buffer
inline constexpr std::uint64_t kOfflineIdFnVaddr = 0x1ede20ULL;
inline constexpr std::uint32_t kOfflineIdFnCode[3] = {0xb0002900u, 0x91116000u, 0xd65f03c0u};
inline constexpr std::uint64_t kOfflineIdVaddr = 0x70e458ULL;
inline constexpr std::size_t kOfflineIdBytes = 21;
//   The "prerequisites are missing" text the gate passes to DeferredLogInFailed (string 0x556b40).
inline constexpr std::uint64_t kPrerequisitesMissingTextVaddr = 0x556b40ULL;

struct PinnedSlot {
  const char* symbol;
  sentinel::RelocKind kind;
  std::uint64_t slot;      // link-time address of the GOT slot
  std::uint64_t function;  // link-time address the slot must hold (defined in libpnsovr), or 0
};

using sentinel::RelocKind;

// The callbacks, registered through these GLOB_DAT slots by RadPluginMain and LogInInternal.
inline constexpr PinnedSlot kOrgScopedIdCallback{
    "_ZN10NRadEngine10SCallbacks22GotLoggedInUserOrgIdCbEP10ovrMessage", RelocKind::kGlobDat, 0x6e2f10, 0x1ece60};
inline constexpr PinnedSlot kLoggedInUserCallback{
    "_ZN10NRadEngine10SCallbacks17GotLoggedInUserCbEP10ovrMessage", RelocKind::kGlobDat, 0x6e48a8, 0x1ecfe4};
inline constexpr PinnedSlot kAccessTokenCallback{
    "_ZN10NRadEngine10SCallbacks28GotLoggedInUserAccessTokenCbEP10ovrMessage", RelocKind::kGlobDat, 0x6e43f0,
    0x1ed1c0};
inline constexpr PinnedSlot kUserProofCallback{
    "_ZN10NRadEngine10CNSOVRUser14GotUserProofCBEP10ovrMessage", RelocKind::kGlobDat, 0x6e4340, 0x1ed578};

// The accessors those callbacks read the answer through (Platform SDK imports, JUMP_SLOT).
inline constexpr PinnedSlot kMessageIsError{"ovr_Message_IsError", RelocKind::kJumpSlot, 0x6dff10, 0};
inline constexpr PinnedSlot kMessageGetString{"ovr_Message_GetString", RelocKind::kJumpSlot, 0x6dc470, 0};
inline constexpr PinnedSlot kMessageGetOrgScopedId{"ovr_Message_GetOrgScopedID", RelocKind::kJumpSlot, 0x6da478, 0};
inline constexpr PinnedSlot kOrgScopedIdGetId{"ovr_OrgScopedID_GetID", RelocKind::kJumpSlot, 0x6dbc28, 0};
inline constexpr PinnedSlot kMessageGetUser{"ovr_Message_GetUser", RelocKind::kJumpSlot, 0x6e1270, 0};
inline constexpr PinnedSlot kUserGetOculusId{"ovr_User_GetOculusID", RelocKind::kJumpSlot, 0x6dda78, 0};
inline constexpr PinnedSlot kMessageGetUserProof{"ovr_Message_GetUserProof", RelocKind::kJumpSlot, 0x6e1b88, 0};
inline constexpr PinnedSlot kUserProofGetNonce{"ovr_UserProof_GetNonce", RelocKind::kJumpSlot, 0x6e0908, 0};

// The requests (measured, never changed).
inline constexpr PinnedSlot kGetOrgScopedId{"ovr_User_GetOrgScopedID", RelocKind::kJumpSlot, 0x6dbd98, 0};
inline constexpr PinnedSlot kGetLoggedInUser{"ovr_User_GetLoggedInUser", RelocKind::kJumpSlot, 0x6dd180, 0};
inline constexpr PinnedSlot kGetAccessToken{"ovr_User_GetAccessToken", RelocKind::kJumpSlot, 0x6dfa68, 0};
inline constexpr PinnedSlot kGetUserProof{"ovr_User_GetUserProof", RelocKind::kJumpSlot, 0x6e15a8, 0};

// Call sites of ovr_User_GetOrgScopedID that belong to the login: the return address (the instruction after the
// `bl` to the PLT stub 0x1a8b30) of LogInInternal (0x1ec99c), GotLoggedInUserOrgIdCb's re-request (0x1ecf80) and
// RadPluginMain (0x2069bc; the user id is the logged-in user, the delegate GotLoggedInUserOrgIdCb). The nine
// other callers are CNSOVRSocial's (SUserList::Add, JoinedCB, SyncRoom x2, GotRemoteOrgIdCB, AddInvitableUser,
// GotInvitableUserOrgIdCB, GotFriendOrgIdCB, GotRecentlyMetUserOrgIdCB): their requests carry other users and
// their callbacks are not ours, so they go to the SDK. tools/pinned_ovr_import_walk.py --sites checks this list
// against every call site in the real library (tools/pinned_ovr_sites.txt).
inline constexpr std::uint64_t kOrgRequestLoginReturns[] = {0x1ec9a0, 0x1ecf84, 0x2069c0};

// The same for CNSOVRSocial's nine call sites (SUserList::Add 0x1f22f0, JoinedCB 0x1f4970, SyncRoom 0x1f876c and
// 0x1f8b94, GotRemoteOrgIdCB 0x1f902c, AddInvitableUser 0x1f998c, GotInvitableUserOrgIdCB 0x1fa0f8, GotFriendOrgIdCB
// 0x1fcb8c, GotRecentlyMetUserOrgIdCB 0x1fd29c; each +4). Only these are refused once the social facade is selected;
// a caller this build does not know goes to the SDK. Checked against tools/pinned_ovr_sites.txt like the login's.
inline constexpr std::uint64_t kOrgRequestSocialReturns[] = {0x1f22f4, 0x1f4974, 0x1f8770, 0x1f8b98, 0x1f9030,
                                                             0x1f9990, 0x1fa0fc, 0x1fcb90, 0x1fd2a0};

inline bool IsSocialOrgRequestCaller(const void* caller, std::uintptr_t base) noexcept {
  if (caller == nullptr || base == 0) return false;
  const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(caller);
  if (address < base) return false;
  for (const std::uint64_t site : kOrgRequestSocialReturns) {
    if (address - base == site) return true;
  }
  return false;
}

inline bool IsLoginOrgRequestCaller(const void* caller, std::uintptr_t base) noexcept {
  if (caller == nullptr || base == 0) return false;
  const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(caller);
  if (address < base) return false;
  for (const std::uint64_t site : kOrgRequestLoginReturns) {
    if (address - base == site) return true;
  }
  return false;
}

// The entitlement request (#411): RadPluginMain asks Meta whether the viewer owns the app after the log line
// "Checking OVR entitlement..." (callers 0x206824 and 0x206ae4) and discards the request id; the only reader of
// the answer is the message pump (Update 0x207534), which hard-exits the game on an error of that type. The
// hook answers locally with request id 0, so no request leaves and no message comes back.
inline constexpr PinnedSlot kEntitlementRequest{"ovr_Entitlement_GetIsViewerEntitled", RelocKind::kJumpSlot, 0x6df638, 0};

// The message-level imports the local answers hook (#411): the pump pops a message, reads its type and
// request id, and frees it (Update 0x207534).
inline constexpr PinnedSlot kPopMessage{"ovr_PopMessage", RelocKind::kJumpSlot, 0x6e02d0, 0};
inline constexpr PinnedSlot kMessageGetType{"ovr_Message_GetType", RelocKind::kJumpSlot, 0x6de8a0, 0};
inline constexpr PinnedSlot kMessageGetRequestId{"ovr_Message_GetRequestID", RelocKind::kJumpSlot, 0x6e0ce0, 0};
inline constexpr PinnedSlot kFreeMessage{"ovr_FreeMessage", RelocKind::kJumpSlot, 0x6df448, 0};

// Read, never hooked: what a callback handler uses to report the Oculus error code and to tell a
// transient error from a permanent one (ovr_Error_GetMessage returns the JSON the game reads
// "error|is_transient" from).
inline constexpr PinnedSlot kMessageGetError{"ovr_Message_GetError", RelocKind::kJumpSlot, 0x6df518, 0};
inline constexpr PinnedSlot kErrorGetCode{"ovr_Error_GetCode", RelocKind::kJumpSlot, 0x6d9e78, 0};
inline constexpr PinnedSlot kErrorGetHttpCode{"ovr_Error_GetHttpCode", RelocKind::kJumpSlot, 0x6df9f8, 0};
inline constexpr PinnedSlot kErrorGetMessage{"ovr_Error_GetMessage", RelocKind::kJumpSlot, 0x6df270, 0};

inline constexpr PinnedSlot kAll[] = {
    kOrgScopedIdCallback, kLoggedInUserCallback, kAccessTokenCallback, kUserProofCallback,
    kMessageIsError,      kMessageGetString,     kMessageGetOrgScopedId, kOrgScopedIdGetId,
    kMessageGetUser,      kUserGetOculusId,      kMessageGetUserProof,  kUserProofGetNonce,
    kGetOrgScopedId,      kGetLoggedInUser,      kGetAccessToken,       kGetUserProof,
    kEntitlementRequest,  kPopMessage,           kMessageGetType,       kMessageGetRequestId,
    kFreeMessage,
    kMessageGetError,     kErrorGetCode,         kErrorGetHttpCode,     kErrorGetMessage,
};

// The GotTarget for `slot`. With a load bias, a slot that names a function libpnsovr defines must
// hold exactly base + function; otherwise the backend only requires an address in an executable
// mapping.
inline sentinel::GotTarget TargetFor(const PinnedSlot& slot, std::uintptr_t base = 0) {
  const void* expected =
      base != 0 && slot.function != 0 ? reinterpret_cast<const void*>(base + slot.function) : nullptr;
  return sentinel::GotTarget(kPnsovr, slot.symbol, slot.kind, kPnsovrBuildId, slot.slot, expected);
}

}  // namespace nevr_quest_login::PrerequisiteTargets
