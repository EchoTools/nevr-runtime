// Host test for the sentinel's run-time half of the libr15 build-time patch (src/quest/sentinel/libr15_attach.h),
// ADR 0021 test 4 and the Attach half of test 9.
//
// The image under test is libr15_live_image.h's: a synthetic STOCK libr15 at the real table's addresses, the SHIPPED
// patcher (PatchLibr15) run on it, the patched file laid out in host memory (text pages r-x, so a stray store into code
// would fault) and handed to Attach through the lookup seam. That ties the patcher's output to the sentinel's checks.
//
// What is not run here: the trampolines and the stubs execute AArch64; this host is x86-64, so their bytes are
// compared, not run (arm64_reloc_test runs the relocator's sequences on an interpreter).
//
// Run: libr15_attach_test <fixtures dir> [name-filter]

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "arm64_reloc.h"
#include "credits_text.h"
#include "hook_install.h"
#include "libr15_attach.h"
#include "libr15_identity.h"
#include "libr15_live_image.h"
#include "mini_test.h"
#include "pinned_targets.h"
#include "process_end_hook.h"
#include "quest/data/sha256.h"
#include "quest/patch/stub_codec.h"

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
using libr15_patch::ActivateStatus;
using libr15_patch::AttachResult;
using libr15_patch::AttachStatus;
using libr15_patch::AttachTestAccess;

// ---- the happy path ---------------------------------------------------------------------------------------------------

TEST(TheShippedPatchersOutputIsVerifiedAndEverySlotHoldsItsTrampoline) {
  Env env;
  const std::vector<std::uint8_t> file = PatchedBytes();
  LiveImage live(file);
  g_live = &live;
  live.FillCanaries();
  const std::vector<std::string> mapsBefore = live.MapsLines();

  const AttachResult r = libr15_patch::Attach(Opts());
  CHECK_EQ(r.status, AttachStatus::kVerified);
  CHECK(libr15_patch::Verified());
  CHECK(libr15_patch::VerifiedBuildId() != nullptr);
  CHECK(std::string(libr15_patch::VerifiedBuildId()) == patch::PatchedBuildId());
  CHECK(g.ends.empty());

  std::set<void*> distinct;
  for (std::size_t i = 0; i < patch::kRowCount; ++i) {
    void* const value = *live.Slot(i);
    if (patch::kRows[i].kind == patch::RowKind::kInlineHook) {
      CHECK(value != nullptr && value != LiveImage::Canary(i));
      CHECK(!live.Intersects(value, 1));  // a trampoline is its own anonymous page, not a page of the image
      distinct.insert(value);
    } else {
      CHECK(value == reinterpret_cast<void*>(live.Base() + credits_text::kBlobVaddr));  // the game's own blob
    }
  }
  CHECK_EQ(distinct.size(), 5u);

  // One verified line, once.
  CHECK_EQ(CountLines("\"event\":\"libr15_patch\""), 1);
  const std::string* line = FindLine("\"status\":\"verified\"");
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK(Has(*line, "\"build_id\":\"" + patch::PatchedBuildId() + "\""));
    CHECK(Has(*line, std::string("\"stock_build_id\":\"") + patch::kLibR15StockBuildId + "\""));
    CHECK(Has(*line, "\"rows\":6"));
    CHECK(Has(*line, "\"slot_area\":\"" + Hex(patch::kSlotAreaVaddr) + "\""));
    CHECK(!Has(*line, "extra_rows"));
  }

  // Data only: the only protection calls are the five trampoline pages (RW to RX), none on the image.
  CHECK_EQ(g.protects.size(), 5u);
  for (const ProtectCall& c : g.protects) CHECK_EQ(c.prot, PROT_READ | PROT_EXEC);
  ExpectNoImageProtection(live);
  CHECK_EQ(g.mapCalls, 5);
  CHECK(live.MapsLines() == mapsBefore);
}

