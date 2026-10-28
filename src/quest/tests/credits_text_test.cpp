// Host test for the Quest credits install (sentinel/credits_text.h, #970; docs/adr/0021 section 8, test 6): Install on a
// libr15 that Attach verified builds `block + the game's own bytes + NUL` in anonymous memory, makes it read-only and stores
// its address in the credits row's slot with one atomic store; every refusal leaves the slot as Attach set it.
//
// The image is libr15_live_image.h's: the shipped patcher's output for a synthetic stock libr15, verified by Attach. The two
// ADRP+LDR pairs that read the slot execute AArch64 and are not run here; their words are the patcher's and Attach's
// (libr15_patcher_test, libr15_attach_test).
//
// Run: credits_text_test [name-filter]

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "credits_text.h"
#include "libr15_attach.h"
#include "libr15_live_image.h"
#include "mini_test.h"
#include "quest/data/sha256.h"
#include "runtime/patch/credits_block.h"
#include "runtime/patch/credits_text.h"

NEVR_LIVE_IMAGE_MPROTECT_OVERRIDE()

// The block the build renders from config/credits.json is linked into the sentinel (tools/render_credits.py checks it at
// build time); this test links its own two-line one, so the default path is exercised against a known value.
namespace nevr_credits_block {
extern const char kText[] = "Linked block one\nLinked block two\n";
extern const std::size_t kSize = sizeof(kText) - 1;
}  // namespace nevr_credits_block

namespace {

using namespace live_fixture;
using namespace nevr_quest;
using libr15_patch::AttachStatus;

constexpr const char kBlock[] = "Project line one\nProject line two\n";
constexpr std::size_t kBlockLen = sizeof(kBlock) - 1;

credits_text::InstallOptions Options(const char* block = kBlock, std::size_t blockLen = kBlockLen) {
  credits_text::InstallOptions o;
  o.lookup = &Lookup;
  o.block = block;
  o.blockSize = blockLen;
  o.map = &RecordingMap;
  o.protect = &RecordingProtect;
  return o;
}

std::size_t CreditsRow() {
  for (std::size_t i = 0; i < patch::kRowCount; ++i) {
    if (std::strcmp(patch::kRows[i].id, "credits.pairs") == 0) return i;
  }
  CHECK(false);
  return patch::kRowCount;
}

void AttachVerified(LiveImage& live) {
  g_live = &live;
  CHECK_EQ(libr15_patch::Attach(Opts()).status, AttachStatus::kVerified);
  g.lines.clear();
}

// Puts `game` (without a terminator) and a NUL at the game's blob address in the live image.
void PlantBlob(const LiveImage& live, const std::string& game) {
  std::memcpy(reinterpret_cast<void*>(live.Base() + credits_text::kBlobVaddr), game.c_str(), game.size() + 1);
}

// The slot as Attach left it: the game's own blob.
void* StockBlobAddress(const LiveImage& live) { return reinterpret_cast<void*>(live.Base() + credits_text::kBlobVaddr); }

const std::string* CreditsLine() { return FindLine("\"event\":\"credits_text\""); }

void ExpectRefusal(const char* reason) {
  const std::string* line = CreditsLine();
  CHECK(line != nullptr);
  if (line == nullptr) return;
  CHECK(Has(*line, "\"status\":\"skipped\""));
  CHECK(Has(*line, std::string("\"reason\":\"") + reason + "\""));
  CHECK(!Has(*line, "\"status\":\"applied\""));
}

// ---- the slot accessor ----------------------------------------------------------------------------------------------------

TEST(TheCreditsSlotIsTheCreditsRowsSlotOnlyOnceAttachVerified) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  CHECK(libr15_patch::CreditsSlot() == nullptr);  // before Attach
  CHECK_EQ(libr15_patch::Attach(Opts()).status, AttachStatus::kVerified);
  CHECK(libr15_patch::CreditsSlot() == live.Slot(CreditsRow()));
  libr15_patch::ResetForTest();
  CHECK(libr15_patch::CreditsSlot() == nullptr);  // a forgotten record names no slot
}

// ---- the install --------------------------------------------------------------------------------------------------------

