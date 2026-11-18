// #2242 (H-WALL-a, H-WALL-b): the barrier wall's veil and point decisions, its renew rule and the checked global-length patch
// (runtime/combat/wall_script.h). The decisions are pure, so
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
#include "runtime/patch/ecfx_rules.h"

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
  EXPECT_EQ(wall::CellDecision(0x12345604u), wall::Decision::kUndecided);
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
std::vector<EchoVR::LogLevel> g_levels;
void CaptureLog(EchoVR::LogLevel level, const CHAR* message) {
  g_logged.emplace_back(message);
  g_levels.push_back(level);
}

// Captures every Log() line of the test, and detaches the sink when the test ends.
struct CaptureScope {
  CaptureScope() {
    g_logged.clear();
    g_levels.clear();
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

// --- H-WALL-b (#2242): the point token, the mute, POINT's state, the FX queue writer and the registration record -----------
// Values pinned from the records (veil_fx.py, ec_rules.h, dronefix.cpp): each is a test, not a run.

namespace ecfx = nevr_ecfx;

namespace {

constexpr std::uint32_t kExpectedPointTokenRva = 0x2104CE0;     // the EC page's point token block (ec_rules.h kPointTok)
constexpr std::uint32_t kExpectedFxQueueRva = 0x2104DA0;        // FX_QUEUE_RVA (veil_fx.py:656; dronefix.cpp:443)
constexpr std::uint32_t kExpectedFxDeferRva = 0x2104E20;        // FX_DEFER_RVA (veil_fx.py:658; dronefix.cpp:444)
constexpr std::uint32_t kExpectedPointFiredRva = 0x2104D44;     // POINT_FIRED_RVA (veil_fx.py:561)
constexpr std::uint32_t kExpectedPointStateOffset = 0x15C;      // POINT_STATE = GLOBAL_LEN + 4 (veil_fx.py:576)
constexpr std::uint64_t kEntryA = 0x180001000ULL;               // a 16-aligned entry: the drain takes the kind from its low bits
constexpr std::uint64_t kEntryB = 0x180002230ULL;               // another 16-aligned entry
constexpr std::uint64_t kMisalignedEntry = 0x180001008ULL;      // low nibble 8: the kind would read as another kind
constexpr std::uint64_t kCtxA = 0x700000001000ULL;              // a wall's node: any non-zero value
constexpr std::uint64_t kCtxB = 0x700000002000ULL;
constexpr std::uint32_t kPointDelay = 1000000;                  // 0.1 s in 100 ns ticks; a multiple of 4 (see the charge test)
constexpr std::uint64_t kWrapNow = 0x100000100ULL;              // low dword 0x100, after the wrap of the low dword
constexpr std::uint32_t kWrapStart = 0xFFFFFF00U;               // the charge-up started just before the wrap
constexpr std::uint32_t kWrapElapsed = 0x200;                   // kWrapNow - kWrapStart, in 100 ns ticks
constexpr int kKindCount = 4;                                   // charge, burst, boom, sound
constexpr std::uint64_t kSlotCount = ecfx::kSlots;

// A fake FX queue: the six { node, word } slots of the EC page, all free.
struct FakeQueue {
  std::uint64_t words[2 * ecfx::kSlots] = {};
  bool AllFree() const {
    for (std::uint64_t w : words) {
      if (w != 0) return false;
    }
    return true;
  }
};

// One entry the drain ran.
struct Ran {
  std::uint64_t ctx;
  std::uint64_t fn;
  int kind;
};

// The wall a sound's node belongs to: the drain reads it from the node (nevr_ecfx::Env::wallOf). The test's node is its own wall.
std::uint64_t WallOfNode(std::uint64_t ctx) { return ctx; }

// Runs one shipping drain over `queue` and returns the entries it ran.
std::vector<Ran> Drain(std::uint64_t* queue) {
  ecfx::Drain drain;
  ecfx::Env env;
  env.wallOf = &WallOfNode;
  ecfx::Counts counts;
  std::vector<Ran> ran;
  drain.Plan(queue, env, /*nowMs=*/100, [&ran](const ecfx::Run& r) { ran.push_back(Ran{r.ctx, r.fn, r.kind}); }, &counts);
  return ran;
}

}  // namespace

TEST(WallScriptPins, PointTokenAndFxQueueAreTheRecordsOffsets) {
  EXPECT_EQ(wall::kPointTokenRva, kExpectedPointTokenRva);
  EXPECT_EQ(wall::kFxQueueRva, kExpectedFxQueueRva);
  EXPECT_EQ(wall::kFxDeferRva, kExpectedFxDeferRva);
  EXPECT_EQ(wall::kPointFiredRva, kExpectedPointFiredRva);
  EXPECT_EQ(wall::kPointStateOffset, kExpectedPointStateOffset);
  EXPECT_EQ(kSlotCount, 6u);
}

TEST(WallScriptCell, PointIsItsOwnDecisionAndKeepsTheClock) {
  constexpr std::uint32_t kClock = 7;
  const std::uint32_t cell = wall::WithDecision(kClock << 8, wall::Decision::kPoint);
  EXPECT_EQ(wall::CellDecision(cell), wall::Decision::kPoint);
  EXPECT_EQ(wall::CellClock(cell), kClock);
}

// --- the point token: a point wall takes it first; +10 of POINT's own block --------------------------------------------

TEST(WallScriptPoint, AHeldPointTokenIsTakenFirstAndCountsPlusTenOfItsBlock) {
  FakeToken point = HeldToken(1, kNow + 1000);
  FakeToken veil = HeldToken(1, kNow + 1000);
  std::uint32_t cell = 0;
  EXPECT_EQ(wall::DecideWithPoint(point.bytes, veil.bytes, cell, kNow, /*decideForGood=*/false), wall::Verdict::kPoint);
  EXPECT_EQ(wall::CellDecision(cell), wall::Decision::kPoint);
  EXPECT_EQ(point.Count(), 0);
  EXPECT_EQ(point.Shown(), 1);  // +10 of POINT's block: a wall that went off as POINT
  EXPECT_EQ(veil.Count(), 1);   // the veil's token is not touched
  EXPECT_EQ(veil.Shown(), 0);
}

TEST(WallScriptPoint, WithNoPointTokenTheVeilTokenIsTaken) {
  FakeToken point = HeldToken(0, kNow + 1000);
  FakeToken veil = HeldToken(1, kNow + 1000);
  std::uint32_t cell = 0;
  EXPECT_EQ(wall::DecideWithPoint(point.bytes, veil.bytes, cell, kNow, /*decideForGood=*/false), wall::Verdict::kVeil);
  EXPECT_EQ(wall::CellDecision(cell), wall::Decision::kVeil);
  EXPECT_EQ(veil.Count(), 0);
  EXPECT_EQ(veil.Shown(), 1);
  EXPECT_EQ(point.Shown(), 0);
}

TEST(WallScriptPoint, ADecidedWallKeepsItsVeilAndTakesNoPointToken) {
  // A decided wall takes nothing: a held point token stays held for the next undecided wall.
  FakeToken point = HeldToken(1, kNow + 1000);
  FakeToken veil = HeldToken(1, kNow + 1000);
  std::uint32_t cell = wall::WithDecision(0, wall::Decision::kVeil);
  EXPECT_EQ(wall::DecideWithPoint(point.bytes, veil.bytes, cell, kNow, /*decideForGood=*/true), wall::Verdict::kVeil);
  EXPECT_EQ(wall::CellDecision(cell), wall::Decision::kVeil);
  EXPECT_EQ(point.Count(), 1);
  EXPECT_EQ(point.Shown(), 0);
  EXPECT_EQ(veil.Count(), 1);
}

TEST(WallScriptPoint, AnExpiredPointTokenIsNotTakenAndTheBarrierCountIsTheVeilsPlusFourteen) {
  FakeToken point = HeldToken(1, kNow - 1);  // expired one tick ago
  FakeToken veil = HeldToken(0, kNow + 1000);
  std::uint32_t cell = 0;
  EXPECT_EQ(wall::DecideWithPoint(point.bytes, veil.bytes, cell, kNow, /*decideForGood=*/true), wall::Verdict::kBarrier);
  EXPECT_EQ(point.Count(), 1);
  EXPECT_EQ(point.Shown(), 0);
  EXPECT_EQ(point.Barriers(), 0);
  EXPECT_EQ(veil.Barriers(), 1);  // +14 is the veil block's, for a barrier, as in Decide
}

TEST(WallScriptPoint, ALookWithNoTokenLeavesThePointWallUndecided) {
  FakeToken point = HeldToken(0, kNow + 1000);
  FakeToken veil = HeldToken(0, kNow + 1000);
  std::uint32_t cell = 0;
  EXPECT_EQ(wall::DecideWithPoint(point.bytes, veil.bytes, cell, kNow, /*decideForGood=*/false), wall::Verdict::kUndecided);
  EXPECT_EQ(wall::CellDecision(cell), wall::Decision::kUndecided);
  EXPECT_EQ(point.Shown(), 0);
  EXPECT_EQ(veil.Barriers(), 0);
}

TEST(WallScriptPoint, ADecidedWallKeepsItsDecisionAndTakesNoToken) {
  FakeToken point = HeldToken(1, kNow + 1000);
  FakeToken veil = HeldToken(1, kNow + 1000);
  std::uint32_t cell = wall::WithDecision(0, wall::Decision::kVeil);
  EXPECT_EQ(wall::DecideWithPoint(point.bytes, veil.bytes, cell, kNow, /*decideForGood=*/false), wall::Verdict::kVeil);
  EXPECT_EQ(point.Count(), 1);  // a veil wall does not take a point token
  EXPECT_EQ(veil.Count(), 1);
  cell = wall::WithDecision(0, wall::Decision::kPoint);
  EXPECT_EQ(wall::DecideWithPoint(point.bytes, veil.bytes, cell, kNow, /*decideForGood=*/false), wall::Verdict::kPoint);
  EXPECT_EQ(point.Count(), 1);  // a point wall takes at most one token
  EXPECT_EQ(veil.Count(), 1);
}

// --- the mute: a veil or point wall's own sounds are silent, a barrier's or undecided wall's pass through (veil_fx.py:614-622) ---

TEST(WallScriptMute, AVeilAndAPointWallsOwnSoundIsMutedAndABarrierOrUndecidedOneIsNot) {
  EXPECT_TRUE(wall::MutesSound(wall::Decision::kVeil));
  EXPECT_TRUE(wall::MutesSound(wall::Decision::kPoint));
  EXPECT_FALSE(wall::MutesSound(wall::Decision::kBarrier));
  EXPECT_FALSE(wall::MutesSound(wall::Decision::kUndecided));
}

// --- POINT's state dword at +15Ch: 0 not started, 1 gone off, else the charge-up's start stamp ------------------------

TEST(WallScriptPointState, AStartStampIsNeverTheNotStartedOrGoneOffState) {
  for (std::uint64_t now : {0ULL, 1ULL, 2ULL, 3ULL, kNow, ~0ULL}) {
    const std::uint32_t stamp = wall::PointStartStamp(now);
    EXPECT_NE(stamp, wall::kPointNotStarted) << now;
    EXPECT_NE(stamp, wall::kPointGoneOff) << now;
  }
}

TEST(WallScriptPointState, AChargeStartedAtALowDwordOfZeroIsStillCharging) {
  // veil_fx.py stamps `now | 1`, which at a low dword of 0 or 1 reads as gone off (1). The port's stamp never does.
  const std::uint32_t state = wall::PointStartStamp(0);
  EXPECT_TRUE(wall::PointIsCharging(state));
  EXPECT_NE(state, wall::kPointGoneOff);
}

TEST(WallScriptPointState, NotStartedAndGoneOffAreNotCharging) {
  EXPECT_FALSE(wall::PointIsCharging(wall::kPointNotStarted));
  EXPECT_FALSE(wall::PointIsCharging(wall::kPointGoneOff));
  EXPECT_TRUE(wall::PointIsCharging(wall::PointStartStamp(kNow)));
  EXPECT_FALSE(wall::PointChargeDone(wall::kPointGoneOff, kNow + kPointDelay, kPointDelay));
}

TEST(WallScriptPointState, TheChargeIsDoneAtItsDelayAndNotBefore) {
  // kNow's low 20 bits are 0, so the stamp is exact; kPointDelay is a multiple of 4, so bit 1 of each later time is clear.
  const std::uint32_t state = wall::PointStartStamp(kNow);
  EXPECT_FALSE(wall::PointChargeDone(state, kNow + kPointDelay - 1, kPointDelay));
  EXPECT_TRUE(wall::PointChargeDone(state, kNow + kPointDelay, kPointDelay));
}

TEST(WallScriptPointState, TheElapsedTimeIsKeptAcrossTheWrapOfTheLowDword) {
  const std::uint32_t state = wall::PointStartStamp(kWrapStart);
  EXPECT_EQ(wall::PointElapsed(state, kWrapNow), kWrapElapsed);
  EXPECT_TRUE(wall::PointChargeDone(state, kWrapNow, kWrapElapsed));
  EXPECT_FALSE(wall::PointChargeDone(state, kWrapNow, kWrapElapsed + 1));
}

// --- the FX queue: the wall writes an entry, the shipping drain (nevr_ecfx::Drain) runs it -------------------------------

TEST(WallScriptFx, AQueuedEffectIsRunOnceByTheShippingDrainAndItsSlotIsFreed) {
  FakeQueue queue;
  ASSERT_EQ(wall::EnqueueWallFx(queue.words, kCtxA, kEntryA, ecfx::kBurst), wall::EnqueueResult::kQueued);
  EXPECT_EQ(queue.words[0], kCtxA);
  EXPECT_EQ(queue.words[1], kEntryA | static_cast<std::uint64_t>(ecfx::kBurst));
  const std::vector<Ran> ran = Drain(queue.words);
  ASSERT_EQ(ran.size(), 1u);
  EXPECT_EQ(ran[0].ctx, kCtxA);
  EXPECT_EQ(ran[0].fn, kEntryA);
  EXPECT_EQ(ran[0].kind, ecfx::kBurst);
  EXPECT_TRUE(queue.AllFree());
}

TEST(WallScriptFx, EveryKindIsStoredInTheLowTwoBitsAndRunAsItsKind) {
  for (int kind = 0; kind < kKindCount; ++kind) {
    FakeQueue queue;
    ASSERT_EQ(wall::EnqueueWallFx(queue.words, kCtxB, kEntryB, kind), wall::EnqueueResult::kQueued) << kind;
    EXPECT_EQ(queue.words[1], kEntryB | static_cast<std::uint64_t>(kind)) << kind;
    const std::vector<Ran> ran = Drain(queue.words);
    if (kind == ecfx::kBoom) {
      // The drain frees a boom unrun (the stun boom is left to the server, nevr_ecfx::Drain::Plan): its slot is still freed.
      EXPECT_TRUE(ran.empty());
      EXPECT_TRUE(queue.AllFree());
      continue;
    }
    ASSERT_EQ(ran.size(), 1u) << kind;
    EXPECT_EQ(ran[0].kind, kind);
    EXPECT_EQ(ran[0].fn, kEntryB);
  }
}

TEST(WallScriptFx, AMisalignedEntryIsRefusedAndNothingIsQueued) {
  FakeQueue queue;
  EXPECT_EQ(wall::EnqueueWallFx(queue.words, kCtxA, kMisalignedEntry, ecfx::kBurst), wall::EnqueueResult::kMisaligned);
  EXPECT_TRUE(queue.AllFree());
  EXPECT_TRUE(Drain(queue.words).empty());
}

TEST(WallScriptFx, AZeroEntryAndAZeroNodeAreRefusedAndNothingIsQueued) {
  FakeQueue queue;
  EXPECT_EQ(wall::EnqueueWallFx(queue.words, kCtxA, 0, ecfx::kCharge), wall::EnqueueResult::kNoEntry);
  EXPECT_EQ(wall::EnqueueWallFx(queue.words, 0, kEntryA, ecfx::kCharge), wall::EnqueueResult::kNoNode);
  EXPECT_TRUE(queue.AllFree());
}

TEST(WallScriptFx, AKindOutsideTheFourIsRefused) {
  FakeQueue queue;
  EXPECT_EQ(wall::EnqueueWallFx(queue.words, kCtxA, kEntryA, kKindCount), wall::EnqueueResult::kBadKind);
  EXPECT_EQ(wall::EnqueueWallFx(queue.words, kCtxA, kEntryA, -1), wall::EnqueueResult::kBadKind);
  EXPECT_TRUE(queue.AllFree());
}

TEST(WallScriptFx, ASeventhEffectFindsTheQueueFullAndIsDropped) {
  FakeQueue queue;
  for (int i = 0; i < ecfx::kSlots; ++i) {
    ASSERT_EQ(wall::EnqueueWallFx(queue.words, kCtxA + static_cast<std::uint64_t>(i), kEntryA, ecfx::kBoom),
              wall::EnqueueResult::kQueued)
        << i;
  }
  std::uint64_t before[2 * ecfx::kSlots];
  std::memcpy(before, queue.words, sizeof before);
  EXPECT_EQ(wall::EnqueueWallFx(queue.words, kCtxB, kEntryB, ecfx::kSound), wall::EnqueueResult::kFull);
  for (int i = 0; i < 2 * ecfx::kSlots; ++i) EXPECT_EQ(queue.words[i], before[i]) << i;
}

// --- the registration record: W-REG's entry installs nothing and says so, once, at Warning -----------------------------

TEST(WallScriptRegister, RegisterInstallsNothingAndWritesOneWarningThatSaysSo) {
  CaptureScope capture;
  wall::Register();
  ASSERT_EQ(g_logged.size(), 1u);
  EXPECT_NE(g_logged[0].find("[NEVR.COMBAT] point wall not installed hooks=0"), std::string::npos) << g_logged[0];
  EXPECT_NE(g_logged[0].find("consequence="), std::string::npos) << g_logged[0];
  ASSERT_EQ(g_levels.size(), 1u);
  EXPECT_EQ(g_levels[0], EchoVR::LogLevel::Warning);
}
