// Host test for the feature files that take their hooks through rows of the build-time patched libr15
// (docs/adr/0021 sections 5 and 7, test 6): the lobby-full policy (rows lobby_full.join, .failure, .success), the UI texture
// mips (row ui_texture.attach), the splash skip (row splash_skip.widget_update) and the grey default irradiance texture (row
// irradiance_default.upload, and the refusal of its GOT half).
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
#include "irradiance_default.h"
#include "irradiance_default_hook.h"
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

TEST(ASecondTextureInstallIsRefusedByTheRowReturnsZeroAndLeavesTheFirstHook) {
  Env env;
  LiveImage live(PatchedBytes());
  AttachVerified(live);
  PlantTheCalledFunctions(live);
  CHECK_EQ(ui_texture_mips::InstallUiTextureMips(&Lookup), 1u);
  void* const first = *live.Slot(RowIndex("ui_texture.attach"));
  g.lines.clear();

  CHECK_EQ(ui_texture_mips::InstallUiTextureMips(&Lookup), 0u);

  CHECK(*live.Slot(RowIndex("ui_texture.attach")) == first);
  const std::string* line = RowLine("ui_texture", "ui_texture.attach");
  CHECK(line != nullptr);
  if (line != nullptr) CHECK(Has(*line, "\"status\":\"already_activated\""));
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

// ---- the grey default irradiance texture (#2091) ------------------------------------------------------------------------

bool NoLibr15(const char*, sentinel::ElfImage*) { return false; }

// Stand-ins for the game: Initialize uploads the default 3D texture through the thunk entry of the helper, as the game's `bl` reaches
// the row's stub; the helper records the texel it was given.
unsigned char g_irradianceResources[0x2900];
const unsigned char g_irradianceBlack[4] = {0, 0, 0, 0};
unsigned char g_irradianceSeen[4];
int g_irradianceUploads = 0;
void IrradianceStandInUpload(void*, const void*, std::uint32_t, std::int32_t, std::int32_t, const void* texel, const char*) {
  ++g_irradianceUploads;
  std::memcpy(g_irradianceSeen, texel, sizeof(g_irradianceSeen));
}
std::int32_t IrradianceStandInInitialize(void* self) {
  reinterpret_cast<irradiance_default::UploadThunk::Fn>(irradiance_default::UploadThunk::EntryAddress())(
      static_cast<unsigned char*>(self) + irradiance_default::kDefault3dTextureOffset, nullptr, irradiance_default::kTexelFormat, 0, 0,
      g_irradianceBlack, "Default 3D Texture");
  return 0;
}

TEST(TheIrradianceRowInstallActivatesItsRowAndLogsItsTarget) {
  Env env;
  LiveImage live(PatchedBytes());
  AttachVerified(live);
  void* const trampoline = *live.Slot(RowIndex("irradiance_default.upload"));
  const std::vector<std::string> mapsBefore = live.MapsLines();

  CHECK(irradiance_default::InstallUploadRow());

  CHECK(*live.Slot(RowIndex("irradiance_default.upload")) != trampoline);  // the thunk entry replaced the trampoline in the slot
  const std::string* line = RowLine("irradiance_default", "irradiance_default.upload");
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK(Has(*line, "\"status\":\"installed\""));
    CHECK(Has(*line, "\"hook\":\"image upload helper\""));
    CHECK(Has(*line, "\"target\":\"" + TargetOf(live, "irradiance_default.upload") + "\""));
  }
  ExpectNoImageProtection(live);  // one atomic store; no page was written or re-protected
  CHECK(live.MapsLines() == mapsBefore);
}