TEST(TheTrampolinesAreTheTablesStockWordsRelocatedForTheirSitesAndEndAtSitePlus12) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  CHECK_EQ(libr15_patch::Attach(Opts()).status, AttachStatus::kVerified);
  for (std::size_t i = 0; i < patch::kRowCount; ++i) {
    const patch::Row& row = patch::kRows[i];
    if (row.kind != patch::RowKind::kInlineHook) continue;
    const std::uint32_t window[3] = {row.sites[0].words[0], row.sites[0].words[1], row.sites[0].words[2]};
    const std::uint64_t site = live.Base() + row.sites[0].Vaddr();
    sentinel::Trampoline want;
    sentinel::RelocRefusal refusal{};
    CHECK(sentinel::BuildTrampoline(site, window, want, refusal));
    const std::uint32_t* got = static_cast<const std::uint32_t*>(*live.Slot(i));
    bool same = true;
    for (std::size_t w = 0; w < want.count; ++w) same = same && got[w] == want.words[w];
    CHECK(same);
    // The three displaced words are not PC-relative in this table, so they are copied unchanged.
    CHECK_EQ(got[0], row.sites[0].words[0]);
    CHECK_EQ(got[1], row.sites[0].words[1]);
    CHECK_EQ(got[2], row.sites[0].words[2]);
    // The continue address, site + 12, sits in the literal pool.
    bool continues = false;
    for (std::size_t w = 0; w + 1 < want.count; w += 2) {
      std::uint64_t quad = 0;
      std::memcpy(&quad, got + w, sizeof quad);
      continues = continues || quad == site + 12;
    }
    CHECK(continues);
    char perms[5] = {};
    CHECK(sentinel::MappingPermsOf(reinterpret_cast<std::uintptr_t>(got), perms));
    CHECK_EQ(std::string(perms), std::string("r-xp"));
  }
}

TEST(TheCreditsSlotValueIsTheBlobTheStockPairsLoad) {
  // Attach stores base + credits_text::kBlobVaddr; that is what the replaced ADRP+ADD pairs computed.
  const patch::Row* credits = nullptr;
  for (const patch::Row& row : patch::kRows) {
    if (row.kind == patch::RowKind::kCreditsPair) credits = &row;
  }
  CHECK(credits != nullptr);
  if (credits == nullptr) return;
  for (std::size_t pair = 0; pair < 2; ++pair) {
    const patch::Site& adrp = credits->sites[2 * pair];
    const patch::Site& add = credits->sites[2 * pair + 1];
    unsigned rd = 0;
    std::uint64_t page = 0;
    CHECK(patch::DecodeAdrp(adrp.words[0], adrp.Vaddr(), &rd, &page));
    CHECK_EQ(page + ((add.words[0] >> 10) & 0xFFFu), credits_text::kBlobVaddr);
  }
}

TEST(TheIdTheSentinelsConsumersExpectIsTheOneTheShippedPatcherWrote) {
  // sentinel::pinned::LibR15PatchedBuildId() is what every GotTarget, the hwdump and the integration compare the live note
  // with; the patcher's output is what the live note is. Attach reads the note of the patcher's output, so the two meet here.
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  const char* const expected = sentinel::pinned::LibR15PatchedBuildId();
  CHECK_EQ(std::strlen(expected), 40u);
  CHECK(std::strcmp(expected, sentinel::pinned::kLibR15StockBuildId) != 0);  // the patched file's ID is not the store file's
  CHECK(expected == sentinel::pinned::LibR15PatchedBuildId());               // computed once: a stable pointer
  CHECK(sentinel::pinned::LibR15ExtraRows().empty());                       // the public build carries no extra rows
  CHECK_EQ(libr15_patch::Attach(Opts()).status, AttachStatus::kVerified);
  CHECK(libr15_patch::VerifiedBuildId() != nullptr);
  if (libr15_patch::VerifiedBuildId() != nullptr) CHECK_EQ(std::string(libr15_patch::VerifiedBuildId()), std::string(expected));
}

// ---- identity: the refusals of ADR section 7 --------------------------------------------------------------------------

