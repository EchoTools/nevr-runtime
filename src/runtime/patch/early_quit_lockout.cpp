#include "runtime/patch/early_quit_lockout.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstring>
#include <ctime>

#include <cstdio>
#include <string>

#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/patching.h"
#include "runtime/log/boot_log_tee.h"
#include "runtime/patch/early_quit_lockout_rules.h"

namespace nevr_early_quit_lockout {
namespace {

// CR15NetGame fields (Quest CR15NetGame::LoadEarlyQuitPenalty 0x126c958 has the setter inlined).
constexpr std::uintptr_t kPenaltyTsOffset = 0x64820;  // penaltyts: the update's stamp, the setter's "newer" gate
constexpr std::uintptr_t kExpiryOffset = 0x64828;     // lockoutexpiretimestamp, read by the expression; we write it
constexpr std::uintptr_t kFlagsOffset = 0x2DA0;       // pointer to the u64 flags qword
constexpr std::uint64_t kCountdownBit = 1ULL << 45;   // cleared by CR15NetGame::Update (0x1401bbdb0) at expiry
constexpr std::uint64_t kPenaltyUpdateEvent = 0xA2E48B35C7CD078FULL;  // delegate_onearlyquitpenaltyupdate

// CR15NetGame::SetEarlyQuitPenaltyLevel: PUSH RDI; SUB RSP,0x20; MOV RAX,[RCX+0x64820].
constexpr std::uint64_t kSetPenaltyVA = 0x1401AE2E0;
constexpr std::array<std::uint8_t, 13> kSetPenaltyPrologue = {0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48,
                                                              0x8B, 0x81, 0x20, 0x48, 0x06, 0x00};
using SetPenaltyFn = void (*)(void* netGame, std::int64_t stamp, std::int64_t expiry, std::int32_t numEarlyQuits,
                              std::int32_t numSteadyMatches, std::int32_t numSteadyEarlyQuits, std::uint8_t penaltyLevel,
                              std::uint8_t steadyPlayerLevel);

// CR15NetEarlyQuitPenaltyExpression::operator() (RDX = SOutput; +0x00 lockoutcountdownactive, +0x10
// lockoutcountdownsec).
constexpr std::uint64_t kPenaltyExpressionVA = 0x140D96190;
constexpr std::array<std::uint8_t, 16> kPenaltyExpressionPrologue = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                                                     0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};
using PenaltyExpressionFn = void (*)(void* expression, void* output, void* thread, void* context);

// The SNSEarlyQuitFeatureFlags callback, CR15NetGame::EarlyQuitFeatureFlagsCB (Quest 0x126ac24), 35 bytes: MOV
// RAX,[RDX+8]; MOVZX EDX,byte [RAX]; AND DL,0xDD; MOV [RCX+0x64847],DL; LEA RDX,[event word]; AND RDX,-8; MOV
// RDX,[RDX]; JMP DispatchEventToSession. The stock mask 0xDD clears bits 1 and 5. The hook replaces the whole
// callback; its 16 prologue bytes (the mask among them) are checked before the replacement is installed.
constexpr const char* kFeatureFlagsName = "EarlyQuitFeatureFlagsCB";
constexpr std::uint64_t kFeatureFlagsCallbackVA = 0x1401618B0;
constexpr std::array<std::uint8_t, 16> kFeatureFlagsCallbackPrologue = {0x48, 0x8B, 0x42, 0x08, 0x0F, 0xB6, 0x10, 0x80,
                                                                        0xE2, 0xDD, 0x88, 0x91, 0x47, 0x48, 0x06, 0x00};
constexpr std::size_t kFeatureFlagsPayloadOffset = 8;      // message+8: the payload pointer
constexpr std::size_t kFeatureFlagsStoreOffset = 0x64847;  // netGame byte the callback stores
// The event word the callback loads: 0xc690a8ff1cf8af99, delegate_onearlyquitfeatureflagsupdate.
constexpr std::uint64_t kFeatureFlagsEventVA = 0x1416DCD78;

// CR15NetGame::DispatchEventToSession: (netGame, event). The session-event trace may have detoured it.
constexpr std::uint64_t kDispatchEventVA = 0x1401A9FE0;
constexpr std::array<std::uint8_t, 16> kDispatchEventPrologue = {0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74,
                                                                 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48};
using DispatchEventFn = void (*)(void* netGame, std::uint64_t event);

std::uintptr_t g_gameBase = 0;
SetPenaltyFn g_originalSetPenalty = nullptr;
PenaltyExpressionFn g_originalPenaltyExpression = nullptr;
std::atomic<int> g_lastCountdownActive{-1};  // for logging transitions only; the expression runs often

void Dispatch(void* netGame, std::uint64_t event) {
  void* target = nevr::ResolveVA_Checked(g_gameBase, kDispatchEventVA);
  if (!nevr_hook_guard::IsOurDetour(target) &&
      !nevr::ValidatePrologue(target, kDispatchEventPrologue.data(), kDispatchEventPrologue.size())) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.EARLYQUIT] penalty event not raised: DispatchEventToSession prologue mismatch");
    return;
  }
  reinterpret_cast<DispatchEventFn>(target)(netGame, event);
}

