#include "quest/login/login_prerequisites.h"

#include <time.h>

#include <atomic>
#include <cstdint>

#include "quest/sentinel/hook_log.h"

// Built -fno-exceptions (src/quest/CMakeLists.txt): every handler below is live while game code
// runs. Nothing here has a destructor or a try/catch, and no namespace-scope object needs a
// dynamic initializer (all are constant-initialized), so nothing lands in .init_array.

namespace QuestLogin {

namespace {

// One login callback in progress. Lives on the handler's stack while the game's callback runs;
// the accessor handlers reach it through g_active. An attempt is published (claimed) only when it
// may stand in, so the accessor handlers act only for a claimed attempt.
struct Attempt {
  Prerequisite which;
  bool errored;          // the real ovr_Message_IsError said so
  bool error_measured;   // ovr_Message_IsError was available to ask
  bool substituted;      // the game was given a stand-in
  const char* reason;    // why (a fixed token)
  const char* accessor;  // the last accessor that handed out a stand-in, or ""
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

std::atomic<std::uint64_t> g_callbacks[kPrerequisiteCount] = {};
std::atomic<std::uint64_t> g_requests[kPrerequisiteCount] = {};

// Per-attempt state, reset by EndPrerequisiteAttempt. 0 means "not started".
std::atomic<std::uint64_t> g_transient_first_ms[kPrerequisiteCount] = {};
std::atomic<std::uint64_t> g_transient_passes[kPrerequisiteCount] = {};
std::atomic<std::uint64_t> g_callback_window_ms[kPrerequisiteCount] = {};
std::atomic<std::uint64_t> g_callback_logged[kPrerequisiteCount] = {};
std::atomic<std::uint64_t> g_request_window_ms[kPrerequisiteCount] = {};
std::atomic<std::uint64_t> g_request_logged[kPrerequisiteCount] = {};

std::uint64_t RealMonotonicMs() noexcept {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 1;
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000u + 1u;
}
std::atomic<MonotonicMsFn> g_clock{&RealMonotonicMs};
std::uint64_t NowMs() { return g_clock.load(std::memory_order_acquire)(); }

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
// handed out a stand-in.
void MarkSubstituted(Attempt& attempt, const char* reason, const char* accessor) {
  if (!attempt.substituted) {
    attempt.substituted = true;
    attempt.reason = reason;
  }
  attempt.accessor = accessor;
}

// ---- the game's transient test, as a strict JSON walk ----------------------------------------
// No allocation, depth-limited. A key with an escape sequence never matches (jansson would decode
// it; a real Oculus error does not escape these two plain-ASCII keys).

struct Json {
  const char* p;
};

void SkipWs(Json& j) {
  while (*j.p == ' ' || *j.p == '\t' || *j.p == '\n' || *j.p == '\r') ++j.p;
}

// Reads a string at j.p ('"' expected). Sets [*start, *start + *len) to the raw contents and
// *escaped when it holds a backslash.
bool ReadString(Json& j, const char** start, std::size_t* len, bool* escaped) {
  if (*j.p != '"') return false;
  ++j.p;
  *start = j.p;
  *escaped = false;
  while (*j.p != '"') {
    const unsigned char c = static_cast<unsigned char>(*j.p);
    if (c == '\0' || c < 0x20) return false;
    if (c == '\\') {
      *escaped = true;
      ++j.p;
      if (*j.p == '\0') return false;
    }
    ++j.p;
  }
  *len = static_cast<std::size_t>(j.p - *start);
  ++j.p;
  return true;
}

bool KeyIs(const char* start, std::size_t len, bool escaped, const char* want) {
  if (escaped) return false;
  std::size_t i = 0;
  for (; i < len; ++i) {
    if (want[i] == '\0' || want[i] != start[i]) return false;
  }
  return want[i] == '\0';
}

bool Literal(Json& j, const char* word) {
  const char* q = j.p;
  for (; *word != '\0'; ++word, ++q) {
    if (*q != *word) return false;
  }
  j.p = q;
  return true;
}

bool SkipValue(Json& j, int depth);

bool SkipNumber(Json& j) {
  if (*j.p == '-') ++j.p;
  if (*j.p < '0' || *j.p > '9') return false;
  while (*j.p >= '0' && *j.p <= '9') ++j.p;
  if (*j.p == '.') {
    ++j.p;
    if (*j.p < '0' || *j.p > '9') return false;
    while (*j.p >= '0' && *j.p <= '9') ++j.p;
  }
  if (*j.p == 'e' || *j.p == 'E') {
    ++j.p;
    if (*j.p == '+' || *j.p == '-') ++j.p;
    if (*j.p < '0' || *j.p > '9') return false;
    while (*j.p >= '0' && *j.p <= '9') ++j.p;
  }
  return true;
}

// Walks an object at j.p. For each member calls `on_member(key...)`, which consumes the value.
template <typename OnMember>
bool WalkObject(Json& j, int depth, OnMember on_member) {
  if (depth > 32 || *j.p != '{') return false;
  ++j.p;
  SkipWs(j);
  if (*j.p == '}') {
    ++j.p;
    return true;
  }
  for (;;) {
    const char* key = nullptr;
    std::size_t len = 0;
    bool escaped = false;
    SkipWs(j);
    if (!ReadString(j, &key, &len, &escaped)) return false;
    SkipWs(j);
    if (*j.p != ':') return false;
    ++j.p;
    SkipWs(j);
    if (!on_member(key, len, escaped)) return false;
    SkipWs(j);
    if (*j.p == ',') {
      ++j.p;
      continue;
    }
    if (*j.p == '}') {
      ++j.p;
      return true;
    }
    return false;
  }
}

bool SkipValue(Json& j, int depth) {
  if (depth > 32) return false;
  SkipWs(j);
  switch (*j.p) {
    case '"': {
      const char* s = nullptr;
      std::size_t n = 0;
      bool e = false;
      return ReadString(j, &s, &n, &e);
    }
    case '{':
      return WalkObject(j, depth + 1, [&](const char*, std::size_t, bool) { return SkipValue(j, depth + 1); });
    case '[': {
      ++j.p;
      SkipWs(j);
      if (*j.p == ']') {
        ++j.p;
        return true;
      }
      for (;;) {
        if (!SkipValue(j, depth + 1)) return false;
        SkipWs(j);
        if (*j.p == ',') {
          ++j.p;
          continue;
        }
        if (*j.p == ']') {
          ++j.p;
          return true;
        }
        return false;
      }
    }
    case 't': return Literal(j, "true");
    case 'f': return Literal(j, "false");
    case 'n': return Literal(j, "null");
    default: return SkipNumber(j);
  }
}

sentinel::LogLevel LevelFor(const Attempt& attempt) {
  if (attempt.substituted) return sentinel::LogLevel::kWarn;
  if (attempt.errored) return sentinel::LogLevel::kError;  // the game got the error itself
  return sentinel::LogLevel::kInfo;
}

// Windowed cap: true when this record may be logged with its fields; *summary when this is the
// first record past the cap in the current window (log one summary line instead).
bool WindowAllows(std::atomic<std::uint64_t>& window_start, std::atomic<std::uint64_t>& logged,
                  std::uint64_t limit, bool* summary) {
  *summary = false;
  const std::uint64_t now = NowMs();
  const std::uint64_t start = window_start.load(std::memory_order_relaxed);
  if (start == 0 || now - start >= kLogWindowMs) {
    window_start.store(now, std::memory_order_relaxed);
    logged.store(0, std::memory_order_relaxed);
  }
  const std::uint64_t n = logged.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= limit) return true;
  *summary = n == limit + 1;
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
                           const FakeHandle& fake, const char* stand_in, const char* accessor) {
  if (handle == &fake) return stand_in;  // never hand a fake handle to the real SDK
  Attempt* attempt = AttemptForHandle(handle, which);
  const char* real = original(handle);
  if (attempt == nullptr || UsableString(real)) return real;
  MarkSubstituted(*attempt, "empty_value", accessor);
  return stand_in;
}

}  // namespace

bool IsTransientErrorMessage(const char* message) noexcept {
  // The game decodes only when the first byte is '{' (0x1ecea8 `cmp w8,#0x7b`) and treats a decode
  // error as not transient; CJson::Boolean on "error|is_transient" is true only for the literal true.
  if (message == nullptr || message[0] != '{') return false;
  Json j{message};
  bool transient = false;
  const bool parsed = WalkObject(j, 0, [&](const char* key, std::size_t len, bool escaped) {
    if (!KeyIs(key, len, escaped, "error")) return SkipValue(j, 1);
    if (*j.p != '{') {
      transient = false;  // a later "error" member replaces an earlier one, as in jansson
      return SkipValue(j, 1);
    }
    bool inner = false;
    const bool ok = WalkObject(j, 1, [&](const char* k, std::size_t n, bool e) {
      if (!KeyIs(k, n, e, "is_transient")) return SkipValue(j, 2);
      if (Literal(j, "true")) {
        inner = true;
        return true;
      }
      inner = false;
      return SkipValue(j, 2);
    });
    transient = inner;
    return ok;
  });
  if (!parsed) return false;
  SkipWs(j);
  return *j.p == '\0' && transient;
}

const char* PrerequisiteCall(Prerequisite which) {
  switch (which) {
    case Prerequisite::OrgScopedId: return "ovr_User_GetOrgScopedID";
    case Prerequisite::LoggedInUser: return "ovr_User_GetLoggedInUser";
    case Prerequisite::AccessToken: return "ovr_User_GetAccessToken";
    default: return "ovr_User_GetUserProof";
  }
}

bool SubstitutionAllowed(int accessors_hooked, bool have_is_error) noexcept {
  return accessors_hooked == 8 && have_is_error;
}

void ConfigurePrerequisites(const OvrErrorApi& api, bool substitute, ReadyFn ready) noexcept {
  StandIn::Generate();
  g_config.api = api;
  g_config.substitute = substitute && api.message_is_error != nullptr;
  g_config.ready = ready;
  g_configured.store(true, std::memory_order_release);
}

void EndPrerequisiteAttempt() noexcept {
  for (std::size_t i = 0; i < kPrerequisiteCount; ++i) {
    g_transient_first_ms[i].store(0, std::memory_order_relaxed);
    g_transient_passes[i].store(0, std::memory_order_relaxed);
    g_callback_window_ms[i].store(0, std::memory_order_relaxed);
    g_callback_logged[i].store(0, std::memory_order_relaxed);
    g_request_window_ms[i].store(0, std::memory_order_relaxed);
    g_request_logged[i].store(0, std::memory_order_relaxed);
  }
}

void OnPrerequisiteCallback(Prerequisite which, GameCallback original, void* self, void* message) noexcept {
  if (!g_configured.load(std::memory_order_acquire) || message == nullptr) {
    original(self, message);
    return;
  }
  const std::size_t i = Index(which);
  const std::uint64_t call = g_callbacks[i].fetch_add(1, std::memory_order_relaxed) + 1;
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
    if (error != nullptr && api.error_get_message != nullptr) {
      transient = IsTransientErrorMessage(api.error_get_message(error));
    }
  }

