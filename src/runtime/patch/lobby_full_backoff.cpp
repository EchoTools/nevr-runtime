#include "runtime/patch/lobby_full_backoff.h"

#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/compat/lobby_frame.h"
#include "runtime/compat/lobby_join_state.h"
#include "runtime/compat/social_party.h"
#include "runtime/compat/ws_bridge.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/patching.h"
#include "runtime/log/boot_log_tee.h"
#include "runtime/patch/follow_record_rules.h"
#include "runtime/patch/lobby_failure_message_rules.h"
#include "runtime/patch/lobby_full_backoff_rules.h"
#include "runtime/patch/party_follow_dedup_rules.h"

namespace nevr_lobby_full_backoff {
namespace {

// CR15NetGame::JoinPartyLobby(this): PUSH RBX; SUB RSP,0x50; MOV RBX,RCX. The game queues it every frame while
// FollowParty (0x14016bc20) finds a lobby to follow.
constexpr std::uint64_t kJoinPartyLobbyVA = 0x140178D10;
constexpr std::array<std::uint8_t, 9> kJoinPartyLobbyPrologue = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x8B, 0xD9};
using JoinPartyLobbyFn = void (*)(void* netGame);

// CR15NetClientLobby::LobbySessionFailureCB(this, msg): MOV R11,RSP; MOV [R11+0x20],RBX; PUSH RSI; SUB RSP,0x480.
// msg+0x10 is the payload; payload+0x18 is the failure code, 8 for "lobby full".
constexpr std::uint64_t kFailureCbVA = 0x14017D380;
constexpr std::array<std::uint8_t, 15> kFailureCbPrologue = {0x4C, 0x8B, 0xDC, 0x49, 0x89, 0x5B, 0x20, 0x56,
                                                             0x48, 0x81, 0xEC, 0x80, 0x04, 0x00, 0x00};
using FailureCbFn = void (*)(void* lobbyClient, void* msg);

// CR15NetGame::GetPartyLobbyId(party, out) 0x14017c6a0: MOV [RSP+0x18],RBX. Returns the party's lobby SUuid in RAX
// (= out). JoinPartyLobby passes the party object at [this+0x647c8]. It only reads; no hook is installed on it (#1413).
constexpr std::uint64_t kGetPartyLobbyIdVA = 0x14017C6A0;
constexpr std::array<std::uint8_t, 5> kGetPartyLobbyIdPrologue = {0x48, 0x89, 0x5C, 0x24, 0x18};
using GetPartyLobbyIdFn = void* (*)(void* party, void* out);
constexpr std::size_t kNetGamePartyOffset = 0x647C8;

// CR15NetGame::CanFollowParty(this) 0x1401a74c0: PUSH RBX; SUB RSP,0x20; MOV RBX,RCX. It returns 1 when following the
// party is allowed now. FollowParty (0x14016bc20) queues JoinPartyLobby only when it returns nonzero; JoinPartyLobby
// calls it too. The game state is the int at this+0; the lobby status is at [[this+0x40]+0x448], read in states 5 and 9.
constexpr std::uint64_t kCanFollowPartyVA = 0x1401A74C0;
constexpr std::array<std::uint8_t, 9> kCanFollowPartyPrologue = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xD9};
using CanFollowPartyFn = int (*)(void* netGame);
constexpr std::size_t kNetGameLobbyClientOffset = 0x40;

constexpr std::size_t kLobbyStatusOffset = 0x448;  // the callback's own gate: status - 4 < 3 (unsigned)
constexpr std::size_t kLobbyGameOffset = 0x8;      // *(lobbyClient + 8) is the CR15NetGame the error block lives in
constexpr std::size_t kMsgPayloadOffset = 0x10;
constexpr std::size_t kFailureCodeOffset = 0x18;
constexpr std::uint8_t kLobbyFullCode = 8;

std::uintptr_t g_gameBase = 0;
JoinPartyLobbyFn g_originalJoinPartyLobby = nullptr;
FailureCbFn g_originalFailureCb = nullptr;
GetPartyLobbyIdFn g_getPartyLobbyId = nullptr;  // null when its prologue does not match: the dedup is then off
CanFollowPartyFn g_originalCanFollowParty = nullptr;

