// Host tests for the first-run data fetch state machine (src/quest/data/fetch.cpp) against a scripted Transport: resume,
// a dropped connection, a server that ignores Range, a tampered body, no network, no space, a hostile archive, cancel.

#include <algorithm>
#include <csignal>
#include <cstdint>
#include <string>
#include <sys/resource.h>
#include <vector>

#include "quest/data/fetch.h"
#include "quest/data/sha256.h"
#include "quest/data/staging.h"
#include "quest/tests/data_test_support.h"
#include "quest/tests/mini_test.h"

using namespace nevr_quest::data;
using namespace data_test;

namespace {

struct Step {
  long status = 206;
  bool honor_range = true;       // false: the body starts at byte 0 whatever the Range asked
  std::int64_t drop_after = -1;  // >= 0: send this many body bytes, then the connection drops
  bool completes = true;         // false: no response at all (the network is down)
  std::string extra;             // appended to the body (a body longer than the pin)
  int flip_at = -1;              // flip one byte of the body at this offset into the zip
  std::int64_t short_by = 0;     // > 0: the response ends cleanly this many bytes before the pin (completed, but short)
};

class ScriptedTransport : public Transport {
 public:
  ScriptedTransport(std::string payload, std::vector<Step> steps) : payload_(std::move(payload)), steps_(std::move(steps)) {}

  TransportResult GetRange(const std::string&, std::uint64_t offset, const BodyFn& on_body,
                           const KeepGoingFn& keep_going) override {
    offsets.push_back(offset);
    const Step& step = steps_[std::min(offsets.size() - 1, steps_.size() - 1)];
    TransportResult r;
    if (!step.completes) {
      r.code = 7;  // could not connect
      return r;
    }
    r.http_status = step.status;
    if (step.status != 206 && step.status != 200) {
      on_body(step.status, "<html>nope</html>", 17);
      r.completed = true;
      return r;
    }
    std::string body = payload_;
    if (step.flip_at >= 0) body[static_cast<std::size_t>(step.flip_at)] ^= 0x01;
    body += step.extra;
    const std::size_t from = step.honor_range ? static_cast<std::size_t>(offset) : 0;
    std::size_t to = body.size();
    if (step.drop_after >= 0) to = std::min(to, from + static_cast<std::size_t>(step.drop_after));
    if (step.short_by > 0) to -= static_cast<std::size_t>(step.short_by);
    for (std::size_t at = from; at < to;) {
      if (!keep_going()) {
        r.interrupted = true;
        return r;
      }
      const std::size_t n = std::min<std::size_t>(to - at, 8192);
      if (!on_body(step.status, body.data() + at, n)) return r;  // the fetch refused the bytes
      at += n;
    }
    r.completed = step.drop_after < 0;
    r.code = r.completed ? 0 : 18;  // a body shorter than promised
    return r;
  }

  std::vector<std::uint64_t> offsets;

 private:
  std::string payload_;
  std::vector<Step> steps_;
};

// Bytes that do not compress, so the zip is large enough for the byte offsets the tests use.
std::string Noise(std::size_t n) {
  std::string s;
  std::uint32_t x = 12345;
  for (std::size_t i = 0; i < n; ++i) {
    x = x * 1664525u + 1013904223u;
    s.push_back(static_cast<char>(x >> 24));
  }
  return s;
}

std::string GoodZip() {
  return BuildZip({{"_data/", "", 0},
                   {"_data/5932408047/", "", 0},
                   {"_data/5932408047/rad15/", "", 0},
                   {"_data/5932408047/rad15/android/", "", 0},
                   {"_data/5932408047/rad15/android/manifests/", "", 0},
                   {"_data/5932408047/rad15/android/manifests/48037dc70b0ecab2", Pattern(20000), 8},
                   {"_data/5932408047/rad15/android/packages/", "", 0},
                   {"_data/5932408047/rad15/android/packages/48037dc70b0ecab2_0", Noise(300000), 8}});
}

struct Rig {
  Scratch scratch;
  std::string zip;
  FetchConfig config;
  std::vector<std::string> events;
  int backoffs = 0;

