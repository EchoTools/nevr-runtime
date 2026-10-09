#include "quest/login/login_prerequisites.h"

#include <atomic>
#include <cstdint>

#include "quest/sentinel/hook_log.h"

// Built -fno-exceptions (src/quest/CMakeLists.txt): every handler below is live while game code
// runs. Nothing here has a destructor or a try/catch, and no namespace-scope object needs a
// dynamic initializer (all are constant-initialized), so nothing lands in .init_array.

namespace QuestLogin {

namespace {

// One login callback in progress. Lives on the handler's stack while the game's callback runs;
// the accessor handlers reach it through g_active. An attempt is published (claimed) only when
// allow_synth is true, so the accessor handlers substitute only for a claimed attempt.
struct Attempt {
  Prerequisite which;
  bool errored;          // the real ovr_Message_IsError said so
  bool error_measured;   // ovr_Message_IsError was available to ask
  bool substituted;      // the game was given a synthesized value
  const char* reason;    // why (a fixed token)
  const char* accessor;  // which accessor substituted, or ""
};

// Plain aggregate: zero-initialized static storage, published by g_configured.
struct Config {
  OvrErrorApi api;
  bool substitute;
  ReadyFn ready;
};

Config g_config;
std::atomic<bool> g_configured{false};

std::atomic<Attempt*> g_active{nullptr};
std::atomic<const void*> g_active_message{nullptr};
std::atomic<const void*> g_active_handle{nullptr};
std::atomic<bool> g_synthesized{false};

std::atomic<std::uint64_t> g_callbacks[kPrerequisiteCount] = {};
std::atomic<std::uint64_t> g_requests[kPrerequisiteCount] = {};
std::atomic<std::uint64_t> g_transient_passes[kPrerequisiteCount] = {};

// Stand-ins for a handle Oculus did not give. Only their addresses matter; a fake handle is never
// passed to a real Platform SDK function.
struct FakeHandle {
  char tag;
};
constexpr FakeHandle kFakeOrgScopedId{'o'};
constexpr FakeHandle kFakeUser{'u'};
constexpr FakeHandle kFakeUserProof{'p'};

std::size_t Index(Prerequisite which) { return static_cast<std::size_t>(which); }

bool UsableString(const char* value) {
  return value != nullptr && value[0] != '\0' && !(value[0] == '?' && value[1] == '\0');
}

bool UsableId(std::uint64_t value) { return value != 0 && value != ~std::uint64_t{0}; }

// The attempt handling `message` for `which`, or nullptr. Only the thread running that callback
// can hold the message, so a match is that thread's own attempt.
Attempt* AttemptFor(const void* message, Prerequisite which) {
  if (message == nullptr || g_active_message.load(std::memory_order_acquire) != message) return nullptr;
  Attempt* attempt = g_active.load(std::memory_order_acquire);
  return attempt != nullptr && attempt->which == which ? attempt : nullptr;
}

// The attempt whose real first-level accessor returned `handle`, or nullptr.
Attempt* AttemptForHandle(const void* handle, Prerequisite which) {
  if (handle == nullptr || g_active_handle.load(std::memory_order_acquire) != handle) return nullptr;
  Attempt* attempt = g_active.load(std::memory_order_acquire);
  return attempt != nullptr && attempt->which == which ? attempt : nullptr;
}

// The reason is the first one (an ovr error stays the reason); the accessor is the last one that
// substituted, so for an error it names the accessor that handed the game the synthesized value.
void MarkSubstituted(Attempt& attempt, const char* reason, const char* accessor) {
  if (!attempt.substituted) {
    attempt.substituted = true;
    attempt.reason = reason;
    g_synthesized.store(true, std::memory_order_release);
  }
  attempt.accessor = accessor;
}

// Scans an Oculus error message (JSON) for "is_transient" set true. The game reads the same signal
// as the boolean path "error|is_transient" and re-requests when it is set (callbacks 0x1ece60 etc.);
// a byte scan keeps this noexcept and allocation-free. Conservative: an absent or unreadable
// message is not transient.
bool IsTransient(const char* message) {
  if (message == nullptr) return false;
  const char* key = "is_transient";
  for (const char* p = message; *p != '\0'; ++p) {
    const char* k = key;
    const char* q = p;
    while (*k != '\0' && *q == *k) {
      ++q;
      ++k;
    }
    if (*k != '\0') continue;
    // After the key, skip quotes/colon/spaces to the value and test for "true".
    while (*q == '"' || *q == ':' || *q == ' ' || *q == '\t') ++q;
    if (q[0] == 't' && q[1] == 'r' && q[2] == 'u' && q[3] == 'e') return true;
  }
  return false;
}

// First-level accessor (message -> handle) for `which`.
const void* HandleAccessor(const void* (*original)(const void*), const void* message, Prerequisite which,
                           const FakeHandle& fake, const char* accessor) {
  Attempt* attempt = AttemptFor(message, which);
  if (attempt == nullptr) return original(message);
  if (attempt->errored) {
    MarkSubstituted(*attempt, "ovr_error", accessor);
    return &fake;
  }
  const void* handle = original(message);
  if (handle == nullptr) {
    MarkSubstituted(*attempt, "empty_value", accessor);
    return &fake;
  }
  g_active_handle.store(handle, std::memory_order_release);
  return handle;
}

// Second-level string accessor (handle -> string) for `which`.
const char* StringAccessor(const char* (*original)(const void*), const void* handle, Prerequisite which,
                           const FakeHandle& fake, const char* synthesized, const char* accessor) {
  if (handle == &fake) return synthesized;  // never hand a fake handle to the real SDK
  Attempt* attempt = AttemptForHandle(handle, which);
  const char* real = original(handle);
  if (attempt == nullptr || UsableString(real)) return real;
  MarkSubstituted(*attempt, "empty_value", accessor);
  return synthesized;
}

sentinel::LogLevel LevelFor(const Attempt& attempt) {
  if (attempt.substituted) return sentinel::LogLevel::kWarn;
  if (attempt.errored) return sentinel::LogLevel::kError;  // the game got the error itself
  return sentinel::LogLevel::kInfo;
}

}  // namespace

const char* PrerequisiteCall(Prerequisite which) {
  switch (which) {
    case Prerequisite::OrgScopedId: return "ovr_User_GetOrgScopedID";
    case Prerequisite::LoggedInUser: return "ovr_User_GetLoggedInUser";
    case Prerequisite::AccessToken: return "ovr_User_GetAccessToken";
    default: return "ovr_User_GetUserProof";
  }
}

void ConfigurePrerequisites(const OvrErrorApi& api, bool substitute, ReadyFn ready) noexcept {
  g_config.api = api;
  g_config.substitute = substitute && api.message_is_error != nullptr;
  g_config.ready = ready;
  g_configured.store(true, std::memory_order_release);
}

bool PrerequisitesSynthesizedSinceReset() noexcept { return g_synthesized.load(std::memory_order_acquire); }
void ResetPrerequisitesSynthesisMark() noexcept { g_synthesized.store(false, std::memory_order_release); }

void OnPrerequisiteCallback(Prerequisite which, GameCallback original, void* self, void* message) noexcept {
  if (!g_configured.load(std::memory_order_acquire) || message == nullptr) {
    original(self, message);
    return;
  }
  const std::uint64_t call = g_callbacks[Index(which)].fetch_add(1, std::memory_order_relaxed) + 1;
  const OvrErrorApi& api = g_config.api;

  Attempt attempt{which, false, false, false, "ok", ""};
  long long error_code = 0;
  long long http_code = 0;
  bool transient = false;
  if (api.message_is_error != nullptr) {
    attempt.error_measured = true;
    attempt.errored = api.message_is_error(message);
  } else {
    attempt.reason = "unmeasured";
  }
  if (attempt.errored && api.message_get_error != nullptr) {
    const void* error = api.message_get_error(message);
    if (error != nullptr && api.error_get_code != nullptr) error_code = api.error_get_code(error);
    if (error != nullptr && api.error_get_http_code != nullptr) http_code = api.error_get_http_code(error);
    if (error != nullptr && api.error_get_message != nullptr) transient = IsTransient(api.error_get_message(error));
  }

  // A transient error is passed through so the game's own re-request runs, up to a cap per
  // prerequisite; after the cap a permanently-transient error is synthesized so login still
  // proceeds. The pass is only counted when it is actually taken (ready and able to substitute),
  // so a not-ready transient error does not burn the budget.
  const bool ready = g_config.ready != nullptr && g_config.ready();
  const bool could_substitute = g_config.substitute && ready;
  bool transient_hold = false;
  if (could_substitute && transient &&
      g_transient_passes[Index(which)].load(std::memory_order_relaxed) < kMaxTransientPasses) {
    g_transient_passes[Index(which)].fetch_add(1, std::memory_order_relaxed);
    transient_hold = true;
  }
  const bool allow_synth = could_substitute && !transient_hold;

  bool claimed = false;
  if (allow_synth) {
    Attempt* expected = nullptr;
    if (g_active.compare_exchange_strong(expected, &attempt, std::memory_order_acq_rel)) {
      g_active_message.store(message, std::memory_order_release);
      claimed = true;
    }
  }

  original(self, message);

  if (claimed) {
    g_active_message.store(nullptr, std::memory_order_release);
    g_active_handle.store(nullptr, std::memory_order_release);
    g_active.store(nullptr, std::memory_order_release);
  }

  if (attempt.errored && !attempt.substituted) {
    // The game handled the error itself: say why nothing was substituted.
    attempt.reason = !g_config.substitute ? "substitution_unavailable"
                     : !ready             ? "not_ready"
                     : transient_hold     ? "transient_passthrough"
                     : !claimed           ? "busy"
                                          : "error_not_consulted";  // the game never asked IsError
  }

  // Cap the per-call log so a callback the game spins on cannot flood the sink; the counter above
  // still counts every one.
  if (call <= kCallbackLogLimit) {
    sentinel::LogFields(LevelFor(attempt), "quest_login_prerequisite",
                        {{"call", PrerequisiteCall(which)},
                         {"result", attempt.substituted ? "synthesized" : "real"},
                         {"reason", attempt.reason},
                         {"accessor", attempt.accessor},
                         {"ovr_error", attempt.error_measured ? (attempt.errored ? 1 : 0) : -1},
                         {"error_code", error_code},
                         {"http_code", http_code},
                         {"callback", call}});
  } else if (call == kCallbackLogLimit + 1) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "quest_login_prerequisite",
                        {{"call", PrerequisiteCall(which)}, {"status", "log_limit_reached"}, {"callback", call}});
  }
}

