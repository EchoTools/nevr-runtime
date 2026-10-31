// Host tests for the first-run data step (src/quest/data/data_step.cpp): the three outcomes of the data-root decision
// (use, fetch, replace), the visible message when the fetch cannot be had (no network, no space, a tampered zip) and
// that it names the path, the build and the action, and that existing data is never fetched again.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
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
  std::vector<std::string> warns;  // the events that went out at Warning

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
    config.on_step_warn = [this](const Event& e) {
      std::string line = e.name;
      for (const auto& f : e.fields) line += " " + f.first + "=" + f.second;
      warns.push_back(line);
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
  CHECK_EQ(rig.events.size(), static_cast<std::size_t>(2));
  CHECK(Contains(rig.events[0], "data_check action=use source=android_media"));
  CHECK(Contains(rig.events[1], "data_packages "));  // a manifest outside the pinned table: said, not checked
  CHECK(Contains(rig.events[1], "outcome=unpinned"));
  CHECK(rig.warns.empty());
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

// ---- the engine root: the one root the game reads this launch ----

TEST(OnlyTheEngineRootCountsAsData) {
  // This build's data is in the media root, and the engine reads the legacy root: the media data is not the game's.
  Rig rig;
  rig.present = {Media(), Media("_data"), Media(kManifestRel)};
  rig.config.engine_root = nevr_quest::data_root::kLegacyRoot;
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kFetched);
  CHECK_EQ(r.action, DataAction::kFetch);
  CHECK_EQ(r.root, Legacy());  // the fetch is for the root the engine reads
  CHECK(Contains(rig.events[0], "action=fetch source=none root=" + Legacy()));
}

TEST(DataInTheEngineRootIsUsedWhateverElseIsThere) {
  Rig rig;
  rig.present = {Legacy("_data"), Legacy(kManifestRel)};
  rig.config.engine_root = nevr_quest::data_root::kLegacyRoot;
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kUsed);
  CHECK_EQ(r.root, Legacy());
  CHECK_EQ(net.calls, 0);
}

// ---- allow_fetch false (a release build): look, tell the player, write nothing ----

TEST(WithoutAllowFetchMissingDataIsPresentedHeldAndNotFetched) {
  Rig rig;
  rig.config.allow_fetch = false;
  rig.config.engine_root = nevr_quest::data_root::kLegacyRoot;
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kMissing);
  CHECK_EQ(net.calls, 0);  // no network at all
  CHECK_EQ(r.message, std::string(kGameDataMissing));
  CHECK_EQ(rig.shown.size(), static_cast<std::size_t>(1));
  CHECK_EQ(rig.shown[0], std::string(kGameDataMissing));
  CHECK_EQ(rig.order.size(), static_cast<std::size_t>(2));  // present, hold: no download notice
  CHECK_EQ(rig.order[0], std::string("present"));
  CHECK_EQ(rig.order[1], std::string("hold"));
  CHECK(!Exists(rig.Root()));  // nothing was created
  CHECK(Contains(rig.events[0], "action=fetch source=none"));
  CHECK(Contains(kGameDataMissing, "Echo Launcher"));
}

TEST(WithoutAllowFetchDataOfAnotherBuildIsMissingAndLeftAlone) {
  Rig rig;
  rig.present = {Legacy(), Legacy("_data")};  // `_data` there, the pinned manifest is not
  rig.config.allow_fetch = false;
  rig.config.engine_root = nevr_quest::data_root::kLegacyRoot;
  rig.PutOldData();
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kMissing);
  CHECK_EQ(r.action, DataAction::kReplace);
  CHECK_EQ(net.calls, 0);
  CHECK_EQ(ReadFile(rig.Root() + "/_data/old"), std::string("old"));
  CHECK(!Exists(rig.Root() + "/_data.replaced-20261022T000000Z"));
}

