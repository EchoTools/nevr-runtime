#include "runtime/patch/party_invite_gate.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstring>

#include "abi/echovr_functions.h"
#include "core/logging.h"
#include "nevr_common.h"
#include "runtime/hook/patching.h"
#include "runtime/hook/symbol_corpus.h"
#include "runtime/lifecycle/config.h"

namespace PartyInviteGate {
namespace {

constexpr std::uint64_t kBooleanVA = 0x1405EE870;
constexpr std::uint64_t kDispatchEventVA = 0x1401A9FE0;
constexpr std::array<std::uint8_t, 24> kBooleanPrologue = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C,
                                                           0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57,
                                                           0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x20};
constexpr std::array<std::uint8_t, 24> kDispatchPrologue = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                                            0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48,
                                                            0x8B, 0xF1, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0x89};

using BooleanFn = std::uint32_t (*)(void* json, const char* path, std::uint32_t defaultValue, std::uint32_t logMissing);
using DispatchEventFn = void (*)(void* netGame, std::uint64_t eventId);

BooleanFn g_originalBoolean = nullptr;
DispatchEventFn g_originalDispatch = nullptr;
std::atomic<std::uint32_t> g_gateForced{0};
std::atomic<std::uint32_t> g_events{0};

std::uint32_t BooleanHook(void* json, const char* path, std::uint32_t defaultValue, std::uint32_t logMissing) {
  const std::uint32_t original = g_originalBoolean(json, path, defaultValue, logMissing);
  const std::uint32_t result = BooleanResult(path, original);
  if (result != original && g_gateForced.fetch_add(1, std::memory_order_relaxed) < 4) {
    Log(EchoVR::LogLevel::Info, "[NEVR.PARTY] profile flag %s read as true (profile said %u)", kFirstMatchPath,
        original);
  }
  return result;
}

// Every event the game sends its session. The party-invite errors are the ones that explain a
// click that never reaches the facade, so they log at Warning; everything else is capped.
void DispatchEventHook(void* netGame, std::uint64_t eventId) {
  const std::uint32_t count = g_events.fetch_add(1, std::memory_order_relaxed) + 1;
  const char* name = EchoVR::LookupSymbolName(eventId);
  const bool inviteError = eventId == kErrorFirstMatchNotCompleted || eventId == kErrorOffline ||
                           (name != nullptr && std::strstr(name, "partyinvite") != nullptr);
  if (inviteError || count <= 200) {
    char label[160];
    EchoVR::FormatSymbolId(label, sizeof(label), eventId);
    Log(inviteError ? EchoVR::LogLevel::Warning : EchoVR::LogLevel::Info,
        "[NEVR.PARTY] session event #%u id=0x%016llx %s", count, static_cast<unsigned long long>(eventId), label);
  }
  g_originalDispatch(netGame, eventId);
}

template <typename Fn, std::size_t Size>
void InstallChecked(std::uintptr_t gameBase, std::uint64_t va, const std::array<std::uint8_t, Size>& prologue,
                    Fn& original, PVOID detour, const char* name) {
  void* target = nevr::ResolveVA_Checked(gameBase, va);
  if (!nevr::ValidatePrologue(target, prologue.data(), prologue.size())) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PARTY] %s hook skipped va=0x%llx reason=prologue_mismatch", name,
        static_cast<unsigned long long>(va));
    return;
  }
  original = reinterpret_cast<Fn>(target);
  PatchDetour(&original, detour, name);
}

}  // namespace

void Install(std::uintptr_t gameBase) {
  InstallChecked(gameBase, kBooleanVA, kBooleanPrologue, g_originalBoolean, reinterpret_cast<PVOID>(&BooleanHook),
                 "CJson_Boolean");
  InstallChecked(gameBase, kDispatchEventVA, kDispatchPrologue, g_originalDispatch,
                 reinterpret_cast<PVOID>(&DispatchEventHook), "DispatchEventToSession");
}

}  // namespace PartyInviteGate