void ExpectNothingInstalled(const LiveImage& live, const char* reason) {
  CHECK(!libr15_patch::Verified());
  CHECK(libr15_patch::VerifiedBuildId() == nullptr);
  CHECK(live.SlotsAreCanaries());
  CHECK(g.ends.empty());  // the game runs as it came: the process is not ended
  CHECK(g.protects.empty());
  CHECK(g_rawProtects.empty());
  CHECK_EQ(g.mapCalls, 0);
  const std::string* line = FindLine(reason);
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK(Has(*line, "\"event\":\"libr15_patch\""));
    CHECK(Has(*line, "\"level\":\"error\""));
  }
  CHECK_EQ(CountLines("\"status\":\"verified\""), 0);
}

TEST(TheStockLibr15IsNotPatchedAndNothingIsInstalled) {
  Env env;
  LiveImage live(Stock());
  g_live = &live;
  live.FillCanaries();
  const AttachResult r = libr15_patch::Attach(Opts());
  CHECK_EQ(r.status, AttachStatus::kNotPatched);
  CHECK(r.reason == "libr15_not_patched");
  ExpectNothingInstalled(live, "\"reason\":\"libr15_not_patched\"");
}

TEST(AnotherBuildIdIsABuildIdMismatch) {
  Env env;
  std::vector<std::uint8_t> file = PatchedBytes();
  file[FileOffsetOf(file, kNoteDescVaddr)] ^= 0x01;  // a patched ID with one bit changed
  LiveImage live(file);
  g_live = &live;
  live.FillCanaries();
  const AttachResult r = libr15_patch::Attach(Opts());
  CHECK_EQ(r.status, AttachStatus::kBuildIdMismatch);
  CHECK(r.reason == "build_id_mismatch");
  ExpectNothingInstalled(live, "\"reason\":\"build_id_mismatch\"");
}

// ---- #1737: the one pinned libr15 that is run in service-only mode ----------------------------------------------------

constexpr const char* kPinned = "6e9db28d612019b0e5b2e2ab515e1f9bd15d1a8f8d345060160e3172864540e1";

bool PinnedDigest(const char*, char* out) noexcept {
  std::memcpy(out, kPinned, 65);
  return true;
}

bool OneBitOffDigest(const char*, char* out) noexcept {
  std::memcpy(out, kPinned, 65);
  out[63] = '0';  // ...e1 -> ...e0: one bit
  return true;
}

TEST(ThePinnedStockIdFileIsAcceptedInServiceOnlyModeAndNothingIsInstalled) {
  Env env;
  LiveImage live(Stock());
  g_live = &live;
  live.FillCanaries();
  libr15_patch::AttachOptions o = Opts();
  o.digest = &PinnedDigest;
  const AttachResult r = libr15_patch::Attach(o);
  CHECK_EQ(r.status, AttachStatus::kPinnedServiceOnly);
  CHECK(r.reason == "libr15_pinned_service_only");
  CHECK(!libr15_patch::Verified());
  CHECK(live.SlotsAreCanaries());
  CHECK(g.ends.empty());
  CHECK_EQ(g.mapCalls, 0);
  const std::string* line = FindLine("\"event\":\"libr15_pinned_accept\"");
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK(Has(*line, "\"sha256_prefix\":\"6e9db28d6120\""));
    CHECK(Has(*line, "\"mode\":\"service_only\""));
    CHECK(Has(*line, "\"build_id\":\"b243509c08ce677aeb95fa348016949b3fc45230\""));
  }
  CHECK_EQ(CountLines("\"reason\":\"libr15_not_patched\""), 0);
  sentinel::SetServiceSideBuildId(nullptr);
}

TEST(AStockIdFileOneBitOffThePinIsNotPatched) {
  Env env;
  LiveImage live(Stock());
  g_live = &live;
  live.FillCanaries();
  libr15_patch::AttachOptions o = Opts();
  o.digest = &OneBitOffDigest;
  const AttachResult r = libr15_patch::Attach(o);
  CHECK_EQ(r.status, AttachStatus::kNotPatched);
  CHECK_EQ(CountLines("libr15_pinned_accept"), 0);
  ExpectNothingInstalled(live, "\"reason\":\"libr15_not_patched\"");
}

TEST(ThePinListHasExactlyOneEntry) {
  CHECK_EQ(sizeof(libr15_patch::kLibR15ServiceOnlySha256) / sizeof(libr15_patch::kLibR15ServiceOnlySha256[0]), 1u);
  CHECK(std::strcmp(libr15_patch::kLibR15ServiceOnlySha256[0], kPinned) == 0);
}