TEST(WithoutAllowFetchDataThatIsThereIsUsedAndNothingIsRemoved) {
  Rig rig;
  rig.present = {Legacy("_data"), Legacy(kManifestRel)};
  rig.config.allow_fetch = false;
  rig.config.engine_root = nevr_quest::data_root::kLegacyRoot;
  ::mkdir(rig.Root().c_str(), 0755);
  const std::string part = rig.Root() + "/_data.zip.part";
  WriteFile(part, "whole zip");
  CHECK(MarkOwned(part));
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kUsed);
  CHECK(rig.shown.empty() && rig.order.empty());
  CHECK(Exists(part));  // a build that does not fetch does not clean up after a fetch
  CHECK(Exists(part + ".nevr-owned"));
}

TEST(WithoutAllowFetchAnInterruptedInstallIsNotRecovered) {
  Rig rig;
  g_media_dir = rig.scratch.Path("media");
  g_legacy_dir = rig.scratch.Path("legacy");
  CrashedReplace(g_legacy_dir);
  rig.config.exists = &MappedExists;
  rig.config.recovery_roots = {g_media_dir, g_legacy_dir};
  rig.config.allow_fetch = false;
  rig.config.engine_root = nevr_quest::data_root::kLegacyRoot;
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kMissing);
  CHECK(Exists(g_legacy_dir + "/_data.replaced-20261020T000000Z"));  // left exactly as it was
  CHECK(Exists(g_legacy_dir + "/_data.staging-0123456789ab"));
  CHECK(!Exists(g_legacy_dir + "/_data"));
}

TEST(TheDataCheckNamesTheVersionFolderAndTheManifestHashOfTheDataItFound) {
  // The root holds the pinned manifest id under another version folder: the line names that folder, not the pin.
  Rig rig;
  const std::string other = "_data/7777777777/rad15/android/manifests/48037dc70b0ecab2";
  rig.present = {Media(), Media("_data"), Media(other)};
  rig.config.list_dir = [](const std::string& dir) {
    return dir == Media("_data") ? std::vector<std::string>{"7777777777"} : std::vector<std::string>{};
  };
  rig.config.read_file = [&](const std::string& path, std::string& bytes) {
    if (path != Media(other)) return false;
    bytes = "another build's manifest";
    return true;
  };
  rig.config.allow_fetch = false;
  OneShotTransport net(rig.zip, true);
  RunDataStep(rig.config, net);
  CHECK(!rig.events.empty());
  CHECK(Contains(rig.events[0], "data_version=7777777777"));
  CHECK(!Contains(rig.events[0], "data_version=5932408047"));
  const std::string bytes = "another build's manifest";
  CHECK(Contains(rig.events[0], "manifest_sha256=" + Sha256Hex(bytes.data(), bytes.size())));
}

TEST(TheDataCheckNamesThePinnedFolderAndItsManifestHashWhenThatIsWhatIsThere) {
  Rig rig;
  rig.present = {Media(), Media("_data"), Media(kManifestRel)};
  rig.config.list_dir = [](const std::string& dir) {
    return dir == Media("_data") ? std::vector<std::string>{"5932408047"} : std::vector<std::string>{};
  };
  rig.config.read_file = [](const std::string&, std::string& bytes) {
    bytes = "pinned manifest";
    return true;
  };
  OneShotTransport net(rig.zip, true);
  RunDataStep(rig.config, net);
  const std::string bytes = "pinned manifest";
  CHECK(Contains(rig.events[0], "action=use"));
  CHECK(Contains(rig.events[0], "data_version=5932408047 manifest_sha256=" + Sha256Hex(bytes.data(), bytes.size())));
}

TEST(TheDataCheckSaysNoneWhenThereIsNoDataAndHashesNothing) {
  Rig rig;
  OneShotTransport net(rig.zip, false);
  RunDataStep(rig.config, net);
  CHECK(Contains(rig.events[0], "data_version=none manifest_sha256=none"));
}