  explicit Rig(const std::string& zip_bytes = GoodZip()) : zip(zip_bytes) {
    config.root = scratch.Path("root");
    config.sha256 = Sha256Hex(zip.data(), zip.size());
    config.bytes = zip.size();
    config.extracted_bytes = 1000;
    config.max_attempts = 4;
    config.utc_stamp = "20261022T000000Z";
    config.available_bytes = [](const std::string&) { return std::uint64_t{1} << 40; };
    config.backoff = [this](int) { ++backoffs; };
    config.on_event = [this](const Event& e) {
      std::string line = e.name;
      for (const auto& f : e.fields) line += " " + f.first + "=" + f.second;
      events.push_back(line);
    };
  }
  std::string Part() const { return config.root + "/" + kPartName; }
  bool HasEvent(const std::string& needle) const {
    for (const std::string& e : events) {
      if (e.find(needle) != std::string::npos) return true;
    }
    return false;
  }
};

const char* kManifest = "_data/5932408047/rad15/android/manifests/48037dc70b0ecab2";

}  // namespace

TEST(FetchesVerifiesExtractsAndSwaps) {
  Rig rig;
  ScriptedTransport net(rig.zip, {Step{}});
  const FetchResult r = RunFetch(rig.config, net);
  CHECK_EQ(r.status, FetchStatus::kOk);
  CHECK_EQ(r.attempts, 1);
  CHECK_EQ(net.offsets.size(), static_cast<std::size_t>(1));
  CHECK_EQ(net.offsets[0], static_cast<std::uint64_t>(0));
  CHECK_EQ(ReadFile(rig.config.root + "/" + kManifest).size(), static_cast<std::size_t>(20000));
  CHECK(!Exists(rig.Part()));
  CHECK(!Exists(StagingPath(rig.config.root, rig.config.sha256)));
  CHECK(rig.HasEvent("data_fetch outcome=verified"));
  CHECK(rig.HasEvent("data_extract outcome=ok"));
  CHECK(rig.HasEvent("data_swap outcome=ok"));
}

TEST(EventsNeverCarryTheUrl) {
  Rig rig;
  rig.config.url = "https://example.invalid/_data.zip?token=SECRET";
  ScriptedTransport net(rig.zip, {Step{}});
  CHECK_EQ(RunFetch(rig.config, net).status, FetchStatus::kOk);
  for (const std::string& e : rig.events) CHECK(e.find("SECRET") == std::string::npos && e.find("example") == std::string::npos);
}

TEST(ResumesFromTheSizeOfTheExistingPart) {
  Rig rig;
  CHECK_EQ(::mkdir(rig.config.root.c_str(), 0755), 0);
  WriteFile(rig.Part(), rig.zip.substr(0, 1500));
  ScriptedTransport net(rig.zip, {Step{}});
  CHECK_EQ(RunFetch(rig.config, net).status, FetchStatus::kOk);
  CHECK_EQ(net.offsets.size(), static_cast<std::size_t>(1));
  if (!net.offsets.empty()) CHECK_EQ(net.offsets[0], static_cast<std::uint64_t>(1500));
}

TEST(ADroppedConnectionResumesWhereItStopped) {
  Rig rig;
  Step drop;
  drop.drop_after = 5000;
  ScriptedTransport net(rig.zip, {drop, Step{}});
  const FetchResult r = RunFetch(rig.config, net);
  CHECK_EQ(r.status, FetchStatus::kOk);
  CHECK_EQ(r.attempts, 2);
  CHECK_EQ(net.offsets.size(), static_cast<std::size_t>(2));
  if (net.offsets.size() == 2) CHECK_EQ(net.offsets[1], static_cast<std::uint64_t>(5000));
  CHECK_EQ(rig.backoffs, 1);
}

TEST(AServerThatIgnoresRangeRestartsTheStream) {
  Rig rig;
  CHECK_EQ(::mkdir(rig.config.root.c_str(), 0755), 0);
  WriteFile(rig.Part(), rig.zip.substr(0, 3000));
  Step ignore;
  ignore.status = 200;
  ignore.honor_range = false;
  ScriptedTransport net(rig.zip, {ignore});
  const FetchResult r = RunFetch(rig.config, net);
  CHECK_EQ(r.status, FetchStatus::kOk);
  CHECK_EQ(r.attempts, 1);  // restarted inside the attempt, not by a failed attempt and a retry
  CHECK_EQ(net.offsets.size(), static_cast<std::size_t>(1));
  CHECK_EQ(ReadFile(rig.config.root + "/" + kManifest).size(), static_cast<std::size_t>(20000));
}

