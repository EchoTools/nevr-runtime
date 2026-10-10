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
//
// Substitution is gated on a real NEVR login being ready (ReadyFn, from IdentitySource::Ready,
// whose contract is in login_rewrite.h). It happens only when ready is true AND all eight accessor
// hooks are installed (SubstitutionAllowed). When NEVR is not ready nothing is stood in: the real
// error or empty value passes through and the game's own Oculus login failure runs.
//   not ready               nothing stood in; the real answer (error or value) passes through
//   ready + real usable     passed through unchanged
//   ready + ovr error       ovr_Message_IsError answers false and the accessors return the
//                           stand-in; the real accessors are never called on that message
//   ready + transient error passed through so the game's own re-request runs, for up to
//                           kTransientWindowMs after the first transient error of this attempt (and
//                           at most kMaxTransientPasses passes), then stood in; the budget is reset
//                           when the attempt ends (EndPrerequisiteAttempt) or a non-transient answer
//                           arrives. "Transient" is the game's own test: the error message is a JSON
//                           object whose member "error" is an object whose member "is_transient" is
//                           the literal true (CJson::DecodeFrom then CJson::Boolean("error|is_transient")
//                           at 0x1ecea8/0x1ecf68).
//   ready + real unusable   (a null or empty string, "?", an org id of 0 or -1, a null handle) the
//                           accessor returns the stand-in instead
// The stand-ins (login_standin.h) are generated per process, not shared between headsets, and are
// recognised again by exact comparison. Whether a login may go out is decided at the send, from
// the CURRENT wire state, by FinishLogin/DecideSend (login_rewrite.h): a login whose account id,
// access_token or nonce is a stand-in is never sent, whatever the outcome; a refused or declined
// login puts the stand-in org id and user name back to the game's re-fetch markers.
//
// Residual (what can still read a stand-in after a NEVR login was accepted):
//   access token  the CNSLoggedInUserAccessToken CString is not rewritten (it is an engine string in
//                 the CEnvironment store, written only through the game's CString assignment), so a
//                 stood-in token stays there; libr15's CR15NetMatchmakerQueue Join / Heartbeat /
//                 Leave put it in https://graph.oculus.com URLs (strings 0x2bad390, 0x2bad5a5). It is
//                 a per-process random value, not a shared constant.
//   user name     the 36-byte buffer 0x70e470 is overwritten with the login's NEVR displayname by
//                 FinishLogin before the send; the user object's copy of it at [CNSOVRUser+0x60],
//                 taken by the constructor (0x1edd5c-0x1edd8c) when the user was created, is not.
//
// Every callback the game receives is logged (event "quest_login_prerequisite"): which call,
// whether the game got the real answer or a stand-in, why, and the Oculus error code. Records are
// capped per prerequisite per window (kCallbackLogLimit per kLogWindowMs, then one summary line,
// counters reset when the attempt ends), so every attempt's records exist and a spinning callback
// cannot flood the log. Every request the game issues is logged the same way
// ("quest_login_prerequisite_request", the request id), so a request that is never answered shows
// as a request line with no callback line. No line carries a token, nonce, user name or id value.
//
// Threading. The callbacks run on the thread that pumps the game's OVR messages: libpnsovr's
// Update (0x207534) loops ovr_PopMessage (PLT 0x1b15a0, at 0x20754c/0x207588) and dispatches
// through CNSOVRMailbox::FulfillRequest (0x2ebf04). That function is not exported and its caller
// was not traced, so whether the same thread runs CNSIUsers::Update (0x36bf68, which runs
// UpdateInternal) is not measured. One attempt is claimed at a time; a second callback that
// arrives while one is active is passed through unchanged and logged with reason "busy". The
// accessors decide by comparing the message (or the handle) with the active one, which only the
// thread running that callback can be holding.
//
// Frames. Every handler here is live while game code runs (the callback's original calls the
// accessors, and GotUserProofCB calls SendLogInRequest), so this code is built -fno-exceptions,
// has no try/catch and no object with a destructor (sentinel/callback_thunk.h, rule 1).

#include <cstddef>
#include <cstdint>

#include "quest/login/login_rewrite.h"
#include "quest/login/login_standin.h"

namespace sentinel {
struct ElfImage;
}

