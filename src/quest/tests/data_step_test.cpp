// Host tests for the first-run data step (src/quest/data/data_step.cpp): the three outcomes of the data-root decision
// (use, fetch, replace), the visible message when the fetch cannot be had (no network, no space, a tampered zip) and
// that it names the path, the build and the action, and that existing data is never fetched again.

#include <algorithm>
#include <cstdint>
#include <set>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "quest/data/data_step.h"
#include "quest/data/sha256.h"
#include "quest/tests/data_test_support.h"
#include "quest/tests/mini_test.h"

using namespace nevr_quest::data;
using namespace data_test;

namespace {

constexpr const char* kBuild = "b243509c08ce677aeb95fa348016949b3fc45230";
constexpr const char* kManifestRel = "_data/5932408047/rad15/android/manifests/48037dc70b0ecab2";

// The paths the fake filesystem answers true for.
std::set<std::string>* g_present = nullptr;

bool FakeExists(const char* path) noexcept { return g_present != nullptr && g_present->count(path) != 0; }

std::string Media(const std::string& rel = "") {
  return std::string(nevr_quest::data_root::kMediaRoot) + (rel.empty() ? "" : "/" + rel);
}
std::string Legacy(const std::string& rel = "") {
  return std::string(nevr_quest::data_root::kLegacyRoot) + (rel.empty() ? "" : "/" + rel);
}

class OneShotTransport : public Transport {
 public:
  OneShotTransport(std::string payload, bool network) : payload_(std::move(payload)), network_(network) {}
  TransportResult GetRange(const std::string&, std::uint64_t offset, const BodyFn& on_body,
                           const KeepGoingFn&) override {
    ++calls;
    TransportResult r;
    if (!network_) {
      r.code = 6;  // could not resolve host
      return r;
    }
    r.http_status = 206;
    for (std::size_t at = static_cast<std::size_t>(offset); at < payload_.size();) {
      const std::size_t n = std::min<std::size_t>(payload_.size() - at, 8192);
      if (!on_body(206, payload_.data() + at, n)) return r;
      at += n;
    }
    r.completed = true;
    return r;
  }
  int calls = 0;

 private:
  std::string payload_;
  bool network_;
};

std::string GoodZip() {
  return BuildZip({{"_data/", "", 0},
                   {"_data/5932408047/", "", 0},
                   {"_data/5932408047/rad15/", "", 0},
                   {"_data/5932408047/rad15/android/", "", 0},
                   {"_data/5932408047/rad15/android/manifests/", "", 0},
                   {"_data/5932408047/rad15/android/manifests/48037dc70b0ecab2", Pattern(20000), 8}});
}

struct Rig {
  Scratch scratch;
  std::string zip;
  std::set<std::string> present;
  StepConfig config;
  std::vector<std::string> shown;   // what present() received, in order
  std::vector<std::string> order;   // "present" and "hold", in call order
  std::vector<std::string> events;

  explicit Rig(const std::string& zip_bytes = GoodZip()) : zip(zip_bytes) {
    g_present = &present;
    config.exists = &FakeExists;
    config.build_id = kBuild;
    config.fetch_root_override = scratch.Path("root");
    config.fetch.sha256 = Sha256Hex(zip.data(), zip.size());
    config.fetch.bytes = zip.size();
    config.fetch.extracted_bytes = 1000;
    config.fetch.max_attempts = 2;
    config.fetch.utc_stamp = "20261022T000000Z";
    config.fetch.available_bytes = [](const std::string&) { return std::uint64_t{1} << 40; };
    config.fetch.backoff = [](int) {};
    config.present = [this](const std::string& m) {
      shown.push_back(m);
      order.push_back("present");
    };
    config.hold = [this] { order.push_back("hold"); };
    config.on_step = [this](const Event& e) {
      std::string line = e.name;
      for (const auto& f : e.fields) line += " " + f.first + "=" + f.second;
      events.push_back(line);
    };
  }
  ~Rig() { g_present = nullptr; }
  std::string Root() const { return config.fetch_root_override; }
  // An old `_data` of another build in the fetch root.
  void PutOldData() const {
    ::mkdir(Root().c_str(), 0755);
    ::mkdir((Root() + "/_data").c_str(), 0755);
    WriteFile(Root() + "/_data/old", "old");
  }
};

bool Contains(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }

}  // namespace

