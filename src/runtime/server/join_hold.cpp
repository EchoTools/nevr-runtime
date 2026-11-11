#include "runtime/server/join_hold.h"

#include <winsock2.h>
#include <windows.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/addresses.h"
#include "runtime/hook/patching.h"
#include "runtime/lifecycle/service_config.h"
#include "runtime/server/join_hold_rules.h"
#include "runtime/server/messages.h"

namespace nevr_join_hold {
namespace {

// The prologues the two detours are written against (echovr.exe, 8 bytes each; tools/hook_identity_manifest.json).
constexpr std::array<std::uint8_t, 8> kLoadLevelPrologue = {0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x08, 0x48};
constexpr std::array<std::uint8_t, 8> kEnableGameSpacesPrologue = {0x40, 0x55, 0x53, 0x57, 0x48, 0x8D, 0xAC, 0x24};

// CncaGameSpace::LoadLevel(this, p2, p3) returns a 64-bit value (the decompilation reads three parameters); its caller
// is the game-space init fiber, once per load fiber. CR15NetGame::EnableGameSpaces(netGame) runs once per entry to
// NetGame state 9 from the game's update queue. Neither is per-frame; the detours forward every argument and the
// return value unchanged.
using LoadLevelFn = std::uint64_t(__fastcall*)(void* gameSpace, std::uint64_t p2, void* p3);
using EnableGameSpacesFn = std::uint64_t(__fastcall*)(void* netGame);

LoadLevelFn g_originalLoadLevel = nullptr;
EnableGameSpacesFn g_originalEnableGameSpaces = nullptr;

std::mutex g_mutex;
Gate g_gate;
std::atomic<bool> g_active{false};  // hooks in and the key above 0: the gate is in use
std::atomic<std::size_t> g_queued{0};  // the queue size, readable without the lock (ReleaseDue's idle path)
std::string g_sessionId;               // for the detours' lines, under g_mutex

std::uint64_t NowMs() { return GetTickCount64(); }

std::string HexBytes(const void* address, std::size_t count) {
  if (address == nullptr) return "unreadable";
  std::string out;
  char byte[4];
  for (std::size_t i = 0; i < count; ++i) {
    std::snprintf(byte, sizeof(byte), "%02x", static_cast<const std::uint8_t*>(address)[i]);
    out += byte;
  }
  return out;
}

const char* Show(const char* sessionId) { return sessionId != nullptr && sessionId[0] != '\0' ? sessionId : "none"; }

std::uint64_t __fastcall LoadLevelDetour(void* gameSpace, std::uint64_t p2, void* p3) {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_gate.OnLoadBegin(NowMs());
    Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] join hold load begin pending=%zu", g_gate.Pending());
  }
  const std::uint64_t result = g_originalLoadLevel(gameSpace, p2, p3);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_gate.OnLoadEnd(NowMs());
    Log(EchoVR::LogLevel::Debug, "[NEVR.GAMESERVER] join hold load end pending=%zu", g_gate.Pending());
  }
  return result;
}

std::uint64_t __fastcall EnableGameSpacesDetour(void* netGame) {
  const std::uint64_t result = g_originalEnableGameSpaces(netGame);
  std::lock_guard<std::mutex> lock(g_mutex);
  const std::uint64_t now = NowMs();
  if (g_gate.OnSetEnabled(now)) {
    Log(EchoVR::LogLevel::Info, "[NEVR.GAMESERVER] join hold level set enabled session=%s after_ms=%llu",
        Show(g_sessionId.c_str()), static_cast<unsigned long long>(now - g_gate.ArmedMs()));
  }
  return result;
}

void LogDropped(std::size_t dropped, const char* cause, const char* sessionId) {
  if (dropped == 0) return;
  Log(EchoVR::LogLevel::Warning,
      "[NEVR.GAMESERVER] join hold cleared dropped=%zu cause=%s session=%s (held accepts for a session that is gone "
      "were not forwarded)",
      dropped, cause, Show(sessionId));
}

bool HookOne(std::uint64_t rva, const std::array<std::uint8_t, 8>& prologue, void** original, PVOID detour,
             const char* name) {
  const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(EchoVR::g_GameBaseAddress);
  void* address = reinterpret_cast<void*>(base + rva);
  if (!nevr::ValidatePrologue(address, prologue.data(), prologue.size())) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.GAMESERVER] join hold not armed name=%s va=0x%llX reason=prologue_mismatch expected=%s actual=%s "
        "(accepts are forwarded at once)",
        name, static_cast<unsigned long long>(0x140000000ULL + rva), HexBytes(prologue.data(), prologue.size()).c_str(),
        HexBytes(address, prologue.size()).c_str());
    return false;
  }
  *original = address;
  if (!PatchDetour(original, detour, name)) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.GAMESERVER] join hold not armed name=%s va=0x%llX reason=detour_failed expected=%s actual=%s (accepts "
        "are forwarded at once)",
        name, static_cast<unsigned long long>(0x140000000ULL + rva), HexBytes(prologue.data(), prologue.size()).c_str(),
        HexBytes(address, prologue.size()).c_str());
    return false;
  }
  return true;
}

}  // namespace