// The game's network callbacks and its frame loop may run on different threads.
std::mutex g_mutex;
Policy g_policy;

// The skip log's run state. JoinPartyLobby runs on the game's thread; the mutex keeps the state whole if another thread
// ever calls it.
std::mutex g_skipLogMutex;
nevr_party_follow_dedup::SkipLogState g_skipLog;

// The proceed record's run state; the hooks take it under g_recordMutex. A skip or a hold ends the run (EndProceedRun).
std::mutex g_recordMutex;
nevr_follow_record::ProceedLogState g_proceedLog;
nevr_follow_record::DecisionState g_decision;

void EndProceedRun() {
  std::lock_guard<std::mutex> lock(g_recordMutex);
  nevr_follow_record::NoteNoProceed(g_proceedLog);
}

// The leader's lobby, read from the party object the game holds: false when the dedup cannot read it (its prologue did
// not match, or there is no party), and then the follow is neither skipped nor named by lobby.
bool ReadLeaderLobby(void* netGame, nevr_party_follow_dedup::Guid& leader) {
  if (g_getPartyLobbyId == nullptr || netGame == nullptr) return false;
  std::uint8_t* game = static_cast<std::uint8_t*>(netGame);
  void* party = nullptr;
  std::memcpy(&party, game + kNetGamePartyOffset, sizeof(party));
  if (party == nullptr) return false;
  g_getPartyLobbyId(party, leader.data());
  return true;
}

// True when the follow was skipped: the leader's lobby is the one this client is in or is joining, and that answer is
// younger than the window (#1413). FollowParty queues JoinPartyLobby on every CR15NetGame::Update, so a skip repeats each
// tick for up to the window. The Info line is written once per run of identical skips; the repeats are counted and carried
// on the next Info line as repeats=<n> (party_follow_dedup_rules.h, NoteSkip).
bool TrySkipDuplicateFollow(const nevr_party_follow_dedup::Guid& leader) {
  nevr_lobby_join_state::Stamp current{};
  nevr_lobby_join_state::Stamp joining{};
  ::LobbyJoinSnapshot(&current, &joining);
  const std::uint64_t nowMs = nevr_lobby_join_state::SteadyNowMs();
  const nevr_party_follow_dedup::Verdict verdict = nevr_party_follow_dedup::Decide(leader, current, joining, nowMs);
  if (verdict.skip == nevr_party_follow_dedup::Skip::None) {
    std::lock_guard<std::mutex> lock(g_skipLogMutex);
    nevr_party_follow_dedup::NoteNoSkip(g_skipLog);
    return false;
  }
  nevr_party_follow_dedup::SkipLogDecision logDecision{};
  {
    std::lock_guard<std::mutex> lock(g_skipLogMutex);
    logDecision = nevr_party_follow_dedup::NoteSkip(g_skipLog, leader, verdict.skip);
  }
  if (logDecision.writeInfo) {
    const std::uint64_t partyId = nevr_social_party::Global().Snapshot().partyId;
    Log(EchoVR::LogLevel::Info,
        "[NEVR.WS] party follow skipped lobby=%s reason=%s age_ms=%llu repeats=%llu party_id=%llu",
        nevr_lobby_frame::FormatGuid(leader).c_str(),
        verdict.skip == nevr_party_follow_dedup::Skip::AlreadyIn ? "already_in" : "already_joining",
        static_cast<unsigned long long>(verdict.ageMs), static_cast<unsigned long long>(logDecision.repeats),
        static_cast<unsigned long long>(partyId));
  }
  return true;
}

