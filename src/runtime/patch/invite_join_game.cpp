#include "runtime/patch/invite_join_game.h"

#include <windows.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include "abi/echovr.h"
#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "nevr/pc/nevr_common.h"  // N97: the one ValidatePrologue
#include "runtime/hook/addresses.h"
#include "runtime/lifecycle/config.h"                 // g_pGame
#include "runtime/lifecycle/cli.h"                    // g_isServer
#include "runtime/lifecycle/crash_recovery.h"         // GameLoopThreadId
#include "runtime/lifecycle/readable_memory.h"        // IsReadableMemory
#include "runtime/lifecycle/return_to_lobby_queue.h"  // NetGameFromContext, ReadPointer
#include "runtime/lifecycle/server_bringup.h"         // the prologues of QueueGameThreadCall and BeginMultiplayerLobby
#include "runtime/patch/invite_join.h"
#include "runtime/patch/invite_service.h"
#include "runtime/patch/invite_wizard.h"  // ParseResolved

// #1401 (D3 of #906), docs/adr/0017 ("Join"): the game-side seam of the join (patch/invite_join.h). Everything here runs on the
// game's thread (OfferResolved is called from the per-frame applier, which the dispatcher gates to that thread).

namespace nevr_invite_join_game {

namespace {

void Say(nevr_invite::LogLevel level, const std::string& line) {
  EchoVR::LogLevel mapped = EchoVR::LogLevel::Info;
  if (level == nevr_invite::LogLevel::Warning) mapped = EchoVR::LogLevel::Warning;
  if (level == nevr_invite::LogLevel::Error) mapped = EchoVR::LogLevel::Error;
  Log(mapped, "%s", line.c_str());
}

bool IsWritableProtection(DWORD protection) {
  if ((protection & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
  switch (protection & 0xffU) {
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
      return true;
    default:
      return false;
  }
}

// Every byte of [address, address + length) is in committed, writable memory. VirtualQuery only; the address is never dereferenced.
bool IsWritableMemory(const void* address, std::size_t length) {
  if (address == nullptr || length == 0) return false;
  const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(address);
  const std::uintptr_t end = start + length;
  if (end < start) return false;
  std::uintptr_t cursor = start;
  while (cursor < end) {
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(reinterpret_cast<const void*>(cursor), &info, sizeof(info)) == 0) return false;
    if (info.State != MEM_COMMIT || !IsWritableProtection(info.Protect)) return false;
    const std::uintptr_t regionEnd = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
    if (regionEnd <= cursor) return false;
    cursor = regionEnd;
  }
  return true;
}

std::string Hex(const void* at, std::size_t size) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  const auto* bytes = static_cast<const unsigned char*>(at);
  for (std::size_t i = 0; i < size; ++i) {
    out.push_back(kDigits[bytes[i] >> 4]);
    out.push_back(kDigits[bytes[i] & 0x0F]);
  }
  return out;
}

std::string Address(const void* at) {
  char text[32];
  std::snprintf(text, sizeof(text), "0x%llX", static_cast<unsigned long long>(reinterpret_cast<std::uintptr_t>(at)));
  return text;
}

// The net game of the game context, as the game's own GetNetGameFromContext reads it; null while there is no context.
void* NetGame() {
  if (g_pGame == nullptr ||
      !nevr_crash_recovery::IsReadableMemory(g_pGame, nevr_patch_addresses::CONTEXT_NETGAME_OFFSET + sizeof(void*))) {
    return nullptr;
  }
  return nevr_return_to_lobby_queue::NetGameFromContext(g_pGame);
}

// The game object (CR15Game) of the net game: `*(netGame + 8)`, the field NetGameScheduleReturnToLobby reads. Null when unreadable.
void* GameObject() {
  void* netGame = NetGame();
  if (netGame == nullptr ||
      !nevr_crash_recovery::IsReadableMemory(netGame, nevr_patch_addresses::NETGAME_GAME_OFFSET + sizeof(void*))) {
    return nullptr;
  }
  return const_cast<void*>(nevr_return_to_lobby_queue::ReadPointer(netGame, nevr_patch_addresses::NETGAME_GAME_OFFSET));
}

std::optional<EchoVR::NetGameState> NetState() {
  void* netGame = NetGame();
  if (netGame == nullptr ||
      !nevr_crash_recovery::IsReadableMemory(netGame, nevr_patch_addresses::NETGAME_GAME_OFFSET + sizeof(void*))) {
    return std::nullopt;
  }
  INT32 state = 0;
  std::memcpy(&state, static_cast<const unsigned char*>(netGame) + nevr_patch_addresses::NETGAME_STATE_OFFSET, sizeof(state));
  return static_cast<EchoVR::NetGameState>(state);
}

// The game object's bytes, through the staging's checks: a read of readable memory, a write of writable memory, nothing else.
class GameObjectMemory final : public nevr_invite_join::StagingMemory {
 public:
  explicit GameObjectMemory(void* game) : game_(static_cast<unsigned char*>(game)) {}

  bool Read(std::uintptr_t offset, void* out, std::size_t size) override {
    const unsigned char* at = game_ + offset;
    if (!nevr_crash_recovery::IsReadableMemory(at, size)) return false;
    std::memcpy(out, at, size);
    return true;
  }

