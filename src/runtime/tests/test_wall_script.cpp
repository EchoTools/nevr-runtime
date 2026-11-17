// #2242 (H-WALL-a): the barrier wall's veil decision and renew rule (runtime/combat/wall_script.h). The decision is pure, so
// the tests drive it on buffers: the veil token block (count, expiry, shown, barriers), the wall's cell (one dword: the
// decision in its low byte, the renew clock above it) and the checked global-length patch at 0x5CC0. The patch runs on a
// fake script image that holds the stock bytes; nothing is executed and no game image is read.
//
// NOT shown by these tests: a wall that takes a veil token on screen (its show site, 0x6D78, is not installed; see
// wall_script.h), the renew stub's effect in a match, and the veil token's +10 / +14 counts seen by VeilNet on another
// machine. No live run was made.

#include <winsock2.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/logging.h"
#include "runtime/combat/wall_script.h"

namespace wall = nevr_combat::wall_script;
namespace checked = nevr_checked_patch;

namespace {

// Values pinned here, each cited to the file that holds it. A change to one of them fails a test, not a run.
constexpr std::uint64_t kExpectedScript = 0xac4e9374a146d3a4ULL;  // the wall script, ac4e9374a146d3a4.dll (symcache_data.cpp:57032)
constexpr std::uint32_t kExpectedVeilTokenRva = 0x2104CC0;        // the EC page's veil token block (ec_rules.h kVeilTok)
constexpr std::uint32_t kExpectedGlobalLenFnRva = 0x5CC0;         // script_get_global_data_len (veil_fx.py:140)
constexpr std::uint32_t kExpectedRenewAfterUnits = 76;            // 8 s in 2^20 x 100 ns units (veil_fx.py:802)
constexpr std::uint32_t kExpectedRenewAfterPhysUnits = 133;       // 14 s in the same units (veil_fx.py:807)
constexpr std::uint64_t kUnitTicks = 1ULL << 20;                  // one renew unit, in 100 ns ticks
constexpr std::uint32_t kPointStateOffset = 0x15C;                // POINT_STATE = GLOBAL_LEN + 4 (veil_fx.py:576): H-WALL-b's cell
constexpr std::uint32_t kStockGlobalLen = 0x158;                  // `mov eax, 158h` (veil_fx.py:136)
constexpr std::uint32_t kWriteErrorCode = 5;                      // the Win32 error the failing fake writers report
constexpr std::size_t kImageBytes = 0x6000;
constexpr std::size_t kRecordCap = 512;

// A fake veil token block: the four fields the decision reads or writes, at the offsets the module names.
struct FakeToken {
  alignas(8) std::uint8_t bytes[0x20] = {};
  std::int32_t& Count() { return *reinterpret_cast<std::int32_t*>(bytes + wall::kTokenCount); }
  std::uint64_t& Expiry() { return *reinterpret_cast<std::uint64_t*>(bytes + wall::kTokenExpiry); }
  std::int32_t& Shown() { return *reinterpret_cast<std::int32_t*>(bytes + wall::kTokenShown); }
  std::int32_t& Barriers() { return *reinterpret_cast<std::int32_t*>(bytes + wall::kTokenBarriers); }
};

// A held token: `count` tokens, expiring at `expiry`.
FakeToken HeldToken(std::int32_t count, std::uint64_t expiry) {
  FakeToken t;
  t.Count() = count;
  t.Expiry() = expiry;
  return t;
}

constexpr std::uint64_t kNow = 1000 * kUnitTicks;

// The patch's writers. A failing writer notes its error in g_errors the way a production writer does (checked_patch.h),
// and the test reads it back through LastWriteError, the WriteErrorFn the patch takes.
nevr_checked_patch::WriteErrors g_errors;
std::size_t g_writes = 0;
std::uint32_t LastWriteError() { return g_errors.last; }
bool CountingWrite(void* dest, const void* src, std::size_t n) {
  ++g_writes;
  std::memcpy(dest, src, n);
  return true;
}
bool SilentFailingWrite(void*, const void*, std::size_t) {
  ++g_writes;
  return g_errors.Note(false, kWriteErrorCode);  // the writer refuses and writes nothing
}
bool WriteThenFail(void* dest, const void* src, std::size_t n) {
  ++g_writes;
  std::memcpy(dest, src, n);  // the bytes go in, and the writer reports a failure anyway
  return g_errors.Note(false, kWriteErrorCode);
}

// A fake script image: the stock get_global_data_len function at its RVA, 6 bytes: mov eax,158h; ret.
const std::uint8_t kStockGlobalLenBytes[6] = {0xb8, 0x58, 0x01, 0x00, 0x00, 0xc3};
const std::uint8_t kPatchedGlobalLenBytes[6] = {0xb8, 0x60, 0x01, 0x00, 0x00, 0xc3};
std::uint8_t* FakeImageFn(std::uint8_t* image) { return image + kExpectedGlobalLenFnRva; }
void PlaceStockGlobalLen(std::uint8_t* image) { std::memcpy(FakeImageFn(image), kStockGlobalLenBytes, sizeof kStockGlobalLenBytes); }

std::uintptr_t FakeVa(std::uint8_t* image) { return reinterpret_cast<std::uintptr_t>(FakeImageFn(image)); }

// The record of an outcome, as text (the same line PatchGlobalLen logs).
std::string RecordOf(std::uintptr_t va, const wall::GlobalLenOutcome& outcome) {
  char line[kRecordCap];
  wall::FormatGlobalLenRecord(line, sizeof line, va, outcome);
  return std::string(line);
}

}  // namespace