void SetPenaltyHook(void* netGame, std::int64_t stamp, std::int64_t expiry, std::int32_t numEarlyQuits,
                    std::int32_t numSteadyMatches, std::int32_t numSteadyEarlyQuits, std::uint8_t penaltyLevel,
                    std::uint8_t steadyPlayerLevel) {
  if (netGame == nullptr) {
    g_originalSetPenalty(netGame, stamp, expiry, numEarlyQuits, numSteadyMatches, numSteadyEarlyQuits, penaltyLevel,
                         steadyPlayerLevel);
    return;
  }
  auto* bytes = static_cast<std::uint8_t*>(netGame);
  std::int64_t storedBefore = 0;
  std::memcpy(&storedBefore, bytes + kPenaltyTsOffset, sizeof(storedBefore));
  g_originalSetPenalty(netGame, stamp, expiry, numEarlyQuits, numSteadyMatches, numSteadyEarlyQuits, penaltyLevel,
                       steadyPlayerLevel);
  std::int64_t storedAfter = 0;
  std::memcpy(&storedAfter, bytes + kPenaltyTsOffset, sizeof(storedAfter));
  const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));  // the clock fcn 0x1400de820 returns
  const Countdown countdown = AfterSetPenalty(storedBefore, storedAfter, stamp, expiry, now);
  if (countdown.applied) {
    std::memcpy(bytes + kExpiryOffset, &countdown.expiry, sizeof(countdown.expiry));
    std::uint64_t* flags = nullptr;
    std::memcpy(&flags, bytes + kFlagsOffset, sizeof(flags));
    if (flags != nullptr) *flags = countdown.active ? (*flags | kCountdownBit) : (*flags & ~kCountdownBit);
  }
  Log(EchoVR::LogLevel::Info,
      "[NEVR.EARLYQUIT] penalty set: stamp=%lld stored_before=%lld stored_after=%lld expiry=%lld now=%lld level=%u "
      "early_quits=%d applied=%d -> lockout expiry=%lld active=%d",
      static_cast<long long>(stamp), static_cast<long long>(storedBefore), static_cast<long long>(storedAfter),
      static_cast<long long>(expiry), static_cast<long long>(now), static_cast<unsigned>(penaltyLevel), numEarlyQuits,
      countdown.applied ? 1 : 0, static_cast<long long>(countdown.expiry), countdown.active ? 1 : 0);
  if (countdown.applied) Dispatch(netGame, kPenaltyUpdateEvent);
}

void PenaltyExpressionHook(void* expression, void* output, void* thread, void* context) {
  g_originalPenaltyExpression(expression, output, thread, context);
  if (output == nullptr) return;
  auto* out = static_cast<std::uint8_t*>(output);
  std::int64_t countdownSec = 0;
  std::memcpy(&countdownSec, out + 0x10, sizeof(countdownSec));
  const bool active = CountdownActive(countdownSec);
  out[0] = active ? 1 : 0;
  if (g_lastCountdownActive.exchange(active ? 1 : 0) != (active ? 1 : 0))
    Log(EchoVR::LogLevel::Info, "[NEVR.EARLYQUIT] lockoutcountdownactive=%d (lockoutcountdownsec=%lld)", active ? 1 : 0,
        static_cast<long long>(countdownSec));
}