  // A transient error is passed through so the game's own re-request runs, for a bounded window per
  // attempt; then a still-transient error is stood in. A non-transient answer ends the window.
  const bool ready = g_config.ready != nullptr && g_config.ready();
  const bool could_substitute = g_config.substitute && ready;
  bool transient_hold = false;
  if (could_substitute && transient) {
    const std::uint64_t now = NowMs();
    std::uint64_t first = g_transient_first_ms[i].load(std::memory_order_relaxed);
    if (first == 0) {
      first = now;
      g_transient_first_ms[i].store(first, std::memory_order_relaxed);
    }
    const std::uint64_t passes = g_transient_passes[i].load(std::memory_order_relaxed);
    if (now - first < kTransientWindowMs && passes < kMaxTransientPasses) {
      g_transient_passes[i].store(passes + 1, std::memory_order_relaxed);
      transient_hold = true;
    }
  }
  if (!transient) {
    g_transient_first_ms[i].store(0, std::memory_order_relaxed);
    g_transient_passes[i].store(0, std::memory_order_relaxed);
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
    // The game handled the error itself: say why nothing was stood in.
    attempt.reason = !g_config.substitute ? "substitution_unavailable"
                     : !ready             ? "not_ready"
                     : transient_hold     ? "transient_passthrough"
                     : !claimed           ? "busy"
                                          : "error_not_consulted";  // the game never asked IsError
  }