// --- the pins -------------------------------------------------------------------------------------------------------------

TEST(WallScriptPins, ScriptAndSitesMatchTheRecords) {
  EXPECT_EQ(wall::kScript, kExpectedScript);
  EXPECT_EQ(wall::kVeilTokenRva, kExpectedVeilTokenRva);
  EXPECT_EQ(wall::kGlobalLenFnRva, kExpectedGlobalLenFnRva);
}

TEST(WallScriptPins, RenewUnitsAreTheSecondsRoundedDown) {
  EXPECT_EQ(wall::kRenewAfterUnits, kExpectedRenewAfterUnits);
  EXPECT_EQ(wall::kRenewAfterPhysUnits, kExpectedRenewAfterPhysUnits);
}

TEST(WallScriptPins, TheCellIsOneDwordInThePatchedBlockAndPointsCellIsNotInIt) {
  // The patch adds 8 bytes to the stock block (+158h..+15Fh). The wall's cell is the dword at +158h and nothing else; POINT's
  // state is at +15Ch (H-WALL-b), so the cell must end before it.
  EXPECT_EQ(wall::kCellOffset, kStockGlobalLen);
  EXPECT_EQ(wall::kGlobalLenWithDecision, kStockGlobalLen + 8);
  EXPECT_LE(wall::kCellOffset + sizeof(std::uint32_t), kPointStateOffset);
}

// --- the cell: one dword, the decision in its low byte, the renew clock above it --------------------------------------

TEST(WallScriptCell, TheDecisionIsTheLowByte) {
  const std::uint32_t cell = wall::WithDecision(0, wall::Decision::kBarrier);
  EXPECT_EQ(cell, 2u);
  EXPECT_EQ(wall::CellDecision(cell), wall::Decision::kBarrier);
}

TEST(WallScriptCell, AByteThatIsNoDecisionReadsAsUndecided) {
  // Only the low byte is the decision: a clock in the upper bits does not change it, and any other low byte is undecided.
  EXPECT_EQ(wall::CellDecision(0x12345603u), wall::Decision::kUndecided);
  EXPECT_EQ(wall::CellDecision(0x00000201u), wall::Decision::kVeil);
}

