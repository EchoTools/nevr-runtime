// Host test for the three feature files that take their hooks through rows of the build-time patched libr15
// (docs/adr/0021 sections 5 and 7, test 6): the lobby-full policy (rows lobby_full.join, .failure, .success), the UI texture
// mips (row ui_texture.attach) and the splash skip (row splash_skip.widget_update).
//
// The image is libr15_live_image.h's: Attach verifies the shipped patcher's output for a synthetic stock libr15, which
// fills every row's slot with its trampoline. Each test then runs one feature's Install against it and reads what the
// install did from the slots and the log. What is not run: the detours and the trampolines execute AArch64, and this host
// is x86-64; the handlers' decisions have their own tests (lobby_full_test, the PC rules headers).
//
// Run: row_hooks_install_test [name-filter]

#include <sys/mman.h>
#include <sys/syscall.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "libr15_attach.h"
#include "libr15_live_image.h"
#include "mini_test.h"
#include "pinned_targets.h"
#include "quest/social/lobby_full_hooks.h"
#include "runtime/log/symcache.h"
#include "splash_skip_hook.h"
#include "ui_texture_mips.h"

// The generated symbol cache is linked into the sentinel; the install tests never look a name up.
namespace evr {
extern const SymCacheEntry kSymCacheData[] = {{1, "stub"}};
extern const int kSymCacheCount = 1;
}  // namespace evr

namespace nevr_quest::libr15_patch {
// Reaches the private activation, as the thunk installer does.
struct AttachTestAccess {
  static ActivateStatus Activate(const char* rowId, void* detour, void** originalOut) {
    return RowActivation::ActivateRow(rowId, detour, originalOut);
  }
};
}  // namespace nevr_quest::libr15_patch

NEVR_LIVE_IMAGE_MPROTECT_OVERRIDE()

