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
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "quest/data/boot_stub.h"
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

// A folder name is judged by its characters and by the file system's own bound (NAME_MAX), not by a number of ours: a long plain name is named.
TEST(TheDataCheckNamesAPlainFolderOfAnyLengthTheFileSystemAllows) {
  Rig rig;
  const std::string longName(NAME_MAX, '7');
  const std::string tooLong(NAME_MAX + 1, '7');
  const std::string path = "_data/" + longName + "/rad15/android/manifests/48037dc70b0ecab2";
  rig.present = {Media(), Media("_data"), Media(path)};
  rig.config.list_dir = [&](const std::string& dir) {
    return dir == Media("_data") ? std::vector<std::string>{tooLong, longName} : std::vector<std::string>{};
  };
  rig.config.read_file = [&](const std::string&, std::string& bytes) {
    bytes = "m";
    return true;
  };
  rig.config.allow_fetch = false;
  OneShotTransport net(rig.zip, true);
  RunDataStep(rig.config, net);
  CHECK(!rig.events.empty() && Contains(rig.events[0], "data_version=" + longName + " "));
  CHECK(!rig.events.empty() && !Contains(rig.events[0], tooLong));  // a name the file system cannot have is never logged
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
  CHECK(!rig.warns.empty() && Contains(rig.warns[0],
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
  CHECK(!rig.warns.empty() && Contains(rig.warns[0], "package=48037dc70b0ecab2_1 outcome=missing expected_bytes=6 found_bytes=none "));
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
  CHECK(!rig.warns.empty() && Contains(rig.warns[0], "package=48037dc70b0ecab2_0 outcome=oversize expected_bytes=4 found_bytes=5 "));
}

TEST(OnlyTheFirstFailingPackageIsReportedInOrder) {
  PinnedRig rig;
  rig.sizes[PinnedRig::PackagePath(0)] = 3;
  rig.sizes[PinnedRig::PackagePath(1)] = 2;
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  rig.CheckUsedWithoutFetch(r, net);
  CHECK_EQ(rig.warns.size(), static_cast<std::size_t>(1));
  CHECK(!rig.warns.empty() && Contains(rig.warns[0], "package=48037dc70b0ecab2_0 outcome=short expected_bytes=4 found_bytes=3 "));
  CHECK(!rig.warns.empty() && !Contains(rig.warns[0], "_1"));
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
    CHECK(!rig.warns.empty() && Contains(rig.warns[0], "outcome=short"));
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
  CHECK(!rig.warns.empty() && Contains(rig.warns[0], "package=48037dc70b0ecab2_1 outcome=sha_mismatch expected_bytes=6 found_bytes=6 "));
  CHECK(!rig.warns.empty() && Contains(rig.warns[0], kWarnConsequence));
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
  CHECK(!rig.warns.empty() && Contains(rig.warns[0], "package=48037dc70b0ecab2_1 outcome=unreadable "));
}

TEST(AMarkerThatCannotBeWrittenIsWarnedAboutAndTheCheckStillPasses) {
  PinnedRig rig;
  rig.write_ok = false;
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  rig.CheckUsedWithoutFetch(r, net);
  CHECK_EQ(rig.warns.size(), static_cast<std::size_t>(1));
  CHECK(!rig.warns.empty() && Contains(rig.warns[0], "data_packages_marker "));
  CHECK(!rig.warns.empty() && Contains(rig.warns[0], "result=write_failed"));
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
  CHECK(rig.events.size() > 1 && Contains(rig.events[1], "data_packages root=" + Media() + " data_version=5932408047 package=48037dc70b0ecab2_1 outcome=short"));
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
  CHECK_EQ(std::string(StepOutcomeName(StepOutcome::kStubbed)), std::string("stubbed"));
}

// ---- the decision table (docs/adr/0011, plan.h Decide, data_step.cpp RunDataStep) ----
//
// Decide: every resolution (what data_root::Resolve found) against whether the pinned manifest is under it.

namespace {

std::set<std::string>* g_decide_present = nullptr;
bool DecideExists(const char* path) noexcept { return g_decide_present != nullptr && g_decide_present->count(path) != 0; }

struct DecideRow {
  const char* name;
  nevr_quest::data_root::Resolution resolution;
  bool manifest_present;
  DataAction action;
  const char* root;
  const char* source;
};

}  // namespace

TEST(DecideCoversEveryResolutionAgainstTheManifest) {
  using nevr_quest::data_root::Resolution;
  const char* const media = nevr_quest::data_root::kMediaRoot;
  const char* const legacy = nevr_quest::data_root::kLegacyRoot;
  const DecideRow rows[] = {
      {"none, manifest absent", Resolution{legacy, "none", false}, false, DataAction::kFetch, media, "none"},
      {"none, manifest present", Resolution{legacy, "none", false}, true, DataAction::kFetch, media, "none"},
      {"unmarked media, manifest absent", Resolution{media, "android_media_unmarked", true}, false, DataAction::kFetch, media,
       "android_media_unmarked"},
      {"unmarked media, manifest present", Resolution{media, "android_media_unmarked", true}, true, DataAction::kFetch, media,
       "android_media_unmarked"},
      {"media, manifest absent", Resolution{media, "android_media", true}, false, DataAction::kReplace, media, "android_media"},
      {"media, manifest present", Resolution{media, "android_media", true}, true, DataAction::kUse, media, "android_media"},
      {"legacy, manifest absent", Resolution{legacy, "legacy", true}, false, DataAction::kReplace, legacy, "legacy"},
      {"legacy, manifest present", Resolution{legacy, "legacy", true}, true, DataAction::kUse, legacy, "legacy"},
      {"engine, manifest absent", Resolution{"/engine/root", "engine", true}, false, DataAction::kReplace, "/engine/root", "engine"},
      {"engine, manifest present", Resolution{"/engine/root", "engine", true}, true, DataAction::kUse, "/engine/root", "engine"},
  };
  for (const DecideRow& row : rows) {
    std::set<std::string> present;
    if (row.manifest_present) present.insert(std::string(row.resolution.root) + "/" + kManifestPath);
    g_decide_present = &present;
    const Decision d = Decide(row.resolution, &DecideExists);
    g_decide_present = nullptr;
    if (d.action != row.action || std::string(d.root) != row.root || std::string(d.source) != row.source) {
      std::fprintf(stderr, "  decide row failed: %s\n", row.name);
    }
    CHECK(d.action == row.action);
    CHECK_EQ(std::string(d.root), std::string(row.root));
    CHECK_EQ(std::string(d.source), std::string(row.source));
  }
}

// RunDataStep: the setup (which root holds what, and whether the engine named one), allow_fetch and what the fetch does. `all_assets` false is the
// stock step. Every resolution data_root::Resolve can make, and the engine root, goes through RunDataStep, not only through Decide.
namespace {

constexpr const char* kEngineRoot = "/engine/root";

struct Setup {
  const char* name;
  std::set<std::string> present;  // what the fake filesystem holds
  std::string engine_root;        // StepConfig::engine_root, empty for none
  DataAction action;              // what Decide says
  std::string root;               // the root it names (StepResult::root of the stock step)
  const char* source;             // the resolution's source token, as `data_check` logs it
};

std::vector<Setup> Setups() {
  const std::string e = kEngineRoot;
  return {
      {"none", {}, "", DataAction::kFetch, Media(), "none"},
      {"unmarked_media", {Media()}, "", DataAction::kFetch, Media(), "android_media_unmarked"},
      {"media_other_build", {Media(), Media("_data")}, "", DataAction::kReplace, Media(), "android_media"},
      {"media_stock", {Media(), Media("_data"), Media(kManifestRel)}, "", DataAction::kUse, Media(), "android_media"},
      {"legacy_other_build", {Legacy("_data")}, "", DataAction::kReplace, Legacy(), "legacy"},
      {"legacy_stock", {Legacy("_data"), Legacy(kManifestRel)}, "", DataAction::kUse, Legacy(), "legacy"},
      {"engine_absent", {}, e, DataAction::kFetch, e, "none"},
      // Only the engine's root counts: data in the media root is not the engine's.
      {"engine_absent_media_holds_stock", {Media(), Media("_data"), Media(kManifestRel)}, e, DataAction::kFetch, e, "none"},
      {"engine_other_build", {e + "/_data"}, e, DataAction::kReplace, e, "engine"},
      {"engine_stock", {e + "/_data", e + "/" + kManifestRel}, e, DataAction::kUse, e, "engine"},
  };
}

const char* ActionText(DataAction a) {
  switch (a) {
    case DataAction::kUse: return "use";
    case DataAction::kFetch: return "fetch";
    case DataAction::kReplace: return "replace";
  }
  return "?";
}

enum class StubState { kAbsent, kCurrent, kNotOurs, kUnreadable };

const char* StubStateName(StubState s) {
  switch (s) {
    case StubState::kAbsent: return "absent";
    case StubState::kCurrent: return "current";
    case StubState::kNotOurs: return "not_ours";
    case StubState::kUnreadable: return "unreadable";
  }
  return "?";
}

struct RowOut {
  StepResult result;
  int calls = 0;
  std::vector<std::string> shown, order, events, warns;
  std::string root;
  bool stub0 = false, stub1 = false;
  std::string stub0_bytes, stub1_bytes;
};

// `behaviour` makes the fetch fail the way a status names (bad_root: the root below a regular file, which is the stubs' root in the whole-game
// mode too); `stub` is what is already at the stub paths, standing for files through the fake filesystem and the read seam.
RowOut RunRow(const Setup& setup, bool allow_fetch, const std::string& behaviour, bool all_assets, StubState stub = StubState::kAbsent) {
  Rig rig(behaviour == "extract_failed" ? std::string("this is not a zip, but its digest is the pinned one") : GoodZip());
  rig.config.allow_fetch = allow_fetch;
  rig.config.all_assets = all_assets;
  if (all_assets) rig.config.all_assets_ready = [](const std::string&) { return AllAssetsResult{}; };  // the table is about the stubs; no-worker has its own test
  rig.config.engine_root = setup.engine_root;
  rig.present = setup.present;
  if (stub != StubState::kAbsent) {
    for (const char* id : {"48037dc70b0ecab2", "2b47aab238f60515"}) rig.present.insert(rig.Root() + "/_data/5932408047/rad15/android/manifests/" + id);
    if (stub == StubState::kCurrent) {
      rig.config.read_file = [](const std::string&, std::string& bytes) {
        bytes = nevr_quest::data::boot_stub::StubFileBytes();
        return true;
      };
    } else if (stub == StubState::kNotOurs) {
      rig.config.read_file = [](const std::string&, std::string& bytes) {
        bytes = "another file";
        return true;
      };
    } else {
      rig.config.read_file = [](const std::string&, std::string&) { return false; };
    }
  }
  bool network = true;
  if (behaviour == "no_space") {
    rig.config.fetch.available_bytes = [](const std::string&) { return std::uint64_t{0}; };
  } else if (behaviour == "bad_root") {
    WriteFile(rig.scratch.Path("afile"), "x");
    rig.config.fetch_root_override = rig.scratch.Path("afile/sub");
  } else if (behaviour == "retries_exhausted") {
    network = false;
  } else if (behaviour == "swap_failed") {
    rig.PutOldData();
    ::mkdir((rig.Root() + "/_data.replaced-20261022T000000Z").c_str(), 0755);
    WriteFile(rig.Root() + "/_data.replaced-20261022T000000Z/occupied", "x");
  } else if (behaviour == "cancelled") {
    rig.config.fetch.keep_going = [] { return false; };
  }
  OneShotTransport net(rig.zip, network);
  RowOut out;
  out.result = RunDataStep(rig.config, net);
  out.calls = net.calls;
  out.shown = rig.shown;
  out.order = rig.order;
  out.events = rig.events;
  out.warns = rig.warns;
  out.root = rig.Root();
  const std::string dir = rig.Root() + "/_data/5932408047/rad15/android/manifests/";
  out.stub0 = Exists(dir + "48037dc70b0ecab2");
  out.stub1 = Exists(dir + "2b47aab238f60515");
  out.stub0_bytes = ReadFile(dir + "48037dc70b0ecab2");
  out.stub1_bytes = ReadFile(dir + "2b47aab238f60515");
  return out;
}

bool AnyStartsWith(const std::vector<std::string>& lines, const std::string& prefix) {
  for (const std::string& l : lines) {
    if (l.rfind(prefix, 0) == 0) return true;
  }
  return false;
}

bool HasEvent(const std::vector<std::string>& lines, const std::string& needle) {
  for (const std::string& l : lines) {
    if (Contains(l, needle)) return true;
  }
  return false;
}

const char* const kFetchBehaviours[] = {"ok", "no_space", "bad_root", "retries_exhausted", "extract_failed", "swap_failed", "cancelled"};

FetchStatus StatusOf(const std::string& behaviour) {
  if (behaviour == "ok") return FetchStatus::kOk;
  if (behaviour == "no_space") return FetchStatus::kNoSpace;
  if (behaviour == "bad_root") return FetchStatus::kBadRoot;
  if (behaviour == "retries_exhausted") return FetchStatus::kRetriesExhausted;
  if (behaviour == "extract_failed") return FetchStatus::kExtractFailed;
  if (behaviour == "swap_failed") return FetchStatus::kSwapFailed;
  return FetchStatus::kCancelled;
}

}  // namespace

TEST(TheStockStepDecisionTableHoldsForEveryResolutionAllowFetchAndFetchStatus) {
  int rows = 0;
  for (const Setup& setup : Setups()) {
    for (bool allow : {false, true}) {
      for (const char* behaviour_name : kFetchBehaviours) {
        const std::string behaviour = behaviour_name;
        // A swap can only fail when there is an old tree to put aside; with any other action it simply succeeds.
        if (behaviour == "swap_failed" && setup.action != DataAction::kReplace) continue;
        // The fetch is never reached for data in use, or without allow_fetch: its behaviour cannot matter, so one row says it.
        if ((setup.action == DataAction::kUse || !allow) && behaviour != "ok") continue;
        ++rows;
        const std::string row = std::string(setup.name) + " allow_fetch=" + (allow ? "1" : "0") + " fetch=" + behaviour;
        const RowOut o = RunRow(setup, allow, behaviour, /*all_assets=*/false);
        const int before = mini_test::Failures();
        // The resolution, read through RunDataStep: the action, the root and the source `data_check` logs.
        CHECK_EQ(o.result.action, setup.action);
        CHECK_EQ(o.result.root, setup.root);
        CHECK(HasEvent(o.events, std::string("data_check action=") + ActionText(setup.action) + " source=" + setup.source + " root=" + setup.root));
        if (setup.action == DataAction::kUse) {
          CHECK_EQ(o.result.outcome, StepOutcome::kUsed);
          CHECK_EQ(o.calls, 0);
          CHECK(o.shown.empty());
        } else if (!allow) {
          CHECK_EQ(o.result.outcome, StepOutcome::kMissing);
          CHECK_EQ(o.result.message, std::string(kGameDataMissing));
          CHECK_EQ(o.calls, 0);
          CHECK_EQ(o.shown.size(), static_cast<std::size_t>(1));
          CHECK(!o.stub0 && !o.stub1);
        } else if (behaviour == "ok") {
          CHECK_EQ(o.result.outcome, StepOutcome::kFetched);
          CHECK_EQ(o.result.status, FetchStatus::kOk);
          CHECK_EQ(o.calls, 1);
          CHECK(o.shown.empty());  // a good fetch shows no floor message
        } else {
          CHECK_EQ(o.result.outcome, StepOutcome::kFailed);
          CHECK(o.result.status == StatusOf(behaviour));
          CHECK(!o.result.message.empty());
          CHECK_EQ(o.shown.size(), static_cast<std::size_t>(1));
          CHECK(!o.shown.empty() && o.shown[0] == o.result.message);
          CHECK(o.order.size() >= 2 && o.order[o.order.size() - 2] == "present" && o.order.back() == "hold");
          const bool reaches_network = behaviour == "retries_exhausted" || behaviour == "extract_failed" || behaviour == "swap_failed";
          CHECK_EQ(o.calls > 0, reaches_network);
        }
        // The stock step never writes a stub manifest, and says nothing of the whole-game mode.
        CHECK(!AnyStartsWith(o.events, "data_all_assets") && !AnyStartsWith(o.events, "data_stub"));
        if (mini_test::Failures() != before) std::fprintf(stderr, "  decision row failed: %s\n", row.c_str());
      }
    }
  }
  // 4 fetch setups x 7 + 3 replace setups x 8 + 3 use setups x 2.
  CHECK_EQ(rows, 4 * 7 + 3 * 8 + 3 * 2);
}

// ---- the whole-game mode: the stub manifests, never a fetch, never an end for missing data ----

namespace {

constexpr const char* kSet0 = "48037dc70b0ecab2";
constexpr const char* kSet1 = "2b47aab238f60515";

std::string StubDir(const std::string& root) { return root + "/_data/5932408047/rad15/android/manifests/"; }

}  // namespace

// The whole-game table: every resolution x allow_fetch x fetch status x what is already at the stub paths. The fetch status cannot reach any
// fetch code in this mode (the step returns before it), so every status row is `stubbed` with no request, and that is what each row asserts; the
// pairing test below shows that the same statuses do fail the stock step. `bad_root` makes the stubs' own root unwritable, so it is the row that
// fails (with stubs already there it has nothing to write and is stubbed).
TEST(TheWholeGameDecisionTableWritesStubsAndNeverFetchesForEveryResolutionAllowFetchStatusAndStubState) {
  const StubState stub_states[] = {StubState::kAbsent, StubState::kCurrent, StubState::kNotOurs, StubState::kUnreadable};
  int rows = 0;
  for (const Setup& setup : Setups()) {
    for (bool allow : {false, true}) {
      for (const char* behaviour_name : kFetchBehaviours) {
        const std::string behaviour = behaviour_name;
        if (behaviour == "swap_failed" && setup.action != DataAction::kReplace) continue;
        for (StubState stub : stub_states) {
          // An unwritable root cannot hold stubs: only the absent state exists for it.
          if (behaviour == "bad_root" && stub != StubState::kAbsent) continue;
          ++rows;
          const std::string row = std::string(setup.name) + " allow_fetch=" + (allow ? "1" : "0") + " fetch=" + behaviour + " stubs=" + StubStateName(stub);
          const RowOut o = RunRow(setup, allow, behaviour, /*all_assets=*/true, stub);
          const int before = mini_test::Failures();
          CHECK_EQ(o.calls, 0);  // no request of any kind: the data zip is not the data of this mode
          CHECK(!AnyStartsWith(o.events, "data_fetch") && !AnyStartsWith(o.events, "data_extract") && !AnyStartsWith(o.events, "data_swap"));
          CHECK(!AnyStartsWith(o.events, "data_check"));     // the stock step's own lookup is not made
          CHECK(o.result.outcome != StepOutcome::kMissing);  // never the release build's end for missing data
          CHECK(HasEvent(o.events, std::string("data_all_assets root=") + o.root + " source=" + setup.source + " would_have=" + ActionText(setup.action) +
                                       " allow_fetch=" + (allow ? "true" : "false")));
          if (behaviour == "bad_root") {
            // The one row the mode can fail on: the root cannot be written, so the engine could not start either.
            CHECK_EQ(o.result.outcome, StepOutcome::kFailed);
            CHECK(o.result.status == FetchStatus::kBadRoot);
            CHECK(o.result.stub_write_failed);
            CHECK_EQ(o.shown.size(), static_cast<std::size_t>(1));
            CHECK(HasEvent(o.warns, "data_stub ") && HasEvent(o.warns, "result=write_failed"));
            CHECK(!o.stub0 && !o.stub1);
          } else {
            CHECK_EQ(o.result.outcome, StepOutcome::kStubbed);
            CHECK(!o.result.stub_write_failed);
            CHECK_EQ(o.result.root, o.root);  // the stubs' root: the fetch-root seam here, the decision's root in production
            CHECK(o.shown.empty() && o.order.empty());
            const std::vector<std::string>& stub_lines = (stub == StubState::kNotOurs || stub == StubState::kUnreadable) ? o.warns : o.events;  // a Warning goes to the warning sink
            for (const char* set : {"48037dc70b0ecab2", "2b47aab238f60515"}) CHECK(HasEvent(stub_lines, std::string("data_stub set=") + set));
            if (stub == StubState::kAbsent) {
              CHECK(o.stub0 && o.stub1);
              CHECK(HasEvent(o.events, "result=written reason=manifest_absent"));
              CHECK(o.warns.empty());
            } else {
              // Present stubs are never written over (they are not on disk here: the fake filesystem and the read seam stand for them).
              CHECK(!o.stub0 && !o.stub1);
              CHECK(!HasEvent(o.events, "result=written"));
              if (stub == StubState::kCurrent) CHECK(HasEvent(o.events, "result=kept reason=stub_current") && o.warns.empty());
              if (stub == StubState::kNotOurs) CHECK(HasEvent(o.warns, "result=kept reason=manifest_not_ours"));
              if (stub == StubState::kUnreadable) CHECK(HasEvent(o.warns, "result=kept reason=manifest_unreadable"));
            }
          }
          if (mini_test::Failures() != before) std::fprintf(stderr, "  whole-game row failed: %s\n", row.c_str());
        }
      }
    }
  }
  // Per resolution and allow_fetch: the non-bad_root statuses x 4 stub states, plus bad_root once. Statuses: 6 for a replace setup (swap_failed
  // included), 5 otherwise. 3 replace setups, 7 others.
  CHECK_EQ(rows, 2 * (3 * (6 * 4 + 1) + 7 * (5 * 4 + 1)));
}

// The same inputs, in both modes: each fetch status that fails the stock step is not reached in the whole-game mode.
TEST(AFetchStatusThatFailsTheStockStepIsNotReachedInTheWholeGameMode) {
  const std::vector<Setup> setups = Setups();
  const Setup& other_build = setups[2];  // media_other_build: a replace, so every status below can happen
  CHECK(std::string(other_build.name) == "media_other_build");
  for (const char* behaviour : {"no_space", "retries_exhausted", "extract_failed", "swap_failed", "cancelled"}) {
    const RowOut stock = RunRow(other_build, /*allow_fetch=*/true, behaviour, /*all_assets=*/false);
    const RowOut whole = RunRow(other_build, /*allow_fetch=*/true, behaviour, /*all_assets=*/true);
    if (stock.result.outcome != StepOutcome::kFailed || whole.result.outcome != StepOutcome::kStubbed) {
      std::fprintf(stderr, "  paired row failed: %s\n", behaviour);
    }
    CHECK_EQ(stock.result.outcome, StepOutcome::kFailed);
    CHECK(stock.result.status == StatusOf(behaviour));
    CHECK_EQ(whole.result.outcome, StepOutcome::kStubbed);
    CHECK_EQ(whole.calls, 0);
    CHECK(whole.stub0 && whole.stub1);
  }
}

TEST(TheStubFilesAreTheEnginesZeroPackageManifestAtTheEnginesPath) {
  const RowOut o = RunRow(Setups()[0], true, "ok", true);
  CHECK_EQ(o.stub0_bytes, nevr_quest::data::boot_stub::StubFileBytes());
  CHECK_EQ(o.stub1_bytes, nevr_quest::data::boot_stub::StubFileBytes());
  CHECK(HasEvent(o.events, "result=written reason=manifest_absent"));
  CHECK(HasEvent(o.events, "bytes=" + std::to_string(nevr_quest::data::boot_stub::StubFileBytes().size())));
}

TEST(AStubThatIsThereIsKeptAndNotWrittenAgain) {
  Rig rig;
  rig.config.all_assets = true;
  rig.config.exists = [](const char* path) noexcept { return Exists(path); };  // the real filesystem: the first run's files are seen
  rig.config.all_assets_ready = [](const std::string&) { return AllAssetsResult{}; };  // a worker, so a second run is silent
  OneShotTransport net(rig.zip, true);
  CHECK_EQ(RunDataStep(rig.config, net).outcome, StepOutcome::kStubbed);
  rig.events.clear();
  rig.warns.clear();
  const StepResult second = RunDataStep(rig.config, net);
  CHECK_EQ(second.outcome, StepOutcome::kStubbed);
  CHECK(HasEvent(rig.events, "data_stub set=48037dc70b0ecab2") && HasEvent(rig.events, "result=kept reason=stub_current"));
  CHECK(!HasEvent(rig.events, "result=written"));
  CHECK(rig.warns.empty());
  CHECK_EQ(net.calls, 0);
}

TEST(AManifestThatIsNotOursIsNeverOverwrittenAndIsWarnedAbout) {
  Rig rig;
  rig.config.all_assets = true;
  rig.config.exists = [](const char* path) noexcept { return Exists(path); };
  ::mkdir(rig.Root().c_str(), 0755);
  ::mkdir((rig.Root() + "/_data").c_str(), 0755);
  ::mkdir((rig.Root() + "/_data/5932408047").c_str(), 0755);
  ::mkdir((rig.Root() + "/_data/5932408047/rad15").c_str(), 0755);
  ::mkdir((rig.Root() + "/_data/5932408047/rad15/android").c_str(), 0755);
  ::mkdir(StubDir(rig.Root()).substr(0, StubDir(rig.Root()).size() - 1).c_str(), 0755);
  WriteFile(StubDir(rig.Root()) + kSet0, "a stock manifest, or a player's file");
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kStubbed);
  CHECK_EQ(ReadFile(StubDir(rig.Root()) + kSet0), std::string("a stock manifest, or a player's file"));
  CHECK_EQ(ReadFile(StubDir(rig.Root()) + kSet1), nevr_quest::data::boot_stub::StubFileBytes());
  CHECK(HasEvent(rig.warns, "data_stub set=48037dc70b0ecab2") && HasEvent(rig.warns, "result=kept reason=manifest_not_ours"));
  CHECK(HasEvent(rig.warns, "consequence="));
}

TEST(AManifestThatCannotBeReadIsKeptAndWarnedAbout) {
  Rig rig;
  rig.config.all_assets = true;
  rig.present = {StubDir(rig.Root()) + kSet0};
  rig.config.read_file = [](const std::string&, std::string&) { return false; };
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kStubbed);
  CHECK(HasEvent(rig.warns, "data_stub set=48037dc70b0ecab2") && HasEvent(rig.warns, "result=kept reason=manifest_unreadable"));
  CHECK(!Exists(StubDir(rig.Root()) + kSet0));  // not written over
  CHECK(Exists(StubDir(rig.Root()) + kSet1));
}