TEST(AFolderNameThatIsNotPlainIsNotLogged) {
  Rig rig;
  rig.present = {Media(), Media("_data")};
  rig.config.list_dir = [](const std::string&) { return std::vector<std::string>{"a b forged=1", "x/y"}; };
  rig.config.allow_fetch = false;
  OneShotTransport net(rig.zip, true);
  RunDataStep(rig.config, net);
  CHECK(Contains(rig.events[0], "data_version=none manifest_sha256=none"));
  CHECK(!Contains(rig.events[0], "forged"));
}

TEST(AFetchLogsTheVersionAndManifestHashOfTheDataItPlaced) {
  Rig rig;
  rig.present = {rig.Root() + "/" + kManifestRel};  // the fake `exists` must see what the fetch extracts; the decision only asks about the media and legacy roots
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kFetched);
  CHECK_EQ(rig.events.size(), static_cast<std::size_t>(2));
  const std::string manifest = Pattern(20000);
  CHECK(Contains(rig.events[1], "data_version root=" + rig.Root()));
  CHECK(Contains(rig.events[1], "data_version=5932408047 manifest_sha256=" + Sha256Hex(manifest.data(), manifest.size())));
}

// ---- the package files of a pinned data set (package_pins.h) ----

namespace {

constexpr const char* kPackageDir = "_data/5932408047/rad15/android/packages/";
constexpr const char* kPinnedManifestBytes = "pinned manifest";
constexpr const char* kWarnConsequence =
    "consequence=the game keeps running on the data that is there; the lobby may load in pieces until the pack is "
    "installed again";

// A rig whose manifest is in a pinned table of its own (the real table's manifest digest cannot be forged from bytes): two
// small packages, `_0` unhashed and `_1` hashed, with the filesystem, the hash and the marker write all injected.
struct PinnedRig : Rig {
  std::string p0_sha = Sha256Hex("AAAA", 4);
  std::string p1_sha = Sha256Hex("BBBBBB", 6);
  std::string manifest_sha = Sha256Hex(kPinnedManifestBytes, std::strlen(kPinnedManifestBytes));
  PinnedPackage packages[2] = {{0, 4, p0_sha.c_str(), false}, {1, 6, p1_sha.c_str(), true}};
  PinnedDataSet set = {manifest_sha.c_str(), packages, 2};
  std::map<std::string, std::uint64_t> sizes;  // the package files the fake filesystem has
  std::string marker;                         // the marker file's content when has_marker
  bool has_marker = false;
  std::string digest = p1_sha;  // what hash_file answers
  bool hash_readable = true;
  bool write_ok = true;
  int size_calls = 0;
  int hash_calls = 0;
  std::vector<std::string> hashed;
  std::vector<std::pair<std::string, std::string>> writes;

  PinnedRig() {
    present = {Media(), Media("_data"), Media(kManifestRel)};
    sizes[PackagePath(0)] = 4;
    sizes[PackagePath(1)] = 6;
    config.pinned_sets = &set;
    config.pinned_set_count = 1;
    config.list_dir = [](const std::string& dir) {
      return dir == Media("_data") ? std::vector<std::string>{"5932408047"} : std::vector<std::string>{};
    };
    config.read_file = [this](const std::string& path, std::string& bytes) {
      if (path == Media(kManifestRel)) {
        bytes = kPinnedManifestBytes;
        return true;
      }
      if (path == MarkerPath() && has_marker) {
        bytes = marker;
        return true;
      }
      return false;
    };
    config.file_size = [this](const std::string& path, std::uint64_t& bytes) {
      ++size_calls;
      const auto it = sizes.find(path);
      if (it == sizes.end()) return false;
      bytes = it->second;
      return true;
    };
    config.hash_file = [this](const std::string& path, std::string& out) {
      ++hash_calls;
      hashed.push_back(path);
      if (!hash_readable) return false;
      out = digest;
      return true;
    };
    config.write_file = [this](const std::string& path, const std::string& bytes) {
      writes.emplace_back(path, bytes);
      return write_ok;
    };
  }
  static std::string PackagePath(unsigned index) {
    return Media(std::string(kPackageDir) + "48037dc70b0ecab2_" + std::to_string(index));
  }
  static std::string MarkerPath() { return Media("nevr-data-packages.verified"); }
  std::string MarkerFor(const std::string& build) const { return manifest_sha + " " + build + "\n"; }
  // The step as nothing in it may fetch, announce or show a message.
  void CheckUsedWithoutFetch(const StepResult& r, const OneShotTransport& net) const {
    CHECK_EQ(r.outcome, StepOutcome::kUsed);
    CHECK_EQ(r.action, DataAction::kUse);
    CHECK_EQ(r.root, Media());
    CHECK_EQ(net.calls, 0);
    CHECK(shown.empty());
    CHECK(order.empty());  // no announce, no present, no hold
    CHECK(r.message.empty());
  }
};

}  // namespace

