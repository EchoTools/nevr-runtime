// Installs the login-prerequisite hooks (login_prerequisites.h) into the pinned libpnsovr.so.
// Built -fno-exceptions: it defines the thunks and their handlers (callback_thunk.h, rule 2).

#include <atomic>
#include <cstdint>
#include <mutex>

#include "quest/login/login_prerequisite_targets.h"
#include "quest/login/login_prerequisite_thunks.h"
#include "quest/login/login_prerequisites.h"
#include "quest/sentinel/callback_thunk.h"
#include "quest/sentinel/got_hook.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest_login {

namespace {

namespace T = PrerequisiteTargets;

// ---- callbacks (the thunk types are in login_prerequisite_thunks.h) ---------------------------
template <Prerequisite P>
void HandleCallback(typename CallbackThunk<P>::Fn original, void* self, void* message) noexcept {
  OnPrerequisiteCallback(P, original, self, message);
}

NEVR_HOOK_RECORD(kOrgCallbackHook, OrgCallbackThunk, &HandleCallback<Prerequisite::OrgScopedId>);
NEVR_HOOK_RECORD(kUserCallbackHook, UserCallbackThunk, &HandleCallback<Prerequisite::LoggedInUser>);
NEVR_HOOK_RECORD(kTokenCallbackHook, TokenCallbackThunk, &HandleCallback<Prerequisite::AccessToken>);
NEVR_HOOK_RECORD(kProofCallbackHook, ProofCallbackThunk, &HandleCallback<Prerequisite::UserProof>);

// ---- accessors -------------------------------------------------------------------------------
NEVR_HOOK_RECORD(kIsErrorHook, IsErrorThunk, &OnMessageIsError);
NEVR_HOOK_RECORD(kGetStringHook, GetStringThunk, &OnMessageGetString);
NEVR_HOOK_RECORD(kGetOrgScopedIdHook, GetOrgScopedIdThunk, &OnMessageGetOrgScopedId);
NEVR_HOOK_RECORD(kOrgScopedIdGetIdHook, OrgScopedIdGetIdThunk, &OnOrgScopedIdGetId);
NEVR_HOOK_RECORD(kGetUserHook, GetUserThunk, &OnMessageGetUser);
NEVR_HOOK_RECORD(kUserGetOculusIdHook, UserGetOculusIdThunk, &OnUserGetOculusId);
NEVR_HOOK_RECORD(kGetUserProofHook, GetUserProofThunk, &OnMessageGetUserProof);
NEVR_HOOK_RECORD(kUserProofGetNonceHook, UserProofGetNonceThunk, &OnUserProofGetNonce);

// ---- requests --------------------------------------------------------------------------------
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

}  // namespace nevr_quest_login
