#include "runtime/patch/invite_choice_hook.h"
#include "runtime/patch/menu_friends_pack.h"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <exception>
#include <string>

#include "core/logging.h"
#include "nevr/pc/nevr_common.h"
#include "runtime/hook/patching.h"
#include "runtime/lifecycle/readable_memory.h"
#include "runtime/patch/invite_wizard.h"

// CncaGameSpace::SendComponentEvent (ReVault echovr.exe 0x14050fb60): RCX = the game space, RDX = the SComponentEvent,
// R8D = flags. The game's own code reads the event at once (word 0 on entry, word 2 as the sender), so the observer
// reads those words without asking the page table; everything it reaches from them goes through a checked read.
// The bytes below are the function up to its first call; none of them is position dependent.
namespace nevr_invite_choice_hook {
namespace {

constexpr std::uint64_t kSendComponentEventVA = 0x14050fb60ULL;
constexpr std::array<std::uint8_t, 35> kSendComponentEventPrologue = {
    0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18, 0x57, 0x48, 0x83,
    0xEC, 0x70, 0x41, 0x8B, 0xF8, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0xF1, 0x45, 0x8B, 0xC8, 0x4C, 0x8B, 0x02};

using SendComponentEventFn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t);

SendComponentEventFn g_original = nullptr;
nevr_invite_choices::State g_choices;
std::atomic<std::uint64_t> g_seen{0};          // every call: the hook's call rate
std::atomic<std::uint64_t> g_unresolved{0};    // wizard symbols whose sender did not resolve to a wizard page

void Observe(std::uint64_t space, std::uint64_t eventAddress) {
  const std::uint64_t seen = g_seen.fetch_add(1, std::memory_order_relaxed) + 1;
  const auto event = static_cast<std::uintptr_t>(eventAddress);
  const auto reader = [event](std::uintptr_t address, void* out, std::size_t length) {
    // The game reads the event itself before anything else; the rest of what is reached is checked.
    const bool inEvent = address >= event && address + length <= event + nevr_invite_choices::kEventBytes;
    if (!inEvent && !nevr_crash_recovery::IsReadableMemory(reinterpret_cast<const void*>(address), length)) {
      return false;
    }
    std::memcpy(out, reinterpret_cast<const void*>(address), length);
    return true;
  };
  nevr_invite_choices::Event e;
  const nevr_invite_choices::Read read =
      nevr_invite_choices::ReadEvent(reader, static_cast<std::uintptr_t>(space), event, &e);
  if (read == nevr_invite_choices::Read::NotWizardSymbol) return;
  if (read != nevr_invite_choices::Read::Wizard) {
    // A wizard event symbol from an actor that is not a wizard page (every other menu uses the same symbols), or one
    // whose sender could not be followed.
    const std::uint64_t unresolved = g_unresolved.fetch_add(1, std::memory_order_relaxed) + 1;
    Log(EchoVR::LogLevel::Debug,
        "[NEVR.INVITE] wizard symbol not on a wizard page read=%s event=%s owner=0x%llx component=0x%llx unresolved=%llu",
        nevr_invite_choices::ReadName(read), nevr_invite_choices::EventName(e.symbol),
        static_cast<unsigned long long>(e.owner), static_cast<unsigned long long>(e.component),
        static_cast<unsigned long long>(unresolved));
    return;
  }
  g_choices.Observe(e);
  Log(EchoVR::LogLevel::Info, "%s", nevr_invite_choices::ChoiceLine(e, g_choices.Snapshot(), seen).c_str());
}

std::uint64_t SendComponentEventHook(std::uint64_t space, std::uint64_t event, std::uint64_t flags) {
  // #1535: the FRIENDS panel sees every event before the game, and may rewrite one it owns (a row click opens the profile).
  nevr_menu_friends_pack::OnComponentEvent(space, event);
  if (event != 0) {
    try {
      Observe(space, event);
    } catch (const std::exception& error) {
      Log(EchoVR::LogLevel::Warning, "[NEVR.INVITE] choice observer failed reason=%s", error.what());
    }
  }
  return g_original(space, event, flags);
}

}  // namespace

nevr_invite_choices::State& Choices() { return g_choices; }

bool Install(std::uintptr_t gameBase) {
  void* target = nevr::ResolveVA_Checked(gameBase, kSendComponentEventVA);
  if (!nevr::ValidatePrologue(target, kSendComponentEventPrologue.data(), kSendComponentEventPrologue.size())) {
    // Rule 6: the hook's name, its address, the pinned bytes and the bytes found there. An unreadable target is not read.
    const std::string expected = nevr_invite::HexBytes(kSendComponentEventPrologue.data(), kSendComponentEventPrologue.size());
    const std::string actual = nevr_crash_recovery::IsReadableMemory(target, kSendComponentEventPrologue.size())
                                   ? nevr_invite::HexBytes(static_cast<const std::uint8_t*>(target),
                                                           kSendComponentEventPrologue.size())
                                   : std::string("unreadable");
    Log(EchoVR::LogLevel::Warning,
        "[NEVR.INVITE] choice observer off reason=prologue_mismatch name=SendComponentEvent va=0x%llx expected=%s actual=%s",
        static_cast<unsigned long long>(kSendComponentEventVA), expected.c_str(), actual.c_str());
    return false;
  }
  g_original = reinterpret_cast<SendComponentEventFn>(target);
  PatchDetour(&g_original, reinterpret_cast<PVOID>(&SendComponentEventHook), "SendComponentEvent");
  Log(EchoVR::LogLevel::Info, "[NEVR.INVITE] choice observer installed va=0x%llx",
      static_cast<unsigned long long>(kSendComponentEventVA));
  return true;
}

}  // namespace nevr_invite_choice_hook
