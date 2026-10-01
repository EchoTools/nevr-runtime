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

// The friend row's invite handler and the social call behind it (ReVault echovr.exe, disassembly of
// 0x14018aa90). After the two event-raising checks the handler has three SILENT exits: no social
// object at netGame+0x647c8; the target's provider nibble differs from the local provider symbol
// (CSymbol64 of CNSProvider::UserProviderID(netGame+0x10)); bit 0x10 differs. 0x140614320 then skips
// silently if the target is already a party member. A click that does nothing left no trace; these
// hooks log the inputs to every exit, once per click.
constexpr std::uint64_t kInviteHandlerVA = 0x14018AA90;
constexpr std::uint64_t kSocialInviteVA = 0x140614320;
constexpr std::uint64_t kUserProviderIdVA = 0x1406186D0;
constexpr std::uint64_t kSymbol64VA = 0x1400F6B40;
constexpr std::array<std::uint8_t, 16> kInviteHandlerPrologue = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83,
                                                                 0xEC, 0x20, 0x48, 0x8B, 0xD9, 0x48, 0x8B, 0xFA};
constexpr std::array<std::uint8_t, 16> kSocialInvitePrologue = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83,
                                                                0xEC, 0x20, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF9};
constexpr std::array<std::uint8_t, 16> kUserProviderIdPrologue = {0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B,
                                                                  0xC2, 0x48, 0x8B, 0xD9, 0x48, 0x85, 0xD2, 0x75};
constexpr std::array<std::uint8_t, 7> kSymbol64Prologue = {0x48, 0x8D, 0x05, 0x78, 0xA3, 0x5D, 0x01};

using InviteHandlerFn = void (*)(void* netGame, std::uint64_t* xpid);
using SocialInviteFn = void (*)(void* social, std::uint64_t accountId);
using UserProviderIdFn = std::uint64_t* (*)(std::uint64_t* out, std::uint64_t user);
using Symbol64Fn = std::uint64_t* (*)(std::uint64_t* out, std::uint64_t value);

using BooleanFn = std::uint32_t (*)(void* json, const char* path, std::uint32_t defaultValue, std::uint32_t logMissing);
using DispatchEventFn = void (*)(void* netGame, std::uint64_t eventId);

BooleanFn g_originalBoolean = nullptr;
InviteHandlerFn g_originalInviteHandler = nullptr;
SocialInviteFn g_originalSocialInvite = nullptr;
UserProviderIdFn g_userProviderId = nullptr;
Symbol64Fn g_symbol64 = nullptr;
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

void InviteHandlerHook(void* netGame, std::uint64_t* xpid) {
  const std::uint64_t target = xpid != nullptr ? xpid[0] : 0;
  const std::uint64_t account = xpid != nullptr ? xpid[1] : 0;
  void* social = nullptr;
  std::memcpy(&social, static_cast<const std::uint8_t*>(netGame) + 0x647C8, sizeof(social));
  // The handler's own provider comparison, recomputed with the same two calls it makes.
  std::uint64_t localSymbol = 0;
  std::uint64_t providerId = 0;
  if (g_userProviderId != nullptr && g_symbol64 != nullptr) {
    std::uint64_t user = 0;
    std::memcpy(&user, static_cast<const std::uint8_t*>(netGame) + 0x10, sizeof(user));
    std::uint64_t scratch[2] = {};
    std::uint64_t symbol[2] = {};
    const std::uint64_t* provider = g_userProviderId(scratch, user);
    if (provider != nullptr) {
      providerId = *provider;
      g_symbol64(symbol, providerId);
    }
    localSymbol = symbol[0];
  }
  const unsigned diff = static_cast<unsigned>((target ^ localSymbol) & 0xFF);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.PARTY] invite handler entered target_provider=%llu account=%llu local_provider_id=0x%016llx "
      "local_provider_code=0x%02x nibble_match=%d flag10_match=%d social=%p",
      static_cast<unsigned long long>(target & 0xF), static_cast<unsigned long long>(account),
      static_cast<unsigned long long>(providerId), static_cast<unsigned>(localSymbol & 0xFF),
      (diff & 0xF) == 0 ? 1 : 0, (diff & 0x10) == 0 ? 1 : 0, social);
  g_originalInviteHandler(netGame, xpid);
}

void SocialInviteHook(void* social, std::uint64_t accountId) {
  Log(EchoVR::LogLevel::Info, "[NEVR.PARTY] invite reached the social object account=%llu social=%p (next: "
      "skipped silently if already a party member, else SendInvite)", static_cast<unsigned long long>(accountId), social);
  g_originalSocialInvite(social, accountId);
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

bool InviteHandlerTraced() { return g_originalInviteHandler != nullptr; }

void Install(std::uintptr_t gameBase) {
  InstallChecked(gameBase, kBooleanVA, kBooleanPrologue, g_originalBoolean, reinterpret_cast<PVOID>(&BooleanHook),
                 "CJson_Boolean");
  InstallChecked(gameBase, kDispatchEventVA, kDispatchPrologue, g_originalDispatch,
                 reinterpret_cast<PVOID>(&DispatchEventHook), "DispatchEventToSession");
  // Called, not hooked: validated before the invite-handler trace may use them.
  void* userProviderId = nevr::ResolveVA_Checked(gameBase, kUserProviderIdVA);
  void* symbol64 = nevr::ResolveVA_Checked(gameBase, kSymbol64VA);
  if (nevr::ValidatePrologue(userProviderId, kUserProviderIdPrologue.data(), kUserProviderIdPrologue.size()) &&
      nevr::ValidatePrologue(symbol64, kSymbol64Prologue.data(), kSymbol64Prologue.size())) {
    g_userProviderId = reinterpret_cast<UserProviderIdFn>(userProviderId);
    g_symbol64 = reinterpret_cast<Symbol64Fn>(symbol64);
  } else {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PARTY] provider-check trace disabled: prologue mismatch");
  }
  InstallChecked(gameBase, kInviteHandlerVA, kInviteHandlerPrologue, g_originalInviteHandler,
                 reinterpret_cast<PVOID>(&InviteHandlerHook), "FriendInviteHandler");
  InstallChecked(gameBase, kSocialInviteVA, kSocialInvitePrologue, g_originalSocialInvite,
                 reinterpret_cast<PVOID>(&SocialInviteHook), "SocialInvite");
}

}  // namespace PartyInviteGate