namespace {

using namespace live_fixture;
using namespace nevr_quest;
using libr15_patch::AttachStatus;
using libr15_patch::AttachTestAccess;

std::size_t RowIndex(const char* id) {
  for (std::size_t i = 0; i < patch::kRowCount; ++i) {
    if (std::strcmp(patch::kRows[i].id, id) == 0) return i;
  }
  CHECK(false);
  return patch::kRowCount;
}

// The address the install log names for a row: the patched function in the live image.
std::string TargetOf(const LiveImage& live, const char* id) {
  return Hex(live.Base() + patch::kRows[RowIndex(id)].sites[0].Vaddr());
}

// Writes `words` at the image's `vaddr`. The page may be one of the image's r-x text pages, so the protection is
// raised and put back through the system call, which the test's protection seam does not see.
void Plant(const LiveImage& live, std::uint64_t vaddr, const std::uint32_t (&words)[4]) {
  void* const addr = reinterpret_cast<void*>(live.Base() + vaddr);
  void* const page = reinterpret_cast<void*>((live.Base() + vaddr) & ~std::uintptr_t{0xFFF});
  CHECK_EQ(syscall(SYS_mprotect, page, std::size_t{0x1000}, PROT_READ | PROT_WRITE), 0L);
  std::memcpy(addr, words, sizeof(words));
  CHECK_EQ(syscall(SYS_mprotect, page, std::size_t{0x1000}, PROT_READ | PROT_EXEC), 0L);
}

void PlantTheCalledFunctions(const LiveImage& live) {
  Plant(live, sentinel::pinned::kSendComponentEventVaddr, sentinel::pinned::kSendComponentEventCode);
  Plant(live, sentinel::pinned::kInputPressedVaddr, sentinel::pinned::kInputPressedCode);
  Plant(live, sentinel::pinned::kForceLoadAllMipsVaddr, sentinel::pinned::kForceLoadAllMipsCode);
}

// The line of `event` whose `row` field is `row`, or nullptr.
const std::string* RowLine(const char* event, const char* row) {
  const std::string e = std::string("\"event\":\"") + event + "\"";
  const std::string r = std::string("\"row\":\"") + row + "\"";
  for (const std::string& l : g.lines) {
    if (Has(l, e) && Has(l, r)) return &l;
  }
  return nullptr;
}

// Attach on the patched synthetic image; every slot then holds its row's trampoline.
void AttachVerified(LiveImage& live) {
  g_live = &live;
  CHECK_EQ(libr15_patch::Attach(Opts()).status, AttachStatus::kVerified);
  g.lines.clear();
}

// ---- the lobby-full policy ----------------------------------------------------------------------------------------------

TEST(TheLobbyFullInstallActivatesItsThreeRowsAndLogsEachWithItsTarget) {
  Env env;
  LiveImage live(PatchedBytes());
  AttachVerified(live);
  void* const trampoline[3] = {*live.Slot(RowIndex("lobby_full.join")), *live.Slot(RowIndex("lobby_full.failure")),
                               *live.Slot(RowIndex("lobby_full.success"))};
  const std::vector<std::string> mapsBefore = live.MapsLines();

  CHECK_EQ(quest_social::lobby_full::InstallLobbyFullHooks(), 3u);

  const char* const rows[3] = {"lobby_full.join", "lobby_full.failure", "lobby_full.success"};
  const char* const hooks[3] = {"join", "failure", "success"};
  void* entry[3];
  for (int i = 0; i < 3; ++i) {
    entry[i] = *live.Slot(RowIndex(rows[i]));
    CHECK(entry[i] != trampoline[i]);  // the thunk entry replaced the trampoline in the slot
    CHECK(entry[i] != nullptr);
    const std::string* line = RowLine("lobby_full", rows[i]);
    CHECK(line != nullptr);
    if (line == nullptr) continue;
    CHECK(Has(*line, "\"status\":\"installed\""));
    CHECK(Has(*line, std::string("\"hook\":\"") + hooks[i] + "\""));
    CHECK(Has(*line, "\"target\":\"" + TargetOf(live, rows[i]) + "\""));
  }
  CHECK(entry[0] != entry[1] && entry[1] != entry[2] && entry[0] != entry[2]);
  ExpectNoImageProtection(live);  // one atomic store per slot; no page was written or re-protected
  CHECK(live.MapsLines() == mapsBefore);
}

TEST(TheLobbyFullInstallBeforeAttachActivatesNothingAndLogsTheStatusOfEachRow) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  live.FillCanaries();

  CHECK_EQ(quest_social::lobby_full::InstallLobbyFullHooks(), 0u);

  CHECK(live.SlotsAreCanaries());
  for (const char* row : {"lobby_full.join", "lobby_full.failure", "lobby_full.success"}) {
    const std::string* line = RowLine("lobby_full", row);
    CHECK(line != nullptr);
    if (line == nullptr) continue;
    CHECK(Has(*line, "\"status\":\"not_verified\""));
    CHECK(Has(*line, "\"target\":\"" + Hex(0) + "\""));
  }
}

TEST(OneLobbyFullRowThatRefusesLeavesTheOtherTwoActivated) {
  Env env;
  LiveImage live(PatchedBytes());
  AttachVerified(live);
  void* original = nullptr;
  void* const taken = reinterpret_cast<void*>(0x4242);
  CHECK_EQ(AttachTestAccess::Activate("lobby_full.failure", taken, &original), libr15_patch::ActivateStatus::kOk);
  g.lines.clear();

  CHECK_EQ(quest_social::lobby_full::InstallLobbyFullHooks(), 2u);

  CHECK(*live.Slot(RowIndex("lobby_full.failure")) == taken);  // the refused row is untouched
  const std::string* refused = RowLine("lobby_full", "lobby_full.failure");
  CHECK(refused != nullptr);
  if (refused != nullptr) CHECK(Has(*refused, "\"status\":\"already_activated\""));
  for (const char* row : {"lobby_full.join", "lobby_full.success"}) {
    const std::string* line = RowLine("lobby_full", row);
    CHECK(line != nullptr);
    if (line != nullptr) CHECK(Has(*line, "\"status\":\"installed\""));
    CHECK(*live.Slot(RowIndex(row)) != taken);
  }
}