TEST(AShortPackageIsWarnedAboutAndTheDataIsStillUsed) {
  PinnedRig rig;
  rig.sizes[PinnedRig::PackagePath(1)] = 5;
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  rig.CheckUsedWithoutFetch(r, net);
  CHECK_EQ(rig.warns.size(), static_cast<std::size_t>(1));
  CHECK(Contains(rig.warns[0],
                 "data_packages root=" + Media() +
                     " data_version=5932408047 package=48037dc70b0ecab2_1 outcome=short expected_bytes=6 found_bytes=5 " +
                     kWarnConsequence));
  CHECK_EQ(rig.events.size(), static_cast<std::size_t>(1));  // data_check only: no second data_packages line
  CHECK_EQ(rig.hash_calls, 0);                                // the sizes did not match: nothing is hashed
  CHECK(rig.writes.empty());                                  // and no marker is written
  CHECK(!Exists(rig.Root()));                                 // no fetch root, no stock re-fetch
}

TEST(AMissingPackageIsWarnedAboutWithNoFoundBytes) {
  PinnedRig rig;
  rig.sizes.erase(PinnedRig::PackagePath(1));
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  rig.CheckUsedWithoutFetch(r, net);
  CHECK_EQ(rig.warns.size(), static_cast<std::size_t>(1));
  CHECK(Contains(rig.warns[0], "package=48037dc70b0ecab2_1 outcome=missing expected_bytes=6 found_bytes=none "));
  CHECK_EQ(rig.hash_calls, 0);
  CHECK(rig.writes.empty());
}

TEST(AnOversizePackageIsWarnedAbout) {
  PinnedRig rig;
  rig.sizes[PinnedRig::PackagePath(0)] = 5;
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  rig.CheckUsedWithoutFetch(r, net);
  CHECK_EQ(rig.warns.size(), static_cast<std::size_t>(1));
  CHECK(Contains(rig.warns[0], "package=48037dc70b0ecab2_0 outcome=oversize expected_bytes=4 found_bytes=5 "));
}

TEST(OnlyTheFirstFailingPackageIsReportedInOrder) {
  PinnedRig rig;
  rig.sizes[PinnedRig::PackagePath(0)] = 3;
  rig.sizes[PinnedRig::PackagePath(1)] = 2;
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  rig.CheckUsedWithoutFetch(r, net);
  CHECK_EQ(rig.warns.size(), static_cast<std::size_t>(1));
  CHECK(Contains(rig.warns[0], "package=48037dc70b0ecab2_0 outcome=short expected_bytes=4 found_bytes=3 "));
  CHECK(!Contains(rig.warns[0], "_1"));
}

