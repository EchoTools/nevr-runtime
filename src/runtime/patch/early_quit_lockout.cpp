#include "runtime/patch/early_quit_lockout.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstring>
#include <ctime>

#include "core/logging.h"
#include "nevr_common.h"
#include "runtime/hook/hook_guard.h"
#include "runtime/hook/patching.h"
#include "runtime/hook/process_memory.h"
#include "runtime/log/boot_log_tee.h"
#include "runtime/patch/early_quit_lockout_rules.h"

namespace EarlyQuitLockout {
namespace {

// CR15NetGame fields (Quest CR15NetGame::LoadEarlyQuitPenalty 0x126c958 has the setter inlined).
constexpr std::uintptr_t kPenaltyTsOffset = 0x64820;  // penaltyts: the game service's lockout expiry
constexpr std::uintptr_t kExpiryOffset = 0x64828;     // lockoutexpiretimestamp, read by the expression
constexpr std::uintptr_t kFlagsOffset = 0x2DA0;       // pointer to the u64 flags qword
constexpr std::uint64_t kCountdownBit = 1ULL << 45;   // cleared by CR15NetGame::Update (0x1401bbdb0) at expiry
constexpr std::uint64_t kPenaltyUpdateEvent = 0xA2E48B35C7CD078FULL;  // delegate_onearlyquitpenaltyupdate

// CR15NetGame::SetEarlyQuitPenaltyLevel: PUSH RDI; SUB RSP,0x20; MOV RAX,[RCX+0x64820].
constexpr std::uint64_t kSetPenaltyVA = 0x1401AE2E0;
constexpr std::array<std::uint8_t, 13> kSetPenaltyPrologue = {0x40, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48,
                                                              0x8B, 0x81, 0x20, 0x48, 0x06, 0x00};
using SetPenaltyFn = void (*)(void* netGame, std::int64_t penaltyTs, std::int64_t unused, std::int32_t numEarlyQuits,
                              std::int32_t numSteadyMatches, std::int32_t numSteadyEarlyQuits, std::uint8_t penaltyLevel,
                              std::uint8_t steadyPlayerLevel);

// CR15NetEarlyQuitPenaltyExpression::operator() (RDX = SOutput; +0x00 lockoutcountdownactive, +0x10
// lockoutcountdownsec).
constexpr std::uint64_t kPenaltyExpressionVA = 0x140D96190;
constexpr std::array<std::uint8_t, 16> kPenaltyExpressionPrologue = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C,
                                                                     0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57};
using PenaltyExpressionFn = void (*)(void* expression, void* output, void* thread, void* context);

// The SNSEarlyQuitFeatureFlags callback (Quest CR15NetGame::EarlyQuitFeatureFlagsCB 0x126ac24): MOV RAX,[RDX+8];
// MOVZX EDX,byte [RAX]; AND DL,0xDD; MOV [RCX+0x64847],DL. The mask's immediate is at +9.
constexpr std::uint64_t kFeatureFlagsCallbackVA = 0x1401618B0;
constexpr std::array<std::uint8_t, 16> kFeatureFlagsCallbackBytes = {0x48, 0x8B, 0x42, 0x08, 0x0F, 0xB6, 0x10, 0x80,
                                                                     0xE2, 0xDD, 0x88, 0x91, 0x47, 0x48, 0x06, 0x00};
constexpr std::size_t kFeatureFlagsMaskOffset = 9;

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

void SetPenaltyHook(void* netGame, std::int64_t penaltyTs, std::int64_t unused, std::int32_t numEarlyQuits,
                    std::int32_t numSteadyMatches, std::int32_t numSteadyEarlyQuits, std::uint8_t penaltyLevel,
                    std::uint8_t steadyPlayerLevel) {
  g_originalSetPenalty(netGame, penaltyTs, unused, numEarlyQuits, numSteadyMatches, numSteadyEarlyQuits, penaltyLevel,
                       steadyPlayerLevel);
  if (netGame == nullptr) return;
  auto* bytes = static_cast<std::uint8_t*>(netGame);
  std::int64_t storedTs = 0;
  std::memcpy(&storedTs, bytes + kPenaltyTsOffset, sizeof(storedTs));
  const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));  // the clock fcn 0x1400de820 returns
  const Countdown countdown = AfterSetPenalty(penaltyTs, storedTs, now);
  std::memcpy(bytes + kExpiryOffset, &countdown.expiry, sizeof(countdown.expiry));
  if (countdown.resetStoredTs) {
    const std::int64_t none = -1;
    std::memcpy(bytes + kPenaltyTsOffset, &none, sizeof(none));
  }
  std::uint64_t* flags = nullptr;
  std::memcpy(&flags, bytes + kFlagsOffset, sizeof(flags));
  if (flags != nullptr) *flags = countdown.active ? (*flags | kCountdownBit) : (*flags & ~kCountdownBit);
  Log(EchoVR::LogLevel::Info,
      "[NEVR.EARLYQUIT] penalty set: incoming penaltyts=%lld stored=%lld now=%lld level=%u early_quits=%d -> lockout "
      "expiry=%lld active=%d%s",
      static_cast<long long>(penaltyTs), static_cast<long long>(storedTs), static_cast<long long>(now),
      static_cast<unsigned>(penaltyLevel), numEarlyQuits, static_cast<long long>(countdown.expiry),
      countdown.active ? 1 : 0, countdown.resetStoredTs ? " (cleared by the game service)" : "");
  Dispatch(netGame, kPenaltyUpdateEvent);
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
    BootLogTee::TeeFprintf("[NEVR.EARLYQUIT] %s hook skipped va=0x%llx reason=prologue_mismatch\n", name,
                           static_cast<unsigned long long>(va));
    return;
  }
  original = reinterpret_cast<Fn>(target);
  if (PatchDetour(&original, detour, name))
    BootLogTee::TeeFprintf("[NEVR.EARLYQUIT] %s hook installed va=0x%llx\n", name, static_cast<unsigned long long>(va));
}