// ---- the UI texture mips ------------------------------------------------------------------------------------------------

TEST(TheTextureInstallActivatesItsRowAfterProvingTheForceLoadPrologue) {
  Env env;
  LiveImage live(PatchedBytes());
  AttachVerified(live);
  PlantTheCalledFunctions(live);
  void* const trampoline = *live.Slot(RowIndex("ui_texture.attach"));
  const std::vector<std::string> mapsBefore = live.MapsLines();

  CHECK_EQ(ui_texture_mips::InstallUiTextureMips(&Lookup), 1u);

  CHECK(*live.Slot(RowIndex("ui_texture.attach")) != trampoline);
  const std::string* line = RowLine("ui_texture", "ui_texture.attach");
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK(Has(*line, "\"status\":\"installed\""));
    CHECK(Has(*line, "\"hook\":\"CGTextureResource::Attach\""));
    CHECK(Has(*line, "\"target\":\"" + TargetOf(live, "ui_texture.attach") + "\""));
  }
  ExpectNoImageProtection(live);
  CHECK(live.MapsLines() == mapsBefore);
}

TEST(TheTextureInstallRefusesAForceLoadPrologueThatIsNotThePinnedOneAndLeavesTheRowPassThrough) {
  Env env;
  LiveImage live(PatchedBytes());
  AttachVerified(live);  // the called functions are not planted: the image holds zero words there
  void* const trampoline = *live.Slot(RowIndex("ui_texture.attach"));

  CHECK_EQ(ui_texture_mips::InstallUiTextureMips(&Lookup), 0u);

  CHECK(*live.Slot(RowIndex("ui_texture.attach")) == trampoline);
  const std::string* line = FindLine("\"status\":\"prologue_mismatch\"");
  CHECK(line != nullptr);
  if (line != nullptr) CHECK(Has(*line, "\"hook\":\"ForceLoadAllMips\""));
}

TEST(TheTextureInstallBeforeAttachReadsNothingFromTheImageAndLogsNotVerified) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  live.FillCanaries();
  PlantTheCalledFunctions(live);  // proven words, but no Attach: still refused

  CHECK_EQ(ui_texture_mips::InstallUiTextureMips(&Lookup), 0u);

  CHECK(live.SlotsAreCanaries());
  const std::string* line = RowLine("ui_texture", "ui_texture.attach");
  CHECK(line != nullptr);
  if (line != nullptr) CHECK(Has(*line, "\"status\":\"not_verified\""));
}

TEST(TheTextureInstallWithNoLibr15LoadedLogsItAndReturnsZero) {
  Env env;
  CHECK_EQ(ui_texture_mips::InstallUiTextureMips(&Lookup), 0u);  // g_live is null: the lookup finds nothing
  CHECK(FindLine("\"status\":\"libr15_not_loaded\"") != nullptr);
}

// ---- the splash skip ----------------------------------------------------------------------------------------------------

TEST(TheSplashSkipInstallActivatesItsRowAfterProvingTheTwoFunctionsItCalls) {
  Env env;
  LiveImage live(PatchedBytes());
  AttachVerified(live);
  PlantTheCalledFunctions(live);
  void* const trampoline = *live.Slot(RowIndex("splash_skip.widget_update"));
  const std::vector<std::string> mapsBefore = live.MapsLines();

  CHECK(splash_skip::Install(&Lookup));

  CHECK(*live.Slot(RowIndex("splash_skip.widget_update")) != trampoline);
  const std::string* line = RowLine("splash_skip", "splash_skip.widget_update");
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK(Has(*line, "\"result\":\"installed\""));
    CHECK(Has(*line, "\"target\":\"" + TargetOf(live, "splash_skip.widget_update") + "\""));
  }
  ExpectNoImageProtection(live);
  CHECK(live.MapsLines() == mapsBefore);
}