TEST(AFailingPackageIsWarnedAboutWhenTheEngineRootIsNamedAndWhenAFetchIsNotAllowed) {
  {
    PinnedRig rig;
    rig.config.engine_root = nevr_quest::data_root::kMediaRoot;
    rig.sizes[PinnedRig::PackagePath(1)] = 5;
    OneShotTransport net(rig.zip, true);
    const StepResult r = RunDataStep(rig.config, net);
    rig.CheckUsedWithoutFetch(r, net);
    CHECK_EQ(rig.warns.size(), static_cast<std::size_t>(1));
    CHECK(Contains(rig.warns[0], "outcome=short"));
  }
  {
    PinnedRig rig;
    rig.config.allow_fetch = false;  // a release build: it still looks, and it still uses the data
    rig.sizes[PinnedRig::PackagePath(1)] = 5;
    OneShotTransport net(rig.zip, true);
    const StepResult r = RunDataStep(rig.config, net);
    rig.CheckUsedWithoutFetch(r, net);
    CHECK_EQ(rig.warns.size(), static_cast<std::size_t>(1));
  }
}

TEST(WholePackagesAreHashedOnceAndTheMarkerIsWritten) {
  PinnedRig rig;
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  rig.CheckUsedWithoutFetch(r, net);
  CHECK(rig.warns.empty());
  CHECK_EQ(rig.size_calls, 2);  // both packages are measured
  CHECK_EQ(rig.hash_calls, 1);  // only the pack's added package is hashed
  CHECK_EQ(rig.hashed.size(), static_cast<std::size_t>(1));
  CHECK_EQ(rig.hashed.empty() ? std::string() : rig.hashed[0], PinnedRig::PackagePath(1));
  CHECK_EQ(rig.writes.size(), static_cast<std::size_t>(1));
  CHECK_EQ(rig.writes.empty() ? std::string() : rig.writes[0].first, PinnedRig::MarkerPath());
  CHECK_EQ(rig.writes.empty() ? std::string() : rig.writes[0].second, rig.manifest_sha + " " + kBuild + "\n");
  CHECK_EQ(rig.events.size(), static_cast<std::size_t>(2));
  CHECK(Contains(rig.events[1], "data_packages root=" + Media() + " data_version=5932408047 manifest_sha256=" +
                                    rig.manifest_sha + " outcome=verified packages=2 sha=checked"));
}

TEST(ASizeIsCheckedOnEveryLaunchButTheHashOnlyOncePerBuild) {
  PinnedRig rig;
  rig.has_marker = true;
  rig.marker = rig.MarkerFor(kBuild);
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  rig.CheckUsedWithoutFetch(r, net);
  CHECK_EQ(rig.size_calls, 2);
  CHECK_EQ(rig.hash_calls, 0);
  CHECK(rig.writes.empty());
  CHECK(Contains(rig.events.back(), "outcome=verified packages=2 sha=skipped"));

  // The marker of another build, or of another manifest, does not count: the package is hashed again.
  PinnedRig other_build;
  other_build.has_marker = true;
  other_build.marker = other_build.MarkerFor("0123456789abcdef0123456789abcdef01234567");
  OneShotTransport net2(other_build.zip, true);
  RunDataStep(other_build.config, net2);
  CHECK_EQ(other_build.hash_calls, 1);
  CHECK_EQ(other_build.writes.size(), static_cast<std::size_t>(1));
  CHECK(Contains(other_build.events.back(), "sha=checked"));

  PinnedRig other_manifest;
  other_manifest.has_marker = true;
  other_manifest.marker = std::string(64, '0') + " " + kBuild + "\n";
  OneShotTransport net3(other_manifest.zip, true);
  RunDataStep(other_manifest.config, net3);
  CHECK_EQ(other_manifest.hash_calls, 1);

  // A short size is still caught on a launch that skips the hash.
  PinnedRig short_again;
  short_again.has_marker = true;
  short_again.marker = short_again.MarkerFor(kBuild);
  short_again.sizes[PinnedRig::PackagePath(1)] = 1;
  OneShotTransport net4(short_again.zip, true);
  RunDataStep(short_again.config, net4);
  CHECK_EQ(short_again.warns.size(), static_cast<std::size_t>(1));
}

TEST(WithoutABuildIdNothingIsHashedAndNoMarkerIsWritten) {
  PinnedRig rig;
  rig.config.build_id.clear();
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  rig.CheckUsedWithoutFetch(r, net);
  CHECK_EQ(rig.hash_calls, 0);
  CHECK(rig.writes.empty());
  CHECK(Contains(rig.events.back(), "outcome=verified packages=2 sha=not_due"));
}