void JoinPartyLobbyHook(void* netGame) {
  nevr_party_follow_dedup::Guid leader{};
  const bool haveLeader = ReadLeaderLobby(netGame, leader);
  if (haveLeader && TrySkipDuplicateFollow(leader)) {
    EndProceedRun();
    return;
  }
  const auto now = static_cast<std::uint64_t>(GetTickCount64());
  bool maySend = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    maySend = g_policy.MaySend(now);
  }
  if (!maySend) {
    EndProceedRun();
    Log(EchoVR::LogLevel::Debug, "[NEVR.LOBBY] full lobby: join held, the lobby refused the last one");
    return;
  }
  // The follow goes on to the game's JoinPartyLobby, which may still refuse it through CanFollowParty (its own check).
  nevr_follow_record::ProceedLogDecision logDecision{};
  {
    std::lock_guard<std::mutex> lock(g_recordMutex);
    logDecision = nevr_follow_record::NoteProceed(g_proceedLog, haveLeader ? leader : nevr_party_follow_dedup::Guid{});
  }
  if (logDecision.writeInfo) {
    const nevr_social_party::View view = nevr_social_party::Global().Snapshot();
    Log(EchoVR::LogLevel::Info, "[NEVR.WS] party follow proceeds lobby=%s party_id=%llu party_size=%zu repeats=%llu",
        haveLeader ? nevr_follow_record::LobbyPrefix(leader).c_str() : "unknown",
        static_cast<unsigned long long>(view.partyId), view.members.size(),
        static_cast<unsigned long long>(logDecision.repeats));
  }
  g_originalJoinPartyLobby(netGame);
}

// The game's follow decision (CanFollowParty). Its answer is logged when it, the game state or the lobby status
// changes, so a run of identical answers is one record. The game state and lobby status are read before the original
// runs, the same values it reads.
int CanFollowPartyHook(void* netGame) {
  int gameState = -1;
  int lobbyStatus = -1;
  if (netGame != nullptr) {
    std::uint8_t* game = static_cast<std::uint8_t*>(netGame);
    std::memcpy(&gameState, game, sizeof(gameState));
    if (gameState == 5 || gameState == 9) {
      void* lobbyClient = nullptr;
      std::memcpy(&lobbyClient, game + kNetGameLobbyClientOffset, sizeof(lobbyClient));
      if (lobbyClient != nullptr)
        std::memcpy(&lobbyStatus, static_cast<std::uint8_t*>(lobbyClient) + kLobbyStatusOffset, sizeof(lobbyStatus));
    }
  }
  const int answer = g_originalCanFollowParty(netGame);
  bool changed = false;
  {
    std::lock_guard<std::mutex> lock(g_recordMutex);
    changed = nevr_follow_record::NoteDecision(g_decision, answer != 0, gameState, lobbyStatus);
  }
  if (changed) {
    const nevr_social_party::View view = nevr_social_party::Global().Snapshot();
    char lobbyStatusText[16];
    if (lobbyStatus < 0)
      std::snprintf(lobbyStatusText, sizeof(lobbyStatusText), "unread");
    else
      std::snprintf(lobbyStatusText, sizeof(lobbyStatusText), "%d", lobbyStatus);
    Log(EchoVR::LogLevel::Info,
        "[NEVR.WS] party follow decision will_follow=%s game_state=%d lobby_status=%s party_id=%llu party_size=%zu",
        answer != 0 ? "yes" : "no", gameState, lobbyStatusText, static_cast<unsigned long long>(view.partyId),
        view.members.size());
  }
  return answer;
}