TEST(ALibr15PatchedWithoutExtraRowsIsAMismatchForAnAttachThatHasThem) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  live.FillCanaries();
  AttachOptions o = Opts();
  o.extraRows = FixtureRows();
  const AttachResult r = libr15_patch::Attach(o);
  CHECK_EQ(r.status, AttachStatus::kBuildIdMismatch);
  ExpectNothingInstalled(live, "\"reason\":\"build_id_mismatch\"");
}

TEST(ALibr15PatchedWithExtraRowsIsAMismatchForAnAttachThatHasNone) {
  Env env;
  LiveImage live(PatchedBytes(FixtureRows()));
  g_live = &live;
  live.FillCanaries();
  const AttachResult r = libr15_patch::Attach(Opts());
  CHECK_EQ(r.status, AttachStatus::kBuildIdMismatch);
  ExpectNothingInstalled(live, "\"reason\":\"build_id_mismatch\"");
}

TEST(NoLibr15AtAllIsModuleNotLoaded) {
  Env env;
  g_live = nullptr;
  const AttachResult r = libr15_patch::Attach(Opts());
  CHECK_EQ(r.status, AttachStatus::kModuleNotLoaded);
  CHECK(!libr15_patch::Verified());
  CHECK(g.ends.empty());
  CHECK(FindLine("\"reason\":\"module_not_loaded\"") != nullptr);
}

TEST(ASecondAttachTouchesNothing) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  CHECK_EQ(libr15_patch::Attach(Opts()).status, AttachStatus::kVerified);
  void* const slot0 = *live.Slot(0);
  const std::size_t protects = g.protects.size();
  CHECK_EQ(libr15_patch::Attach(Opts()).status, AttachStatus::kAlreadyVerified);
  CHECK(*live.Slot(0) == slot0);
  CHECK_EQ(g.protects.size(), protects);
  CHECK_EQ(CountLines("\"status\":\"verified\""), 1);
}

// ---- the row checks ---------------------------------------------------------------------------------------------------

// Runs Attach on `file` (laid out with `skew`) and expects check `check` of row `rowId` to refuse with `reason`: every
// slot untouched, no trampoline made, the process-end call made once with the reason.
void ExpectRowRefusal(const std::vector<std::uint8_t>& file, const char* reason, const char* check, const char* rowId,
                      std::uintptr_t skew = 0, const std::vector<patch::WordPatchRow>& extra = {}) {
  Env env;
  LiveImage live(file, skew);
  g_live = &live;
  live.FillCanaries();
  AttachOptions o = Opts();
  o.extraRows = extra;
  const AttachResult r = libr15_patch::Attach(o);
  CHECK_EQ(r.status, AttachStatus::kRowFailed);
  CHECK(r.reason == reason);
  CHECK(r.rowId == rowId);
  CHECK(!libr15_patch::Verified());
  CHECK(live.SlotsAreCanaries());
  CHECK(g.protects.empty());
  CHECK(g_rawProtects.empty());
  CHECK_EQ(g.mapCalls, 0);
  CHECK_EQ(g.ends.size(), 1u);
  if (g.ends.size() == 1) {
    CHECK_EQ(g.ends[0].code, process_end::kFailureExitCode);
    CHECK(g.ends[0].reason == reason);
  }
  const std::string* line = FindLine(reason);
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK(Has(*line, "\"level\":\"error\""));
    CHECK(Has(*line, std::string("\"id\":\"") + rowId + "\""));
    CHECK(Has(*line, std::string("\"check\":\"") + check + "\""));
  }
  CHECK_EQ(CountLines("\"status\":\"verified\""), 0);
}

std::uint64_t SiteVaddr(std::size_t row, std::size_t site = 0) { return patch::kRows[row].sites[site].Vaddr(); }

TEST(CheckA_AStubWordThatIsNotTheTablesIsRefusedWithTheRowsId) {
  std::vector<std::uint8_t> file = PatchedBytes();
  PutWord(file, SiteVaddr(0) + 8, 0);  // the BR of lobby_full.join
  ExpectRowRefusal(file, "libr15_patch_row_stub", "stub", "lobby_full.join");
}

