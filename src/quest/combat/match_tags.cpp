#include "quest/combat/match_tags.h"

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::match_tags {

namespace {

sentinel::GotHook g_c1Hook;
sentinel::GotHook g_c2Hook;

std::uint32_t LoadFlags(const unsigned char* game, std::size_t offset) noexcept {
  std::uint32_t flags = 0;
  std::memcpy(&flags, game + offset, sizeof(flags));
  return flags;
}

void StoreFlags(unsigned char* game, std::size_t offset, std::uint32_t flags) noexcept {
  std::memcpy(game + offset, &flags, sizeof(flags));
}

// Runs after the constructor returned: each flags word that still holds the stock default gets bit 13, and one line says what each
// held before and after. A null object is the constructor's to refuse; nothing here reads it.
void AfterConstruction(const char* ctor, void* object) noexcept {
  if (object == nullptr) return;
  unsigned char* game = static_cast<unsigned char*>(object);
  const std::uint32_t currentBefore = LoadFlags(game, kCurrentFlagsOffset);
  const std::uint32_t pendingBefore = LoadFlags(game, kPendingFlagsOffset);
  const std::uint32_t currentAfter = DefaultedFlags(currentBefore);
  const std::uint32_t pendingAfter = DefaultedFlags(pendingBefore);
  int changed = 0;
  if (currentAfter != currentBefore) {
    StoreFlags(game, kCurrentFlagsOffset, currentAfter);
    ++changed;
  }
  if (pendingAfter != pendingBefore) {
    StoreFlags(game, kPendingFlagsOffset, pendingAfter);
    ++changed;
  }
  char currentBeforeText[16];
  char currentAfterText[16];
  char pendingBeforeText[16];
  char pendingAfterText[16];
  std::snprintf(currentBeforeText, sizeof(currentBeforeText), "0x%08x", static_cast<unsigned>(currentBefore));
  std::snprintf(currentAfterText, sizeof(currentAfterText), "0x%08x", static_cast<unsigned>(currentAfter));
  std::snprintf(pendingBeforeText, sizeof(pendingBeforeText), "0x%08x", static_cast<unsigned>(pendingBefore));
  std::snprintf(pendingAfterText, sizeof(pendingAfterText), "0x%08x", static_cast<unsigned>(pendingAfter));
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_match_tags_default",
                      {{"ctor", ctor},
                       {"result", changed > 0 ? "set" : "kept"},
                       {"changed", changed},
                       {"current_before", currentBeforeText},
                       {"current_after", currentAfterText},
                       {"pending_before", pendingBeforeText},
                       {"pending_after", pendingAfterText}});
}

// CR15Game::CR15Game(CMainArgs const&), through the C1 and the C2 slot. The original first, so the words read are the ones the
// constructor stored.
void C1Handler(C1Thunk::Fn original, void* game, const void* args) noexcept {
  original(game, args);
  AfterConstruction("c1", game);
}

void C2Handler(C2Thunk::Fn original, void* game, const void* args) noexcept {
  original(game, args);
  AfterConstruction("c2", game);
}

NEVR_HOOK_RECORD(kC1Hook, C1Thunk, &C1Handler);
NEVR_HOOK_RECORD(kC2Hook, C2Thunk, &C2Handler);

}  // namespace

const char* InstallResultName(bool c1Installed, bool c2Installed) noexcept {
  if (c1Installed && c2Installed) return "installed";
  return c1Installed || c2Installed ? "partial" : "refused";
}

bool InstallAt(const sentinel::GotTarget& c1, const sentinel::GotTarget& c2) noexcept {
  C1Thunk::Arm(kC1Hook);
  C2Thunk::Arm(kC2Hook);
  const sentinel::GotStatus s1 = sentinel::InstallThunk<C1Thunk>(g_c1Hook, c1);
  const sentinel::GotStatus s2 = sentinel::InstallThunk<C2Thunk>(g_c2Hook, c2);
  const bool ok1 = s1 == sentinel::GotStatus::kOk;
  const bool ok2 = s2 == sentinel::GotStatus::kOk;
  // A slot that did not take its thunk is never called, but the handler stays off with it.
  if (!ok1) C1Thunk::Disarm();
  if (!ok2) C2Thunk::Disarm();
  const char* result = InstallResultName(ok1, ok2);
  sentinel::LogFields(ok1 && ok2 ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_match_tags_install",
                      {{"result", result}, {"c1", sentinel::GotStatusName(s1)}, {"c2", sentinel::GotStatusName(s2)}});
  return ok1 && ok2;
}

bool Install() noexcept { return InstallAt(sentinel::pinned::LibR15CR15GameC1(), sentinel::pinned::LibR15CR15GameC2()); }

void ArmForTest() noexcept {
  C1Thunk::Arm(kC1Hook);
  C2Thunk::Arm(kC2Hook);
}

void ResetForTest() noexcept {
  C1Thunk::Reset();
  C2Thunk::Reset();
}

}  // namespace nevr_quest::combat::match_tags