TEST(WallScriptCell, ARenewClockWriteKeepsTheDecisionByte) {
  std::uint32_t cell = wall::WithDecision(0, wall::Decision::kVeil);
  wall::NewLife(cell, kNow, wall::kRenewAfterUnits);
  EXPECT_EQ(wall::CellDecision(cell), wall::Decision::kVeil);
  EXPECT_EQ(wall::CellClock(cell), static_cast<std::uint32_t>(kNow >> wall::kTimeShift));
}

// --- the veil token: "token -> veil; +10 / +14" --------------------------------------------------------------------------

TEST(WallScriptVeil, HeldTokenTurnsAnUndecidedWallIntoAVeilAndCountsPlusTen) {
  FakeToken token = HeldToken(2, kNow + 1000);
  std::uint32_t cell = 0;
  EXPECT_EQ(wall::Decide(token.bytes, cell, kNow, /*decideForGood=*/false), wall::Verdict::kVeil);
  EXPECT_EQ(wall::CellDecision(cell), wall::Decision::kVeil);
  EXPECT_EQ(token.Count(), 1);  // one token taken
  EXPECT_EQ(token.Shown(), 1);  // +10: the veil counter
  EXPECT_EQ(token.Barriers(), 0);
}

TEST(WallScriptVeil, ExpiredTokenIsNotTakenAndDecideForGoodMakesABarrierCountingPlusFourteen) {
  FakeToken token = HeldToken(2, kNow - 1);  // expired one tick ago
  std::uint32_t cell = 0;
  EXPECT_EQ(wall::Decide(token.bytes, cell, kNow, /*decideForGood=*/true), wall::Verdict::kBarrier);
  EXPECT_EQ(wall::CellDecision(cell), wall::Decision::kBarrier);
  EXPECT_EQ(token.Count(), 2);  // nothing taken
  EXPECT_EQ(token.Shown(), 0);
  EXPECT_EQ(token.Barriers(), 1);  // +14: the barrier counter
}

TEST(WallScriptVeil, ALookWithoutATokenLeavesTheWallUndecided) {
  FakeToken token = HeldToken(0, kNow + 1000);
  std::uint32_t cell = 0;
  EXPECT_EQ(wall::Decide(token.bytes, cell, kNow, /*decideForGood=*/false), wall::Verdict::kUndecided);
  EXPECT_EQ(wall::CellDecision(cell), wall::Decision::kUndecided);
  EXPECT_EQ(token.Shown(), 0);
  EXPECT_EQ(token.Barriers(), 0);
}

TEST(WallScriptVeil, ATokenAtItsExpiryTickIsNotHeld) {
  FakeToken token = HeldToken(1, kNow);  // now == expiry: the token is no longer held
  std::uint32_t cell = 0;
  EXPECT_EQ(wall::Decide(token.bytes, cell, kNow, /*decideForGood=*/false), wall::Verdict::kUndecided);
  EXPECT_EQ(token.Count(), 1);
}

TEST(WallScriptVeil, ADecidedWallKeepsItsDecisionAndTakesNoSecondToken) {
  FakeToken token = HeldToken(3, kNow + 1000);
  std::uint32_t cell = wall::WithDecision(0, wall::Decision::kVeil);
  EXPECT_EQ(wall::Decide(token.bytes, cell, kNow, /*decideForGood=*/false), wall::Verdict::kVeil);
  EXPECT_EQ(token.Count(), 3);  // a wall takes at most one token
  EXPECT_EQ(token.Shown(), 0);

  cell = wall::WithDecision(0, wall::Decision::kBarrier);
  EXPECT_EQ(wall::Decide(token.bytes, cell, kNow, /*decideForGood=*/false), wall::Verdict::kBarrier);
  EXPECT_EQ(token.Count(), 3);
}

// --- the renew rule ---------------------------------------------------------------------------------------------------

