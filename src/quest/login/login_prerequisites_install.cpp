// Installs the login-prerequisite hooks (login_prerequisites.h) into the pinned libpnsovr.so.
// Built -fno-exceptions: it defines the thunks and their handlers (callback_thunk.h, rule 2).

#include <atomic>
#include <cstdint>
#include <mutex>

#include "quest/login/login_prerequisite_targets.h"
#include "quest/login/login_prerequisites.h"
#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace QuestLogin {

namespace {

namespace T = PrerequisiteTargets;

// ---- callbacks: void (SCallbacks* / CNSOVRUser*, ovrMessage*) --------------------------------
// The mailbox proxies (0x2089d0, 0x208be0) call the registered member pointer with the object in
// x0 and the message in x1 (`add x0,x0,x8,asr #1; mov x1,x2; br x3`).
template <Prerequisite P>
struct CallbackTag {};
template <Prerequisite P>
using CallbackThunk = sentinel::CallbackThunk<CallbackTag<P>, void(void*, void*)>;

template <Prerequisite P>
void HandleCallback(typename CallbackThunk<P>::Fn original, void* self, void* message) noexcept {
  OnPrerequisiteCallback(P, original, self, message);
}

using OrgCallbackThunk = CallbackThunk<Prerequisite::OrgScopedId>;
using UserCallbackThunk = CallbackThunk<Prerequisite::LoggedInUser>;
using TokenCallbackThunk = CallbackThunk<Prerequisite::AccessToken>;
using ProofCallbackThunk = CallbackThunk<Prerequisite::UserProof>;
NEVR_HOOK_RECORD(kOrgCallbackHook, OrgCallbackThunk, &HandleCallback<Prerequisite::OrgScopedId>);
NEVR_HOOK_RECORD(kUserCallbackHook, UserCallbackThunk, &HandleCallback<Prerequisite::LoggedInUser>);
NEVR_HOOK_RECORD(kTokenCallbackHook, TokenCallbackThunk, &HandleCallback<Prerequisite::AccessToken>);
NEVR_HOOK_RECORD(kProofCallbackHook, ProofCallbackThunk, &HandleCallback<Prerequisite::UserProof>);

// ---- accessors ------------------------------------------------------------------------------
// Types from the callers in libpnsovr: IsError is tested with `tbz w0,#0` (0x1ece7c); the handle
// accessors feed x0 straight into the next accessor (0x1ecf08-0x1ecf0c, 0x1ed0f0-0x1ed0f4,
// 0x1ed70c-0x1ed710); GetID's x0 is stored as a 64-bit id (0x1ecf18); the strings are read as
// char const* (0x1ed330-0x1ed35c, 0x1ed0f4-0x1ed114, 0x1ed710-0x1ed91c).
struct IsErrorTag {};
struct GetStringTag {};
struct GetOrgScopedIdTag {};
struct OrgScopedIdGetIdTag {};
struct GetUserTag {};
struct UserGetOculusIdTag {};
struct GetUserProofTag {};
struct UserProofGetNonceTag {};
using IsErrorThunk = sentinel::CallbackThunk<IsErrorTag, bool(const void*)>;
using GetStringThunk = sentinel::CallbackThunk<GetStringTag, const char*(const void*)>;
using GetOrgScopedIdThunk = sentinel::CallbackThunk<GetOrgScopedIdTag, const void*(const void*)>;
using OrgScopedIdGetIdThunk = sentinel::CallbackThunk<OrgScopedIdGetIdTag, std::uint64_t(const void*)>;
using GetUserThunk = sentinel::CallbackThunk<GetUserTag, const void*(const void*)>;
using UserGetOculusIdThunk = sentinel::CallbackThunk<UserGetOculusIdTag, const char*(const void*)>;
using GetUserProofThunk = sentinel::CallbackThunk<GetUserProofTag, const void*(const void*)>;
using UserProofGetNonceThunk = sentinel::CallbackThunk<UserProofGetNonceTag, const char*(const void*)>;
NEVR_HOOK_RECORD(kIsErrorHook, IsErrorThunk, &OnMessageIsError);
NEVR_HOOK_RECORD(kGetStringHook, GetStringThunk, &OnMessageGetString);
NEVR_HOOK_RECORD(kGetOrgScopedIdHook, GetOrgScopedIdThunk, &OnMessageGetOrgScopedId);
NEVR_HOOK_RECORD(kOrgScopedIdGetIdHook, OrgScopedIdGetIdThunk, &OnOrgScopedIdGetId);
NEVR_HOOK_RECORD(kGetUserHook, GetUserThunk, &OnMessageGetUser);
NEVR_HOOK_RECORD(kUserGetOculusIdHook, UserGetOculusIdThunk, &OnUserGetOculusId);
NEVR_HOOK_RECORD(kGetUserProofHook, GetUserProofThunk, &OnMessageGetUserProof);
NEVR_HOOK_RECORD(kUserProofGetNonceHook, UserProofGetNonceThunk, &OnUserProofGetNonce);

// ---- requests -------------------------------------------------------------------------------
// ovr_User_GetOrgScopedID takes the user id in x0 (0x2069b4-0x2069bc, 0x1ec990-0x1ec99c); the
// other three take nothing. All four return the 64-bit request id the game keys the callback by
// (0x1ec9b0, 0x1eca90, 0x1ecce0, 0x1ece20).
template <Prerequisite P>
struct RequestTag {};
using OrgRequestThunk = sentinel::CallbackThunk<RequestTag<Prerequisite::OrgScopedId>, std::uint64_t(std::uint64_t)>;
using UserRequestThunk = sentinel::CallbackThunk<RequestTag<Prerequisite::LoggedInUser>, std::uint64_t()>;
using TokenRequestThunk = sentinel::CallbackThunk<RequestTag<Prerequisite::AccessToken>, std::uint64_t()>;
using ProofRequestThunk = sentinel::CallbackThunk<RequestTag<Prerequisite::UserProof>, std::uint64_t()>;

std::uint64_t HandleOrgRequest(OrgRequestThunk::Fn original, std::uint64_t user) noexcept {
  const std::uint64_t request = original(user);
  NoteRequest(Prerequisite::OrgScopedId, request);
  return request;
}
std::uint64_t HandleUserRequest(UserRequestThunk::Fn original) noexcept {
  const std::uint64_t request = original();
  NoteRequest(Prerequisite::LoggedInUser, request);
  return request;
}
std::uint64_t HandleTokenRequest(TokenRequestThunk::Fn original) noexcept {
  const std::uint64_t request = original();
  NoteRequest(Prerequisite::AccessToken, request);
  return request;
}
std::uint64_t HandleProofRequest(ProofRequestThunk::Fn original) noexcept {
  const std::uint64_t request = original();
  NoteRequest(Prerequisite::UserProof, request);
  return request;
}
NEVR_HOOK_RECORD(kOrgRequestHook, OrgRequestThunk, &HandleOrgRequest);
NEVR_HOOK_RECORD(kUserRequestHook, UserRequestThunk, &HandleUserRequest);
NEVR_HOOK_RECORD(kTokenRequestHook, TokenRequestThunk, &HandleTokenRequest);
NEVR_HOOK_RECORD(kProofRequestHook, ProofRequestThunk, &HandleProofRequest);

// Process-lifetime hook handles. A GotHook has a non-constexpr member initializer, so a
// namespace-scope one would need a dynamic initializer (.init_array); this lives on the heap and
// is never destroyed, the same as login_hook.cpp's state.
struct Hooks {
  sentinel::GotHook callbacks[4];
  sentinel::GotHook accessors[8];
  sentinel::GotHook requests[4];
};
Hooks& H() {
  static Hooks* const hooks = new Hooks();
  return *hooks;
}
std::mutex& InstallMutex() {
  static std::mutex* const mutex = new std::mutex();
  return *mutex;
}
std::atomic<bool> g_installed{false};
PrerequisiteInstall g_result_storage;  // written once under InstallMutex(), then read-only

template <typename Thunk>
bool Install(sentinel::GotHook& hook, const sentinel::HookRecord<Thunk>& record, const T::PinnedSlot& slot,
             std::uintptr_t base) {
  Thunk::Arm(record);
  if (sentinel::InstallThunk<Thunk>(hook, T::TargetFor(slot, base)) == sentinel::GotStatus::kOk) return true;
  Thunk::Disarm();
  return false;
}

// The value the dynamic linker bound into `slot` (BIND_NOW): the function the game calls.
template <typename Fn>
Fn ReadBound(const sentinel::ElfImage& image, const T::PinnedSlot& slot) {
  const sentinel::SlotResolution resolved =
      sentinel::ResolveSlot(image, T::TargetFor(slot), sentinel::kNativeRelocs);
  if (resolved.status != sentinel::GotStatus::kOk || resolved.slot == nullptr) return nullptr;
  void* bound = __atomic_load_n(resolved.slot, __ATOMIC_ACQUIRE);
  return reinterpret_cast<Fn>(bound);
}

}  // namespace