TEST(ATamperedBodyNeverReachesData) {
  Rig rig;
  Step bad;
  bad.flip_at = 700;
  ScriptedTransport net(rig.zip, {bad});
  const FetchResult r = RunFetch(rig.config, net);
  CHECK_EQ(r.status, FetchStatus::kRetriesExhausted);
  CHECK_EQ(r.detail, std::string("sha256_mismatch"));
  CHECK_EQ(r.attempts, 4);
  CHECK(!Exists(rig.config.root + "/_data"));
  CHECK(!Exists(rig.Part()));  // the mismatching part is deleted
  // each attempt started from zero, because the mismatch deleted the part
  for (const std::uint64_t o : net.offsets) CHECK_EQ(o, static_cast<std::uint64_t>(0));
}

TEST(ATamperedBodyThenAGoodOneSucceeds) {
  Rig rig;
  Step bad;
  bad.flip_at = 700;
  ScriptedTransport net(rig.zip, {bad, Step{}});
  const FetchResult r = RunFetch(rig.config, net);
  CHECK_EQ(r.status, FetchStatus::kOk);
  CHECK_EQ(r.attempts, 2);
}

TEST(NoNetworkGivesRetriesExhaustedAndNoData) {
  Rig rig;
  Step down;
  down.completes = false;
  ScriptedTransport net(rig.zip, {down});
  const FetchResult r = RunFetch(rig.config, net);
  CHECK_EQ(r.status, FetchStatus::kRetriesExhausted);
  CHECK_EQ(r.attempts, 4);
  CHECK_EQ(rig.backoffs, 3);
  CHECK(r.detail.find("transport_7") == 0);
  CHECK(!Exists(rig.config.root + "/_data"));
}

TEST(AnHttpErrorCountsAsAFailedAttempt) {
  Rig rig;
  Step gone;
  gone.status = 404;
  ScriptedTransport net(rig.zip, {gone});
  const FetchResult r = RunFetch(rig.config, net);
  CHECK_EQ(r.status, FetchStatus::kRetriesExhausted);
  CHECK_EQ(r.detail, std::string("http_404"));
  CHECK(!Exists(rig.Part()) || ReadFile(rig.Part()).empty());  // the error page was not written into the part
}

TEST(ABodyLongerThanThePinIsRefused) {
  Rig rig;
  Step longer;
  longer.extra = "trailing bytes";
  ScriptedTransport net(rig.zip, {longer});
  const FetchResult r = RunFetch(rig.config, net);
  CHECK_EQ(r.status, FetchStatus::kRetriesExhausted);
  CHECK_EQ(r.detail, std::string("body_longer_than_pin"));
  CHECK(!Exists(rig.config.root + "/_data"));
}

TEST(NoSpaceStopsBeforeAnyRequest) {
  Rig rig;
  rig.config.available_bytes = [](const std::string&) { return std::uint64_t{10}; };
  ScriptedTransport net(rig.zip, {Step{}});
  const FetchResult r = RunFetch(rig.config, net);
  CHECK_EQ(r.status, FetchStatus::kNoSpace);
  CHECK(net.offsets.empty());
  CHECK(rig.HasEvent("data_fetch outcome=no_space"));
}

TEST(ReplacingNeedsTwiceTheSpace) {
  Rig rig;
  const std::uint64_t one = rig.config.bytes + rig.config.extracted_bytes;
  rig.config.available_bytes = [one](const std::string&) { return one + 1; };
  ScriptedTransport net(rig.zip, {Step{}});
  CHECK_EQ(RunFetch(rig.config, net).status, FetchStatus::kOk);  // enough for one tree
  Rig again;
  again.config.replacing = true;
  again.config.available_bytes = [one](const std::string&) { return one + 1; };
  ScriptedTransport net2(again.zip, {Step{}});
  CHECK_EQ(RunFetch(again.config, net2).status, FetchStatus::kNoSpace);
}