TEST(CheckA_AStockSiteIsRefused) {
  std::vector<std::uint8_t> file = PatchedBytes();
  for (std::size_t w = 0; w < 3; ++w) PutWord(file, SiteVaddr(1) + 4 * w, patch::kRows[1].sites[0].words[w]);
  ExpectRowRefusal(file, "libr15_patch_row_stub", "stub", "lobby_full.failure");
}

TEST(CheckA_AStubWithAnotherTablesSlotIsRefused) {
  std::vector<std::uint8_t> file = PatchedBytes();
  patch::Stub other{};
  CHECK(patch::EncodeSlotStub(SiteVaddr(2), patch::kRows[0].SlotVaddr(), &other));  // row 2's site, row 0's slot
  for (std::size_t w = 0; w < 3; ++w) PutWord(file, SiteVaddr(2) + 4 * w, other.words[w]);
  ExpectRowRefusal(file, "libr15_patch_row_stub", "stub", "lobby_full.success");
}

TEST(CheckA_ACreditsLdrThatIsStillTheStockAddIsRefused) {
  std::vector<std::uint8_t> file = PatchedBytes();
  const std::size_t credits = patch::kRowCount - 1;
  PutWord(file, SiteVaddr(credits, 3), patch::kRows[credits].sites[3].words[0]);  // the second pair's LDR back to ADD
  ExpectRowRefusal(file, "libr15_patch_row_stub", "stub", "credits.pairs");
}

TEST(CheckA_ACreditsAdrpOfTheFirstPairThatIsStockIsRefused) {
  std::vector<std::uint8_t> file = PatchedBytes();
  const std::size_t credits = patch::kRowCount - 1;
  PutWord(file, SiteVaddr(credits, 0), patch::kRows[credits].sites[0].words[0]);
  ExpectRowRefusal(file, "libr15_patch_row_stub", "stub", "credits.pairs");
}

TEST(CheckB_ADecodedSlotThatIsNotTheTablesIsRefused) {
  // The words are exactly the stub the table implies, so check (a) passes; but the stub is position independent only for
  // a page-aligned load bias. At a bias that is not, the live ADRP names another slot than base + the table's.
  ExpectRowRefusal(PatchedBytes(), "libr15_patch_row_slot", "slot", "lobby_full.join", 0x200);
}

TEST(CheckC_ASlotOutsideTheGrownSegmentIsRefused) {
  std::vector<std::uint8_t> file = PatchedBytes();
  const std::size_t d = DataLoadIndex(file);
  Elf64_Phdr p = ProgramHeaders(file)[d];
  p.p_memsz = patch::kStockImageEnd - p.p_vaddr;  // the stock segment: the patcher's growth missing
  SetProgramHeader(file, d, p);
  ExpectRowRefusal(file, "libr15_patch_row_segment", "segment", "lobby_full.join");
}

TEST(CheckC_TheCreditsSlotBeyondTheGrownSegmentIsRefused) {
  std::vector<std::uint8_t> file = PatchedBytes();
  const std::size_t d = DataLoadIndex(file);
  Elf64_Phdr p = ProgramHeaders(file)[d];
  // Slots 0 to 4 fit; only the last slot (the credits row's) ends past the segment.
  p.p_memsz = patch::kSlotAreaVaddr + 5 * 8 - p.p_vaddr;
  SetProgramHeader(file, d, p);
  ExpectRowRefusal(file, "libr15_patch_row_segment", "segment", "credits.pairs");
}

TEST(CheckC_ASlotInTheFileBackedPartIsRefused) {
  std::vector<std::uint8_t> file = PatchedBytes();
  const std::size_t d = DataLoadIndex(file);
  Elf64_Phdr p = ProgramHeaders(file)[d];
  p.p_filesz = p.p_memsz;  // no bss part: the slots would be file data
  SetProgramHeader(file, d, p);
  ExpectRowRefusal(file, "libr15_patch_row_segment", "segment", "lobby_full.join");
}

