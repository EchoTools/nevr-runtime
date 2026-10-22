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
#include "quest/data/staging.h"
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
  std::vector<std::string> order;   // "announce", "present" and "hold", in call order
  std::vector<std::string> events;

  explicit Rig(const std::string& zip_bytes = GoodZip()) : zip(zip_bytes) {
    g_present = &present;
    config.exists = &FakeExists;
    config.build_id = kBuild;
    config.fetch_root_override = scratch.Path("root");
    config.recovery_roots = {scratch.Path("root")};  // never the real /sdcard roots
    config.fetch.sha256 = Sha256Hex(zip.data(), zip.size());
    config.fetch.bytes = zip.size();
    config.fetch.extracted_bytes = 1000;
    config.fetch.max_attempts = 2;
    config.fetch.utc_stamp = "20261022T000000Z";
    config.fetch.available_bytes = [](const std::string&) { return std::uint64_t{1} << 40; };
    config.fetch.backoff = [](int) {};
    config.announce = [this] { order.push_back("announce"); };
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
  CHECK(rig.order.empty());  // data that is there is not announced
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
  CHECK_EQ(rig.order.size(), static_cast<std::size_t>(1));  // the announcement only
  CHECK_EQ(rig.order[0], std::string("announce"));
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
  CHECK_EQ(rig.order.size(), static_cast<std::size_t>(3));
  CHECK_EQ(rig.order[0], std::string("announce"));  // before the download
  CHECK_EQ(rig.order[1], std::string("present"));
  CHECK_EQ(rig.order[2], std::string("hold"));
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
  CHECK(Contains(r.message, rig.Root() + "/_data"));  // the path, as in every other failure
  // The download notice comes after the space check, so a step that cannot download never announces one.
  CHECK(std::find(rig.order.begin(), rig.order.end(), std::string("announce")) == rig.order.end());
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
  CHECK(Contains(r.message, rig.Root() + "/_data"));
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

// ---- an interrupted install is put right on every root before the decision ----

namespace {

std::string g_media_dir;
std::string g_legacy_dir;

// The two real roots, mapped to scratch directories, answered from the real filesystem.
bool MappedExists(const char* path) noexcept {
  const std::string p = path;
  const std::string media = nevr_quest::data_root::kMediaRoot;
  const std::string legacy = nevr_quest::data_root::kLegacyRoot;
  std::string real = p;
  if (p.compare(0, media.size(), media) == 0) real = g_media_dir + p.substr(media.size());
  else if (p.compare(0, legacy.size(), legacy) == 0) real = g_legacy_dir + p.substr(legacy.size());
  struct stat st;
  return ::stat(real.c_str(), &st) == 0;
}

void MakeTree(const std::string& dir, const std::string& rel_manifest, const std::string& content) {
  std::string at = dir;
  std::size_t from = 0;
  while (true) {
    const std::size_t slash = rel_manifest.find('/', from);
    if (slash == std::string::npos) break;
    at = dir + "/" + rel_manifest.substr(0, slash);
    ::mkdir(at.c_str(), 0755);
    from = slash + 1;
  }
  WriteFile(dir + "/" + rel_manifest, content);
}

// `root` as a crash between the two renames leaves it: the old tree under a marked `_data.replaced-<utc>`, no `_data`,
// and a marked staging directory of an earlier attempt.
void CrashedReplace(const std::string& root) {
  ::mkdir(root.c_str(), 0755);
  const std::string aside = root + "/_data.replaced-20261020T000000Z";
  ::mkdir(aside.c_str(), 0755);
  CHECK(MarkOwned(aside));
  MakeTree(aside, std::string(kManifestRel).substr(std::string("_data/").size()), "manifest");
  const std::string staging = root + "/_data.staging-0123456789ab";
  CHECK(MarkOwned(staging));
  ::mkdir(staging.c_str(), 0755);
  WriteFile(staging + "/half", "half");
}

}  // namespace

TEST(ALegacyReplaceThatCrashedIsRecoveredBeforeTheDecisionAndNothingIsFetched) {
  Rig rig;
  g_media_dir = rig.scratch.Path("media");
  g_legacy_dir = rig.scratch.Path("legacy");
  ::mkdir(g_media_dir.c_str(), 0755);
  CrashedReplace(g_legacy_dir);
  rig.config.exists = &MappedExists;
  rig.config.recovery_roots = {g_media_dir, g_legacy_dir};
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kUsed);
  CHECK_EQ(net.calls, 0);
  CHECK(Exists(g_legacy_dir + "/" + kManifestRel));                       // the old tree is back as `_data`
  CHECK(!Exists(g_legacy_dir + "/_data.replaced-20261020T000000Z"));      // and nothing is left aside
  CHECK(!Exists(g_legacy_dir + "/_data.replaced-20261020T000000Z.nevr-owned"));
  CHECK(!Exists(g_legacy_dir + "/_data.staging-0123456789ab"));           // the staging leftover is cleaned
  CHECK(!Exists(g_legacy_dir + "/_data.staging-0123456789ab.nevr-owned"));
  CHECK(rig.shown.empty());
}

TEST(AMediaReplaceThatCrashedIsRecoveredWhileTheLegacyRootHoldsData) {
  Rig rig;
  g_media_dir = rig.scratch.Path("media");
  g_legacy_dir = rig.scratch.Path("legacy");
  CrashedReplace(g_media_dir);
  ::mkdir(g_legacy_dir.c_str(), 0755);
  ::mkdir((g_legacy_dir + "/_data").c_str(), 0755);
  MakeTree(g_legacy_dir, kManifestRel, "manifest");
  rig.config.exists = &MappedExists;
  rig.config.recovery_roots = {g_media_dir, g_legacy_dir};
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kUsed);
  CHECK_EQ(net.calls, 0);
  CHECK(Exists(g_media_dir + "/" + kManifestRel));  // the media root is recovered too, not only the one chosen
  CHECK(!Exists(g_media_dir + "/_data.replaced-20261020T000000Z"));
  CHECK(!Exists(g_media_dir + "/_data.staging-0123456789ab"));
  bool recovered_event = false;
  for (const std::string& e : rig.events) recovered_event = recovered_event || e.rfind("data_recover ", 0) == 0;
  CHECK(recovered_event);
}