TEST(ASpaceCheckCountsTheBytesAlreadyInThePart) {
  Rig rig;
  CHECK_EQ(::mkdir(rig.config.root.c_str(), 0755), 0);
  WriteFile(rig.Part(), rig.zip.substr(0, rig.zip.size() - 10));
  rig.config.available_bytes = [&rig](const std::string&) { return rig.config.extracted_bytes + 10; };
  ScriptedTransport net(rig.zip, {Step{}});
  CHECK_EQ(RunFetch(rig.config, net).status, FetchStatus::kOk);
}

TEST(ReplacementKeepsTheOldTreeAside) {
  Rig rig;
  CHECK_EQ(::mkdir(rig.config.root.c_str(), 0755), 0);
  CHECK_EQ(::mkdir((rig.config.root + "/_data").c_str(), 0755), 0);
  WriteFile(rig.config.root + "/_data/old", "old");
  rig.config.replacing = true;
  ScriptedTransport net(rig.zip, {Step{}});
  const FetchResult r = RunFetch(rig.config, net);
  CHECK_EQ(r.status, FetchStatus::kOk);
  CHECK_EQ(r.replaced_to, rig.config.root + "/_data.replaced-20261022T000000Z");
  CHECK_EQ(ReadFile(r.replaced_to + "/old"), std::string("old"));
  CHECK(Exists(rig.config.root + "/_data/5932408047"));
}

TEST(AHostileArchiveWithTheRightDigestLeavesNoData) {
  Rig rig(BuildZip({{"_data/ok", "x", 0}, {"_data/../evil", "x", 0}}));
  ScriptedTransport net(rig.zip, {Step{}});
  const FetchResult r = RunFetch(rig.config, net);
  CHECK_EQ(r.status, FetchStatus::kExtractFailed);
  CHECK(r.detail.find("bad_entry_path") == 0);
  CHECK(!Exists(rig.config.root + "/_data"));
  CHECK(!Exists(StagingPath(rig.config.root, rig.config.sha256)));
  CHECK(!Exists(rig.scratch.Path("evil")));
  CHECK(!Exists(rig.Part()));
}

TEST(AnEntryThatFailsMidExtractionLeavesNoStagingAndNoData) {
  SrcEntry bad{"_data/b", Pattern(5000), 8};
  bad.crc_delta = 1;
  Rig rig(BuildZip({{"_data/a", "first", 0}, bad}));
  ScriptedTransport net(rig.zip, {Step{}});
  const FetchResult r = RunFetch(rig.config, net);
  CHECK_EQ(r.status, FetchStatus::kExtractFailed);
  CHECK(r.detail.find("crc_mismatch") == 0);
  CHECK(!Exists(StagingPath(rig.config.root, rig.config.sha256)));
  CHECK(!Exists(OwnerMarkerPath(StagingPath(rig.config.root, rig.config.sha256))));
  CHECK(!Exists(rig.config.root + "/_data"));
}

TEST(CancelledBeforeTheStartMakesNoRequest) {
  Rig rig;
  rig.config.keep_going = [] { return false; };
  ScriptedTransport net(rig.zip, {Step{}});
  CHECK_EQ(RunFetch(rig.config, net).status, FetchStatus::kCancelled);
  CHECK(net.offsets.empty());
}

TEST(CancelEndsTheTransferAndKeepsThePart) {
  Rig rig;
  int polls = 0;
  rig.config.keep_going = [&polls] { return ++polls < 4; };
  ScriptedTransport net(rig.zip, {Step{}});
  const FetchResult r = RunFetch(rig.config, net);
  CHECK_EQ(r.status, FetchStatus::kCancelled);
  CHECK(!Exists(rig.config.root + "/_data"));
  CHECK(Exists(rig.Part()));
}