TEST(AHashThatDoesNotMatchIsWarnedAboutAndNoMarkerIsWritten) {
  PinnedRig rig;
  rig.digest = std::string(64, 'f');
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  rig.CheckUsedWithoutFetch(r, net);
  CHECK_EQ(rig.hash_calls, 1);
  CHECK(rig.writes.empty());
  CHECK_EQ(rig.warns.size(), static_cast<std::size_t>(1));
  CHECK(Contains(rig.warns[0], "package=48037dc70b0ecab2_1 outcome=sha_mismatch expected_bytes=6 found_bytes=6 "));
  CHECK(Contains(rig.warns[0], kWarnConsequence));
  CHECK_EQ(rig.events.size(), static_cast<std::size_t>(1));  // no `verified` line after a failure
}

TEST(AFileThatCannotBeReadToItsEndIsWarnedAboutAndNoMarkerIsWritten) {
  PinnedRig rig;
  rig.hash_readable = false;
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  rig.CheckUsedWithoutFetch(r, net);
  CHECK(rig.writes.empty());
  CHECK_EQ(rig.warns.size(), static_cast<std::size_t>(1));
  CHECK(Contains(rig.warns[0], "package=48037dc70b0ecab2_1 outcome=unreadable "));
}

TEST(AMarkerThatCannotBeWrittenIsWarnedAboutAndTheCheckStillPasses) {
  PinnedRig rig;
  rig.write_ok = false;
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  rig.CheckUsedWithoutFetch(r, net);
  CHECK_EQ(rig.warns.size(), static_cast<std::size_t>(1));
  CHECK(Contains(rig.warns[0], "data_packages_marker "));
  CHECK(Contains(rig.warns[0], "result=write_failed"));
  CHECK(Contains(rig.events.back(), "outcome=verified packages=2 sha=checked"));
}

TEST(AManifestOutsideThePinnedTableIsNotCheckedAndIsUsedAsBefore) {
  PinnedRig rig;
  rig.config.pinned_sets = nullptr;  // the real table: the test manifest is not in it
  rig.config.pinned_set_count = 0;
  rig.sizes.clear();                 // nothing is there, and nothing is asked
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  rig.CheckUsedWithoutFetch(r, net);
  CHECK_EQ(rig.size_calls, 0);
  CHECK_EQ(rig.hash_calls, 0);
  CHECK(rig.writes.empty());
  CHECK(rig.warns.empty());
  CHECK_EQ(rig.events.size(), static_cast<std::size_t>(2));
  CHECK(Contains(rig.events[1], "data_packages "));
  CHECK(Contains(rig.events[1], "outcome=unpinned"));
}

TEST(TheDataIsNotCheckedWhenItIsFetchedOrReplaced) {
  PinnedRig rig;
  rig.present = {Media(), Media("_data")};  // `_data` there, the manifest is not: replace, not use
  rig.PutOldData();
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.action, DataAction::kReplace);
  CHECK_EQ(rig.size_calls, 0);
  CHECK(rig.warns.empty());
  for (const std::string& e : rig.events) CHECK(!Contains(e, "data_packages"));
}