TEST(InstallStoresBlockThenTheGamesBytesThenNulInAReadOnlyBufferAndPointsTheSlotAtIt) {
  Env env;
  LiveImage live(PatchedBytes());
  AttachVerified(live);
  const std::string game = "Game credits one\nGame credits two\nGame credits three\n";
  PlantBlob(live, game);
  void** const slot = live.Slot(CreditsRow());
  CHECK(*slot == StockBlobAddress(live));  // Attach's value: the game's own blob
  const std::vector<std::string> mapsBefore = live.MapsLines();
  const std::size_t mappedBefore = g.mapped.size();

  CHECK(credits_text::InstallCreditsText(Options()));

  void* const buffer = *slot;
  CHECK(buffer != StockBlobAddress(live));
  CHECK_EQ(g.mapped.size(), mappedBefore + 1);
  CHECK(buffer == g.mapped.back());
  CHECK(!live.Intersects(buffer, 1));  // anonymous memory outside the image
  const std::string expected = std::string(kBlock) + game + std::string(1, '\0');
  CHECK(std::memcmp(buffer, expected.data(), expected.size()) == 0);
  // The buffer ends where it should: the NUL after the game's bytes, and the length the line reports.
  CHECK_EQ(std::strlen(static_cast<const char*>(buffer)), kBlockLen + game.size());
  char perms[5];
  CHECK(sentinel::MappingPermsOf(reinterpret_cast<std::uintptr_t>(buffer), perms));
  CHECK_EQ(std::string(perms), std::string("r--p"));

  const std::string* line = CreditsLine();
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK(Has(*line, "\"status\":\"applied\""));
    CHECK(Has(*line, "\"block_lines\":2"));
    CHECK(Has(*line, "\"block_bytes\":" + std::to_string(kBlockLen)));
    CHECK(Has(*line, "\"game_bytes\":" + std::to_string(game.size())));
    CHECK(Has(*line, "\"total_bytes\":" + std::to_string(expected.size())));
    CHECK(Has(*line, "\"game_sha256\":\"" + data::Sha256Hex(game.data(), game.size()) + "\""));
    CHECK(Has(*line, "\"buffer\":\"" + Hex(reinterpret_cast<std::uintptr_t>(buffer)) + "\""));
    CHECK(Has(*line, "\"slot\":\"" + Hex(reinterpret_cast<std::uintptr_t>(slot)) + "\""));
  }
  // No code and no protection of the image: the one protection change is the buffer's own, through the seam.
  ExpectNoImageProtection(live);
  CHECK(g.protects.back().addr == buffer && g.protects.back().prot == PROT_READ);
  CHECK(live.MapsLines() == mapsBefore);
}

TEST(TheGamesBytesAreCopiedVerbatimWhateverTheyHold) {
  Env env;
  LiveImage live(PatchedBytes());
  AttachVerified(live);
  std::string game;
  for (int c = 1; c < 256; ++c) game.push_back(static_cast<char>(c));  // every byte but the terminator
  PlantBlob(live, game);

  CHECK(credits_text::InstallCreditsText(Options()));

  const std::string expected = std::string(kBlock) + game + std::string(1, '\0');
  CHECK(std::memcmp(*live.Slot(CreditsRow()), expected.data(), expected.size()) == 0);
}

TEST(AnEmptyGameBlobLeavesTheBlockAndTheNul) {
  Env env;
  LiveImage live(PatchedBytes());
  AttachVerified(live);
  PlantBlob(live, "");

  CHECK(credits_text::InstallCreditsText(Options()));

  const std::string expected = std::string(kBlock) + std::string(1, '\0');
  CHECK(std::memcmp(*live.Slot(CreditsRow()), expected.data(), expected.size()) == 0);
  const std::string* line = CreditsLine();
  CHECK(line != nullptr);
  if (line != nullptr) CHECK(Has(*line, "\"game_bytes\":0"));
}

TEST(TheLinkedBlockIsTheDefaultWhenNoBlockIsGiven) {
  Env env;
  LiveImage live(PatchedBytes());
  AttachVerified(live);
  PlantBlob(live, "Game\n");

  credits_text::InstallOptions o = Options();
  o.block = nullptr;
  o.blockSize = 0;
  CHECK(credits_text::InstallCreditsText(o));

  const std::string expected = std::string(nevr_credits_block::kText, nevr_credits_block::kSize) + "Game\n" + std::string(1, '\0');
  CHECK(std::memcmp(*live.Slot(CreditsRow()), expected.data(), expected.size()) == 0);
}

// ---- every refusal leaves the game's text ---------------------------------------------------------------------------------