namespace nevr_quest_login {

enum class Prerequisite : std::uint8_t { OrgScopedId, LoggedInUser, AccessToken, UserProof };
inline constexpr std::size_t kPrerequisiteCount = 4;

// The Oculus request the prerequisite comes from ("ovr_User_GetAccessToken", ...).
const char* PrerequisiteCall(Prerequisite which);

// Transient budget per prerequisite per attempt: pass transient errors through for this long after
// the first one, and at most this many times, then stand in.
inline constexpr std::uint64_t kTransientWindowMs = 5000;
inline constexpr std::uint64_t kMaxTransientPasses = 50;

// Log caps per prerequisite: this many records with their fields per window, then one summary line.
inline constexpr std::uint64_t kCallbackLogLimit = 8;
inline constexpr std::uint64_t kRequestLogLimit = 8;
inline constexpr std::uint64_t kLogWindowMs = 60000;

// True when a real NEVR login is ready (IdentitySource::Ready). nullptr => never stand in.
using ReadyFn = bool (*)() noexcept;
// Resets any stand-in the game currently holds (login_hook), called on a not-ready callback.
using ResetFn = void (*)() noexcept;

// The game's test for a transient Oculus error, on the error message text (see above). Exposed for
// the host test.
bool IsTransientErrorMessage(const char* message) noexcept;

// The Platform SDK functions a callback handler calls itself, read from libpnsovr's own GOT so
// they are the functions the game calls. Any member may be null; what it measures is then
// reported as unmeasured.
struct OvrErrorApi {
  bool (*message_is_error)(const void* message);
  const void* (*message_get_error)(const void* message);
  int (*error_get_code)(const void* error);
  int (*error_get_http_code)(const void* error);
  const char* (*error_get_message)(const void* error);  // the JSON the game reads is_transient from
};

// Substitution needs every accessor hook: a callback hook without them may observe but must not
// claim an attempt, or the game's success path would call a real accessor on an error message.
bool SubstitutionAllowed(int accessors_hooked, bool have_is_error) noexcept;

// Publishes the API, generates the stand-ins and turns the handlers on. `substitute` comes from
// SubstitutionAllowed; `ready` gates every stand-in. Until this is called every handler passes
// straight through and logs nothing. Call once, before the callback hooks are installed.
void ConfigurePrerequisites(const OvrErrorApi& api, bool substitute, ReadyFn ready, ResetFn reset) noexcept;

// Ends the current login attempt: resets the transient budgets and the log windows, so the next
// attempt gets its own. The login send hook calls it on every send decision.
void EndPrerequisiteAttempt() noexcept;

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

// Logs one issued request (capped per window). Called after the real request function returned.
void NoteRequest(Prerequisite which, std::uint64_t request_id) noexcept;

// Callbacks and requests seen so far for `which` (also for tests).
std::uint64_t PrerequisiteCallbacks(Prerequisite which) noexcept;
std::uint64_t PrerequisiteRequests(Prerequisite which) noexcept;

// Test support: back to the unconfigured state with zero counters; replaceable monotonic clock.
void ResetPrerequisitesForTest() noexcept;
using MonotonicMsFn = std::uint64_t (*)() noexcept;
void SetPrerequisiteClockForTest(MonotonicMsFn clock) noexcept;  // nullptr: the real clock

// ---- install (Android; login_prerequisites_install.cpp) ------------------------------------
// Installs the accessor, callback and request hooks into the pinned libpnsovr.so `image` and
// logs one summary line (event "quest_login_prerequisites_install"). Each hook is independent and
// a failed one leaves its slot as it was; substitution is enabled only when SubstitutionAllowed.
// Idempotent; call it after libpnsovr.so is loaded and before RadPluginMain runs (the dlopen
// post-load step does both, through TryInstallLoginHook).
struct PrerequisiteInstall {
  int callbacks = 0;  // of 4
  int accessors = 0;  // of 8
  int requests = 0;   // of 4
  int entitlement = 0;  // of 1: the entitlement request answered locally (#411)
  bool substitute = false;
};
PrerequisiteInstall InstallLoginPrerequisites(const sentinel::ElfImage& image, ReadyFn ready, ResetFn reset) noexcept;

}  // namespace nevr_quest_login
