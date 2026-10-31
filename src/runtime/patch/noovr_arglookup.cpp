#include "runtime/patch/noovr_arglookup.h"

#include <windows.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/addresses.h"
#include "runtime/hook/patching.h"
#include "runtime/patch/cmdline_waiver.h"

namespace nevr_noovr_arglookup {
namespace {

using FindArgIndexFn = std::int64_t (*)(void* mainArgs, std::uint64_t symbol);

FindArgIndexFn g_original = nullptr;
std::uintptr_t g_gameBase = 0;

// noinline: __builtin_return_address(0) must name the game's call site, not this hook's caller.
__attribute__((noinline)) std::int64_t FindArgIndexHook(void* mainArgs, std::uint64_t symbol) {
  const auto site = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(__builtin_return_address(0)) - g_gameBase);
  const std::int64_t original = g_original(mainArgs, symbol);
  const nevr_cmdline_waiver::Answer answer = nevr_cmdline_waiver::Decide(original, symbol, site);
  if (answer.waived) {
    Log(EchoVR::LogLevel::Info,
        "[NEVR.PATCH] noovr_spectatorstream_waiver site=0x%llX symbol=0x%016llX original=%lld returned=%lld "
        "(-noovr does not require -spectatorstream)",
        static_cast<unsigned long long>(site), static_cast<unsigned long long>(symbol),
        static_cast<long long>(original), static_cast<long long>(answer.value));
  }
  return answer.value;
}

// The expected and actual bytes as hex (Rule 6); "unmapped" when there is no memory at the site to read.
void Refuse(const char* what, std::uint64_t va, const std::uint8_t* expected, std::size_t size, const void* actual) {
  constexpr std::size_t kMax = 16;
  char expectedHex[2 * kMax + 1] = {};
  char actualHex[2 * kMax + 1] = "unmapped";
  for (std::size_t i = 0; i < size && i < kMax; ++i) std::snprintf(expectedHex + 2 * i, 3, "%02x", expected[i]);
  if (actual != nullptr) {
    const auto* bytes = static_cast<const std::uint8_t*>(actual);
    for (std::size_t i = 0; i < size && i < kMax; ++i) std::snprintf(actualHex + 2 * i, 3, "%02x", bytes[i]);
  }
  Log(EchoVR::LogLevel::Warning,
      "[NEVR.PATCH] noovr argument lookup hook refused site=%s va=0x%llX expected=%s actual=%s "
      "(-noovr keeps the game's -spectatorstream requirement)",
      what, static_cast<unsigned long long>(va), expectedHex, actualHex);
}

}  // namespace

void Install(std::uintptr_t gameBase) {
  using namespace nevr_patch_addresses;
  g_gameBase = gameBase;

  constexpr std::size_t kPrologueSize = sizeof(CMAINARGS_FIND_ARG_INDEX_BY_SYMBOL_PROLOGUE);
  void* callee = nevr::ResolveVA_Checked(gameBase, CMAINARGS_FIND_ARG_INDEX_BY_SYMBOL);
  if (!nevr::ValidatePrologue(callee, CMAINARGS_FIND_ARG_INDEX_BY_SYMBOL_PROLOGUE, kPrologueSize)) {
    Refuse("callee", CMAINARGS_FIND_ARG_INDEX_BY_SYMBOL, CMAINARGS_FIND_ARG_INDEX_BY_SYMBOL_PROLOGUE, kPrologueSize,
           callee);
    return;
  }

  // The pinned call site: E8 rel32 to the callee, then the game's own CMP RAX,-1 / JNE. The rel32 is computed from
  // the callee VA, so a call that goes anywhere else fails the same check.
  std::array<std::uint8_t, 5 + sizeof(NOOVR_ARG_LOOKUP_CONSUMER)> callSite{};
  const auto rel = static_cast<std::int64_t>(CMAINARGS_FIND_ARG_INDEX_BY_SYMBOL) -
                   static_cast<std::int64_t>(NOOVR_ARG_LOOKUP_CALL + 5);
  const auto rel32 = static_cast<std::int32_t>(rel);
  callSite[0] = 0xE8;
  std::memcpy(callSite.data() + 1, &rel32, sizeof(rel32));
  std::memcpy(callSite.data() + 5, NOOVR_ARG_LOOKUP_CONSUMER, sizeof(NOOVR_ARG_LOOKUP_CONSUMER));
  void* site = nevr::ResolveVA_Checked(gameBase, NOOVR_ARG_LOOKUP_CALL);
  if (!nevr::ValidatePrologue(site, callSite.data(), callSite.size())) {
    Refuse("call_site", NOOVR_ARG_LOOKUP_CALL, callSite.data(), callSite.size(), site);
    return;
  }

  g_original = reinterpret_cast<FindArgIndexFn>(callee);
  if (!PatchDetour(&g_original, reinterpret_cast<PVOID>(&FindArgIndexHook), "CMainArgs::FindArgIndexBySymbol")) return;
  Log(EchoVR::LogLevel::Info, "[NEVR.PATCH] hook installed name=CMainArgs::FindArgIndexBySymbol waivers=%zu prologue=ok",
      static_cast<std::size_t>(std::size(nevr_cmdline_waiver::kWaivers)));
}

}  // namespace nevr_noovr_arglookup