TEST(WallScriptRenew, ARunWithinRenewAfterKeepsTheWallsLife) {
  std::uint32_t cell = 0;
  EXPECT_TRUE(wall::NewLife(cell, kNow, wall::kRenewAfterUnits));  // first ever run: a new life
  // One unit short of RENEW_AFTER since the last run: still the same life.
  EXPECT_FALSE(wall::NewLife(cell, kNow + (wall::kRenewAfterUnits - 1) * kUnitTicks, wall::kRenewAfterUnits));
}

TEST(WallScriptRenew, ARunAfterRenewAfterStartsANewLife) {
  std::uint32_t cell = 0;
  wall::NewLife(cell, kNow, wall::kRenewAfterUnits);
  EXPECT_TRUE(wall::NewLife(cell, kNow + wall::kRenewAfterUnits * kUnitTicks, wall::kRenewAfterUnits));
  // The clock moved to the new run: the next run one unit later is the same life.
  EXPECT_FALSE(wall::NewLife(cell, kNow + (wall::kRenewAfterUnits + 1) * kUnitTicks, wall::kRenewAfterUnits));
}

TEST(WallScriptRenew, ANewLifeClearsTheDecision) {
  std::uint32_t cell = wall::WithDecision(0, wall::Decision::kVeil);
  EXPECT_TRUE(wall::RenewOnEntry(cell, /*aborting=*/false, kNow, wall::kRenewAfterUnits));
  EXPECT_EQ(wall::CellDecision(cell), wall::Decision::kUndecided);
}

TEST(WallScriptRenew, ALifeThatContinuesKeepsTheDecision) {
  std::uint32_t cell = wall::WithDecision(0, wall::Decision::kBarrier);
  EXPECT_TRUE(wall::RenewOnEntry(cell, /*aborting=*/false, kNow, wall::kRenewAfterUnits));  // first run: a new life
  cell = wall::WithDecision(cell, wall::Decision::kBarrier);
  EXPECT_FALSE(wall::RenewOnEntry(cell, /*aborting=*/false, kNow + kUnitTicks, wall::kRenewAfterUnits));
  EXPECT_EQ(wall::CellDecision(cell), wall::Decision::kBarrier);
}

TEST(WallScriptRenew, AnAbortingNodeTouchesNeitherTheClockNorTheDecision) {
  std::uint32_t cell = wall::WithDecision(7u << 8, wall::Decision::kBarrier);
  const std::uint32_t before = cell;
  EXPECT_FALSE(wall::RenewOnEntry(cell, /*aborting=*/true, kNow, wall::kRenewAfterUnits));
  EXPECT_EQ(cell, before);
}

TEST(WallScriptRenew, TheClockWrapsAtTwentyFourBits) {
  // The clock keeps 24 bits of the 2^20 units: a run 3 units after the wrap is 3 units after the last run, not 2^24 - x.
  const std::uint64_t wrapUnits = 1ULL << 24;
  std::uint32_t cell = 0;
  wall::NewLife(cell, (wrapUnits - 2) * kUnitTicks, wall::kRenewAfterUnits);
  EXPECT_FALSE(wall::NewLife(cell, (wrapUnits + 1) * kUnitTicks, wall::kRenewAfterUnits));
}

// --- the checked patch: the global data length at 0x5CC0 --------------------------------------------------------------

TEST(WallScriptPatch, StockBytesAreCheckedThenTheLengthGrowsByEight) {
  std::uint8_t image[kImageBytes];
  std::memset(image, 0, sizeof image);
  PlaceStockGlobalLen(image);
  g_writes = 0;
  const checked::Result r = wall::PatchGlobalLen(FakeImageFn(image), FakeVa(image), CountingWrite, LastWriteError);
  EXPECT_EQ(r, checked::Result::Applied);
  EXPECT_EQ(g_writes, 1u);
  EXPECT_EQ(std::memcmp(FakeImageFn(image), kPatchedGlobalLenBytes, sizeof kPatchedGlobalLenBytes), 0);
}