TEST(OurStagingIsCleanedAndAPlayersDirectoriesAreKept) {
  Rig rig;
  CHECK_EQ(::mkdir(rig.config.root.c_str(), 0755), 0);
  const std::string ours = rig.config.root + "/_data.staging-aaaaaaaaaaaa";
  CHECK(MarkOwned(ours));
  CHECK_EQ(::mkdir(ours.c_str(), 0755), 0);
  const std::string notes = rig.config.root + "/_data.staging-notes";
  CHECK_EQ(::mkdir(notes.c_str(), 0755), 0);
  WriteFile(notes + "/n", "notes");
  const std::string backup = rig.config.root + "/_data.replaced-my-backup";
  CHECK_EQ(::mkdir(backup.c_str(), 0755), 0);
  WriteFile(backup + "/b", "backup");
  ScriptedTransport net(rig.zip, {Step{}});
  CHECK_EQ(RunFetch(rig.config, net).status, FetchStatus::kOk);
  CHECK(!Exists(ours));
  CHECK(!Exists(OwnerMarkerPath(ours)));
  CHECK_EQ(ReadFile(notes + "/n"), std::string("notes"));
  CHECK_EQ(ReadFile(backup + "/b"), std::string("backup"));
  CHECK(rig.HasEvent("step=cleanup removed=1"));
}

TEST(AFetchNeverDeletesThePlayersReplacedData) {
  // Replacing: the old tree goes aside, the fetch succeeds, and a later fetch run does not delete it.
  Rig rig;
  CHECK_EQ(::mkdir(rig.config.root.c_str(), 0755), 0);
  CHECK_EQ(::mkdir((rig.config.root + "/_data").c_str(), 0755), 0);
  WriteFile(rig.config.root + "/_data/old", "old");
  rig.config.replacing = true;
  ScriptedTransport net(rig.zip, {Step{}});
  const FetchResult first = RunFetch(rig.config, net);
  CHECK_EQ(first.status, FetchStatus::kOk);
  ScriptedTransport again(rig.zip, {Step{}});
  rig.config.replacing = false;
  rig.config.utc_stamp = "20261023T000000Z";
  CHECK_EQ(RunFetch(rig.config, again).status, FetchStatus::kOk);
  CHECK_EQ(ReadFile(first.replaced_to + "/old"), std::string("old"));
}

TEST(ACrashBetweenTheSwapRenamesIsRecoveredEvenWhenTheFetchFails) {
  Rig rig;
  CHECK_EQ(::mkdir(rig.config.root.c_str(), 0755), 0);
  const std::string aside = rig.config.root + "/_data.replaced-20261021T000000Z";
  CHECK(MarkOwned(aside));
  CHECK_EQ(::mkdir(aside.c_str(), 0755), 0);
  WriteFile(aside + "/old", "old");
  Step down;
  down.completes = false;
  ScriptedTransport net(rig.zip, {down});
  const FetchResult r = RunFetch(rig.config, net);
  CHECK_EQ(r.status, FetchStatus::kRetriesExhausted);
  CHECK_EQ(ReadFile(rig.config.root + "/_data/old"), std::string("old"));
  CHECK(rig.HasEvent("step=recover"));
}

// A disk that fails at extraction is not the zip's fault: the verified zip stays, so the next run extracts it without a
// download. The failing write is a file-size limit (RLIMIT_FSIZE, SIGXFSZ ignored): write() answers EFBIG, as a full
// disk answers ENOSPC.
TEST(ADiskFailureAtExtractKeepsTheVerifiedZipAndTheNextRunDoesNotDownloadIt) {
  Rig rig(BuildZip({{"_data/a", "first", 0}, {"_data/big", std::string(200000, 'a'), 8}}));
  CHECK(rig.zip.size() < 2000);  // the zip is small, the entry it inflates to is not
  ScriptedTransport net(rig.zip, {Step{}});
  struct rlimit old_limit;
  CHECK_EQ(::getrlimit(RLIMIT_FSIZE, &old_limit), 0);
  void (*old_handler)(int) = ::signal(SIGXFSZ, SIG_IGN);
  struct rlimit small = old_limit;
  small.rlim_cur = 4096;
  CHECK_EQ(::setrlimit(RLIMIT_FSIZE, &small), 0);
  const FetchResult r = RunFetch(rig.config, net);
  const bool kept = Exists(rig.Part());
  const FetchResult again = RunFetch(rig.config, net);
  ::setrlimit(RLIMIT_FSIZE, &old_limit);
  ::signal(SIGXFSZ, old_handler);
  CHECK_EQ(r.status, FetchStatus::kExtractFailed);
  CHECK(r.detail.find("write_failed") == 0);
  CHECK(kept);
  CHECK(!Exists(StagingPath(rig.config.root, rig.config.sha256)));
  CHECK(!Exists(rig.config.root + "/_data"));
  CHECK_EQ(net.offsets.size(), std::size_t{1});  // the second run read the kept zip: no request
  CHECK_EQ(again.status, FetchStatus::kExtractFailed);
  CHECK(Exists(rig.Part()));
  // The disk recovers: the same zip is extracted with no download.
  const FetchResult healed = RunFetch(rig.config, net);
  CHECK_EQ(healed.status, FetchStatus::kOk);
  CHECK_EQ(net.offsets.size(), std::size_t{1});
  CHECK(Exists(rig.config.root + "/_data/big"));
  CHECK(!Exists(rig.Part()));
}

