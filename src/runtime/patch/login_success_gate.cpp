#include "runtime/patch/login_success_gate.h"

#include <windows.h>

#include <array>
#include <cstring>

#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/patching.h"
#include "runtime/patch/login_success_gate_rules.h"

namespace nevr_login_success_gate {
namespace {

// CR15NetGame::LogInSuccess (decompilation ReVault 0x14017f630): its first test is CMP dword [RCX], 2 on the netgame
// state word at +0x0, and the flags pointer (to the qword the bit test reads) is at +0x2DA0.
constexpr std::uint64_t kLogInSuccessVA = 0x14017F630;
constexpr std::uintptr_t kFlagsPointerOffset = 0x2DA0;
// The first 10 bytes of the stock function (MOV [RSP+0x18],RBX; MOV [RSP+0x20],RSI): no RIP-relative operand.
constexpr std::array<std::uint8_t, 10> kLogInSuccessPrologue = {0x48, 0x89, 0x5C, 0x24, 0x18,
                                                                0x48, 0x89, 0x74, 0x24, 0x20};

using LogInSuccessFn = void(__fastcall*)(void* self);
LogInSuccessFn g_original = nullptr;

void __fastcall LogInSuccessHook(void* self) {
  std::uint64_t* flags = nullptr;
  std::int32_t stateBefore = -1;
  if (self != nullptr) {
    std::memcpy(&flags, static_cast<std::uint8_t*>(self) + kFlagsPointerOffset, sizeof(flags));
    std::memcpy(&stateBefore, self, sizeof(stateBefore));
  }
  if (flags == nullptr) {
    // The stock function reads through this pointer unconditionally, so there is nothing to force; run it as it is.
    g_original(self);
    return;
  }
  // Shared word: other threads write other bits, so the set and the restore are interlocked.
  const std::uint64_t force = BitsToForce(*flags);
  auto* word = reinterpret_cast<volatile LONG64*>(flags);
  if (force != 0) InterlockedOr64(word, static_cast<LONG64>(force));
  g_original(self);
  if (force != 0) InterlockedAnd64(word, static_cast<LONG64>(~force));
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] login success bit9 forced was_set=%s state_before=%d",
      force == 0 ? "true" : "false", static_cast<int>(stateBefore));
}

}  // namespace

bool AttachTo(void* target, const char* requestedBy) {
  g_original = reinterpret_cast<LogInSuccessFn>(target);
  // PatchDetour logs a failure at Warning (N126) with the name and the target.
  if (!PatchDetour(&g_original, reinterpret_cast<PVOID>(&LogInSuccessHook), "LogInSuccess")) {
    g_original = nullptr;
    return false;
  }
  Log(EchoVR::LogLevel::Info,
      "[NEVR.PATCH] hooked name=LogInSuccess va=0x%llX why=\"login writes social.community_values_version=1 on every "
      "login (bit 9 forced for the call)\" requested_by=%s",
      static_cast<unsigned long long>(kLogInSuccessVA), requestedBy);
  return true;
}

bool Install(std::uintptr_t gameBase, const char* requestedBy) {
  void* target = nevr::ResolveVA_Checked(gameBase, kLogInSuccessVA);
  if (target == nullptr) {
    Log(EchoVR::LogLevel::Warning, "[NEVR.PATCH] hook skipped name=LogInSuccess va=0x%llX reason=address_unmapped",
        static_cast<unsigned long long>(kLogInSuccessVA));
    return false;
  }
  if (!nevr::ValidatePrologue(target, kLogInSuccessPrologue.data(), kLogInSuccessPrologue.size())) {
    std::uint8_t actual[4] = {0};
    std::memcpy(actual, target, sizeof(actual));
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.PATCH] hook skipped name=LogInSuccess va=0x%llX reason=prologue_mismatch "
        "expected=%02x%02x%02x%02x actual=%02x%02x%02x%02x",
        static_cast<unsigned long long>(kLogInSuccessVA), kLogInSuccessPrologue[0], kLogInSuccessPrologue[1],
        kLogInSuccessPrologue[2], kLogInSuccessPrologue[3], actual[0], actual[1], actual[2], actual[3]);
    return false;
  }
  return AttachTo(target, requestedBy);
}

}  // namespace nevr_login_success_gate
