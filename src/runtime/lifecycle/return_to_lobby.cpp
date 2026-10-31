#include "runtime/lifecycle/return_to_lobby.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include "abi/echovr_functions.h"
#include "core/globals.h"
#include "core/logging.h"
#include "runtime/hook/patching.h"
#include "runtime/lifecycle/cli.h"             // g_isServer
#include "runtime/lifecycle/crash_recovery.h"  // ConsoleShutdownPending
#include "runtime/lifecycle/return_to_lobby_hold.h"
#include "runtime/lifecycle/return_to_lobby_queue.h"

namespace nevr_return_to_lobby {
namespace {

// Prologue of echovr.exe 0x1401a89f0 (NetGameScheduleReturnToLobby): push rbx; sub rsp,0x30; xor edx,edx.
constexpr unsigned char kPrologue[8] = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x33, 0xD2};

std::mutex g_mutex;
nevr_return_to_lobby_hold::Policy g_policy;
EntrantCounter g_counter = nullptr;
PVOID g_heldGame = nullptr;

uint64_t NowMs() { return GetTickCount64(); }

uint64_t LiveEntrants() { return g_counter != nullptr ? g_counter() : 0; }

bool ShutdownPending() { return ConsoleShutdownPending(); }

// After PatchDetour the pointer holds the trampoline to the game's function, so this calls the
// original and never re-enters the hook.
void CallGame(PVOID pGame, const char* caller) { ScheduleNow(pGame, caller); }

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

bool ScheduleNow(PVOID pGame, const char* caller) {
  const nevr_return_to_lobby_queue::Verdict verdict = nevr_return_to_lobby_queue::Check(pGame);
  if (verdict != nevr_return_to_lobby_queue::Verdict::Ready) {
    // The game asks again every tick while a session stays empty: log the first refusal and then
    // each doubling, so a stuck request is visible without filling the log.
    static std::atomic<uint64_t> refused{0};
    const uint64_t count = refused.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((count & (count - 1)) == 0) {
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.PATCH] return to lobby not scheduled caller=%s reason=%s refused=%llu — "
          "the game's call queue is not allocated and the game's function would fault on it (#730)",
          caller, nevr_return_to_lobby_queue::Name(verdict), static_cast<unsigned long long>(count));
    }
    return false;
  }
  EchoVR::NetGameScheduleReturnToLobby(pGame);
  return true;
}

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

uint64_t HoldTtlMs() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_policy.TtlMs();
}

void Request(PVOID pGame) {
  nevr_return_to_lobby_hold::RequestVerdict verdict;
  uint64_t ttlMs = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    verdict = g_policy.OnReturnRequested(NowMs(), LiveEntrants(), ShutdownPending());
    if (verdict == nevr_return_to_lobby_hold::RequestVerdict::Hold) g_heldGame = pGame;
    ttlMs = g_policy.TtlMs();
  }
  if (verdict == nevr_return_to_lobby_hold::RequestVerdict::Proceed) {
    CallGame(pGame, "request");
    return;
  }
  // The game asks again every tick while the session stays empty; only the first request of a
  // hold is logged, the count rides on the line that ends the hold.
  if (verdict == nevr_return_to_lobby_hold::RequestVerdict::Hold) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] return to lobby held: the session has no players (ttl_s=%llu); a player joining or a "
        "shutdown cancels the hold",
        static_cast<unsigned long long>(ttlMs / 1000ULL));
  }
}

void Poll() {
  nevr_return_to_lobby_hold::PollVerdict verdict;
  PVOID game = nullptr;
  uint64_t heldMs = 0;
  uint64_t requests = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t now = NowMs();
    heldMs = g_policy.Holding() ? now - g_policy.HeldSinceMs() : 0;
    verdict = nevr_return_to_lobby_hold::PollIfActive(g_policy, now, LiveEntrants, ShutdownPending);
    game = g_heldGame;
    requests = g_policy.HeldRequests();
    if (verdict != nevr_return_to_lobby_hold::PollVerdict::Keep) g_heldGame = nullptr;
  }
  switch (verdict) {
    case nevr_return_to_lobby_hold::PollVerdict::Keep:
      return;
    case nevr_return_to_lobby_hold::PollVerdict::Cancel:
      Log(EchoVR::LogLevel::Info,
          "[NEVR.PATCH] return to lobby hold cancelled after %llu ms (%llu requests): a player joined or a "
          "shutdown is pending",
          static_cast<unsigned long long>(heldMs), static_cast<unsigned long long>(requests));
      return;
    case nevr_return_to_lobby_hold::PollVerdict::Release:
      Log(EchoVR::LogLevel::Info,
          "[NEVR.PATCH] return to lobby hold expired after %llu ms (%llu requests) — returning to lobby",
          static_cast<unsigned long long>(heldMs), static_cast<unsigned long long>(requests));
      CallGame(game, "hold_release");
      return;
  }
}

}  // namespace nevr_return_to_lobby