TEST(ThePinnedTableHoldsTheCombatPackAsRecorded) {
  CHECK_EQ(kPinnedDataSetCount, static_cast<std::size_t>(1));
  const PinnedDataSet* set = FindPinnedDataSet("e1c4903622e4acf5b747f591868ef67d623e619ee63fd4af0268a593874a63e1");
  CHECK(set != nullptr);
  if (set == nullptr) return;
  CHECK_EQ(set->count, static_cast<std::size_t>(2));
  CHECK_EQ(set->packages[0].index, 0u);
  CHECK_EQ(set->packages[0].bytes, std::uint64_t{929620177});
  CHECK_EQ(std::string(set->packages[0].sha256), std::string("14d26efc718a8f8135039e47bd1321cbb556fc4181bf95ffcb5ef59c406a5a3b"));
  CHECK(!set->packages[0].hashed);
  CHECK_EQ(set->packages[1].index, 1u);
  CHECK_EQ(set->packages[1].bytes, std::uint64_t{689617013});
  CHECK_EQ(std::string(set->packages[1].sha256), std::string("decc77fdfc8ecd3b75ad3120f33dd1980a6a1f8c4c50f566ceb6483d9092db02"));
  CHECK(set->packages[1].hashed);
  // The package file names are made from the manifest id of kManifestPath, which the table does not repeat.
  CHECK(Contains(kManifestPath, "/48037dc70b0ecab2"));
  // The stock manifest (and any other) is not in the table.
  CHECK(FindPinnedDataSet(std::string(64, '0')) == nullptr);
  CHECK(FindPinnedDataSet("none") == nullptr);
}

TEST(TheStockManifestIsNotInThePinnedTable) {
  // A wrong pin for the stock data would make every player download it again; the table holds the combat pack's alone.
  CHECK(FindPinnedDataSet("762c16ae58c9bc70eee071d8c0e2e8fc0408d1f0691032164029294c5cb7ec48") == nullptr);
  CHECK_EQ(kPinnedDataSetCount, static_cast<std::size_t>(1));
  CHECK_EQ(sizeof(kPinnedDataSets) / sizeof(kPinnedDataSets[0]), static_cast<std::size_t>(1));
}

TEST(WithNoWarningSinkAFailureReachesTheStepSinkAtItsLevel) {
  PinnedRig rig;
  rig.config.on_step_warn = nullptr;  // the documented fallback: the warning goes to on_step
  rig.sizes[PinnedRig::PackagePath(1)] = 5;
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  rig.CheckUsedWithoutFetch(r, net);
  CHECK(rig.warns.empty());
  CHECK_EQ(rig.events.size(), static_cast<std::size_t>(2));  // data_check, then the failure
  CHECK(Contains(rig.events[1], "data_packages root=" + Media() + " data_version=5932408047 package=48037dc70b0ecab2_1 outcome=short"));
}