TEST(AStubThatCannotBeWrittenFailsTheStepWithTheFloorMessageAndStopsBeforeTheWorker) {
  Rig rig;
  rig.config.all_assets = true;
  int ready_calls = 0;
  rig.config.all_assets_ready = [&ready_calls](const std::string&) {
    ++ready_calls;
    return AllAssetsResult{};
  };
  std::vector<std::string> attempted;
  rig.config.write_file = [&attempted](const std::string& path, const std::string&) {
    attempted.push_back(path);
    return false;
  };
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kFailed);
  CHECK(r.status == FetchStatus::kBadRoot);
  CHECK(Contains(r.message, StubDir(rig.Root()) + kSet0));  // the file that could not be written
  CHECK_EQ(rig.shown.size(), static_cast<std::size_t>(1));
  CHECK(!rig.shown.empty() && rig.shown[0] == r.message);
  CHECK_EQ(rig.order.size(), static_cast<std::size_t>(2));  // present, hold: the player reads why
  CHECK_EQ(attempted.size(), static_cast<std::size_t>(1));  // the first failure ends it: the second file is not tried
  CHECK_EQ(ready_calls, 0);
  CHECK(HasEvent(rig.warns, "result=write_failed"));
  CHECK_EQ(net.calls, 0);
}

TEST(TheWorkerSeamRunsOnceAfterBothStubsAndItsFailureEndsTheStep) {
  {
    Rig rig;
    rig.config.all_assets = true;
    std::vector<std::string> roots;
    bool both_there = false;
    rig.config.all_assets_ready = [&](const std::string& root) {
      roots.push_back(root);
      both_there = Exists(StubDir(root) + kSet0) && Exists(StubDir(root) + kSet1);
      return AllAssetsResult{};
    };
    OneShotTransport net(rig.zip, true);
    const StepResult r = RunDataStep(rig.config, net);
    CHECK_EQ(r.outcome, StepOutcome::kStubbed);
    CHECK_EQ(roots.size(), static_cast<std::size_t>(1));
    CHECK(!roots.empty() && roots[0] == rig.Root());
    CHECK(both_there);  // the engine reads the stubs, so they are there before anything else goes on
    CHECK(HasEvent(rig.events, "data_all_assets_ready result=ok"));
    CHECK(rig.shown.empty());
  }
  {
    Rig rig;
    rig.config.all_assets = true;
    rig.config.all_assets_ready = [](const std::string&) { return AllAssetsResult{false, "The game files could not be downloaded."}; };
    OneShotTransport net(rig.zip, true);
    const StepResult r = RunDataStep(rig.config, net);
    CHECK(r.status == FetchStatus::kWorkerRefused);
    CHECK_EQ(r.outcome, StepOutcome::kFailed);
    CHECK_EQ(r.message, std::string("The game files could not be downloaded."));
    CHECK_EQ(rig.shown.size(), static_cast<std::size_t>(1));
    CHECK(!rig.shown.empty() && rig.shown[0] == r.message);
    CHECK(!rig.order.empty() && rig.order.back() == "hold");
    CHECK(HasEvent(rig.warns, "data_all_assets_ready result=failed"));
  }
  {
    Rig rig;  // no worker wired: the step says so and still stubs
    rig.config.all_assets = true;
    OneShotTransport net(rig.zip, true);
    CHECK_EQ(RunDataStep(rig.config, net).outcome, StepOutcome::kStubbed);
    CHECK(HasEvent(rig.warns, "data_all_assets_ready result=skipped reason=no_worker"));  // degraded: the engine boots with no asset source
    CHECK(HasEvent(rig.warns, "consequence=") && !HasEvent(rig.events, "data_all_assets_ready result=skipped"));
  }
}