TEST(TheSplashSkipInstallRefusesWhenEitherCalledFunctionIsNotThePinnedOne) {
  const std::uint32_t wrong[4] = {0, 0, 0, 0};
  for (int which = 0; which < 2; ++which) {
    Env env;
    LiveImage live(PatchedBytes());
    AttachVerified(live);
    PlantTheCalledFunctions(live);
    Plant(live, which == 0 ? sentinel::pinned::kSendComponentEventVaddr : sentinel::pinned::kInputPressedVaddr, wrong);
    void* const trampoline = *live.Slot(RowIndex("splash_skip.widget_update"));

    CHECK(!splash_skip::Install(&Lookup));

    CHECK(*live.Slot(RowIndex("splash_skip.widget_update")) == trampoline);
    const std::string* line = FindLine("\"event\":\"splash_skip\"");
    CHECK(line != nullptr);
    if (line != nullptr) {
      CHECK(Has(*line, "\"result\":\"refused\""));
      CHECK(Has(*line, "\"reason\":\"prologue_mismatch\""));
    }
  }
}

TEST(TheSplashSkipInstallBeforeAttachIsRefusedAsNotVerifiedAndStoresNothing) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  live.FillCanaries();
  PlantTheCalledFunctions(live);

  CHECK(!splash_skip::Install(&Lookup));

  CHECK(live.SlotsAreCanaries());
  const std::string* line = FindLine("\"event\":\"splash_skip\"");
  CHECK(line != nullptr);
  if (line != nullptr) CHECK(Has(*line, "\"reason\":\"not_verified\""));
}

TEST(TheSplashSkipInstallWithNoLibr15LoadedIsRefusedWithItsReason) {
  Env env;
  CHECK(!splash_skip::Install(&Lookup));
  const std::string* line = FindLine("\"event\":\"splash_skip\"");
  CHECK(line != nullptr);
  if (line != nullptr) CHECK(Has(*line, "\"reason\":\"module_not_loaded\""));
}

TEST(ASecondSplashSkipInstallIsRefusedByTheRowAndLeavesTheFirstHook) {
  Env env;
  LiveImage live(PatchedBytes());
  AttachVerified(live);
  PlantTheCalledFunctions(live);
  CHECK(splash_skip::Install(&Lookup));
  void* const first = *live.Slot(RowIndex("splash_skip.widget_update"));
  g.lines.clear();

  CHECK(!splash_skip::Install(&Lookup));

  CHECK(*live.Slot(RowIndex("splash_skip.widget_update")) == first);
  const std::string* line = RowLine("splash_skip", "splash_skip.widget_update");
  CHECK(line != nullptr);
  if (line != nullptr) CHECK(Has(*line, "\"result\":\"already_activated\""));
}

// ---- every feature on one image ------------------------------------------------------------------------------------------

TEST(AllThreeFeaturesInstallOnOneVerifiedImageAndTouchNoPageOfIt) {
  Env env;
  LiveImage live(PatchedBytes());
  AttachVerified(live);
  PlantTheCalledFunctions(live);
  const std::vector<std::string> mapsBefore = live.MapsLines();
  const std::size_t protectsBefore = g.protects.size();
  void* const creditsBefore = *live.Slot(RowIndex("credits.pairs"));

  CHECK_EQ(quest_social::lobby_full::InstallLobbyFullHooks(), 3u);
  CHECK_EQ(ui_texture_mips::InstallUiTextureMips(&Lookup), 1u);
  CHECK(splash_skip::Install(&Lookup));

  ExpectNoImageProtection(live);
  CHECK_EQ(g.protects.size(), protectsBefore);
  CHECK(live.MapsLines() == mapsBefore);
  // The credits row's slot is not a hook's: none of the three installs reached it.
  CHECK(*live.Slot(RowIndex("credits.pairs")) == creditsBefore);
}

}  // namespace

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
