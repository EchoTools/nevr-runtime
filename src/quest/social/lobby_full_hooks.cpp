#include "quest/social/lobby_full_hooks.h"

#include "quest/social/lobby_full_backoff.h"

#include <pthread.h>
#include <time.h>

#include <cstring>

#include "callback_thunk.h"
#include "got_hook.h"
#include "hook_log.h"
#include "hook_install.h"
#include "libr15_attach.h"

namespace quest_social::lobby_full {
namespace {

using nevr_quest::libr15_patch::ActivateStatus;
using Verdict = FullLobbyBackoff::Verdict;

// The message offsets the game reads (CR15NetClientLobby::LobbySessionFailureCB, pinned 0x1240cc8). The join's key is the
// 16 bytes RequestJoin is given (Join, pinned 0x1240ba8: `CNSLobby::RequestJoin(param_1 + 0x18, param_2)`).
constexpr std::size_t kJoinKeyOffset = 0x18;
constexpr std::size_t kFailureDataOffset = 0x10;  // *(msg + 0x10) is the failure data
constexpr std::size_t kFailureCodeOffset = 0x18;  // data + 0x18 is the signed failure code

struct JoinTag {};
struct FailureTag {};
struct SuccessTag {};
using JoinThunk = sentinel::CallbackThunk<JoinTag, void(void*, const void*, const void*)>;
using FailureThunk = sentinel::CallbackThunk<FailureTag, void(void*, const void*)>;
using SuccessThunk = sentinel::CallbackThunk<SuccessTag, void(void*, const void*)>;

pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
LobbyKey g_lastJoin{};  // the lobby of the last join the game sent through
bool g_haveLastJoin = false;

// Built on first use, under g_mutex: a load-time constructor is what the sentinel does without.
FullLobbyBackoff& Backoff() {
  static FullLobbyBackoff backoff;
  return backoff;
}

std::uint64_t NowMs() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000u;
}

LobbyKey KeyOf(const void* msg) noexcept {
  LobbyKey key{};
  std::memcpy(key.data(), static_cast<const std::uint8_t*>(msg) + kJoinKeyOffset, key.size());
  return key;
}

// "0x" and the 32 lowercase hex digits of the key, NUL-terminated.
const char* KeyText(const LobbyKey& key, char (&out)[35]) noexcept {
  static const char kHex[] = "0123456789abcdef";
  out[0] = '0';
  out[1] = 'x';
  for (std::size_t i = 0; i < key.size(); ++i) {
    out[2 + 2 * i] = kHex[key[i] >> 4];
    out[3 + 2 * i] = kHex[key[i] & 0xF];
  }
  out[34] = '\0';
  return out;
}

bool FailureCode(const void* msg, std::int8_t* code) noexcept {
  if (msg == nullptr) return false;
  const void* data = *reinterpret_cast<const void* const*>(static_cast<const std::uint8_t*>(msg) + kFailureDataOffset);
  if (data == nullptr) return false;
  *code = static_cast<std::int8_t>(static_cast<const std::uint8_t*>(data)[kFailureCodeOffset]);
  return true;
}

// A join the policy holds back is skipped silently: the game calls Join again on the next tick, so a line here
// would be one line per tick. The decisions that change the policy are logged from the refusal callback.
void OnJoin(JoinThunk::Fn original, void* self, const void* msg, const void* arg) noexcept {
  if (msg == nullptr) {
    original(self, msg, arg);
    return;
  }
  const LobbyKey key = KeyOf(msg);
  FullLobbyBackoff::Decision d;
  pthread_mutex_lock(&g_mutex);
  d = Backoff().OnJoin(key, NowMs());
  if (d.verdict == Verdict::kSend) {
    g_lastJoin = key;
    g_haveLastJoin = true;
  }
  pthread_mutex_unlock(&g_mutex);
  if (d.verdict != Verdict::kSend) return;
  original(self, msg, arg);
}

void OnFailure(FailureThunk::Fn original, void* self, const void* msg) noexcept {
  std::int8_t code = 0;
  if (FailureCode(msg, &code) && code == kLobbyFullCode) {
    LobbyKey key{};
    bool haveLobby = false;
    FullLobbyBackoff::Decision d;
    pthread_mutex_lock(&g_mutex);
    if (g_haveLastJoin) {
      key = g_lastJoin;
      haveLobby = true;
      d = Backoff().OnFull(key, NowMs());
    }
    pthread_mutex_unlock(&g_mutex);
    char keyText[35];
    if (haveLobby && d.verdict == Verdict::kWait) {
      sentinel::LogFields(sentinel::LogLevel::kInfo, "lobby_full",
                          {{"status", "wait"},
                           {"refusal", d.refusals},
                           {"wait_ms", d.waitMs},
                           {"lobby", KeyText(key, keyText)}});
    } else if (haveLobby && d.verdict == Verdict::kGiveUp) {
      sentinel::LogFields(sentinel::LogLevel::kInfo, "lobby_full",
                          {{"status", "give_up"}, {"refusals", d.refusals}, {"lobby", KeyText(key, keyText)}});
    }
  }
  original(self, msg);
}

void OnSuccess(SuccessThunk::Fn original, void* self, const void* msg) noexcept {
  pthread_mutex_lock(&g_mutex);
  if (g_haveLastJoin) Backoff().OnSuccess(g_lastJoin);
  pthread_mutex_unlock(&g_mutex);
  original(self, msg);
}

NEVR_HOOK_RECORD(kJoinRecord, JoinThunk, &OnJoin);
NEVR_HOOK_RECORD(kFailureRecord, FailureThunk, &OnFailure);
NEVR_HOOK_RECORD(kSuccessRecord, SuccessThunk, &OnSuccess);

// Activates one table row. The line keeps its event and fields: `hook` names the function, `target` is the patched
// function's run-time address (0x0 when the row is not verified), and `status` is "installed" or the activation status.
template <typename Thunk>
bool InstallOne(const char* rowId, const char* name) noexcept {
  const ActivateStatus st = sentinel::InstallRowThunk<Thunk>(rowId);
  const bool ok = st == ActivateStatus::kOk;
  char targetText[19];
  sentinel::LogFields(ok ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "lobby_full",
                      {{"status", ok ? "installed" : nevr_quest::libr15_patch::ActivateStatusName(st)},
                       {"hook", name},
                       {"row", rowId},
                       {"target", sentinel::HexString(targetText, nevr_quest::libr15_patch::RowSiteAddress(rowId))}});
  return ok;
}

}  // namespace

std::size_t InstallLobbyFullHooks() noexcept {
  JoinThunk::Arm(kJoinRecord);
  FailureThunk::Arm(kFailureRecord);
  SuccessThunk::Arm(kSuccessRecord);
  std::size_t installed = 0;
  installed += InstallOne<JoinThunk>("lobby_full.join", "join") ? 1 : 0;
  installed += InstallOne<FailureThunk>("lobby_full.failure", "failure") ? 1 : 0;
  installed += InstallOne<SuccessThunk>("lobby_full.success", "success") ? 1 : 0;
  return installed;
}

}  // namespace quest_social::lobby_full