TEST(TheWholeGameModeRestoresAndCleansNothingEvenWhenFetchingIsAllowed) {
  // A marked staging directory of an interrupted download: the stock step (allow_fetch) deletes it; this mode touches nothing it did not make.
  Rig rig;
  rig.config.all_assets = true;
  rig.config.allow_fetch = true;
  ::mkdir(rig.Root().c_str(), 0755);
  const std::string staging = rig.Root() + "/_data.staging-abcdef012345";
  ::mkdir(staging.c_str(), 0755);
  WriteFile(staging + "/x", "x");
  CHECK(MarkOwned(staging));
  OneShotTransport net(rig.zip, true);
  CHECK_EQ(RunDataStep(rig.config, net).outcome, StepOutcome::kStubbed);
  CHECK(Exists(staging) && Exists(OwnerMarkerPath(staging)));
  CHECK(!HasEvent(rig.events, "data_recover"));
}

TEST(WithoutARootOverrideTheStubsGoToTheRootTheDecisionPicked) {
  Rig rig;
  rig.config.all_assets = true;
  rig.config.fetch_root_override.clear();
  rig.present = {Legacy("_data")};  // the legacy root alone holds a `_data`: it is the engine's root
  std::vector<std::string> dirs, files;
  rig.config.make_dirs = [&dirs](const std::string& dir) {
    dirs.push_back(dir);
    return true;
  };
  rig.config.write_file = [&files](const std::string& path, const std::string&) {
    files.push_back(path);
    return true;
  };
  OneShotTransport net(rig.zip, true);
  const StepResult r = RunDataStep(rig.config, net);
  CHECK_EQ(r.outcome, StepOutcome::kStubbed);
  CHECK_EQ(r.root, Legacy());
  CHECK_EQ(files.size(), static_cast<std::size_t>(2));
  CHECK(files.size() == 2 && files[0] == Legacy() + "/_data/5932408047/rad15/android/manifests/" + kSet0);
  CHECK(files.size() == 2 && files[1] == Legacy() + "/_data/5932408047/rad15/android/manifests/" + kSet1);
  CHECK(!dirs.empty() && dirs[0] == Legacy() + "/_data/5932408047/rad15/android/manifests");
}

