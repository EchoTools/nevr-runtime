// The start's decision over pending.json (core/update/engine_start.h; ADR 0024, section 6, R2). The cases are the host's F10
// to F12 (test_host_update_swap.cpp), the start beside a running one, and the cases the host's own tests cannot reach
// through the Win32 edge, decided over facts a fake UpdateFs holds. No Windows header and no file system: this file builds
// and runs on the Linux host as it does under Wine.
//
// Each case also pins how often the decision asks the in-use question: never for a record that is absent, unreadable,
// damaged or stale, and exactly once otherwise.

#include "core/update/engine_start.h"
#include "core/update/names.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace {

using nevr_update::DecidePending;
using nevr_update::PendingAction;
using nevr_update::PendingDecision;
using nevr_update::PendingFacts;
using nevr_update::ReadPendingFacts;
using nevr_update::ReadResult;
using nevr_update::WriteAccess;
using nlohmann::json;
namespace names = nevr_update_names;

constexpr char kUpdateFolder[] = "update";
constexpr char kSeparator = '/';
constexpr char kNewVersion[] = "7.7.7";
constexpr char kOldVersion[] = "1.2.3";
constexpr char kAbiRejected[] = "abi_rejected";
constexpr char kCrashed[] = "crashed";
constexpr char kUnconfirmed[] = "unconfirmed";
/// ERROR_ACCESS_DENIED: a platform error the decision carries through unchanged.
constexpr std::uint32_t kAccessDenied = 5;
/// The first start of a pending release: boot_attempts is 1 once the swap has written the record.
constexpr std::uint64_t kFirstStart = 1;
/// The starts without the mark after which the release is rolled back. The engine names it kBootAttemptsToRollBack.
constexpr std::uint64_t kExpectedBootAttemptsToRollBack = 2;
/// One level past the deepest JSON file of update\ may nest.
constexpr std::size_t kTooDeep = names::kMaxJsonDepth + 1;

// The file system the decision reads: files by path, and the paths that are there and cannot be read.
class FakeFs final : public nevr_update::UpdateFs {
 public:
  char Separator() const noexcept override { return kSeparator; }

  ReadResult ReadSmallFile(const std::string& path, std::string* out) const noexcept override {
    const auto found = files.find(path);
    if (found == files.end()) return ReadResult::kAbsent;
    if (unreadable.count(path) != 0) {
      last_error = read_error;
      return ReadResult::kUnreadable;
    }
    if (found->second.size() > names::kMaxJsonBytes) return ReadResult::kTooBig;
    *out = found->second;
    return ReadResult::kOk;
  }
  bool WriteByRename(const std::string&, const std::string&) const noexcept override { return false; }
  bool RemoveFile(const std::string&) const noexcept override { return false; }
  void RemoveTree(const std::string&) const noexcept override {}
  bool IsLink(const std::string&) const noexcept override { return false; }
  bool Exists(const std::string& path) const noexcept override {
    last_error = exists_error;
    return files.count(path) != 0;
  }
  int LastOsError() const noexcept override { return last_error; }
  std::unique_ptr<nevr_update::FsLock> TakeLock(const std::string&, std::uint32_t, std::uint32_t, int*) const noexcept override {
    return nullptr;
  }
  bool Move(const std::string&, const std::string&, int*) const noexcept override { return false; }

  std::map<std::string, std::string> files;
  std::set<std::string> unreadable;
  /// The platform error a read of an unreadable file leaves, and the one the marker's check leaves (a later call replaces it).
  int read_error = 0;
  int exists_error = 0;
  mutable int last_error = 0;
};

std::string UpdatePath(const std::string& name) { return std::string(kUpdateFolder) + kSeparator + name; }
std::string NewSha() { return std::string(nevr_update::kSha256HexChars, 'b'); }
std::string OldSha() { return std::string(nevr_update::kSha256HexChars, 'c'); }

// A pending record as the host writes it: the new release, the one it replaced, its starts and its recorded failure.
json Record(std::uint64_t boot_attempts, json failed_reason = nullptr) {
  json doc = {{"to", {{"version", kNewVersion}, {"sha256", NewSha()}}},
              {"from", {{"version", kOldVersion}, {"sha256", OldSha()}}},
              {"boot_attempts", boot_attempts},
              {"failed_reason", failed_reason}};
  return doc;
}

