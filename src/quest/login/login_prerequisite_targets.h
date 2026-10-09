#pragma once
// The libpnsovr.so slots the login-prerequisite hooks take (login_prerequisites.h), pinned to the
// store build. Each slot is a link-time address checked against the ELF's own relocation table
// (`readelf -rW libpnsovr.so`); tests/got_pinned_test.cpp resolves every one of them in the real
// library (just test-quest-hooks-pinned). No thunks here, so the pinned test includes it freely.

#include <cstdint>

#include "quest/sentinel/got_hook.h"

namespace QuestLogin::PrerequisiteTargets {

inline constexpr const char* kPnsovr = "libpnsovr.so";
inline constexpr const char* kPnsovrBuildId = "ca47bb8d03e6f43c1825133bbb9c15f174705c51";

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

// Read, never hooked: what a callback handler uses to report the Oculus error code.
inline constexpr PinnedSlot kMessageGetError{"ovr_Message_GetError", RelocKind::kJumpSlot, 0x6df518, 0};
inline constexpr PinnedSlot kErrorGetCode{"ovr_Error_GetCode", RelocKind::kJumpSlot, 0x6d9e78, 0};
inline constexpr PinnedSlot kErrorGetHttpCode{"ovr_Error_GetHttpCode", RelocKind::kJumpSlot, 0x6df9f8, 0};

inline constexpr PinnedSlot kAll[] = {
    kOrgScopedIdCallback, kLoggedInUserCallback, kAccessTokenCallback, kUserProofCallback,
    kMessageIsError,      kMessageGetString,     kMessageGetOrgScopedId, kOrgScopedIdGetId,
    kMessageGetUser,      kUserGetOculusId,      kMessageGetUserProof,  kUserProofGetNonce,
    kGetOrgScopedId,      kGetLoggedInUser,      kGetAccessToken,       kGetUserProof,
    kMessageGetError,     kErrorGetCode,         kErrorGetHttpCode,
};

// The GotTarget for `slot`. With a load bias, a slot that names a function libpnsovr defines must
// hold exactly base + function; otherwise the backend only requires an address in an executable
// mapping.
inline sentinel::GotTarget TargetFor(const PinnedSlot& slot, std::uintptr_t base = 0) {
  const void* expected =
      base != 0 && slot.function != 0 ? reinterpret_cast<const void*>(base + slot.function) : nullptr;
  return sentinel::GotTarget(kPnsovr, slot.symbol, slot.kind, kPnsovrBuildId, slot.slot, expected);
}

}  // namespace QuestLogin::PrerequisiteTargets
