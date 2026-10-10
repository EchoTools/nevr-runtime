#include "runtime/lifecycle/return_to_lobby.h"

#include <cstring>
#include <mutex>

#include "abi/echovr_functions.h"
#include "core/globals.h"
#include "core/logging.h"
#include "runtime/hook/patching.h"
#include "runtime/lifecycle/cli.h"             // g_isServer
#include "runtime/lifecycle/crash_recovery.h"  // ConsoleShutdownPending
#include "runtime/lifecycle/return_to_lobby_hold.h"

namespace ReturnToLobby {
namespace {

// Prologue of echovr.exe 0x1401a89f0 (NetGameScheduleReturnToLobby): push rbx; sub rsp,0x30; xor edx,edx.
constexpr unsigned char kPrologue[8] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x33, 0xD2};

std::mutex g_mutex;
ReturnToLobbyHold::Policy g_policy;
EntrantCounter g_counter = nullptr;
PVOID g_heldGame = nullptr;

uint64_t NowMs() { return GetTickCount64(); }

uint64_t LiveEntrants() { return g_counter != nullptr ? g_counter() : 0; }

bool ShutdownPending() { return g_shutdownRequested != 0 || ConsoleShutdownPending(); }

// After PatchDetour the pointer holds the trampoline to the game's function, so this calls the
// original and never re-enters the hook.
void CallGame(PVOID pGame) {
  if (pGame != nullptr) EchoVR::NetGameScheduleReturnToLobby(pGame);
}

VOID HookedScheduleReturnToLobby(PVOID pGame) { Request(pGame); }

bool ArmDetour(uint64_t ttlSeconds) {
  void* target = EchoVR::g_GameBaseAddress + 0x1A89F0;
  if (std::memcmp(target, kPrologue, sizeof(kPrologue)) != 0) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] hook skipped name=NetGameScheduleReturnToLobby va=0x1401a89f0 reason=prologue_mismatch — "
        "the empty-server TTL (%llu s) will not hold the game's own return to lobby",
        static_cast<unsigned long long>(ttlSeconds));
    return false;
  }
  if (!PatchDetour(&EchoVR::NetGameScheduleReturnToLobby, reinterpret_cast<PVOID>(&HookedScheduleReturnToLobby),
                   "EchoVR::NetGameScheduleReturnToLobby")) {
    return false;
  }
  Log(EchoVR::LogLevel::Info,
      "[NEVR.PATCH] empty-server TTL armed ttl_s=%llu — a return to lobby with no players is held up to the TTL",
      static_cast<unsigned long long>(ttlSeconds));
  return true;
}

}  // namespace

void SetEntrantCounter(EntrantCounter counter) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_counter = counter;
}

bool Configure(uint64_t ttlSeconds) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_policy.SetTtlMs(ttlSeconds * 1000ULL);
  }
  if (ttlSeconds == 0 || !g_isServer) return true;

  // The hold is armed only together with the detour: without it the runtime's own CODE_ENDED
  // return would be held while the game's own returns were not.
  const bool armed = ArmDetour(ttlSeconds);
  if (!armed) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_policy.SetTtlMs(0);
  }
  return armed;
}

void Request(PVOID pGame) {
  ReturnToLobbyHold::RequestVerdict verdict;
  uint64_t ttlMs = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    verdict = g_policy.OnReturnRequested(NowMs(), LiveEntrants(), ShutdownPending());
    if (verdict == ReturnToLobbyHold::RequestVerdict::Hold) g_heldGame = pGame;
    ttlMs = g_policy.TtlMs();
  }
  if (verdict == ReturnToLobbyHold::RequestVerdict::Proceed) {
    CallGame(pGame);
    return;
  }
  // The game asks again every tick while the session stays empty; only the first request of a
  // hold is logged, the count rides on the line that ends the hold.
  if (verdict == ReturnToLobbyHold::RequestVerdict::Hold) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] return to lobby held: the session has no players (ttl_s=%llu); a player joining or a "
        "shutdown cancels the hold",
        static_cast<unsigned long long>(ttlMs / 1000ULL));
  }
}

void Poll() {
  ReturnToLobbyHold::PollVerdict verdict;
  PVOID game = nullptr;
  uint64_t heldMs = 0;
  uint64_t requests = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t now = NowMs();
    heldMs = g_policy.Holding() ? now - g_policy.HeldSinceMs() : 0;
    verdict = ReturnToLobbyHold::PollIfActive(g_policy, now, LiveEntrants, ShutdownPending);
    game = g_heldGame;
    requests = g_policy.HeldRequests();
    if (verdict != ReturnToLobbyHold::PollVerdict::Keep) g_heldGame = nullptr;
  }
  switch (verdict) {
    case ReturnToLobbyHold::PollVerdict::Keep:
      return;
    case ReturnToLobbyHold::PollVerdict::Cancel:
      Log(EchoVR::LogLevel::Info,
          "[NEVR.PATCH] return to lobby hold cancelled after %llu ms (%llu requests): a player joined or a "
          "shutdown is pending",
          static_cast<unsigned long long>(heldMs), static_cast<unsigned long long>(requests));
      return;
    case ReturnToLobbyHold::PollVerdict::Release:
      Log(EchoVR::LogLevel::Info,
          "[NEVR.PATCH] return to lobby hold expired after %llu ms (%llu requests) — returning to lobby",
          static_cast<unsigned long long>(heldMs), static_cast<unsigned long long>(requests));
      CallGame(game);
      return;
  }
}

}  // namespace ReturnToLobby