PendingFacts FactsOf(const FakeFs& fs, const std::string& root_runtime_sha256 = NewSha(), bool finished_swap = false) {
  return ReadPendingFacts(fs, kUpdateFolder, root_runtime_sha256, finished_swap);
}

WriteAccess Writable() { return WriteAccess{}; }
WriteAccess InUse() { return WriteAccess{WriteAccess::Kind::kInUse, 0}; }
WriteAccess NotWritable(std::uint32_t error) { return WriteAccess{WriteAccess::Kind::kNotWritable, error}; }

// The answer the in-use question gives, and how many times the decision asked it.
struct CountingAnswer {
  explicit CountingAnswer(WriteAccess given) : answer(given) {}
  WriteAccess answer;
  int asked = 0;
};

PendingDecision Decide(const PendingFacts& facts, CountingAnswer& write_access) {
  const std::function<WriteAccess()> ask = [&write_access]() {
    ++write_access.asked;
    return write_access.answer;
  };
  return DecidePending(facts, ask);
}

// Writes a pending record and, when `crashed`, the crash marker, into a fresh file system.
FakeFs WithRecord(const json& record, bool crashed = false) {
  FakeFs fs;
  fs.files[UpdatePath(names::kPendingJson)] = record.dump();
  if (crashed) fs.files[UpdatePath(names::kCrashedFile)] = "";
  return fs;
}

TEST(EnginePendingDecision, AbsentPendingIsNothingAndAsksNothing) {
  const FakeFs fs{};
  CountingAnswer write_access{Writable()};
  const PendingDecision decision = Decide(FactsOf(fs), write_access);
  EXPECT_EQ(decision.action, PendingAction::kNone);
  EXPECT_EQ(write_access.asked, 0);
}

TEST(EnginePendingDecision, AnUnreadablePendingIsHeldAndNothingIsJudged) {
  FakeFs fs = WithRecord(Record(kFirstStart));
  fs.unreadable.insert(UpdatePath(names::kPendingJson));
  CountingAnswer write_access{Writable()};
  const PendingDecision decision = Decide(FactsOf(fs), write_access);
  EXPECT_EQ(decision.action, PendingAction::kUnreadable);
  EXPECT_EQ(write_access.asked, 0);
}

TEST(EnginePendingDecision, ARecordLargerThanTheCapIsDamagedAndAsksNothing) {
  FakeFs fs;
  fs.files[UpdatePath(names::kPendingJson)] = std::string(names::kMaxJsonBytes + 1, ' ');
  CountingAnswer write_access{Writable()};
  const PendingDecision decision = Decide(FactsOf(fs), write_access);
  EXPECT_EQ(decision.action, PendingAction::kDamaged);
  EXPECT_EQ(write_access.asked, 0);
}

TEST(EnginePendingDecision, ANonJsonRecordIsDamaged) {
  FakeFs fs;
  fs.files[UpdatePath(names::kPendingJson)] = "not json";
  CountingAnswer write_access{Writable()};
  EXPECT_EQ(Decide(FactsOf(fs), write_access).action, PendingAction::kDamaged);
  EXPECT_EQ(write_access.asked, 0);
}

TEST(EnginePendingDecision, ARecordNestedTooDeepIsDamagedAndNeverParsed) {
  FakeFs fs;
  fs.files[UpdatePath(names::kPendingJson)] = std::string(kTooDeep, '[') + std::string(kTooDeep, ']');
  CountingAnswer write_access{Writable()};
  EXPECT_EQ(Decide(FactsOf(fs), write_access).action, PendingAction::kDamaged);
  EXPECT_EQ(write_access.asked, 0);
}

TEST(EnginePendingDecision, AnObjectThatIsNotAPendingRecordIsDamaged) {
  FakeFs fs = WithRecord(json{{"boot_attempts", kFirstStart}});
  CountingAnswer write_access{Writable()};
  EXPECT_EQ(Decide(FactsOf(fs), write_access).action, PendingAction::kDamaged);
  EXPECT_EQ(write_access.asked, 0);
}