TEST(WallScriptPatch, AnAlreadyPatchedSiteIsLeftAlone) {
  std::uint8_t image[kImageBytes];
  std::memset(image, 0, sizeof image);
  PlaceStockGlobalLen(image);
  wall::PatchGlobalLen(FakeImageFn(image), FakeVa(image), CountingWrite, LastWriteError);
  g_writes = 0;
  EXPECT_EQ(wall::PatchGlobalLen(FakeImageFn(image), FakeVa(image), CountingWrite, LastWriteError),
            checked::Result::AlreadyApplied);
  EXPECT_EQ(g_writes, 0u);
}

TEST(WallScriptPatch, AChangedByteRefusesAndWritesNothing) {
  std::uint8_t image[kImageBytes];
  std::memset(image, 0, sizeof image);
  PlaceStockGlobalLen(image);
  FakeImageFn(image)[1] = 0x59;  // a different build: the immediate is not 158h
  std::uint8_t before[6];
  std::memcpy(before, FakeImageFn(image), sizeof before);
  g_writes = 0;
  EXPECT_EQ(wall::PatchGlobalLen(FakeImageFn(image), FakeVa(image), CountingWrite, LastWriteError), checked::Result::Mismatch);
  EXPECT_EQ(g_writes, 0u);
  EXPECT_EQ(std::memcmp(FakeImageFn(image), before, sizeof before), 0);
}

TEST(WallScriptPatch, AWriterThatRefusesIsReportedAsWriteFailed) {
  std::uint8_t image[kImageBytes];
  std::memset(image, 0, sizeof image);
  PlaceStockGlobalLen(image);
  g_errors = {};
  g_writes = 0;
  EXPECT_EQ(wall::PatchGlobalLen(FakeImageFn(image), FakeVa(image), SilentFailingWrite, LastWriteError),
            checked::Result::WriteFailed);
  EXPECT_EQ(g_writes, 1u);
}

// --- the Rule 6 / Rule 10 record of a refused or unconfirmed patch (logging.md NEVR.COMBAT row) -----------------------

TEST(WallScriptRecord, AWrittenWithErrorNamesTheWriterError) {
  std::uint8_t image[kImageBytes];
  std::memset(image, 0, sizeof image);
  PlaceStockGlobalLen(image);
  g_errors = {};
  const wall::GlobalLenOutcome outcome = wall::ApplyGlobalLen(FakeImageFn(image), WriteThenFail, LastWriteError);
  EXPECT_EQ(outcome.result, checked::Result::WrittenWithError);
  EXPECT_EQ(outcome.writeError, kWriteErrorCode);
  const std::string line = RecordOf(FakeVa(image), outcome);
  EXPECT_NE(line.find("[NEVR.COMBAT] wall script patch not applied name=wall_global_len va=0x"), std::string::npos) << line;
  EXPECT_NE(line.find(" result=written_with_error write_error=5 "), std::string::npos) << line;
}

TEST(WallScriptRecord, ActualIsTheBytesFoundBeforeTheWrite) {
  // WriteThenFail puts the patched bytes in and then reports an error: the record's `actual` must be the stock bytes the
  // site held before the write, not the patched bytes the site holds afterwards.
  std::uint8_t image[kImageBytes];
  std::memset(image, 0, sizeof image);
  PlaceStockGlobalLen(image);
  g_errors = {};
  const wall::GlobalLenOutcome outcome = wall::ApplyGlobalLen(FakeImageFn(image), WriteThenFail, LastWriteError);
  EXPECT_EQ(std::memcmp(outcome.found, kStockGlobalLenBytes, sizeof kStockGlobalLenBytes), 0);
  EXPECT_NE(RecordOf(FakeVa(image), outcome).find("actual=b858010000c3"), std::string::npos);
}

