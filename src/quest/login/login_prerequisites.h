#pragma once
// Quest login prerequisites: the four Oculus Platform answers the game needs before it calls
// CNSUser::SendLogInRequest, measured and, when Oculus gives no usable answer, supplied.
//
// What the game needs (libpnsovr.so, pinned build ca47bb8d..., ELF vaddrs):
//   org-scoped id   ovr_User_GetOrgScopedID -> SCallbacks::GotLoggedInUserOrgIdCb 0x1ece60 stores
//                   ovr_OrgScopedID_GetID(ovr_Message_GetOrgScopedID(msg)) in the global 0x70e3e0
//                   (0x1ecf18); on error it stores -1 (0x1ecef0).
//   logged-in user  ovr_User_GetLoggedInUser -> SCallbacks::GotLoggedInUserCb 0x1ecfe4 copies
//                   ovr_User_GetOculusID(ovr_Message_GetUser(msg)) into the 36-byte buffer 0x70e470
//                   (0x1ed128-0x1ed17c); on error it writes nothing, so the buffer keeps the ""
//                   LogInInternal put there (0x1eca20-0x1eca74).
//   access token    ovr_User_GetAccessToken -> SCallbacks::GotLoggedInUserAccessTokenCb 0x1ed1c0
//                   stores ovr_Message_GetString(msg) in the CNSLoggedInUserAccessToken CString
//                   (0x1ed330-0x1ed4c8); on error it stores "?" (string 0x61d3cb, 0x1ed258-0x1ed274).
//   user proof      ovr_User_GetUserProof -> CNSOVRUser::GotUserProofCB 0x1ed578 puts
//                   ovr_UserProof_GetNonce(ovr_Message_GetUserProof(msg)) and the token into the
//                   login CJson and calls SendLogInRequest through vtable+0x8 (0x1ed9c4); on error
//                   it fails the login with 500 (0x1ed62c).
// The first three are fetched by RadPluginMain (0x206960, 0x2069bc, 0x206a00) and again by
// CNSOVRUser::LogInInternal when a value is missing. LogInInternal (0x1ecd0c-0x1ece10) and, for a
// pending login, UpdateInternal (0x1ed9e8) call ovr_User_GetUserProof only when the org id is
// neither 0 nor -1, the user buffer is neither "?" nor empty, and the token is neither "?" nor
// shorter than two bytes; otherwise UpdateInternal fails the login with "Log in request failed:
// One or more prerequisites are missing" (string 0x556b40, 0x1edb54).
//
// How the answers are supplied. The four callbacks are registered through GLOB_DAT slots
// (0x6e2f10, 0x6e48a8, 0x6e43f0, 0x6e4340) that every registration reads, and they read the answer
// only through the ovr_* accessors libpnsovr imports through JUMP_SLOTs. Hooking the callback
// slots gives a handler that sees every answer before the game does; hooking the eight accessors
// lets that handler substitute an answer inside the game's own success path, so the game writes
// its globals, logs and continues exactly as for a real answer. A substitution happens only for
// the one message a login callback is handling right now (the active attempt below): every other
// caller of the same accessors (social, rooms, IAP) gets the real function untouched.
//   real answer, usable     passed through unchanged
//   ovr error               ovr_Message_IsError answers false and the accessors return the
//                           synthesized value; the real accessors are never called on that message
//   real answer, unusable   (a null or empty string, "?", an org id of 0 or -1, a null handle) the
//                           accessor returns the synthesized value instead
// The synthesized values are not credentials: the login rewrite (login_rewrite.h) replaces
// accountid, access_token, nonce and displayname with the NEVR identity before anything is sent.
//
// Every callback the game receives is logged once (event "quest_login_prerequisite"): which call,
// whether the game got the real answer or a synthesized one, why, and the Oculus error code. Every
// request the game issues is logged too ("quest_login_prerequisite_request", the request id), so
// a request that is never answered shows as a request line with no callback line. No line carries
// a token, a nonce, a user name or an id value.
//
// Threading. The callbacks run on whatever thread pumps the game's OVR mailbox. One attempt is
// active at a time; a second callback that arrives while one is active is passed through
// unchanged and logged with reason "busy". The accessors decide by comparing the message (or the
// handle) with the active one, which only the thread running that callback can be holding.
//
// Frames. Every handler here is live while game code runs (the callback's original calls the
// accessors, and GotUserProofCB calls SendLogInRequest), so this code is built -fno-exceptions,
// has no try/catch and no object with a destructor (sentinel/callback_thunk.h, rule 1).