// ---- the legacy root is read only with the storage permission ----

TEST(WithoutTheGrantTheLegacyRootIsNotReadAndTheMediaRootDecides) {
  Rig rig;
  g_media_dir = rig.scratch.Path("media");
  g_legacy_dir = rig.scratch.Path("legacy");
  ::mkdir(g_media_dir.c_str(), 0755);
  ::mkdir(g_legacy_dir.c_str(), 0755);
  ::mkdir((g_legacy_dir + "/_data").c_str(), 0755);
  MakeTree(g_legacy_dir, kManifestRel, "manifest");  // this build's data, in the legacy root only
  rig.config.exists = &MappedExists;
  rig.config.recovery_roots = {g_media_dir};
  {
    rig.config.legacy_root_readable = true;
    OneShotTransport net(rig.zip, true);
    const StepResult r = RunDataStep(rig.config, net);
    CHECK_EQ(r.outcome, StepOutcome::kUsed);  // with the grant: used
    CHECK_EQ(net.calls, 0);
  }
  Rig denied;
  g_media_dir = denied.scratch.Path("media");
  g_legacy_dir = denied.scratch.Path("legacy");
  ::mkdir(g_media_dir.c_str(), 0755);
  ::mkdir(g_legacy_dir.c_str(), 0755);
  ::mkdir((g_legacy_dir + "/_data").c_str(), 0755);
  MakeTree(g_legacy_dir, kManifestRel, "manifest");
  denied.config.exists = &MappedExists;
  denied.config.recovery_roots = {g_media_dir};
  denied.config.legacy_root_readable = false;
  OneShotTransport net(denied.zip, true);
  const StepResult r = RunDataStep(denied.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kFetched);  // without it: the legacy data is not seen, the media root fetches
  bool skipped = false;
  for (const std::string& e : denied.events) skipped = skipped || e.rfind("data_legacy_skipped ", 0) == 0;
  CHECK(skipped);
}

TEST(WithoutTheGrantDataInTheMediaRootIsUsed) {
  Rig rig;
  const std::string media_data = Media("_data");
  rig.present = {Media(), media_data, Media(kManifestRel)};
  rig.config.legacy_root_readable = false;
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kUsed);
  CHECK_EQ(net.calls, 0);
  CHECK(std::string(r.root) == nevr_quest::data_root::kMediaRoot);
}

TEST(WithTheGrantTheLegacyRootIsStillRead) {
  Rig rig;
  rig.present = {Legacy(), Legacy("_data"), Legacy(kManifestRel)};
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kUsed);
  CHECK(std::string(r.root) == nevr_quest::data_root::kLegacyRoot);
}

// The production default: with no list, every candidate the config may read; Y1 (skipping the default) fails here.
TEST(TheRecoveryRootsDefaultToTheCandidatesTheConfigMayRead) {
  StepConfig config;
  std::vector<std::string> roots = RecoveryRootsFor(config);
  CHECK_EQ(roots.size(), static_cast<std::size_t>(2));
  CHECK_EQ(roots[0], std::string(nevr_quest::data_root::kMediaRoot));
  CHECK_EQ(roots[1], std::string(nevr_quest::data_root::kLegacyRoot));
  config.legacy_root_readable = false;
  roots = RecoveryRootsFor(config);
  CHECK_EQ(roots.size(), static_cast<std::size_t>(1));
  CHECK_EQ(roots[0], std::string(nevr_quest::data_root::kMediaRoot));
  config.recovery_roots = {nevr_quest::data_root::kLegacyRoot, "/b"};
  CHECK_EQ(RecoveryRootsFor(config).size(), static_cast<std::size_t>(1));  // a caller's list is filtered too
  config.legacy_root_readable = true;
  CHECK_EQ(RecoveryRootsFor(config).size(), static_cast<std::size_t>(2));
}

// ---- a whole download left beside data in use ----

TEST(AMarkedPartBesideDataInUseIsRemovedAndAnUnmarkedOneIsLeft) {
  Rig rig;
  rig.present = {Media(), Media("_data"), Media(kManifestRel)};
  rig.config.fetch_root_override = rig.scratch.Path("root");
  ::mkdir(rig.Root().c_str(), 0755);
  const std::string part = rig.Root() + "/_data.zip.part";
  WriteFile(part, "whole zip");
  CHECK(MarkOwned(part));
  OneShotTransport net(rig.zip, true);
  RunDataStep(rig.config, net);
  CHECK(!Exists(part));
  CHECK(!Exists(part + ".nevr-owned"));

  Rig other;
  other.present = {Media(), Media("_data"), Media(kManifestRel)};
  ::mkdir(other.Root().c_str(), 0755);
  const std::string theirs = other.Root() + "/_data.zip.part";
  WriteFile(theirs, "a player's file with our name");
  OneShotTransport net2(other.zip, true);
  RunDataStep(other.config, net2);
  CHECK_EQ(ReadFile(theirs), std::string("a player's file with our name"));
}

TEST(TheDownloadNoticeIsGivenOnceAndAfterTheSpaceCheck) {
  Rig rig;
  OneShotTransport net(rig.zip, true);
  RunDataStep(rig.config, net);
  CHECK_EQ(std::count(rig.order.begin(), rig.order.end(), std::string("announce")), 1);
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