TEST(AStagingDirectoryThatIsNotOursIsNeverDeleted) {
  Rig rig;
  CHECK_EQ(::mkdir(rig.config.root.c_str(), 0755), 0);
  const std::string staging = StagingPath(rig.config.root, rig.config.sha256);
  CHECK_EQ(::mkdir(staging.c_str(), 0755), 0);
  WriteFile(staging + "/theirs", "theirs");
  ScriptedTransport net(rig.zip, {Step{}});
  const FetchResult r = RunFetch(rig.config, net);
  CHECK_EQ(r.status, FetchStatus::kExtractFailed);
  CHECK_EQ(r.detail, std::string("staging_not_ours"));
  CHECK_EQ(ReadFile(staging + "/theirs"), std::string("theirs"));
}

TEST(NoMarkerIsLeftBehindAfterASuccessfulFetch) {
  Rig rig;
  ScriptedTransport net(rig.zip, {Step{}});
  CHECK_EQ(RunFetch(rig.config, net).status, FetchStatus::kOk);
  CHECK(!Exists(OwnerMarkerPath(rig.Part())));
  CHECK(!Exists(OwnerMarkerPath(StagingPath(rig.config.root, rig.config.sha256))));
}

TEST(ThePartIsMarkedWhileItIsKept) {
  Rig rig;
  Step drop;
  drop.drop_after = 1000;
  ScriptedTransport net(rig.zip, {drop, drop, drop, drop});
  rig.config.max_attempts = 1;
  CHECK_EQ(RunFetch(rig.config, net).status, FetchStatus::kRetriesExhausted);
  CHECK(Exists(rig.Part()));
  CHECK(IsOwned(rig.Part()));
}

TEST(ADigestMismatchThenAResumeLeavesTheMarkerOnTheRecreatedPart) {
  Rig rig;
  Step bad;
  bad.flip_at = 700;
  Step drop;
  drop.drop_after = 1000;
  ScriptedTransport net(rig.zip, {bad, drop});
  rig.config.max_attempts = 2;
  CHECK_EQ(RunFetch(rig.config, net).status, FetchStatus::kRetriesExhausted);
  // Attempt 1 ended in a mismatch and deleted the part; attempt 2 created it again and was cut off: it is kept for a
  // resume, so it carries the marker that says it is ours.
  CHECK(Exists(rig.Part()));
  CHECK(IsOwned(rig.Part()));
}

TEST(AShortBodyKeepsThePartAndTheNextAttemptResumesFromIt) {
  Rig rig;
  Step shorter;
  shorter.short_by = 5000;
  ScriptedTransport net(rig.zip, {shorter, Step{}});
  const FetchResult r = RunFetch(rig.config, net);
  CHECK_EQ(r.status, FetchStatus::kOk);
  CHECK_EQ(r.attempts, 2);
  CHECK_EQ(net.offsets.size(), std::size_t{2});
  CHECK_EQ(net.offsets[0], static_cast<std::uint64_t>(0));
  CHECK_EQ(net.offsets[1], static_cast<std::uint64_t>(rig.zip.size() - 5000));  // resumed, not restarted
  CHECK(Exists(rig.config.root + "/" + kManifest));
}