#include <cstddef>
#include <cstdint>

namespace sentinel {
struct ElfImage;
}

namespace QuestLogin {

enum class Prerequisite : std::uint8_t { OrgScopedId, LoggedInUser, AccessToken, UserProof };
inline constexpr std::size_t kPrerequisiteCount = 4;

// The Oculus request the prerequisite comes from ("ovr_User_GetAccessToken", ...).
const char* PrerequisiteCall(Prerequisite which);

// What the game receives when Oculus gives no usable answer. None is a credential.
inline constexpr std::uint64_t kSynthesizedOrgScopedId = 0x4e455652ULL;  // "NEVR"; neither 0 nor -1
inline constexpr const char kSynthesizedOculusId[] = "nevr-quest-player";  // fits the 36-byte buffer
inline constexpr const char kSynthesizedAccessToken[] = "nevr-synthesized-oculus-access-token";
inline constexpr const char kSynthesizedNonce[] = "nevr-synthesized-oculus-nonce";

// The Platform SDK functions a callback handler calls itself, read from libpnsovr's own GOT so
// they are the functions the game calls. Any member may be null; what it measures is then
// reported as unmeasured.
struct OvrErrorApi {
  bool (*message_is_error)(const void* message);
  const void* (*message_get_error)(const void* message);
  int (*error_get_code)(const void* error);
  int (*error_get_http_code)(const void* error);
};

// Publishes the API and turns the handlers on. `substitute` is true only when all eight accessor
// hooks are installed: a callback hook without them may observe but must not claim an attempt,
// or the game's success path would call a real accessor on an error message. Until this is called
// every handler passes straight through and logs nothing. Call once, before the callback hooks are
// installed.
void ConfigurePrerequisites(const OvrErrorApi& api, bool substitute) noexcept;

// ---- handler bodies -------------------------------------------------------------------------
// `original` is the real function behind the hooked slot, as CallbackThunk hands it over.

using GameCallback = void (*)(void* self, void* message);
void OnPrerequisiteCallback(Prerequisite which, GameCallback original, void* self, void* message) noexcept;

bool OnMessageIsError(bool (*original)(const void*), const void* message) noexcept;
const char* OnMessageGetString(const char* (*original)(const void*), const void* message) noexcept;
const void* OnMessageGetOrgScopedId(const void* (*original)(const void*), const void* message) noexcept;
std::uint64_t OnOrgScopedIdGetId(std::uint64_t (*original)(const void*), const void* handle) noexcept;
const void* OnMessageGetUser(const void* (*original)(const void*), const void* message) noexcept;
const char* OnUserGetOculusId(const char* (*original)(const void*), const void* handle) noexcept;
const void* OnMessageGetUserProof(const void* (*original)(const void*), const void* message) noexcept;
const char* OnUserProofGetNonce(const char* (*original)(const void*), const void* handle) noexcept;

// Logs one issued request (the first kRequestLogLimit per prerequisite, then one line saying the
// rest are counted only). Called after the real request function has returned.
inline constexpr std::uint64_t kRequestLogLimit = 8;
void NoteRequest(Prerequisite which, std::uint64_t request_id) noexcept;

// Callbacks and requests seen so far for `which` (also for tests).
std::uint64_t PrerequisiteCallbacks(Prerequisite which) noexcept;
std::uint64_t PrerequisiteRequests(Prerequisite which) noexcept;

// Test support: back to the unconfigured state with zero counters.
void ResetPrerequisitesForTest() noexcept;

// ---- install (Android; login_prerequisites_install.cpp) ------------------------------------
// Installs the accessor, callback and request hooks into the pinned libpnsovr.so `image` and
// logs one summary line (event "quest_login_prerequisites_install"). Each hook is independent and
// a failed one leaves its slot as it was; substitution is enabled only when every accessor hook is
// in place. Idempotent; call it after libpnsovr.so is loaded and before RadPluginMain runs (the
// dlopen post-load step does both, through TryInstallLoginHook).
struct PrerequisiteInstall {
  int callbacks = 0;  // of 4
  int accessors = 0;  // of 8
  int requests = 0;   // of 4
  bool substitute = false;
};
PrerequisiteInstall InstallLoginPrerequisites(const sentinel::ElfImage& image) noexcept;

}  // namespace QuestLogin