TEST(CheckC_ASlotInAReadOnlySegmentIsRefused) {
  std::vector<std::uint8_t> file = PatchedBytes();
  const std::size_t d = DataLoadIndex(file);
  Elf64_Phdr p = ProgramHeaders(file)[d];
  p.p_flags = PF_R;
  SetProgramHeader(file, d, p);
  ExpectRowRefusal(file, "libr15_patch_row_segment", "segment", "lobby_full.join");
}

TEST(CheckC_ASlotInsideRelroIsRefused) {
  std::vector<std::uint8_t> file = PatchedBytes();
  const std::size_t r = RelroIndex(file);
  Elf64_Phdr p = ProgramHeaders(file)[r];
  p.p_memsz = patch::kSlotAreaVaddr + 8 - p.p_vaddr;  // GNU_RELRO reaching the first slot
  SetProgramHeader(file, r, p);
  ExpectRowRefusal(file, "libr15_patch_row_segment", "segment", "lobby_full.join");
}

TEST(AWordPatchRowWhoseLiveWordDiffersIsRefusedAndNamesTheRow) {
  const std::vector<patch::WordPatchRow> rows = FixtureRows();
  std::vector<std::uint8_t> file = PatchedBytes(rows);
  const std::uint8_t stockBytes[4] = {0xe0, 0x03, 0x1f, 0xaa};
  std::uint32_t stockWord = 0;
  std::memcpy(&stockWord, stockBytes, 4);
  PutWord(file, kFixturePage + 0x10, stockWord);  // the replacement put back to the stock word
  ExpectRowRefusal(file, "libr15_patch_row_words", "words", "x.example_guard", 0, rows);
}

TEST(AWordPatchRowOutsideEveryLoadIsRefused) {
  std::vector<patch::WordPatchRow> rows = FixtureRows();
  rows[0].revaultVa = 0x7f000010;  // not in the image
  // The ID of a build with these rows differs from the file's, so build the file with the same rows.
  const patch::PatchSpec spec = Spec();
  // The patcher refuses a word outside the text, so the file cannot be made; Attach must refuse the same row on a
  // file whose build ID names it, which only a stage-1 defect could produce. Here the ID is forged to say so.
  const patch::BuildIdBytes forged = patch::DeriveBuildId(spec.stockBuildId, patch::CanonicalTable(spec.rows, spec.rowCount, rows, patch::kSlotAreaVaddr));
  std::vector<std::uint8_t> patched = PatchedBytes();
  std::memcpy(patched.data() + FileOffsetOf(patched, kNoteDescVaddr), forged.data(), forged.size());
  ExpectRowRefusal(patched, "libr15_patch_row_words", "words", "x.example_guard", 0, rows);
}

TEST(AnImageWithTheExtraRowsAppliedIsVerifiedAndTheLineNeverNamesTheRows) {
  Env env;
  const std::vector<patch::WordPatchRow> rows = FixtureRows();
  LiveImage live(PatchedBytes(rows));
  g_live = &live;
  AttachOptions o = Opts();
  o.extraRows = rows;
  const std::string fixture = Fixture();
  o.extraRowsSha256 = data::Sha256Hex(fixture.data(), fixture.size());
  CHECK_EQ(libr15_patch::Attach(o).status, AttachStatus::kVerified);
  CHECK(std::string(libr15_patch::VerifiedBuildId()) == patch::PatchedBuildId(rows));
  CHECK(patch::PatchedBuildId(rows) != patch::PatchedBuildId());
  const std::string* line = FindLine("\"status\":\"verified\"");
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK(Has(*line, "\"extra_rows\":1"));
    CHECK(Has(*line, "\"extra_rows_sha256\":\"" + o.extraRowsSha256 + "\""));
    CHECK(!Has(*line, "x.example_guard"));
    CHECK(!Has(*line, "1500010"));
  }
}

// ---- the trampolines ----------------------------------------------------------------------------------------------------