PrerequisiteInstall InstallLoginPrerequisites(const sentinel::ElfImage& image, ReadyFn ready, ResetFn reset) noexcept {
  const std::lock_guard<std::mutex> lock(InstallMutex());
  if (g_installed.load(std::memory_order_acquire)) return g_result_storage;
  const std::uintptr_t base = image.base;
  Hooks& hooks = H();
  PrerequisiteInstall result;

  // Read before ovr_Message_IsError is hooked, so this is the real function.
  OvrErrorApi api{};
  api.message_is_error = ReadBound<bool (*)(const void*)>(image, T::kMessageIsError);
  api.message_get_error = ReadBound<const void* (*)(const void*)>(image, T::kMessageGetError);
  api.error_get_code = ReadBound<int (*)(const void*)>(image, T::kErrorGetCode);
  api.error_get_http_code = ReadBound<int (*)(const void*)>(image, T::kErrorGetHttpCode);
  api.error_get_message = ReadBound<const char* (*)(const void*)>(image, T::kErrorGetMessage);

  // The accessors first: they pass through unless a login callback has claimed the message, and
  // no callback can claim one before ConfigurePrerequisites below.
  result.accessors += Install(hooks.accessors[0], kIsErrorHook, T::kMessageIsError, base) ? 1 : 0;
  result.accessors += Install(hooks.accessors[1], kGetStringHook, T::kMessageGetString, base) ? 1 : 0;
  result.accessors += Install(hooks.accessors[2], kGetOrgScopedIdHook, T::kMessageGetOrgScopedId, base) ? 1 : 0;
  result.accessors += Install(hooks.accessors[3], kOrgScopedIdGetIdHook, T::kOrgScopedIdGetId, base) ? 1 : 0;
  result.accessors += Install(hooks.accessors[4], kGetUserHook, T::kMessageGetUser, base) ? 1 : 0;
  result.accessors += Install(hooks.accessors[5], kUserGetOculusIdHook, T::kUserGetOculusId, base) ? 1 : 0;
  result.accessors += Install(hooks.accessors[6], kGetUserProofHook, T::kMessageGetUserProof, base) ? 1 : 0;
  result.accessors += Install(hooks.accessors[7], kUserProofGetNonceHook, T::kUserProofGetNonce, base) ? 1 : 0;

  result.substitute = SubstitutionAllowed(result.accessors, api.message_is_error != nullptr);
  ConfigurePrerequisites(api, result.substitute, ready, reset);

  result.callbacks += Install(hooks.callbacks[0], kOrgCallbackHook, T::kOrgScopedIdCallback, base) ? 1 : 0;
  result.callbacks += Install(hooks.callbacks[1], kUserCallbackHook, T::kLoggedInUserCallback, base) ? 1 : 0;
  result.callbacks += Install(hooks.callbacks[2], kTokenCallbackHook, T::kAccessTokenCallback, base) ? 1 : 0;
  result.callbacks += Install(hooks.callbacks[3], kProofCallbackHook, T::kUserProofCallback, base) ? 1 : 0;

  result.requests += Install(hooks.requests[0], kOrgRequestHook, T::kGetOrgScopedId, base) ? 1 : 0;
  result.requests += Install(hooks.requests[1], kUserRequestHook, T::kGetLoggedInUser, base) ? 1 : 0;
  result.requests += Install(hooks.requests[2], kTokenRequestHook, T::kGetAccessToken, base) ? 1 : 0;
  result.requests += Install(hooks.requests[3], kProofRequestHook, T::kGetUserProof, base) ? 1 : 0;

  const bool complete = result.callbacks == 4 && result.accessors == 8 && result.requests == 4;
  sentinel::LogFields(complete ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError,
                      "quest_login_prerequisites_install",
                      {{"status", complete ? "installed" : "partial"},
                       {"callbacks", result.callbacks},
                       {"accessors", result.accessors},
                       {"requests", result.requests},
                       {"substitution", result.substitute ? "on" : "off"},
                       {"ready_gated", ready != nullptr ? 1 : 0},
                       {"error_api", api.message_get_error != nullptr && api.error_get_code != nullptr ? 1 : 0}});
  // What can still read a stand-in after a NEVR login (login_prerequisites.h "Residual"): a stood-in
  // access token stays in the engine's token string, read by the matchmaker queue URLs; the user
  // object's construction-time copy of the name is not rewritten. Say so once at install.
  if (result.substitute) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "quest_login_prerequisites_residual",
                        {{"access_token_string", "matchmaker_queue_urls"},
                         {"user_object_name_copy", "not_rewritten"}});
  }
  g_result_storage = result;
  g_installed.store(true, std::memory_order_release);
  return result;
}

}  // namespace QuestLogin