  bool Write(std::uintptr_t offset, const void* in, std::size_t size) override {
    unsigned char* at = game_ + offset;
    if (!IsWritableMemory(at, size)) return false;
    std::memcpy(at, in, size);
    return true;
  }

 private:
  unsigned char* game_;
};

// Rule 6: a prologue that differs is logged with its address and both byte strings, so the mismatch can be read from the line.
void LogPrologueMismatch(const char* name, const void* at, const std::uint8_t* expected, std::size_t size) {
  const std::string actual =
      nevr_crash_recovery::IsReadableMemory(at, size) ? Hex(at, size) : std::string("unreadable");
  Say(nevr_invite::LogLevel::Warning, std::string("[NEVR.INVITE] join prologue check failed function=") + name +
                                          " va=" + Address(at) + " expected=" + Hex(expected, size) + " actual=" + actual);
}

bool OnGameThread() {
  const unsigned long loop = GameLoopThreadId();
  return loop != 0 && loop == GetCurrentThreadId();
}

bool ProloguesValid() {
  const void* queue = reinterpret_cast<const void*>(EchoVR::QueueGameThreadCall);
  const void* begin = reinterpret_cast<const void*>(EchoVR::BeginMultiplayerLobby);
  if (!nevr::ValidatePrologue(queue, nevr_server_bringup::kQueueGameThreadCallPrologue,
                              sizeof(nevr_server_bringup::kQueueGameThreadCallPrologue))) {
    LogPrologueMismatch("QueueGameThreadCall", queue, nevr_server_bringup::kQueueGameThreadCallPrologue,
                        sizeof(nevr_server_bringup::kQueueGameThreadCallPrologue));
    return false;
  }
  if (!nevr::ValidatePrologue(begin, nevr_server_bringup::kBeginMultiplayerLobbyPrologue,
                              sizeof(nevr_server_bringup::kBeginMultiplayerLobbyPrologue))) {
    LogPrologueMismatch("BeginMultiplayerLobby", begin, nevr_server_bringup::kBeginMultiplayerLobbyPrologue,
                        sizeof(nevr_server_bringup::kBeginMultiplayerLobbyPrologue));
    return false;
  }
  return true;
}

// The game object and the prior bytes of the last verified stage, for the undo when the begin cannot run.
void* g_stagedGame = nullptr;
nevr_invite_join::PriorStage g_stagedPrior;

bool IsServer() { return g_isServer != FALSE; }

nevr_invite_join::StageOutcome StageLobby(const nevr_invite_join::LobbyBytes& lobby, std::uint16_t team) {
  void* game = GameObject();
  if (game == nullptr) {
    nevr_invite_join::StageOutcome outcome;
    outcome.step = nevr_invite_join::StageStep::Read;
    outcome.field = nevr_invite_join::StageField::Lobby;
    return outcome;
  }
  GameObjectMemory memory(game);
  nevr_invite_join::StageOutcome outcome = nevr_invite_join::StageAndVerify(memory, lobby, team, Say);
  if (outcome.ok()) {
    g_stagedGame = game;
    g_stagedPrior = outcome.prior;
  }
  return outcome;
}

// False when there is no game object to begin on; nothing is called then.
bool Begin() {
  void* game = GameObject();
  if (game == nullptr) {
    Say(nevr_invite::LogLevel::Warning, "[NEVR.INVITE] join begin not called, no game object after the stage");
    return false;
  }
  EchoVR::BeginMultiplayerLobby(game);
  return true;
}

// Puts the last stage back through the game object it was staged in, when that memory is still writable.
bool Unstage() {
  if (g_stagedGame == nullptr) return false;
  GameObjectMemory memory(g_stagedGame);
  const bool restored = nevr_invite_join::Unstage(memory, g_stagedPrior, Say);
  g_stagedGame = nullptr;
  return restored;
}

std::int64_t NowMs() {
  return static_cast<std::int64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// One join window for the run: it is the join's own state, so it lives with the one seam.
nevr_invite_join::Joiner& TheJoiner() {
  static nevr_invite_join::Joiner joiner(nevr_invite_join::Seam{IsServer, OnGameThread, ProloguesValid, NetState,
                                                                StageLobby, Begin, Unstage, NowMs, Say});
  return joiner;
}

}  // namespace

void OfferResolved(const std::vector<nevr_invite::Answer>& answers) {
  for (const nevr_invite::Answer& answer : answers) {
    if (answer.op != nevr_invite::Op::Resolve || answer.result != nevr_invite::Result::Ok) continue;
    // A resolve with no usable match id is offered as an empty id: the join logs it as a stage refusal with lobby=invalid.
    const std::optional<nevr_invite::Resolved> resolved = nevr_invite::ParseResolved(answer.body);
    TheJoiner().Offer(resolved.has_value() ? std::string_view(resolved->match_id) : std::string_view(),
                      nevr_invite_service::Active());
  }
}

void OfferMatch(std::string_view match_id) { TheJoiner().Offer(match_id, nevr_invite_service::Active()); }

}  // namespace nevr_invite_join_game