  bool summary = false;
  if (WindowAllows(g_callback_window_ms[i], g_callback_logged[i], kCallbackLogLimit, &summary)) {
    sentinel::LogFields(LevelFor(attempt), "quest_login_prerequisite",
                        {{"call", PrerequisiteCall(which)},
                         {"result", attempt.substituted ? "stand_in" : "real"},
                         {"reason", attempt.reason},
                         {"accessor", attempt.accessor},
                         {"ovr_error", attempt.error_measured ? (attempt.errored ? 1 : 0) : -1},
                         {"error_code", error_code},
                         {"http_code", http_code},
                         {"callback", call}});
  } else if (summary) {
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
    return StandIn::AccessToken();
  }
  const char* real = original(message);
  if (UsableString(real)) return real;
  MarkSubstituted(*attempt, "empty_value", "ovr_Message_GetString");
  return StandIn::AccessToken();
}

const void* OnMessageGetOrgScopedId(const void* (*original)(const void*), const void* message) noexcept {
  return HandleAccessor(original, message, Prerequisite::OrgScopedId, kFakeOrgScopedId,
                        "ovr_Message_GetOrgScopedID");
}

std::uint64_t OnOrgScopedIdGetId(std::uint64_t (*original)(const void*), const void* handle) noexcept {
  if (handle == &kFakeOrgScopedId) return StandIn::OrgId();
  Attempt* attempt = AttemptForHandle(handle, Prerequisite::OrgScopedId);
  const std::uint64_t real = original(handle);
  if (attempt == nullptr || UsableId(real)) return real;
  MarkSubstituted(*attempt, "empty_value", "ovr_OrgScopedID_GetID");
  return StandIn::OrgId();
}