void ExpectTrampolineRefusal(const char* op, int errnoValue, std::size_t failingRow) {
  LiveImage live(PatchedBytes());
  g_live = &live;
  live.FillCanaries();
  const AttachResult r = libr15_patch::Attach(Opts());
  CHECK_EQ(r.status, AttachStatus::kTrampolineFailed);
  CHECK(r.reason == "libr15_patch_trampoline_failed");
  CHECK(r.rowId == patch::kRows[failingRow].id);
  CHECK(!libr15_patch::Verified());
  CHECK(live.SlotsAreCanaries());  // nothing activated: no slot holds a trampoline
  CHECK_EQ(g.ends.size(), 1u);
  if (g.ends.size() == 1) {
    CHECK_EQ(g.ends[0].code, process_end::kFailureExitCode);
    CHECK(g.ends[0].reason == "libr15_patch_trampoline_failed");
  }
  const std::string* line = FindLine("\"reason\":\"libr15_patch_trampoline_failed\"");
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK(Has(*line, std::string("\"op\":\"") + op + "\""));
    CHECK(Has(*line, "\"errno\":" + std::to_string(errnoValue)));
    CHECK(Has(*line, "\"page_perms\":\"r-xp\""));  // the site's page, read from /proc/self/maps
    CHECK(Has(*line, std::string("\"id\":\"") + patch::kRows[failingRow].id + "\""));
  }
  for (void* page : g.mapped) CHECK(UnmappedNow(page));  // every page made so far is given back
  CHECK_EQ(CountLines("\"status\":\"verified\""), 0);
}

TEST(AFailedTrampolineMapEndsTheProcessWithItsErrnoAndActivatesNothing) {
  Env env;
  g.mapFailAt = 2;
  ExpectTrampolineRefusal("trampoline_map", ENOMEM, 2);
  CHECK_EQ(g.mapped.size(), 2u);  // the two before it were made
}

TEST(AFailedTrampolineProtectEndsTheProcessWithItsErrnoAndActivatesNothing) {
  Env env;
  g.protectFailAt = 1;
  ExpectTrampolineRefusal("trampoline_rx", EACCES, 1);
  CHECK_EQ(g.mapped.size(), 2u);
}

// ---- activation -----------------------------------------------------------------------------------------------------------

void DummyDetour() {}

TEST(ActivationPublishesTheTrampolineThenStoresTheDetourAndTouchesNoPage) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  CHECK_EQ(libr15_patch::Attach(Opts()).status, AttachStatus::kVerified);
  const std::vector<std::string> mapsBefore = live.MapsLines();
  const std::size_t protectsBefore = g.protects.size();
  void* const trampoline = *live.Slot(0);
  void* original = reinterpret_cast<void*>(0x1234);
  void* const detour = reinterpret_cast<void*>(&DummyDetour);
  g.lines.clear();
  CHECK_EQ(AttachTestAccess::Activate("lobby_full.join", detour, &original), ActivateStatus::kOk);
  CHECK(*live.Slot(0) == detour);
  CHECK(original == trampoline);
  const std::string* line = FindLine("\"event\":\"inline_hook\"");
  CHECK(line != nullptr);
  if (line != nullptr) {
    CHECK(Has(*line, "\"status\":\"ok\""));
    CHECK(Has(*line, "\"id\":\"lobby_full.join\""));
    CHECK(Has(*line, "\"target\":\"" + Hex(live.Base() + SiteVaddr(0)) + "\""));
    CHECK(Has(*line, "\"slot\":\"" + Hex(reinterpret_cast<std::uintptr_t>(live.Slot(0))) + "\""));
    CHECK(Has(*line, "\"trampoline\":\"" + Hex(reinterpret_cast<std::uintptr_t>(trampoline)) + "\""));
    CHECK(Has(*line, "\"page_perms\":\"r-xp\""));
  }
  CHECK_EQ(g.protects.size(), protectsBefore);
  ExpectNoImageProtection(live);
  CHECK(live.MapsLines() == mapsBefore);
}