void FailureCbHook(void* lobbyClient, void* msg) {
  std::uint8_t code = 0;
  bool gateOpen = false;
  std::uint8_t* gameBase = nullptr;
  // The service's message, copied before the original runs: the payload belongs to the game and may be released when
  // the callback returns (#1443).
  char message[nevr_lobby_failure_message::kMessageSize] = {};
  if (lobbyClient != nullptr && msg != nullptr) {
    auto* client = static_cast<std::uint8_t*>(lobbyClient);
    std::int32_t status = 0;
    std::memcpy(&status, client + kLobbyStatusOffset, sizeof(status));
    gateOpen = static_cast<std::uint32_t>(status - 4) < 3U;
    std::memcpy(&gameBase, client + kLobbyGameOffset, sizeof(gameBase));
    void* payload = nullptr;
    std::memcpy(&payload, static_cast<std::uint8_t*>(msg) + kMsgPayloadOffset, sizeof(payload));
    if (payload != nullptr) {
      code = static_cast<std::uint8_t*>(payload)[kFailureCodeOffset];
      std::memcpy(message, static_cast<std::uint8_t*>(payload) + nevr_lobby_failure_message::kMessageOffset,
                  sizeof(message));
    }
  }
  g_originalFailureCb(lobbyClient, msg);
  if (!gateOpen) return;

  // The game shows a fixed literal for this code and drops the service's message (#1443). Put the message on the error
  // page under the code's own name. Codes 11 and 12 already show it, so they are not touched.
  if (nevr_lobby_failure_message::TakesServiceMessage(code) &&
      nevr_lobby_failure_message::WriteErrorBlock(gameBase, nevr_lobby_failure_message::Title(code), message,
                                                  sizeof(message))) {
    Log(EchoVR::LogLevel::Info, "[NEVR.LOBBY] failure message shown code=%u title=%s message_bytes=%zu",
        static_cast<unsigned int>(code), nevr_lobby_failure_message::Title(code),
        nevr_lobby_failure_message::MessageBytes(message, sizeof(message)));
  }

  if (code != kLobbyFullCode) {
    // Another outcome ends a lobby-full run: the next follow starts from the first wait.
    std::lock_guard<std::mutex> lock(g_mutex);
    g_policy.OnSuccess();
    return;
  }
  const auto now = static_cast<std::uint64_t>(GetTickCount64());
  Refusal refusal{};
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    refusal = g_policy.OnLobbyFull(now);
  }
  if (refusal.gaveUp) {
    Log(EchoVR::LogLevel::Info, "[NEVR.LOBBY] full lobby: refused %d times, giving up for %llu ms",
        refusal.count, static_cast<unsigned long long>(refusal.delayMs));
  } else {
    Log(EchoVR::LogLevel::Info, "[NEVR.LOBBY] full lobby: refused %d of %d, next join in %llu ms", refusal.count,
        kGiveUpAfter, static_cast<unsigned long long>(refusal.delayMs));
  }
}

template <typename Fn, std::size_t Size>
void HookChecked(std::uint64_t va, const std::array<std::uint8_t, Size>& prologue, Fn& original, PVOID detour,
                 const char* name) {
  void* target = nevr::ResolveVA_Checked(g_gameBase, va);
  if (!nevr::ValidatePrologue(target, prologue.data(), prologue.size())) {
    nevr_boot_log_tee::TeeFprintf("[NEVR.LOBBY] %s hook skipped va=0x%llx reason=prologue_mismatch\n", name,
                                  static_cast<unsigned long long>(va));
    return;
  }
  original = reinterpret_cast<Fn>(target);
  if (PatchDetour(&original, detour, name))
    nevr_boot_log_tee::TeeFprintf("[NEVR.LOBBY] %s hook installed va=0x%llx\n", name, static_cast<unsigned long long>(va));
}

}  // namespace

void Install(std::uintptr_t gameBase) {
  g_gameBase = gameBase;
  void* getPartyLobbyId = nevr::ResolveVA_Checked(g_gameBase, kGetPartyLobbyIdVA);
  if (nevr::ValidatePrologue(getPartyLobbyId, kGetPartyLobbyIdPrologue.data(), kGetPartyLobbyIdPrologue.size())) {
    g_getPartyLobbyId = reinterpret_cast<GetPartyLobbyIdFn>(getPartyLobbyId);
  } else {
    nevr_boot_log_tee::TeeFprintf("[NEVR.WS] party follow dedup off reason=prologue_mismatch va=0x%llx\n",
                                  static_cast<unsigned long long>(kGetPartyLobbyIdVA));
  }
  HookChecked(kJoinPartyLobbyVA, kJoinPartyLobbyPrologue, g_originalJoinPartyLobby,
              reinterpret_cast<PVOID>(&JoinPartyLobbyHook), "JoinPartyLobby");
  HookChecked(kCanFollowPartyVA, kCanFollowPartyPrologue, g_originalCanFollowParty,
              reinterpret_cast<PVOID>(&CanFollowPartyHook), "CanFollowParty");
  HookChecked(kFailureCbVA, kFailureCbPrologue, g_originalFailureCb, reinterpret_cast<PVOID>(&FailureCbHook),
              "LobbySessionFailureCB");
}

}  // namespace nevr_lobby_full_backoff