// The size of a manifest the step will read is a share of the device's memory (the game itself reads a manifest whole, sized by the
// file, and is bounded by its allocator: CSysCompression::DecompressAndReadFile libr15 0x2b9e0a0), one function every memory-based
// limit goes through (core/device_memory.h).
TEST(TheManifestReadLimitIsAShareOfTheDevicesMemory) {
  CHECK_EQ(ManifestReadLimit(8000000), nevr_device::Share(8000000, kMemoryShareForLocalManifest));
  CHECK(ManifestReadLimit(1000000) < ManifestReadLimit(2000000));
  CHECK(ManifestReadLimit(2000000) < ManifestReadLimit(4000000000ull));
  CHECK_EQ(ManifestReadLimit(0), static_cast<std::uint64_t>(0));
}

TEST(AManifestFileIsReadUpToTheDevicesLimitAndRefusedPastIt) {
  Scratch scratch;
  const std::string path = scratch.Path("manifest");
  const std::string bytes(100000, 'm');
  WriteFile(path, bytes);
  StepConfig big;
  big.memory_available = [](std::uint64_t* m) { *m = 100000 * kMemoryShareForLocalManifest; return true; };  // the limit is exactly the file
  std::string got;
  CHECK(ReadBoundedFile(big, path, got));
  CHECK_EQ(got, bytes);
  StepConfig small;
  small.memory_available = [](std::uint64_t* m) { *m = 100000 * kMemoryShareForLocalManifest - kMemoryShareForLocalManifest; return true; };  // one byte short
  CHECK(!ReadBoundedFile(small, path, got));
  StepConfig larger;  // a device with more memory reads what a smaller one refused
  larger.memory_available = [](std::uint64_t* m) { *m = 1000 * 1000 * 1000; return true; };
  CHECK(ReadBoundedFile(larger, path, got));
}