TEST(EnginePendingDecision, AStaleRecordIsDroppedAndAsksNothing) {
  // The root holds the old runtime, not the one this record names: the record is not about the root (ADR 0024, section 4.7).
  FakeFs fs = WithRecord(Record(kFirstStart));
  CountingAnswer write_access{Writable()};
  const PendingDecision decision = Decide(FactsOf(fs, OldSha()), write_access);
  EXPECT_EQ(decision.action, PendingAction::kStale);
  EXPECT_EQ(decision.pending.to_version, kNewVersion);
  EXPECT_EQ(write_access.asked, 0);
}

TEST(EnginePendingDecision, AnEmptyRootDigestIsNotStale) {
  // No readable runtime in the root (the edge gives no digest) names no other release: the record is judged as usual.
  FakeFs fs = WithRecord(Record(kFirstStart));
  CountingAnswer write_access{Writable()};
  EXPECT_EQ(Decide(FactsOf(fs, ""), write_access).action, PendingAction::kCountTry);
  EXPECT_EQ(write_access.asked, 1);
}

// F10: a failure the release recorded is named before a crash or a count.
TEST(EnginePendingDecision, F10AFailedReasonRollsBackWithThatReasonBeforeACrashOrACount) {
  FakeFs fs = WithRecord(Record(kExpectedBootAttemptsToRollBack, kAbiRejected), true);
  CountingAnswer write_access{Writable()};
  const PendingDecision decision = Decide(FactsOf(fs), write_access);
  EXPECT_EQ(decision.action, PendingAction::kRollBack);
  EXPECT_EQ(decision.reason, kAbiRejected);
  EXPECT_EQ(write_access.asked, 1);
}

// F11: the crash marker rolls the release back at the next start, at once.
TEST(EnginePendingDecision, F11ACrashMarkerRollsTheReleaseBackAtTheNextStart) {
  FakeFs fs = WithRecord(Record(kFirstStart), true);
  CountingAnswer write_access{Writable()};
  const PendingDecision decision = Decide(FactsOf(fs), write_access);
  EXPECT_EQ(decision.action, PendingAction::kRollBack);
  EXPECT_EQ(decision.reason, kCrashed);
  EXPECT_EQ(write_access.asked, 1);
}

// F12: a release with no mark after two starts is rolled back at the third start, and not before.
TEST(EnginePendingDecision, F12AReleaseWithoutTheMarkAfterTwoStartsIsRolledBack) {
  FakeFs fs = WithRecord(Record(kExpectedBootAttemptsToRollBack));
  CountingAnswer write_access{Writable()};
  const PendingDecision decision = Decide(FactsOf(fs), write_access);
  EXPECT_EQ(decision.action, PendingAction::kRollBack);
  EXPECT_EQ(decision.reason, kUnconfirmed);
  EXPECT_EQ(decision.pending.boot_attempts, kExpectedBootAttemptsToRollBack);
  EXPECT_EQ(write_access.asked, 1);
}

TEST(EnginePendingDecision, F12ASecondStartIsAnotherTryAndIsCounted) {
  FakeFs fs = WithRecord(Record(kFirstStart));
  CountingAnswer write_access{Writable()};
  const PendingDecision decision = Decide(FactsOf(fs), write_access);
  ASSERT_EQ(decision.action, PendingAction::kCountTry);
  EXPECT_EQ(decision.counted.value("boot_attempts", std::uint64_t{0}), kFirstStart + 1);
  EXPECT_EQ(write_access.asked, 1);
}

TEST(EnginePendingDecision, TheRollBackThresholdIsTheNamedConstantTwo) {
  EXPECT_EQ(nevr_update::kBootAttemptsToRollBack, kExpectedBootAttemptsToRollBack);
}

// A start beside a running one (test_host_update_swap: ASecondStartInsideTheFirstOnesTrial...): not a try, and no condemnation.
TEST(EnginePendingDecision, AStartBesideARunningOneIsNotATry) {
  FakeFs fs = WithRecord(Record(kFirstStart));
  CountingAnswer write_access{InUse()};
  EXPECT_EQ(Decide(FactsOf(fs), write_access).action, PendingAction::kNotCountedInUse);
  EXPECT_EQ(write_access.asked, 1);
}

TEST(EnginePendingDecision, AnInUseRuntimeDefersARollBackAndCondemnsNothing) {
  FakeFs fs = WithRecord(Record(kExpectedBootAttemptsToRollBack));
  CountingAnswer write_access{InUse()};
  const PendingDecision decision = Decide(FactsOf(fs), write_access);
  EXPECT_EQ(decision.action, PendingAction::kDeferredInUse);
  EXPECT_EQ(decision.reason, kUnconfirmed);
  EXPECT_EQ(write_access.asked, 1);
}