void KeepFeatureFlagBit1() {
  void* target = nevr::ResolveVA_Checked(g_gameBase, kFeatureFlagsCallbackVA);
  if (!nevr::ValidatePrologue(target, kFeatureFlagsCallbackBytes.data(), kFeatureFlagsCallbackBytes.size())) {
    BootLogTee::TeeFprintf("[NEVR.EARLYQUIT] feature-flag mask patch skipped va=0x%llx reason=bytes_mismatch\n",
                           static_cast<unsigned long long>(kFeatureFlagsCallbackVA));
    return;
  }
  // The game service decides which bits are set; the client keeps them all.
  std::uint8_t keepAll = 0xFF;
  ProcessMemcpy(static_cast<std::uint8_t*>(target) + kFeatureFlagsMaskOffset, &keepAll, sizeof(keepAll));
  BootLogTee::TeeFprintf("[NEVR.EARLYQUIT] feature-flag mask 0xdd -> 0xff va=0x%llx\n",
                         static_cast<unsigned long long>(kFeatureFlagsCallbackVA + kFeatureFlagsMaskOffset));
}

}  // namespace

void Install(std::uintptr_t gameBase) {
  g_gameBase = gameBase;
  HookChecked(kSetPenaltyVA, kSetPenaltyPrologue, g_originalSetPenalty, reinterpret_cast<PVOID>(&SetPenaltyHook),
              "SetEarlyQuitPenaltyLevel");
  HookChecked(kPenaltyExpressionVA, kPenaltyExpressionPrologue, g_originalPenaltyExpression,
              reinterpret_cast<PVOID>(&PenaltyExpressionHook), "EarlyQuitPenaltyExpression");
  KeepFeatureFlagBit1();
}

}  // namespace EarlyQuitLockout