void Install(bool isServer) {
  const char* text = isServer ? NevrCfgJoinHoldMs() : nullptr;
  const InstallPlan plan = PlanInstall(isServer, text);
  for (const Line& line : plan.lines)
    Log(line.warning ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info, "%s", line.text.c_str());
  if (plan.timeoutMs == 0) return;

  // Both prologues are checked before either detour goes in, so a mismatch leaves the game untouched.
  const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(EchoVR::g_GameBaseAddress);
  const struct {
    std::uint64_t rva;
    const std::array<std::uint8_t, 8>* prologue;
    const char* name;
  } sites[] = {{nevr_patch_addresses::CR15NETGAME_ENABLE_GAME_SPACES, &kEnableGameSpacesPrologue, "EnableGameSpaces"},
               {nevr_patch_addresses::CNCAGAMESPACE_LOAD_LEVEL, &kLoadLevelPrologue, "LoadLevel"}};
  bool allMatch = true;
  for (const auto& s : sites) {
    const void* address = reinterpret_cast<const void*>(base + s.rva);
    if (!nevr::ValidatePrologue(address, s.prologue->data(), s.prologue->size())) {
      allMatch = false;
      Log(EchoVR::LogLevel::Warning,
          "[NEVR.GAMESERVER] join hold not armed name=%s va=0x%llX reason=prologue_mismatch expected=%s actual=%s "
          "(accepts are forwarded at once)",
          s.name, static_cast<unsigned long long>(0x140000000ULL + s.rva),
          HexBytes(s.prologue->data(), s.prologue->size()).c_str(), HexBytes(address, s.prologue->size()).c_str());
    }
  }
  if (!allMatch) return;

  const bool enableOk = HookOne(nevr_patch_addresses::CR15NETGAME_ENABLE_GAME_SPACES, kEnableGameSpacesPrologue,
                                reinterpret_cast<void**>(&g_originalEnableGameSpaces),
                                reinterpret_cast<PVOID>(&EnableGameSpacesDetour), "EnableGameSpaces");
  const bool loadOk = enableOk && HookOne(nevr_patch_addresses::CNCAGAMESPACE_LOAD_LEVEL, kLoadLevelPrologue,
                                          reinterpret_cast<void**>(&g_originalLoadLevel),
                                          reinterpret_cast<PVOID>(&LoadLevelDetour), "LoadLevel");
  if (!loadOk) return;  // an EnableGameSpaces detour alone never arms the gate: g_active stays false

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_gate.Configure(plan.timeoutMs);
  }
  g_active.store(true, std::memory_order_release);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.GAMESERVER] join hold on timeout_ms=%u settle_ms=%llu hooks=2 (accepts wait until the session's levels are "
      "enabled)",
      plan.timeoutMs, static_cast<unsigned long long>(kSettleMs));
}

bool Admit(const void* encoded, std::size_t size, std::size_t entrants, const char* sessionId) {
  if (!g_active.load(std::memory_order_acquire) || encoded == nullptr) return false;
  Held held;
  held.bytes.assign(static_cast<const std::uint8_t*>(encoded), static_cast<const std::uint8_t*>(encoded) + size);
  held.entrants = entrants;
  std::lock_guard<std::mutex> lock(g_mutex);
  held.arrivedMs = NowMs();
  if (!g_gate.Admit(std::move(held))) return false;
  g_queued.store(g_gate.QueueSize(), std::memory_order_release);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.GAMESERVER] join hold held accept count=%zu queued=%zu pending_loads=%zu session=%s (forwarded when the "
      "session's levels are enabled)",
      entrants, g_gate.QueueSize(), g_gate.Pending(), Show(sessionId));
  return true;
}

void ReleaseDue(void* broadcaster, const char* sessionId) {
  if (!g_active.load(std::memory_order_acquire) || g_queued.load(std::memory_order_acquire) == 0) return;
  Release due;
  std::uint64_t now = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    now = NowMs();
    due = g_gate.TakeDue(now);
    g_queued.store(g_gate.QueueSize(), std::memory_order_release);
  }
  for (const Held& held : due.accepts) {
    const std::uint64_t waited = now - held.arrivedMs;
    const bool timeout = due.reason == ReleaseReason::kTimeout;
    Log(timeout ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info,
        "[NEVR.GAMESERVER] join hold released accept count=%zu after_ms=%llu reason=%s session=%s%s", held.entrants,
        static_cast<unsigned long long>(waited), ReleaseReasonName(due.reason), Show(sessionId),
        timeout ? " (the levels were not seen enabled; the accept is forwarded anyway)" : "");
    if (broadcaster != nullptr && EchoVR::BroadcasterReceiveLocalEvent != nullptr) {
      EchoVR::BroadcasterReceiveLocalEvent(static_cast<EchoVR::Broadcaster*>(broadcaster),
                                           Sym::LobbyAcceptPlayersSuccessV2, "SNSLobbyAcceptPlayersSuccessv2",
                                           const_cast<std::uint8_t*>(held.bytes.data()), held.bytes.size());
    }
  }
}

void OnSessionCreate(const char* sessionId) {
  if (!g_active.load(std::memory_order_acquire)) return;
  std::size_t dropped = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_sessionId = sessionId != nullptr ? sessionId : "";
    dropped = g_gate.Arm(NowMs());
    g_queued.store(0, std::memory_order_release);
  }
  LogDropped(dropped, "session_create", sessionId);
}

void OnSessionEnd(const char* cause, const char* sessionId) {
  if (!g_active.load(std::memory_order_acquire)) return;
  std::size_t dropped = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    dropped = g_gate.Clear();
    g_queued.store(0, std::memory_order_release);
  }
  LogDropped(dropped, cause, sessionId);
}

}  // namespace nevr_join_hold