TEST(AnInstallBeforeAttachIsRefusedAsNotVerifiedAndMapsNothing) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  live.FillCanaries();
  PlantBlob(live, "Game\n");

  CHECK(!credits_text::InstallCreditsText(Options()));

  ExpectRefusal("not_verified");
  CHECK(live.SlotsAreCanaries());
  CHECK_EQ(g.mapCalls, 0);
  CHECK(g.protects.empty());
}

TEST(AnInstallOnTheStockLibr15IsRefusedAsNotPatchedAndMapsNothing) {
  Env env;
  LiveImage live(Stock());
  g_live = &live;
  PlantBlob(live, "Game\n");

  CHECK(!credits_text::InstallCreditsText(Options()));

  ExpectRefusal(libr15_patch::kReasonNotPatched);
  CHECK_EQ(g.mapCalls, 0);
  CHECK(g.protects.empty());
}

TEST(AnInstallWithNoLibr15LoadedIsRefusedAndMapsNothing) {
  Env env;
  CHECK(!credits_text::InstallCreditsText(Options()));
  ExpectRefusal("module_not_loaded");
}

TEST(ABlockTheRuntimeGuardRefusesLeavesTheGamesOwnBlobInTheSlot) {
  struct Case {
    const char* name;
    std::string block;
    const char* reason;
  };
  const Case cases[] = {
      {"empty", "", "empty_block"},
      {"no final newline", "Project line", "block_not_terminated"},
      {"column separator", "Left|Right\n", "block_forbidden_byte"},
      {"carriage return", "Project\r\n", "block_forbidden_byte"},
      {"line over 64 bytes", std::string(65, 'x') + "\n", "block_line_too_long"},
      {"more than 200 lines", std::string(201, '\n'), "block_too_many_lines"},
  };
  for (const Case& c : cases) {
    Env env;
    LiveImage live(PatchedBytes());
    AttachVerified(live);
    PlantBlob(live, "Game\n");
    const std::size_t mappedBefore = g.mapped.size();
    const std::size_t protectsBefore = g.protects.size();

    CHECK(!credits_text::InstallCreditsText(Options(c.block.data(), c.block.size())));

    std::fprintf(stderr, "  case: %s\n", c.name);
    ExpectRefusal(c.reason);
    CHECK(*live.Slot(CreditsRow()) == StockBlobAddress(live));
    CHECK_EQ(g.mapped.size(), mappedBefore);
    CHECK_EQ(g.protects.size(), protectsBefore);
  }
}

TEST(AGameBlobWithNoTerminatorInSixtyFourKilobytesIsRefusedAndMapsNothing) {
  Env env;
  LiveImage live(PatchedBytes());
  AttachVerified(live);
  std::memset(reinterpret_cast<void*>(live.Base() + credits_text::kBlobVaddr), 'A', 64 * 1024);
  const std::size_t mappedBefore = g.mapped.size();

  CHECK(!credits_text::InstallCreditsText(Options()));

  ExpectRefusal("blob_not_terminated");
  CHECK(*live.Slot(CreditsRow()) == StockBlobAddress(live));
  CHECK_EQ(g.mapped.size(), mappedBefore);
}

TEST(AFailedBufferMapLeavesTheSlotAndLogsIt) {
  Env env;
  LiveImage live(PatchedBytes());
  AttachVerified(live);
  PlantBlob(live, "Game\n");
  g.mapFailAt = g.mapCalls;  // the next map call, the buffer's

  CHECK(!credits_text::InstallCreditsText(Options()));

  ExpectRefusal("buffer_map_failed");
  CHECK(*live.Slot(CreditsRow()) == StockBlobAddress(live));
}

TEST(AFailedBufferProtectUnmapsTheBufferAndLeavesTheSlot) {
  Env env;
  LiveImage live(PatchedBytes());
  AttachVerified(live);
  PlantBlob(live, "Game\n");
  g.protectFailAt = static_cast<int>(g.protects.size());  // the next protect call, the buffer's
  const std::size_t mappedBefore = g.mapped.size();

  CHECK(!credits_text::InstallCreditsText(Options()));

  ExpectRefusal("buffer_protect_failed");
  CHECK(*live.Slot(CreditsRow()) == StockBlobAddress(live));
  CHECK_EQ(g.mapped.size(), mappedBefore + 1);
  CHECK(UnmappedNow(g.mapped.back()));  // not left behind
}

}  // namespace

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