TEST(ActivatingEveryInlineRowNeverCallsTheProtectionSeamOnTheImage) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  const std::vector<std::string> mapsBefore = live.MapsLines();
  CHECK_EQ(libr15_patch::Attach(Opts()).status, AttachStatus::kVerified);
  for (const patch::Row& row : patch::kRows) {
    if (row.kind != patch::RowKind::kInlineHook) continue;
    void* original = nullptr;
    CHECK_EQ(AttachTestAccess::Activate(row.id, reinterpret_cast<void*>(&DummyDetour), &original), ActivateStatus::kOk);
  }
  // Zero calls whose range intersects the image's mapping: the test that fails if a code write comes back.
  ExpectNoImageProtection(live);
  CHECK_EQ(g.protects.size(), 5u);
  CHECK(live.MapsLines() == mapsBefore);
  for (std::size_t i = 0; i < patch::kRowCount; ++i) {
    if (patch::kRows[i].kind == patch::RowKind::kInlineHook) CHECK(*live.Slot(i) == reinterpret_cast<void*>(&DummyDetour));
  }
}

TEST(ASecondActivationOfARowIsRefusedAndChangesNothing) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  CHECK_EQ(libr15_patch::Attach(Opts()).status, AttachStatus::kVerified);
  void* original = nullptr;
  void* const first = reinterpret_cast<void*>(&DummyDetour);
  CHECK_EQ(AttachTestAccess::Activate("lobby_full.failure", first, &original), ActivateStatus::kOk);
  void* const trampoline = original;
  void* other = reinterpret_cast<void*>(0x4242);
  void* nonce = reinterpret_cast<void*>(0x77);
  CHECK_EQ(AttachTestAccess::Activate("lobby_full.failure", other, &nonce), ActivateStatus::kAlreadyActivated);
  CHECK(*live.Slot(1) == first);
  CHECK(nonce == reinterpret_cast<void*>(0x77));
  CHECK(trampoline != nullptr);
}

TEST(ActivationBeforeAttachIsRefusedAndStoresNothing) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  live.FillCanaries();
  void* original = nullptr;
  CHECK_EQ(AttachTestAccess::Activate("lobby_full.join", reinterpret_cast<void*>(&DummyDetour), &original), ActivateStatus::kNotVerified);
  CHECK(original == nullptr);
  CHECK(live.SlotsAreCanaries());
  CHECK(FindLine("\"status\":\"not_verified\"") != nullptr);
}

TEST(ActivationRefusesAnUnknownRowTheCreditsRowAndNullArguments) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  CHECK_EQ(libr15_patch::Attach(Opts()).status, AttachStatus::kVerified);
  void* original = nullptr;
  void* const detour = reinterpret_cast<void*>(&DummyDetour);
  void* const creditsSlot = *live.Slot(patch::kRowCount - 1);
  CHECK_EQ(AttachTestAccess::Activate("no.such_row", detour, &original), ActivateStatus::kUnknownRow);
  CHECK_EQ(AttachTestAccess::Activate("credits.pairs", detour, &original), ActivateStatus::kNotInlineRow);
  CHECK(*live.Slot(patch::kRowCount - 1) == creditsSlot);
  CHECK_EQ(AttachTestAccess::Activate(nullptr, detour, &original), ActivateStatus::kBadArgument);
  CHECK_EQ(AttachTestAccess::Activate("lobby_full.join", nullptr, &original), ActivateStatus::kBadArgument);
  CHECK_EQ(AttachTestAccess::Activate("lobby_full.join", detour, nullptr), ActivateStatus::kBadArgument);
  CHECK(original == nullptr);
}

struct RowTestTag {};
using RowTestThunk = sentinel::CallbackThunk<RowTestTag, int(int)>;

TEST(InstallRowThunkStoresTheThunksEntryAndPublishesTheTrampolineAsItsOriginal) {
  Env env;
  LiveImage live(PatchedBytes());
  g_live = &live;
  CHECK_EQ(libr15_patch::Attach(Opts()).status, AttachStatus::kVerified);
  void* const trampoline = *live.Slot(3);
  CHECK_EQ(sentinel::InstallRowThunk<RowTestThunk>("ui_texture.attach"), ActivateStatus::kOk);
  CHECK(*live.Slot(3) == RowTestThunk::EntryAddress());
  CHECK(*RowTestThunk::OriginalOut() == trampoline);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: libr15_attach_test <fixtures dir> [name-filter]\n");
    return 2;
  }
  g_fixtures = argv[1];
  return mini_test::RunAll(argc - 1, argv + 1);
}