// A refusal says why: the reason is what the step logs in the manifest's place.
TEST(AFileThatIsNotReadSaysWhy) {
  Scratch scratch;
  const std::string path = scratch.Path("manifest");
  WriteFile(path, std::string(1000, 'm'));
  std::string got;
  const char* why = "";
  StepConfig small;
  small.memory_available = [](std::uint64_t* m) { *m = 100 * kMemoryShareForLocalManifest; return true; };
  CHECK(!ReadBoundedFile(small, path, got, &why));
  CHECK_EQ(std::string(why), std::string("over_limit"));
  StepConfig none;
  none.memory_available = [](std::uint64_t*) { return false; };
  CHECK(!ReadBoundedFile(none, path, got, &why));
  CHECK_EQ(std::string(why), std::string("no_memory_fact"));
  StepConfig big;
  big.memory_available = [](std::uint64_t* m) { *m = 1000 * 1000 * 1000; return true; };
  CHECK(!ReadBoundedFile(big, scratch.Path("absent"), got, &why));
  CHECK_EQ(std::string(why), std::string("unreadable"));
  CHECK(ReadBoundedFile(big, path, got, &why));
  CHECK_EQ(std::string(why), std::string(""));
}

// The step's own log line names the reason where the manifest's hash would be (real files, as the seams test above).
TEST(TheStepLogsWhyAManifestWasNotReadInPlaceOfItsHash) {
  Rig rig;
  const std::string root = rig.scratch.Path("real");
  ::mkdir(root.c_str(), 0755);
  const std::string manifest_bytes = "a real manifest on disk";
  const std::string manifest_sha = Sha256Hex(manifest_bytes.data(), manifest_bytes.size());
  MakeTree(root, kManifestRel, manifest_bytes);
  MakeTree(root, std::string(kPackageDir) + "48037dc70b0ecab2_0", "abc");
  MakeTree(root, std::string(kPackageDir) + "48037dc70b0ecab2_1", "abc");
  const char* const abc_sha = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
  const PinnedPackage packages[2] = {{0, 3, abc_sha, false}, {1, 3, abc_sha, true}};
  const PinnedDataSet set = {manifest_sha.c_str(), packages, 2};
  rig.config.pinned_sets = &set;
  rig.config.pinned_set_count = 1;
  rig.config.engine_root = root;
  rig.config.exists = &MappedExists;
  rig.config.memory_available = [](std::uint64_t*) { return false; };  // the device gives no memory fact
  OneShotTransport net(rig.zip, true);
  RunDataStep(rig.config, net);
  CHECK(!rig.events.empty() && Contains(rig.events[0], "manifest_sha256=unreadable:no_memory_fact"));
  rig.events.clear();
  rig.config.memory_available = [](std::uint64_t* m) { *m = 8 * kMemoryShareForLocalManifest; return true; };  // a limit of 8 bytes
  OneShotTransport net2(rig.zip, true);
  RunDataStep(rig.config, net2);
  CHECK(!rig.events.empty() && Contains(rig.events[0], "manifest_sha256=unreadable:over_limit"));
}

TEST(AManifestIsNotReadWhenTheDeviceGivesNoMemoryFact) {
  Scratch scratch;
  const std::string path = scratch.Path("manifest");
  WriteFile(path, "tiny");
  StepConfig none;
  none.memory_available = [](std::uint64_t*) { return false; };
  std::string got;
  CHECK(!ReadBoundedFile(none, path, got));  // nothing is guessed in the number's place
  StepConfig injected;
  injected.read_file = [](const std::string&, std::string& bytes) { bytes = "from the seam"; return true; };
  CHECK(ReadBoundedFile(injected, path, got) && got == "from the seam");  // the test seam still answers first
  StepConfig real;  // the real memory fact of this machine reads a small file
  CHECK(ReadBoundedFile(real, path, got) && got == "tiny");
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