bool OnMessageIsError(bool (*original)(const void*), const void* message) noexcept {
  if (message != nullptr && g_active_message.load(std::memory_order_acquire) == message) {
    Attempt* attempt = g_active.load(std::memory_order_acquire);
    if (attempt != nullptr && attempt->errored) {
      MarkSubstituted(*attempt, "ovr_error", "ovr_Message_IsError");
      return false;
    }
  }
  return original(message);
}

const char* OnMessageGetString(const char* (*original)(const void*), const void* message) noexcept {
  Attempt* attempt = AttemptFor(message, Prerequisite::AccessToken);
  if (attempt == nullptr) return original(message);
  if (attempt->errored) {
    MarkSubstituted(*attempt, "ovr_error", "ovr_Message_GetString");
    return kSynthesizedAccessToken;
  }
  const char* real = original(message);
  if (UsableString(real)) return real;
  MarkSubstituted(*attempt, "empty_value", "ovr_Message_GetString");
  return kSynthesizedAccessToken;
}

const void* OnMessageGetOrgScopedId(const void* (*original)(const void*), const void* message) noexcept {
  return HandleAccessor(original, message, Prerequisite::OrgScopedId, kFakeOrgScopedId,
                        "ovr_Message_GetOrgScopedID");
}

std::uint64_t OnOrgScopedIdGetId(std::uint64_t (*original)(const void*), const void* handle) noexcept {
  if (handle == &kFakeOrgScopedId) return kSynthesizedOrgScopedId;
  Attempt* attempt = AttemptForHandle(handle, Prerequisite::OrgScopedId);
  const std::uint64_t real = original(handle);
  if (attempt == nullptr || UsableId(real)) return real;
  MarkSubstituted(*attempt, "empty_value", "ovr_OrgScopedID_GetID");
  return kSynthesizedOrgScopedId;
}