TEST(WallScriptRecord, AWriteFailedRecordNamesTheWriterError) {
  std::uint8_t image[kImageBytes];
  std::memset(image, 0, sizeof image);
  PlaceStockGlobalLen(image);
  g_errors = {};
  const wall::GlobalLenOutcome outcome = wall::ApplyGlobalLen(FakeImageFn(image), SilentFailingWrite, LastWriteError);
  EXPECT_EQ(outcome.result, checked::Result::WriteFailed);
  const std::string line = RecordOf(FakeVa(image), outcome);
  EXPECT_NE(line.find(" result=write_failed write_error=5 "), std::string::npos) << line;
  EXPECT_NE(line.find("consequence="), std::string::npos) << line;
}

TEST(WallScriptRecord, AMismatchRecordHasNoWriteError) {
  std::uint8_t image[kImageBytes];
  std::memset(image, 0, sizeof image);
  PlaceStockGlobalLen(image);
  FakeImageFn(image)[1] = 0x59;
  g_errors = {};
  const wall::GlobalLenOutcome outcome = wall::ApplyGlobalLen(FakeImageFn(image), CountingWrite, LastWriteError);
  EXPECT_EQ(outcome.result, checked::Result::Mismatch);
  const std::string line = RecordOf(FakeVa(image), outcome);
  EXPECT_NE(line.find("actual=b859010000c3"), std::string::npos) << line;
  EXPECT_EQ(line.find("write_error"), std::string::npos) << line;
}

// --- the Log() line PatchGlobalLen writes (Rule 6 / Rule 10), read through the boot log sink --------------------------

namespace {

std::vector<std::string> g_logged;
void CaptureLog(EchoVR::LogLevel, const CHAR* message) { g_logged.emplace_back(message); }

// Captures every Log() line of the test, and detaches the sink when the test ends.
struct CaptureScope {
  CaptureScope() {
    g_logged.clear();
    SetBootLogSink(&CaptureLog);
  }
  ~CaptureScope() { SetBootLogSink(nullptr); }
};

}  // namespace

TEST(WallScriptLog, AnAppliedPatchWritesNoLine) {
  CaptureScope capture;
  std::uint8_t image[kImageBytes];
  std::memset(image, 0, sizeof image);
  PlaceStockGlobalLen(image);
  g_errors = {};
  EXPECT_EQ(wall::PatchGlobalLen(FakeImageFn(image), FakeVa(image), CountingWrite, LastWriteError), checked::Result::Applied);
  EXPECT_TRUE(g_logged.empty());
}

TEST(WallScriptLog, AMismatchWritesOneWarningWithTheBytesFoundAndNoWriteError) {
  CaptureScope capture;
  std::uint8_t image[kImageBytes];
  std::memset(image, 0, sizeof image);
  PlaceStockGlobalLen(image);
  FakeImageFn(image)[1] = 0x59;
  g_errors = {};
  EXPECT_EQ(wall::PatchGlobalLen(FakeImageFn(image), FakeVa(image), CountingWrite, LastWriteError), checked::Result::Mismatch);
  ASSERT_EQ(g_logged.size(), 1u);
  EXPECT_NE(g_logged[0].find("[NEVR.COMBAT] wall script patch not applied"), std::string::npos) << g_logged[0];
  EXPECT_NE(g_logged[0].find("actual=b859010000c3"), std::string::npos) << g_logged[0];
  EXPECT_EQ(g_logged[0].find("write_error"), std::string::npos) << g_logged[0];
}

TEST(WallScriptLog, AFailedWriteNamesTheWriterErrorInTheLine) {
  CaptureScope capture;
  std::uint8_t image[kImageBytes];
  std::memset(image, 0, sizeof image);
  PlaceStockGlobalLen(image);
  g_errors = {};
  EXPECT_EQ(wall::PatchGlobalLen(FakeImageFn(image), FakeVa(image), SilentFailingWrite, LastWriteError),
            checked::Result::WriteFailed);
  ASSERT_EQ(g_logged.size(), 1u);
  EXPECT_NE(g_logged[0].find(" result=write_failed write_error=5 "), std::string::npos) << g_logged[0];
  EXPECT_NE(g_logged[0].find("consequence="), std::string::npos) << g_logged[0];
}