// The production defaults of the file seams (a stat, a chunked hash, a temporary and a rename, a bounded read) over a real
// directory: file_size, hash_file, write_file and read_file are left null. The pinned set is a custom one, with the
// manifest digest taken from the bytes written and two packages of "abc".
TEST(TheRealFileSeamsCheckSizesHashAndMarkerOnDisk) {
  Rig rig;
  const std::string root = rig.scratch.Path("real");
  ::mkdir(root.c_str(), 0755);
  const std::string manifest_bytes = "a real manifest on disk";
  const std::string manifest_sha = Sha256Hex(manifest_bytes.data(), manifest_bytes.size());
  const std::string pkg0 = root + "/" + kPackageDir + "48037dc70b0ecab2_0";
  const std::string pkg1 = root + "/" + kPackageDir + "48037dc70b0ecab2_1";
  const std::string marker = root + "/nevr-data-packages.verified";
  MakeTree(root, kManifestRel, manifest_bytes);
  MakeTree(root, std::string(kPackageDir) + "48037dc70b0ecab2_0", "abc");
  MakeTree(root, std::string(kPackageDir) + "48037dc70b0ecab2_1", "abc");

  const char* const abc_sha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  CHECK_EQ(Sha256Hex("abc", 3), std::string(abc_sha));
  const PinnedPackage packages[2] = {{0, 3, abc_sha, false}, {1, 3, abc_sha, true}};
  const PinnedDataSet set = {manifest_sha.c_str(), packages, 2};
  rig.config.pinned_sets = &set;
  rig.config.pinned_set_count = 1;
  rig.config.engine_root = root;
  rig.config.exists = &MappedExists;  // the real filesystem (the mapped roots are not under the scratch directory)
  CHECK(!rig.config.file_size && !rig.config.hash_file && !rig.config.write_file && !rig.config.read_file &&
        !rig.config.list_dir);

  auto run = [&](const char* what) {
    rig.events.clear();
    rig.warns.clear();
    OneShotTransport net(rig.zip, true);
    const StepResult r = RunDataStep(rig.config, net);
    if (r.outcome != StepOutcome::kUsed || r.action != DataAction::kUse || r.root != root || net.calls != 0 ||
        !rig.shown.empty() || !rig.order.empty()) {
      std::fprintf(stderr, "  the data was not simply used: %s\n", what);
      CHECK(false);
    }
  };

  // First launch of the build: both sizes match, the added package is hashed, the marker is written whole.
  run("first launch");
  CHECK(rig.warns.empty());
  CHECK(!rig.events.empty() && Contains(rig.events.back(), "outcome=verified packages=2 sha=checked"));
  CHECK_EQ(ReadFile(marker), manifest_sha + " " + kBuild + "\n");
  CHECK(!Exists(marker + ".tmp"));  // the temporary was renamed away

  // Second launch: the marker names this manifest and build, so the hash is skipped.
  run("second launch");
  CHECK(rig.warns.empty());
  CHECK(!rig.events.empty() && Contains(rig.events.back(), "outcome=verified packages=2 sha=skipped"));

  // One byte of _1 changed, same size, and no marker: the hash catches it. No marker is written, the data is still used.
  WriteFile(pkg1, "abd");
  CHECK_EQ(::unlink(marker.c_str()), 0);
  run("changed byte");
  CHECK_EQ(rig.warns.size(), static_cast<std::size_t>(1));
  CHECK(!rig.warns.empty() &&
        Contains(rig.warns[0], "package=48037dc70b0ecab2_1 outcome=sha_mismatch expected_bytes=3 found_bytes=3 "));
  CHECK(!Exists(marker));
  CHECK_EQ(rig.events.size(), static_cast<std::size_t>(1));  // data_check only: no `verified` line

  // Shorter and longer than pinned: the real sizes are reported.
  WriteFile(pkg1, "a");
  run("truncated");
  CHECK_EQ(rig.warns.size(), static_cast<std::size_t>(1));
  CHECK(!rig.warns.empty() && Contains(rig.warns[0], "package=48037dc70b0ecab2_1 outcome=short expected_bytes=3 found_bytes=1 "));
  WriteFile(pkg1, "abcd");
  run("extended");
  CHECK_EQ(rig.warns.size(), static_cast<std::size_t>(1));
  CHECK(!rig.warns.empty() && Contains(rig.warns[0], "package=48037dc70b0ecab2_1 outcome=oversize expected_bytes=3 found_bytes=4 "));

  // Absent, or something that is not a regular file: missing.
  CHECK_EQ(::unlink(pkg1.c_str()), 0);
  run("deleted");
  CHECK_EQ(rig.warns.size(), static_cast<std::size_t>(1));
  CHECK(!rig.warns.empty() && Contains(rig.warns[0], "package=48037dc70b0ecab2_1 outcome=missing expected_bytes=3 found_bytes=none "));
  CHECK_EQ(::mkdir(pkg1.c_str(), 0755), 0);
  run("a directory in its place");
  CHECK_EQ(rig.warns.size(), static_cast<std::size_t>(1));
  CHECK(!rig.warns.empty() && Contains(rig.warns[0], "outcome=missing"));

  // The stock package is reported first when it fails too.
  CHECK_EQ(::unlink(pkg0.c_str()), 0);
  run("stock package gone");
  CHECK_EQ(rig.warns.size(), static_cast<std::size_t>(1));
  CHECK(!rig.warns.empty() && Contains(rig.warns[0], "package=48037dc70b0ecab2_0 outcome=missing"));
}

TEST(TheMissingOutcomeHasAName) {
  CHECK_EQ(std::string(StepOutcomeName(StepOutcome::kMissing)), std::string("missing"));
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