const void* OnMessageGetUser(const void* (*original)(const void*), const void* message) noexcept {
  return HandleAccessor(original, message, Prerequisite::LoggedInUser, kFakeUser, "ovr_Message_GetUser");
}

const char* OnUserGetOculusId(const char* (*original)(const void*), const void* handle) noexcept {
  return StringAccessor(original, handle, Prerequisite::LoggedInUser, kFakeUser, kSynthesizedOculusId,
                        "ovr_User_GetOculusID");
}

const void* OnMessageGetUserProof(const void* (*original)(const void*), const void* message) noexcept {
  return HandleAccessor(original, message, Prerequisite::UserProof, kFakeUserProof,
                        "ovr_Message_GetUserProof");
}

const char* OnUserProofGetNonce(const char* (*original)(const void*), const void* handle) noexcept {
  return StringAccessor(original, handle, Prerequisite::UserProof, kFakeUserProof, kSynthesizedNonce,
                        "ovr_UserProof_GetNonce");
}

void NoteRequest(Prerequisite which, std::uint64_t request_id) noexcept {
  const std::uint64_t count = g_requests[Index(which)].fetch_add(1, std::memory_order_relaxed) + 1;
  if (count <= kRequestLogLimit) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "quest_login_prerequisite_request",
                        {{"call", PrerequisiteCall(which)}, {"request_id", request_id}, {"request", count}});
  } else if (count == kRequestLogLimit + 1) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "quest_login_prerequisite_request",
                        {{"call", PrerequisiteCall(which)}, {"status", "log_limit_reached"}, {"request", count}});
  }
}

std::uint64_t PrerequisiteCallbacks(Prerequisite which) noexcept {
  return g_callbacks[Index(which)].load(std::memory_order_relaxed);
}

std::uint64_t PrerequisiteRequests(Prerequisite which) noexcept {
  return g_requests[Index(which)].load(std::memory_order_relaxed);
}

void ResetPrerequisitesForTest() noexcept {
  g_configured.store(false, std::memory_order_release);
  g_config = Config{};
  g_active.store(nullptr, std::memory_order_release);
  g_active_message.store(nullptr, std::memory_order_release);
  g_active_handle.store(nullptr, std::memory_order_release);
  g_synthesized.store(false, std::memory_order_release);
  for (std::size_t i = 0; i < kPrerequisiteCount; ++i) {
    g_callbacks[i].store(0, std::memory_order_relaxed);
    g_requests[i].store(0, std::memory_order_relaxed);
    g_transient_passes[i].store(0, std::memory_order_relaxed);
  }
}

}  // namespace QuestLogin