template <typename Fn, std::size_t Size>
void HookChecked(std::uint64_t va, const std::array<std::uint8_t, Size>& prologue, Fn& original, PVOID detour,
                 const char* name) {
  void* target = nevr::ResolveVA_Checked(g_gameBase, va);
  if (!nevr::ValidatePrologue(target, prologue.data(), prologue.size())) {
    nevr_boot_log_tee::TeeFprintf("[NEVR.EARLYQUIT] %s hook skipped va=0x%llx reason=prologue_mismatch\n", name,
                           static_cast<unsigned long long>(va));
    return;
  }
  original = reinterpret_cast<Fn>(target);
  if (PatchDetour(&original, detour, name))
    nevr_boot_log_tee::TeeFprintf("[NEVR.EARLYQUIT] %s hook installed va=0x%llx\n", name, static_cast<unsigned long long>(va));
}

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

// The replacement for the whole callback. The byte the service sent is stored as it is (the stock mask is not
// applied), then the event the game's data holds is dispatched, as the stock tail call did.
void FeatureFlagsCallbackHook(void* netGame, void* message) {
  auto* bytes = static_cast<std::uint8_t*>(netGame);
  const std::uint8_t* payload = nullptr;
  std::memcpy(&payload, static_cast<const std::uint8_t*>(message) + kFeatureFlagsPayloadOffset, sizeof(payload));
  const std::uint8_t sent = *payload;
  const std::uint8_t before = bytes[kFeatureFlagsStoreOffset];
  bytes[kFeatureFlagsStoreOffset] = sent;
  Log(EchoVR::LogLevel::Debug, "[NEVR.EARLYQUIT] feature flags set value=0x%02x was=0x%02x", sent, before);
  const void* eventWord = nevr::ResolveVA_Checked(g_gameBase, kFeatureFlagsEventVA);
  if (eventWord == nullptr) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.EARLYQUIT] feature flags event not raised: event word unreadable va=0x%llX",
        static_cast<unsigned long long>(kFeatureFlagsEventVA));
    return;
  }
  std::uint64_t event = 0;
  std::memcpy(&event, eventWord, sizeof(event));
  Dispatch(netGame, event);
}

// Replaces the callback once its 16 prologue bytes match. On a mismatch the stock callback stays, with its mask.
void ReplaceFeatureFlagsCallback() {
  void* target = nevr::ResolveVA_Checked(g_gameBase, kFeatureFlagsCallbackVA);
  if (!nevr::ValidatePrologue(target, kFeatureFlagsCallbackPrologue.data(), kFeatureFlagsCallbackPrologue.size())) {
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.EARLYQUIT] hook skipped name=%s va=0x%llX reason=prologue_mismatch expected=%s actual=%s "
        "(the stock callback keeps its mask 0xdd, so the feature flags are cut to bits 0, 2-4, 6 and 7)",
        kFeatureFlagsName, static_cast<unsigned long long>(kFeatureFlagsCallbackVA),
        HexBytes(kFeatureFlagsCallbackPrologue.data(), kFeatureFlagsCallbackPrologue.size()).c_str(),
        HexBytes(target, kFeatureFlagsCallbackPrologue.size()).c_str());
    return;
  }
  void* original = target;  // PatchDetour reads the target from here; the trampoline is never called.
  if (!PatchDetour(&original, reinterpret_cast<PVOID>(&FeatureFlagsCallbackHook), kFeatureFlagsName)) return;
  Log(EchoVR::LogLevel::Info,
      "[NEVR.EARLYQUIT] hooked name=EarlyQuitFeatureFlagsCB va=0x%llX why=\"the client keeps every feature-flag bit "
      "the service sends (the stock callback clears bits 1 and 5)\"",
      static_cast<unsigned long long>(kFeatureFlagsCallbackVA));
}

}  // namespace

void Install(std::uintptr_t gameBase) {
  g_gameBase = gameBase;
  HookChecked(kSetPenaltyVA, kSetPenaltyPrologue, g_originalSetPenalty, reinterpret_cast<PVOID>(&SetPenaltyHook),
              "SetEarlyQuitPenaltyLevel");
  HookChecked(kPenaltyExpressionVA, kPenaltyExpressionPrologue, g_originalPenaltyExpression,
              reinterpret_cast<PVOID>(&PenaltyExpressionHook), "EarlyQuitPenaltyExpression");
  ReplaceFeatureFlagsCallback();
}

}  // namespace nevr_early_quit_lockout