TEST(EnginePendingDecision, ANonWritableRuntimeBlocksARollBackWithItsError) {
  FakeFs fs = WithRecord(Record(kFirstStart), true);
  CountingAnswer write_access{NotWritable(kAccessDenied)};
  const PendingDecision decision = Decide(FactsOf(fs), write_access);
  EXPECT_EQ(decision.action, PendingAction::kBlockedNotWritable);
  EXPECT_EQ(decision.reason, kCrashed);
  EXPECT_EQ(decision.os_error, kAccessDenied);
  EXPECT_EQ(write_access.asked, 1);
}

TEST(EnginePendingDecision, ANonWritableRuntimeIsNotATryWithItsError) {
  FakeFs fs = WithRecord(Record(kFirstStart));
  CountingAnswer write_access{NotWritable(kAccessDenied)};
  const PendingDecision decision = Decide(FactsOf(fs), write_access);
  EXPECT_EQ(decision.action, PendingAction::kNotCountedNotWritable);
  EXPECT_EQ(decision.os_error, kAccessDenied);
  EXPECT_EQ(write_access.asked, 1);
}

// The load that follows a swap whose cleanup this start finished is the release's first try, not a second one.
TEST(EnginePendingDecision, ACleanupThisStartFinishedIsNotATry) {
  FakeFs fs = WithRecord(Record(kFirstStart));
  CountingAnswer write_access{Writable()};
  EXPECT_EQ(Decide(FactsOf(fs, NewSha(), true), write_access).action, PendingAction::kNotCountedSwapFinished);
  EXPECT_EQ(write_access.asked, 1);
}

TEST(EnginePendingDecision, ReadPendingFactsReadsTheRecordAndTheMarkerThroughTheFileSystem) {
  FakeFs fs = WithRecord(Record(kFirstStart), true);
  const PendingFacts facts = FactsOf(fs, OldSha(), true);
  EXPECT_EQ(facts.pending_read, ReadResult::kOk);
  EXPECT_EQ(facts.pending_text, Record(kFirstStart).dump());
  EXPECT_TRUE(facts.crashed);
  EXPECT_EQ(facts.root_runtime_sha256, OldSha());
  EXPECT_TRUE(facts.finished_swap);
  EXPECT_EQ(facts.pending_os_error, 0u) << "a read that succeeds leaves no platform error";
}

// The error of the record's read is the one the read left. The marker is asked for after the read, and the UpdateFs here
// leaves another error on that call, so a capture taken after the marker check would hold the wrong error.
TEST(EnginePendingDecision, ReadPendingFactsKeepsTheErrorOfTheRecordsRead) {
  FakeFs fs = WithRecord(Record(kFirstStart), true);
  fs.unreadable.insert(UpdatePath(names::kPendingJson));
  fs.read_error = EACCES;
  fs.exists_error = ENOENT;
  const PendingFacts facts = FactsOf(fs);
  EXPECT_EQ(facts.pending_read, ReadResult::kUnreadable);
  EXPECT_EQ(facts.pending_os_error, static_cast<std::uint32_t>(EACCES));
}

TEST(EnginePendingDecision, ReadPendingFactsReportsAnAbsentRecordAndAnAbsentMarker) {
  const FakeFs fs{};
  const PendingFacts facts = FactsOf(fs);
  EXPECT_EQ(facts.pending_read, ReadResult::kAbsent);
  EXPECT_FALSE(facts.crashed);
}

TEST(EnginePendingDecision, ShaAndReasonVocabularyIsWhatTheJournalAndStateReadersNeed) {
  EXPECT_TRUE(nevr_update::IsSha256Hex(NewSha()));
  EXPECT_FALSE(nevr_update::IsSha256Hex(std::string(nevr_update::kSha256HexChars - 1, 'b')));
  EXPECT_FALSE(nevr_update::IsSha256Hex(std::string(nevr_update::kSha256HexChars, 'B')));
  EXPECT_TRUE(nevr_update::IsReasonToken(kAbiRejected));
  EXPECT_FALSE(nevr_update::IsReasonToken("Abi_Rejected"));
  EXPECT_FALSE(nevr_update::IsReasonToken(""));
}

}  // namespace