TEST(DataInTheMediaRootIsUsedAndNotFetched) {
  Rig rig;
  rig.present = {Media(), Media("_data"), Media(kManifestRel)};
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kUsed);
  CHECK_EQ(r.action, DataAction::kUse);
  CHECK_EQ(r.root, Media());
  CHECK_EQ(net.calls, 0);
  CHECK(rig.shown.empty());
  CHECK_EQ(rig.events.size(), static_cast<std::size_t>(1));
  CHECK(Contains(rig.events[0], "data_check action=use source=android_media"));
}

TEST(DataInTheLegacyRootIsUsedAndNotFetched) {
  Rig rig;
  rig.present = {Legacy("_data"), Legacy(kManifestRel)};
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kUsed);
  CHECK_EQ(r.root, Legacy());
  CHECK_EQ(net.calls, 0);
}

TEST(AnUnmarkedMediaRootIsFetchedInto) {
  // The media directory exists and holds no `_data` (a first fetch that was interrupted, or a directory Android made).
  Rig rig;
  rig.present = {Media()};
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kFetched);
  CHECK_EQ(r.action, DataAction::kFetch);
  CHECK_EQ(net.calls, 1);
  CHECK(Contains(rig.events[0], "action=fetch source=android_media_unmarked"));
}

TEST(NoDataFetchesVerifiesAndPlacesIt) {
  Rig rig;
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kFetched);
  CHECK_EQ(r.action, DataAction::kFetch);
  CHECK_EQ(net.calls, 1);
  CHECK(Exists(rig.Root() + "/" + kManifestRel));
  CHECK(rig.shown.empty());
  CHECK(Contains(rig.events[0], "action=fetch source=none"));
}

TEST(DataOfAnotherBuildIsReplacedAndTheOldOneKeptAside) {
  Rig rig;
  rig.present = {Media(), Media("_data")};  // `_data` there, the pinned manifest is not
  rig.PutOldData();
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kFetched);
  CHECK_EQ(r.action, DataAction::kReplace);
  CHECK(Exists(rig.Root() + "/" + kManifestRel));
  CHECK_EQ(ReadFile(rig.Root() + "/_data.replaced-20261022T000000Z/old"), std::string("old"));
}

TEST(NoNetworkShowsTheFloorMessageThenHoldsIt) {
  Rig rig;
  OneShotTransport net(rig.zip, false);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kFailed);
  CHECK_EQ(rig.shown.size(), static_cast<std::size_t>(1));
  CHECK_EQ(rig.shown[0], r.message);
  CHECK(Contains(r.message, rig.Root() + "/_data"));  // the path
  CHECK(Contains(r.message, kBuild));                 // the build
  CHECK(Contains(r.message, "Check the network connection and start the game again"));  // the action
  CHECK_EQ(rig.order.size(), static_cast<std::size_t>(2));
  CHECK_EQ(rig.order[0], std::string("present"));
  CHECK_EQ(rig.order[1], std::string("hold"));
  CHECK(!Exists(rig.Root() + "/_data"));
}

TEST(NoSpaceNamesTheFreeAndNeededMiB) {
  Rig rig;
  rig.config.fetch.extracted_bytes = std::uint64_t{600} * 1024 * 1024;
  rig.config.fetch.available_bytes = [](const std::string&) { return std::uint64_t{300} * 1024 * 1024; };
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kFailed);
  CHECK_EQ(r.status, FetchStatus::kNoSpace);
  CHECK_EQ(net.calls, 0);
  CHECK(Contains(r.message, "300 MiB free"));
  CHECK(Contains(r.message, "601 MiB needed"));  // (zip + 1000 bytes) rounded up
}

TEST(ATamperedZipIsRefusedAndNothingIsPlaced) {
  Rig rig;
  std::string tampered = rig.zip;
  tampered[tampered.size() / 2] ^= 0x01;
  OneShotTransport net(tampered, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kFailed);
  CHECK_EQ(r.status, FetchStatus::kRetriesExhausted);
  CHECK(!Exists(rig.Root() + "/_data"));
  CHECK(Contains(r.message, "The download failed"));
  CHECK_EQ(rig.shown.size(), static_cast<std::size_t>(1));
}

TEST(AReplaceThatFailsNamesTheOldDataAndLeavesIt) {
  Rig rig;
  rig.present = {Media(), Media("_data")};
  rig.PutOldData();
  OneShotTransport net(rig.zip, false);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kFailed);
  CHECK(Contains(r.message, "left as it is"));
  CHECK_EQ(ReadFile(rig.Root() + "/_data/old"), std::string("old"));
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
