#include "runtime/patch/lobby_full_backoff.h"

#include <windows.h>

#include <array>
#include <cstring>
#include <mutex>

#include "core/logging.h"
#include "nevr_common.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/patching.h"
#include "runtime/log/boot_log_tee.h"
#include "runtime/patch/lobby_full_backoff_rules.h"

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

constexpr std::size_t kLobbyStatusOffset = 0x448;  // the callback's own gate: status - 4 < 3 (unsigned)
constexpr std::size_t kMsgPayloadOffset = 0x10;
constexpr std::size_t kFailureCodeOffset = 0x18;
constexpr std::uint8_t kLobbyFullCode = 8;

std::uintptr_t g_gameBase = 0;
JoinPartyLobbyFn g_originalJoinPartyLobby = nullptr;
FailureCbFn g_originalFailureCb = nullptr;

// The game's network callbacks and its frame loop may run on different threads.
std::mutex g_mutex;
Policy g_policy;

void JoinPartyLobbyHook(void* netGame) {
  const auto now = static_cast<std::uint64_t>(GetTickCount64());
  bool maySend = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    maySend = g_policy.MaySend(now);
  }
  if (!maySend) {
    Log(EchoVR::LogLevel::Debug, "[NEVR.LOBBY] full lobby: join held, the lobby refused the last one");
    return;
  }
  g_originalJoinPartyLobby(netGame);
}

void FailureCbHook(void* lobbyClient, void* msg) {
  std::uint8_t code = 0;
  bool gateOpen = false;
  if (lobbyClient != nullptr && msg != nullptr) {
    auto* client = static_cast<std::uint8_t*>(lobbyClient);
    std::int32_t status = 0;
    std::memcpy(&status, client + kLobbyStatusOffset, sizeof(status));
    gateOpen = static_cast<std::uint32_t>(status - 4) < 3U;
    void* payload = nullptr;
    std::memcpy(&payload, static_cast<std::uint8_t*>(msg) + kMsgPayloadOffset, sizeof(payload));
    if (payload != nullptr) code = static_cast<std::uint8_t*>(payload)[kFailureCodeOffset];
  }
  g_originalFailureCb(lobbyClient, msg);
  if (!gateOpen) return;

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
  HookChecked(kJoinPartyLobbyVA, kJoinPartyLobbyPrologue, g_originalJoinPartyLobby,
              reinterpret_cast<PVOID>(&JoinPartyLobbyHook), "JoinPartyLobby");
  HookChecked(kFailureCbVA, kFailureCbPrologue, g_originalFailureCb, reinterpret_cast<PVOID>(&FailureCbHook),
              "LobbySessionFailureCB");
}

}  // namespace nevr_lobby_full_backoff