const void* OnMessageGetUser(const void* (*original)(const void*), const void* message) noexcept {
  return HandleAccessor(original, message, Prerequisite::LoggedInUser, kFakeUser, "ovr_Message_GetUser");
}

const char* OnUserGetOculusId(const char* (*original)(const void*), const void* handle) noexcept {
  return StringAccessor(original, handle, Prerequisite::LoggedInUser, kFakeUser, StandIn::OculusId(),
                        "ovr_User_GetOculusID");
}

const void* OnMessageGetUserProof(const void* (*original)(const void*), const void* message) noexcept {
  return HandleAccessor(original, message, Prerequisite::UserProof, kFakeUserProof,
                        "ovr_Message_GetUserProof");
}

const char* OnUserProofGetNonce(const char* (*original)(const void*), const void* handle) noexcept {
  return StringAccessor(original, handle, Prerequisite::UserProof, kFakeUserProof, StandIn::Nonce(),
                        "ovr_UserProof_GetNonce");
}

void NoteRequest(Prerequisite which, std::uint64_t request_id) noexcept {
  const std::size_t i = Index(which);
  const std::uint64_t count = g_requests[i].fetch_add(1, std::memory_order_relaxed) + 1;
  bool summary = false;
  if (WindowAllows(g_request_window_ms[i], g_request_logged[i], kRequestLogLimit, &summary)) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "quest_login_prerequisite_request",
                        {{"call", PrerequisiteCall(which)}, {"request_id", request_id}, {"request", count}});
  } else if (summary) {
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
  for (std::size_t i = 0; i < kPrerequisiteCount; ++i) {
    g_callbacks[i].store(0, std::memory_order_relaxed);
    g_requests[i].store(0, std::memory_order_relaxed);
  }
  EndPrerequisiteAttempt();
}

void SetPrerequisiteClockForTest(MonotonicMsFn clock) noexcept {
  g_clock.store(clock != nullptr ? clock : &RealMonotonicMs, std::memory_order_release);
}

}  // namespace QuestLogin
