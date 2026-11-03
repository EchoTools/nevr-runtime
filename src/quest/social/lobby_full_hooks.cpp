#include "quest/social/lobby_full_hooks.h"

#include "quest/social/lobby_full_backoff.h"
#include "quest/social/lobby_full_layout.h"

#include <pthread.h>
#include <time.h>

#include <cstring>

#include "callback_thunk.h"
#include "got_hook.h"
#include "hook_log.h"
#include "hook_install.h"
#include "libr15_attach.h"
#include "runtime/patch/lobby_failure_message_rules.h"

namespace quest_social::lobby_full {
namespace {

using nevr_quest::libr15_patch::ActivateStatus;
using Verdict = FullLobbyBackoff::Verdict;

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

// The declines already logged in this process, under g_declineMutex (built on first use, as Backoff is).
pthread_mutex_t g_declineMutex = PTHREAD_MUTEX_INITIALIZER;
nevr_lobby_failure_message::DeclineLog& Declines() {
  static nevr_lobby_failure_message::DeclineLog declines;
  return declines;
}

// The refusal records said so far, under the same lock (built on first use, as Declines is).
RefusalSayings& Sayings() {
  static RefusalSayings sayings;
  return sayings;
}

// True the first time this refusal record is said for the code in this process, false after (under g_declineMutex).
bool FirstRefusalSaying(std::uint8_t code, RefusalSayings::Record record) noexcept {
  pthread_mutex_lock(&g_declineMutex);
  const bool first = Sayings().FirstTime(code, record);
  pthread_mutex_unlock(&g_declineMutex);
  return first;
}

// The decline record: the decision and the text of the PC callback's line (lobby_failure_message_rules.h), as typed fields.
// The code, title and reason are fields; `msg` is the consequence, the sentence the PC line ends with.
void WriteDeclineLine(std::uint8_t code, nevr_lobby_failure_message::Decline reason) {
  sentinel::LogFields(sentinel::LogLevel::kInfo, "lobby_full",
                      {{"status", "message_not_shown"},
                       {"code", static_cast<std::uint64_t>(code)},
                       {"title", nevr_lobby_failure_message::Title(code)},
                       {"reason", nevr_lobby_failure_message::DeclineToken(reason)},
                       {"msg", nevr_lobby_failure_message::kDeclineConsequence}});
}

// A repeat of a decline this process has said: the same decision, the game's page unchanged, as a Debug record.
void WriteRepeatLine(std::uint8_t code, nevr_lobby_failure_message::Decline reason) {
  sentinel::LogFields(sentinel::LogLevel::kDebug, "lobby_full",
                      {{"status", "page_unchanged"},
                       {"code", static_cast<std::uint64_t>(code)},
                       {"reason", nevr_lobby_failure_message::DeclineToken(reason)},
                       {"msg", nevr_lobby_failure_message::kDeclineConsequence}});
}

// refusal_dropped. `game_status` is the status the gate was decided from; it is left out when there was no lobby client,
// so the record does not state a status the game never reported.
void WriteDropped(sentinel::LogLevel level, const RefusalReads& reads) {
  constexpr const char* kMsg = "the game ignored this refusal, so the back-off does not count it and no message is shown";
  if (reads.haveStatus) {
    sentinel::LogFields(level, "lobby_full",
                        {{"status", "refusal_dropped"},
                         {"code", static_cast<std::uint64_t>(kLobbyFullCode)},
                         {"reason", "status_gate_closed"},
                         {"game_status", reads.status},
                         {"msg", kMsg}});
  } else {
    sentinel::LogFields(level, "lobby_full",
                        {{"status", "refusal_dropped"},
                         {"code", static_cast<std::uint64_t>(kLobbyFullCode)},
                         {"reason", "status_gate_closed"},
                         {"msg", kMsg}});
  }
}

std::uint64_t NowMs() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000u + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000u;
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
  const bool lobbyFull = FailureCode(msg, &code) && code == kLobbyFullCode;
  // The game object whose error page shows the service's message, and the message, copied before the original runs.
  RefusalReads reads;
  if (lobbyFull) reads = ReadRefusal(self, msg);
  original(self, msg);
  if (!lobbyFull) return;

  if (!reads.gateOpen) {
    // The game returned at once on this refusal (its status is out of range), so it is neither counted nor shown. The first
    // one in the process is Info; each repeat is a Debug record with the same fields (N78: counted, not dropped).
    const bool first =
        FirstRefusalSaying(static_cast<std::uint8_t>(kLobbyFullCode), RefusalSayings::Record::kDroppedGateClosed);
    WriteDropped(first ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kDebug, reads);
    return;
  }

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
  const char* lobbyText = haveLobby ? KeyText(key, keyText) : "unknown";
  if (!haveLobby) {
    // No join went through this hook, so there is no lobby to count the refusal against; the page is still shown.
    // The first one in the process is Info; each repeat is a Debug record with the same fields.
    const bool first =
        FirstRefusalSaying(static_cast<std::uint8_t>(kLobbyFullCode), RefusalSayings::Record::kUncountedNoJoin);
    sentinel::LogFields(first ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kDebug, "lobby_full",
                        {{"status", "refusal_uncounted"},
                         {"code", static_cast<std::uint64_t>(kLobbyFullCode)},
                         {"reason", "no_join_recorded"},
                         {"msg",
                          "no join went through this hook, so the back-off has no lobby to count this refusal against; "
                          "the page is still shown"}});
  } else if (d.verdict == Verdict::kWait) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "lobby_full",
                        {{"status", "wait"}, {"refusal", d.refusals}, {"wait_ms", d.waitMs}, {"lobby", lobbyText}});
  } else if (d.verdict == Verdict::kGiveUp) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "lobby_full",
                        {{"status", "cooldown"},
                         {"refusals", d.refusals},
                         {"cooldown_ms", d.waitMs},
                         {"lobby", lobbyText}});
  }

  // The game shows a fixed page for code 8 and drops the service's message (#1443). Put the message on the error page,
  // under the code's own name. The write is data in the game object (the error block), not a code page (ADR 0021).
  if (nevr_lobby_failure_message::WriteErrorBlock(reads.game, nevr_lobby_failure_message::Title(kLobbyFullCode),
                                                  reads.message, sizeof reads.message)) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "lobby_full",
                        {{"status", "message_shown"},
                         {"code", static_cast<std::uint64_t>(kLobbyFullCode)},
                         {"lobby", lobbyText},
                         {"message_bytes", static_cast<std::uint64_t>(nevr_lobby_failure_message::MessageBytes(
                                               reads.message, sizeof reads.message))}});
  } else {
    // The game keeps its own page, and the decline says why: the PC callback's decision. Its first write is Info, once per
    // code and reason; a repeat of the same decision is a Debug page_unchanged record.
    const nevr_lobby_failure_message::Decline reason = DeclineOf(reads);
    pthread_mutex_lock(&g_declineMutex);
    const bool first =
        NoteMessageNotShown(Declines(), reads, static_cast<std::uint8_t>(kLobbyFullCode), &WriteDeclineLine);
    pthread_mutex_unlock(&g_declineMutex);
    if (!first && reason != nevr_lobby_failure_message::Decline::kNone) {
      WriteRepeatLine(static_cast<std::uint8_t>(kLobbyFullCode), reason);
    }
  }
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