TEST(AShortBodyThatNeverCompletesLeavesTheMarkedPart) {
  Rig rig;
  Step shorter;
  shorter.short_by = 5000;
  ScriptedTransport net(rig.zip, {shorter, shorter, shorter});
  rig.config.max_attempts = 2;
  CHECK_EQ(RunFetch(rig.config, net).status, FetchStatus::kRetriesExhausted);
  CHECK(Exists(rig.Part()));
  CHECK(IsOwned(rig.Part()));
  CHECK(!Exists(rig.config.root + "/_data"));
}

TEST(TheDefaultBackoffGrowsFromTwoSecondsToACap) {
  CHECK_EQ(DefaultBackoffMs(1), 0u);
  CHECK_EQ(DefaultBackoffMs(2), 2000u);
  CHECK_EQ(DefaultBackoffMs(3), 4000u);
  CHECK_EQ(DefaultBackoffMs(4), 8000u);
  CHECK_EQ(DefaultBackoffMs(5), 16000u);
  CHECK_EQ(DefaultBackoffMs(6), 30000u);
  CHECK_EQ(DefaultBackoffMs(60), 30000u);
}

TEST(WithoutACallerBackoffTheFetchSleepsTheDefaultBeforeEachRetry) {
  Rig rig;
  rig.config.backoff = nullptr;
  std::vector<unsigned> slept;
  rig.config.sleep_ms = [&slept](unsigned ms) { slept.push_back(ms); };
  Step down;
  down.completes = false;
  ScriptedTransport net(rig.zip, {down});
  CHECK_EQ(RunFetch(rig.config, net).status, FetchStatus::kRetriesExhausted);  // 4 attempts: waits before 2, 3 and 4
  unsigned total = 0;
  for (const unsigned ms : slept) {
    CHECK(ms > 0 && ms <= kBackoffSliceMs);
    total += ms;
  }
  CHECK_EQ(total, 2000u + 4000u + 8000u);
}

TEST(ACancelEndsTheBackoffWait) {
  Rig rig;
  rig.config.backoff = nullptr;
  int sleeps = 0;
  rig.config.sleep_ms = [&sleeps](unsigned) { ++sleeps; };
  rig.config.keep_going = [&sleeps] { return sleeps < 3; };
  Step down;
  down.completes = false;
  ScriptedTransport net(rig.zip, {down});
  CHECK_EQ(RunFetch(rig.config, net).status, FetchStatus::kCancelled);
  CHECK_EQ(sleeps, 3);  // not the 8 slices of the first wait
  CHECK_EQ(net.offsets.size(), std::size_t{1});
}

TEST(APartLongerThanThePinIsDiscarded) {
  Rig rig;
  CHECK_EQ(::mkdir(rig.config.root.c_str(), 0755), 0);
  WriteFile(rig.Part(), rig.zip + "extra");
  ScriptedTransport net(rig.zip, {Step{}});
  CHECK_EQ(RunFetch(rig.config, net).status, FetchStatus::kOk);
  CHECK(!net.offsets.empty());
  if (!net.offsets.empty()) CHECK_EQ(net.offsets[0], static_cast<std::uint64_t>(0));
}

TEST(ACompletePartIsVerifiedWithoutARequest) {
  Rig rig;
  CHECK_EQ(::mkdir(rig.config.root.c_str(), 0755), 0);
  WriteFile(rig.Part(), rig.zip);
  ScriptedTransport net(rig.zip, {Step{}});
  CHECK_EQ(RunFetch(rig.config, net).status, FetchStatus::kOk);
  CHECK(net.offsets.empty());
}

TEST(ARootThatCannotBeCreatedIsBadRoot) {
  Rig rig;
  WriteFile(rig.scratch.Path("file"), "x");
  rig.config.root = rig.scratch.Path("file/sub");
  ScriptedTransport net(rig.zip, {Step{}});
  CHECK_EQ(RunFetch(rig.config, net).status, FetchStatus::kBadRoot);
}

TEST(ProgressReportsTheBytesWritten) {
  Rig rig;
  std::uint64_t last = 0;
  rig.config.on_progress = [&last](std::uint64_t d, std::uint64_t) { last = d; };
  ScriptedTransport net(rig.zip, {Step{}});
  CHECK_EQ(RunFetch(rig.config, net).status, FetchStatus::kOk);
  CHECK_EQ(last, static_cast<std::uint64_t>(rig.zip.size()));
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