// The install arms the handler it activates: with the Initialize half armed too, an Initialize that uploads the default 3D texture
// through the row's thunk entry gives the helper the grey texel.
TEST(TheIrradianceRowInstallArmsTheUploadHandler) {
  Env env;
  LiveImage live(PatchedBytes());
  AttachVerified(live);
  irradiance_default::UploadThunk::Reset();
  irradiance_default::InitializeThunk::Reset();
  CHECK(irradiance_default::InstallUploadRow());
  *irradiance_default::UploadThunk::OriginalOut() = reinterpret_cast<void*>(&IrradianceStandInUpload);  // the trampoline is AArch64
  *irradiance_default::InitializeThunk::OriginalOut() = reinterpret_cast<void*>(&IrradianceStandInInitialize);
  irradiance_default::ResetForTest();
  irradiance_default::ArmInitializeForTest();
  g_irradianceUploads = 0;
  std::memset(g_irradianceSeen, 0xEE, sizeof(g_irradianceSeen));

  CHECK_EQ(reinterpret_cast<irradiance_default::InitializeThunk::Fn>(irradiance_default::InitializeThunk::EntryAddress())(g_irradianceResources), 0);

  CHECK_EQ(g_irradianceUploads, 1);
  CHECK(g_irradianceSeen[0] == 0x6b && g_irradianceSeen[1] == 0x6b && g_irradianceSeen[2] == 0x6b && g_irradianceSeen[3] == 0xff);
  irradiance_default::UploadThunk::Reset();
  irradiance_default::InitializeThunk::Reset();
}

TEST(TheIrradianceRowInstallBeforeAttachActivatesNothingAndLogsNotVerified) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  live.FillCanaries();

  CHECK(!irradiance_default::InstallUploadRow());

  CHECK(live.SlotsAreCanaries());
  const std::string* line = RowLine("irradiance_default", "irradiance_default.upload");
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK(Has(*line, "\"status\":\"not_verified\""));
    CHECK(Has(*line, "\"target\":\"" + Hex(0) + "\""));
  }
}

TEST(ASecondIrradianceRowInstallIsRefusedByTheRowAndLeavesTheFirstHook) {
  Env env;
  LiveImage live(PatchedBytes());
  AttachVerified(live);
  CHECK(irradiance_default::InstallUploadRow());
  void* const first = *live.Slot(RowIndex("irradiance_default.upload"));
  g.lines.clear();

  CHECK(!irradiance_default::InstallUploadRow());

  CHECK(*live.Slot(RowIndex("irradiance_default.upload")) == first);
  const std::string* line = RowLine("irradiance_default", "irradiance_default.upload");
  CHECK(line != nullptr);
  if (line != nullptr) CHECK(Has(*line, "\"status\":\"already_activated\""));
}

// The GOT half is installed first: with it refused the row is never activated, so the upload helper stays the game's.
TEST(AnIrradianceInstallWhoseGotHookIsRefusedLeavesTheRowPassThrough) {
  Env env;
  LiveImage live(PatchedBytes());
  AttachVerified(live);
  void* const trampoline = *live.Slot(RowIndex("irradiance_default.upload"));

  CHECK(!irradiance_default::InstallAt(sentinel::pinned::LibR15CGEngineResourcesInitialize(), &NoLibr15));

  CHECK(*live.Slot(RowIndex("irradiance_default.upload")) == trampoline);
  CHECK(RowLine("irradiance_default", "irradiance_default.upload") == nullptr);  // the row's install was never reached
  const std::string* line = FindLine("\"hook\":\"CGEngineResources::Initialize\"");
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK(Has(*line, "\"level\":\"error\""));
    CHECK(!Has(*line, "\"status\":\"installed\""));
    CHECK(Has(*line, "\"symbol\":\"_ZN10NRadEngine17CGEngineResources10InitializeEv\""));
  }

  // The Initialize handler is armed before the slot is tried, so a slot that does take the entry finds a live handler: an Initialize
  // run through the entry (its original published by hand, as the GOT install would) writes its outcome line.
  irradiance_default::ResetForTest();
  irradiance_default::UploadThunk::Reset();  // an earlier test left the row's trampoline (AArch64 code) as its original
  *irradiance_default::InitializeThunk::OriginalOut() = reinterpret_cast<void*>(&IrradianceStandInInitialize);
  g.lines.clear();
  CHECK_EQ(reinterpret_cast<irradiance_default::InitializeThunk::Fn>(irradiance_default::InitializeThunk::EntryAddress())(g_irradianceResources), 0);
  CHECK(FindLine("\"event\":\"irradiance_default\"") != nullptr);
  irradiance_default::InitializeThunk::Reset();
  irradiance_default::UploadThunk::Reset();
}

}  // namespace

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
