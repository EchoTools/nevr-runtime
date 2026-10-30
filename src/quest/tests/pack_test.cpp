// Host tests for the bundled content pack step (src/quest/data/pack.cpp, docs/adr/0029-quest-bundled-content-pack.md): an APK
// built here, byte by byte, carries (or does not carry) assets/nevr-pack; the step runs over a scratch data root.
// Covers: no pack, no APK, a pack placed (package first, manifest last, the stock manifest kept), idempotence, a repaired
// package, a stock manifest the pack does not replace, no space (and its boundary), a digest that does not match, a kill between
// the writes and the next run's cleanup, a backup that is never overwritten, every way pack.json can be unusable, and the APK
// path of a library.

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "quest/data/pack.h"
#include "quest/data/sha256.h"
#include "quest/data/staging.h"
#include "quest/tests/data_test_support.h"
#include "quest/tests/mini_test.h"

using namespace nevr_quest::data;
using namespace data_test;

namespace {

constexpr const char* kVersionDir = "5932408047";
constexpr const char* kManifestId = "48037dc70b0ecab2";

const std::string kStockManifest = "STOCK-MANIFEST-" + Pattern(3000);
const std::string kPackManifest = "PACK-MANIFEST-" + Pattern(5000);
const std::string kPackPackage = "PACK-PACKAGE-" + Pattern(70000);

struct Logged {
  EventLevel level;
  Event event;
};

std::string Field(const Event& e, const std::string& key) {
  for (const auto& kv : e.fields) {
    if (kv.first == key) return kv.second;
  }
  return "<absent>";
}

struct Rig {
  Scratch scratch;
  std::string root = scratch.Path("root");
  std::string android = root + "/_data/" + kVersionDir + "/rad15/android";
  std::string apk = scratch.Path("game.apk");
  std::vector<Logged> events;
  std::vector<std::string> shown;
  int holds = 0;
  std::vector<std::string> probes;       // "<point> <file name>", in order
  std::function<bool(const char*, const std::string&)> probe;
  std::function<std::uint64_t(const std::string&)> available;

  // What the next APK is built from. A test changes these before BuildApk.
  nlohmann::json pack = BasePack();
  std::string package_bytes = kPackPackage;
  std::string manifest_bytes = kPackManifest;
  bool with_pack = true;
  bool package_in_apk = true;
  std::int64_t package_crc_delta = 0;
  std::uint16_t package_method = 0;  // stored, as the repack writes it

  Rig() {
    ::mkdir(root.c_str(), 0755);
    MakeDirsBelow(root, std::string("_data/") + kVersionDir + "/rad15/android/manifests");
    MakeDirsBelow(root, std::string("_data/") + kVersionDir + "/rad15/android/packages");
    WriteStock(kStockManifest);
  }

  static nlohmann::json BasePack() {
    nlohmann::json files = nlohmann::json::array();
    files.push_back({{"path", std::string("packages/") + kManifestId + "_1"},
                     {"sha256", Sha256Hex(kPackPackage.data(), kPackPackage.size())},
                     {"size", kPackPackage.size()}});
    files.push_back({{"path", std::string("manifests/") + kManifestId},
                     {"sha256", Sha256Hex(kPackManifest.data(), kPackManifest.size())},
                     {"size", kPackManifest.size()}});
    return {{"schema", 1},
            {"id", "combat-pack"},
            {"version", "1.2.3"},
            {"stock_data_version", kVersionDir},
            {"requires_stock_manifest_sha256", nlohmann::json::array({Sha256Hex(kStockManifest.data(), kStockManifest.size())})},
            {"files", files},
            {"source_zip_sha256", std::string(64, 'a')},
            {"source_zip_size", 123456}};
  }

  std::string ManifestPath() const { return android + "/manifests/" + kManifestId; }
  std::string PackagePath() const { return android + "/packages/" + kManifestId + "_1"; }
  void WriteStock(const std::string& bytes) { WriteFile(ManifestPath(), bytes); }

  void BuildApk() {
    std::vector<SrcEntry> entries;
    entries.emplace_back("AndroidManifest.xml", "manifest-xml");
    for (int i = 0; i < 100; ++i) entries.emplace_back("res/layout/f" + std::to_string(i) + ".xml", "layout " + std::to_string(i));
    entries.emplace_back("lib/arm64-v8a/libx.so", Pattern(500), 0);
    if (with_pack) {
      entries.emplace_back("assets/nevr-pack/pack.json", pack.dump(), 8);
      if (package_in_apk) {
        SrcEntry package(std::string("assets/nevr-pack/files/packages/") + kManifestId + "_1", package_bytes, package_method);
        package.crc_delta = package_crc_delta;
        entries.push_back(package);
      }
      entries.emplace_back(std::string("assets/nevr-pack/files/manifests/") + kManifestId, manifest_bytes, 0);
    }
    WriteFile(apk, BuildZip(entries));
  }

  PackResult Run() {
    events.clear();
    shown.clear();
    holds = 0;
    probes.clear();
    PackConfig config;
    config.apk_path = apk;
    config.root = root;
    config.available_bytes = available;
    config.on_event = [this](EventLevel level, const Event& e) { events.push_back({level, e}); };
    config.present = [this](const std::string& m) { shown.push_back(m); };
    config.hold = [this] { ++holds; };
    config.probe = [this](const char* point, const std::string& path) {
      probes.push_back(std::string(point) + " " + path.substr(path.rfind('/') + 1));
      return probe ? probe(point, path) : true;
    };
    return RunPackStep(config);
  }

  const Logged* Find(const std::string& name, const std::string& key = "", const std::string& value = "") const {
    for (const Logged& l : events) {
      if (l.event.name == name && (key.empty() || Field(l.event, key) == value)) return &l;
    }
    return nullptr;
  }
  int Count(const std::string& name) const {
    int n = 0;
    for (const Logged& l : events) n += l.event.name == name ? 1 : 0;
    return n;
  }
};

std::string Hex(const std::string& bytes) { return Sha256Hex(bytes.data(), bytes.size()); }

std::uint64_t PlacedBytes() { return kPackPackage.size() + kPackManifest.size(); }

// Every name in a directory, sorted, without the dot entries.
std::vector<std::string> Names(const std::string& dir) {
  std::vector<std::string> out;
  if (DIR* d = ::opendir(dir.c_str())) {
    while (const dirent* e = ::readdir(d)) {
      const std::string n = e->d_name;
      if (n != "." && n != "..") out.push_back(n);
    }
    ::closedir(d);
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace

// ---------------- no pack ----------------

TEST(AnApkWithNoPackIsOneInfoLineAndNothingWritten) {
  Rig rig;
  rig.with_pack = false;
  rig.BuildApk();
  const std::vector<std::string> before = Names(rig.android + "/manifests");
  const PackResult r = rig.Run();
  CHECK(r.outcome == PackOutcome::kNone);
  CHECK_EQ(rig.events.size(), std::size_t{1});
  const Logged* check = rig.Find("pack_check");
  CHECK(check != nullptr && check->level == EventLevel::kInfo && Field(check->event, "outcome") == "none");
  CHECK(rig.Find("pack_step") == nullptr);
  CHECK(rig.shown.empty() && rig.holds == 0);
  CHECK(Names(rig.android + "/manifests") == before);
  CHECK_EQ(ReadFile(rig.ManifestPath()), kStockManifest);
  CHECK(!Exists(rig.PackagePath()));
}

TEST(AnUnknownOrUnreadableApkIsNoneWithItsReasonAndNothingWritten) {
  Rig rig;
  rig.BuildApk();
  PackConfig config;
  config.root = rig.root;
  std::vector<Logged> events;
  config.on_event = [&events](EventLevel level, const Event& e) { events.push_back({level, e}); };
  CHECK(RunPackStep(config).outcome == PackOutcome::kNone);  // no path at all
  CHECK(events.size() == 1 && Field(events[0].event, "reason") == "apk_path_unknown");
  events.clear();
  WriteFile(rig.scratch.Path("not.apk"), "this is not a zip archive, not even close");
  config.apk_path = rig.scratch.Path("not.apk");
  CHECK(RunPackStep(config).outcome == PackOutcome::kNone);
  CHECK(events.size() == 1 && Field(events[0].event, "reason") == "apk_unreadable");
  CHECK(!Exists(rig.PackagePath()));
}

// ---------------- placing ----------------

TEST(ThePackIsPlacedPackageFirstManifestLastAndTheStockManifestIsKept) {
  Rig rig;
  rig.BuildApk();
  const PackResult r = rig.Run();
  CHECK(r.outcome == PackOutcome::kPlaced && r.status == PackStatus::kOk);
  CHECK_EQ(ReadFile(rig.PackagePath()), kPackPackage);
  CHECK_EQ(ReadFile(rig.ManifestPath()), kPackManifest);
  CHECK_EQ(ReadFile(rig.ManifestPath() + ".stock"), kStockManifest);
  CHECK(IsOwned(rig.ManifestPath() + ".stock"));
  // No temporary and no marker of one is left; the only marker is the backup's.
  const std::vector<std::string> manifests = Names(rig.android + "/manifests");
  const std::vector<std::string> want = {std::string(kManifestId), std::string(kManifestId) + ".stock",
                                         std::string(kManifestId) + ".stock.nevr-owned"};
  CHECK(manifests == want);
  CHECK(Names(rig.android + "/packages") == std::vector<std::string>{std::string(kManifestId) + "_1"});
  // The events: the check, one place per file in the order written, the step, and the data now in place.
  const Logged* check = rig.Find("pack_check");
  CHECK(check != nullptr && Field(check->event, "outcome") == "place" && Field(check->event, "id") == "combat-pack");
  CHECK_EQ(Field(check->event, "manifest_sha256"), Hex(kStockManifest));
  CHECK_EQ(Field(check->event, "pack_manifest_sha256"), Hex(kPackManifest));
  std::vector<std::string> places;
  for (const Logged& l : rig.events) {
    if (l.event.name == "pack_place") places.push_back(Field(l.event, "path") + "=" + Field(l.event, "result"));
  }
  const std::vector<std::string> want_places = {std::string("packages/") + kManifestId + "_1=placed",
                                                std::string("manifests/") + kManifestId + "=placed"};
  CHECK(places == want_places);
  const Logged* manifest_place = rig.Find("pack_place", "path", std::string("manifests/") + kManifestId);
  CHECK(manifest_place != nullptr && Field(manifest_place->event, "stock_backup") == "kept");
  CHECK_EQ(Field(manifest_place->event, "sha256"), Hex(kPackManifest));
  CHECK_EQ(Field(manifest_place->event, "bytes"), std::to_string(kPackManifest.size()));
  const Logged* step = rig.Find("pack_step");
  CHECK(step != nullptr && step->level == EventLevel::kInfo && Field(step->event, "outcome") == "placed");
  CHECK(Field(step->event, "duration_ms") != "<absent>" && Field(step->event, "version") == "1.2.3" &&
        Field(step->event, "status") == "ok" && Field(step->event, "root") == rig.root);
  // A report says which data is in place: the data_version event carries the pack's manifest hash.
  const Logged* version = rig.Find("data_version");
  CHECK(version != nullptr && Field(version->event, "manifest_sha256") == Hex(kPackManifest) &&
        Field(version->event, "data_version") == kVersionDir && Field(version->event, "pack") == "combat-pack");
  CHECK(rig.shown.empty());
}

TEST(TheGameNeverSeesAManifestWithoutThePackageItNames) {
  Rig rig;
  rig.BuildApk();
  bool checked = false;
  rig.probe = [&](const char* point, const std::string& path) {
    if (std::string(point) == "tmp_written" && path == rig.ManifestPath()) {
      // The manifest's replacement is complete but not in place: the stock manifest is still the manifest and the package is
      // already whole.
      checked = true;
      CHECK_EQ(ReadFile(rig.ManifestPath()), kStockManifest);
      CHECK_EQ(ReadFile(rig.PackagePath()), kPackPackage);
    }
    if (std::string(point) == "tmp_written" && path == rig.PackagePath()) {
      CHECK(!Exists(rig.PackagePath()));  // the package is not under its name before it is whole
      CHECK_EQ(ReadFile(rig.ManifestPath()), kStockManifest);
    }
    return true;
  };
  CHECK(rig.Run().outcome == PackOutcome::kPlaced);
  CHECK(checked);
  const std::vector<std::string> order = {std::string("tmp_written ") + kManifestId + "_1", std::string("renamed ") + kManifestId + "_1",
                                          std::string("tmp_written ") + kManifestId, std::string("renamed ") + kManifestId};
  CHECK(rig.probes == order);
}

TEST(ARunOverAPlacedPackIsCurrentAndWritesNothing) {
  Rig rig;
  rig.BuildApk();
  CHECK(rig.Run().outcome == PackOutcome::kPlaced);
  struct stat before_manifest, before_package;
  CHECK_EQ(::stat(rig.ManifestPath().c_str(), &before_manifest), 0);
  CHECK_EQ(::stat(rig.PackagePath().c_str(), &before_package), 0);
  const std::vector<std::string> names = Names(rig.android + "/manifests");
  const PackResult r = rig.Run();
  CHECK(r.outcome == PackOutcome::kCurrent && r.status == PackStatus::kOk);
  const Logged* check = rig.Find("pack_check");
  CHECK(check != nullptr && Field(check->event, "outcome") == "current");
  CHECK_EQ(Field(check->event, "manifest_sha256"), Hex(kPackManifest));
  CHECK(rig.probes.empty());  // not one temporary was written
  CHECK(rig.Find("data_version") == nullptr);
  struct stat after_manifest, after_package;
  CHECK_EQ(::stat(rig.ManifestPath().c_str(), &after_manifest), 0);
  CHECK_EQ(::stat(rig.PackagePath().c_str(), &after_package), 0);
  CHECK(before_manifest.st_ino == after_manifest.st_ino && before_manifest.st_mtime == after_manifest.st_mtime);
  CHECK(before_package.st_ino == after_package.st_ino && before_package.st_mtime == after_package.st_mtime);
  CHECK(Names(rig.android + "/manifests") == names);
  for (const Logged& l : rig.events) {
    if (l.event.name == "pack_place") CHECK_EQ(Field(l.event, "result"), std::string("current"));
  }
  const Logged* step = rig.Find("pack_step");
  CHECK(step != nullptr && Field(step->event, "outcome") == "current");
}

TEST(APackageThatWentMissingIsPlacedAgainAndTheManifestIsLeftAlone) {
  Rig rig;
  rig.BuildApk();
  CHECK(rig.Run().outcome == PackOutcome::kPlaced);
  CHECK_EQ(::unlink(rig.PackagePath().c_str()), 0);
  struct stat before;
  CHECK_EQ(::stat(rig.ManifestPath().c_str(), &before), 0);
  const PackResult r = rig.Run();
  CHECK(r.outcome == PackOutcome::kPlaced);
  CHECK_EQ(ReadFile(rig.PackagePath()), kPackPackage);
  struct stat after;
  CHECK_EQ(::stat(rig.ManifestPath().c_str(), &after), 0);
  CHECK(before.st_ino == after.st_ino);
  const Logged* check = rig.Find("pack_check");
  CHECK(check != nullptr && Field(check->event, "outcome") == "place");
  const Logged* manifest = rig.Find("pack_place", "path", std::string("manifests/") + kManifestId);
  CHECK(manifest != nullptr && Field(manifest->event, "result") == "current");
}

TEST(APackageWithTheRightSizeAndOtherBytesIsReplaced) {
  Rig rig;
  rig.BuildApk();
  CHECK(rig.Run().outcome == PackOutcome::kPlaced);
  std::string wrong = kPackPackage;
  wrong[10] = static_cast<char>(wrong[10] ^ 0x01);
  WriteFile(rig.PackagePath(), wrong);
  CHECK(rig.Run().outcome == PackOutcome::kPlaced);
  CHECK_EQ(ReadFile(rig.PackagePath()), kPackPackage);
}

TEST(AnExistingStockBackupIsNeverOverwritten) {
  Rig rig;
  rig.BuildApk();
  WriteFile(rig.ManifestPath() + ".stock", "AN-EARLIER-BACKUP");
  const PackResult r = rig.Run();
  CHECK(r.outcome == PackOutcome::kPlaced);
  CHECK_EQ(ReadFile(rig.ManifestPath() + ".stock"), std::string("AN-EARLIER-BACKUP"));
  const Logged* manifest = rig.Find("pack_place", "path", std::string("manifests/") + kManifestId);
  CHECK(manifest != nullptr && Field(manifest->event, "stock_backup") == "existing");
  CHECK_EQ(ReadFile(rig.ManifestPath()), kPackManifest);
}

TEST(AStockManifestThatIsASecondRequiredHashIsReplacedToo) {
  Rig rig;
  const std::string other = "THE-OTHER-STOCK-MANIFEST";
  rig.pack["requires_stock_manifest_sha256"] = nlohmann::json::array({Hex(kStockManifest), Hex(other)});
  std::sort(rig.pack["requires_stock_manifest_sha256"].begin(), rig.pack["requires_stock_manifest_sha256"].end());
  rig.WriteStock(other);
  rig.BuildApk();
  CHECK(rig.Run().outcome == PackOutcome::kPlaced);
  CHECK_EQ(ReadFile(rig.ManifestPath() + ".stock"), other);
}

// ---------------- a manifest the pack does not replace ----------------

TEST(AManifestThePackDoesNotNameIsLeftAloneWithAWarningNamingBothHashes) {
  Rig rig;
  rig.WriteStock("SOME-OTHER-BUILDS-MANIFEST");
  rig.BuildApk();
  const PackResult r = rig.Run();
  CHECK(r.outcome == PackOutcome::kStockMismatch);
  CHECK_EQ(ReadFile(rig.ManifestPath()), std::string("SOME-OTHER-BUILDS-MANIFEST"));
  CHECK(!Exists(rig.PackagePath()) && !Exists(rig.ManifestPath() + ".stock"));
  CHECK(Names(rig.android + "/manifests") == std::vector<std::string>{std::string(kManifestId)});
  const Logged* check = rig.Find("pack_check");
  CHECK(check != nullptr && check->level == EventLevel::kWarn && Field(check->event, "outcome") == "stock_mismatch");
  CHECK_EQ(Field(check->event, "manifest_sha256"), Hex("SOME-OTHER-BUILDS-MANIFEST"));
  CHECK_EQ(Field(check->event, "requires_stock_manifest_sha256"), Hex(kStockManifest));
  CHECK_EQ(Field(check->event, "pack_manifest_sha256"), Hex(kPackManifest));
  CHECK(rig.probes.empty());
  CHECK(rig.shown.empty() && rig.holds == 0);  // the game starts on what is there
  const Logged* step = rig.Find("pack_step");
  CHECK(step != nullptr && step->level == EventLevel::kWarn && Field(step->event, "outcome") == "stock_mismatch");
}

TEST(NoManifestAtAllIsAMismatchAndAMarkerWithoutOneChangesNothing) {
  Rig rig;
  CHECK_EQ(::unlink(rig.ManifestPath().c_str()), 0);
  rig.BuildApk();
  CHECK(rig.Run().outcome == PackOutcome::kStockMismatch);
  CHECK_EQ(Field(rig.Find("pack_check")->event, "manifest_sha256"), std::string("none"));
  CHECK(!Exists(rig.PackagePath()) && !Exists(rig.ManifestPath()));
}

TEST(AManifestThatIsALinkIsNotReadThroughTheLink) {
  Rig rig;
  WriteFile(rig.scratch.Path("elsewhere"), kStockManifest);  // the right bytes, behind a link
  CHECK_EQ(::unlink(rig.ManifestPath().c_str()), 0);
  CHECK_EQ(::symlink(rig.scratch.Path("elsewhere").c_str(), rig.ManifestPath().c_str()), 0);
  rig.BuildApk();
  CHECK(rig.Run().outcome == PackOutcome::kStockMismatch);
  CHECK_EQ(Field(rig.Find("pack_check")->event, "manifest_sha256"), std::string("unreadable"));
  CHECK_EQ(ReadFile(rig.scratch.Path("elsewhere")), kStockManifest);
  CHECK(!Exists(rig.PackagePath()));
}

// ---------------- space ----------------

TEST(NoSpaceWritesNothingAndSaysFreeAndNeededMiB) {
  Rig rig;
  rig.BuildApk();
  rig.available = [](const std::string&) { return std::uint64_t{10} * 1024 * 1024; };
  const PackResult r = rig.Run();
  CHECK(r.outcome == PackOutcome::kFailed && r.status == PackStatus::kNoSpace);
  CHECK(!Exists(rig.PackagePath()) && !Exists(rig.ManifestPath() + ".stock"));
  CHECK_EQ(ReadFile(rig.ManifestPath()), kStockManifest);
  CHECK(Names(rig.android + "/packages").empty());
  CHECK(rig.probes.empty());
  CHECK(rig.shown.size() == 1 && rig.holds == 1);
  CHECK(rig.shown[0] == r.message);
  const std::uint64_t needed = PlacedBytes() + kPackHeadroomBytes;
  const std::string mib_needed = std::to_string((needed + 1048575) / 1048576);
  CHECK(r.message.find("10 MiB free") != std::string::npos);
  CHECK(r.message.find(mib_needed + " MiB needed") != std::string::npos);
  CHECK(r.message.find("combat-pack") != std::string::npos);
  const Logged* step = rig.Find("pack_step");
  CHECK(step != nullptr && step->level == EventLevel::kError && Field(step->event, "outcome") == "failed" &&
        Field(step->event, "status") == "no_space");
}

TEST(TheSpaceCheckPassesAtExactlyWhatIsNeededAndFailsOneByteShort) {
  const std::uint64_t needed = PlacedBytes() + kPackHeadroomBytes;
  {
    Rig rig;
    rig.BuildApk();
    rig.available = [needed](const std::string&) { return needed - 1; };
    CHECK(rig.Run().status == PackStatus::kNoSpace);
  }
  {
    Rig rig;
    rig.BuildApk();
    rig.available = [needed](const std::string&) { return needed; };
    CHECK(rig.Run().outcome == PackOutcome::kPlaced);
  }
}

TEST(OnlyTheFilesThatAreNotInPlaceAreCountedForSpace) {
  Rig rig;
  rig.BuildApk();
  CHECK(rig.Run().outcome == PackOutcome::kPlaced);
  CHECK_EQ(::unlink(rig.PackagePath().c_str()), 0);  // only the package is to be written again
  const std::uint64_t needed = kPackPackage.size() + kPackHeadroomBytes;
  rig.available = [needed](const std::string&) { return needed; };
  CHECK(rig.Run().outcome == PackOutcome::kPlaced);
}

TEST(AStockManifestThatChangesBeforeItIsKeptAsideIsNotReplaced) {
  Rig rig;
  rig.BuildApk();
  rig.probe = [&](const char* point, const std::string& path) {
    if (std::string(point) == "tmp_written" && path == rig.ManifestPath()) rig.WriteStock("CHANGED-AFTER-THE-CHECK");
    return true;
  };
  const PackResult r = rig.Run();
  CHECK(r.outcome == PackOutcome::kFailed && r.status == PackStatus::kBackupFailed);
  CHECK_EQ(ReadFile(rig.ManifestPath()), std::string("CHANGED-AFTER-THE-CHECK"));  // not replaced
  CHECK(!Exists(rig.ManifestPath() + ".stock") && !Exists(rig.ManifestPath() + kPackTmpSuffix));
  CHECK(Names(rig.android + "/manifests") == std::vector<std::string>{std::string(kManifestId)});  // no marker left either
  CHECK(rig.shown.size() == 1 && rig.holds == 1);
}

// ---------------- a digest that does not match ----------------

TEST(APackageWhoseBytesAreNotTheDigestIsNotPlacedAndLeavesNoTemporary) {
  Rig rig;
  rig.pack["files"][0]["sha256"] = std::string(64, '0');
  rig.BuildApk();
  const PackResult r = rig.Run();
  CHECK(r.outcome == PackOutcome::kFailed && r.status == PackStatus::kHashMismatch);
  CHECK(!Exists(rig.PackagePath()));
  CHECK_EQ(ReadFile(rig.ManifestPath()), kStockManifest);
  CHECK(Names(rig.android + "/packages").empty());  // not the temporary, not its marker
  CHECK(Names(rig.android + "/manifests") == std::vector<std::string>{std::string(kManifestId)});
  CHECK(rig.shown.size() == 1 && rig.holds == 1);
  const Logged* place = rig.Find("pack_place", "result", "failed");
  CHECK(place != nullptr && Field(place->event, "status") == "hash_mismatch");
}

TEST(AManifestWhoseBytesAreNotTheDigestLeavesTheStockManifestAndNoBackup) {
  Rig rig;
  rig.pack["files"][1]["sha256"] = std::string(64, '1');
  rig.BuildApk();
  const PackResult r = rig.Run();
  CHECK(r.outcome == PackOutcome::kFailed && r.status == PackStatus::kHashMismatch);
  CHECK_EQ(ReadFile(rig.ManifestPath()), kStockManifest);
  CHECK(!Exists(rig.ManifestPath() + ".stock"));  // the backup is made only once the replacement verified
  CHECK(Names(rig.android + "/manifests") == std::vector<std::string>{std::string(kManifestId)});
  CHECK_EQ(ReadFile(rig.PackagePath()), kPackPackage);  // the verified package stays: harmless without its manifest
}

TEST(AnEntryOfTheWrongSizeIsAnInvalidPackAndNothingIsTouched) {
  Rig rig;
  rig.package_bytes = kPackPackage + "x";  // the entry is one byte longer than pack.json says
  rig.BuildApk();
  const PackResult r = rig.Run();
  CHECK(r.outcome == PackOutcome::kInvalid);
  CHECK_EQ(Field(rig.Find("pack_check")->event, "reason"), std::string("file_size"));
  CHECK(!Exists(rig.PackagePath()));
}

TEST(AnEntryThatFailsItsCrcIsNotPlacedAndLeavesNoTemporary) {
  Rig rig;
  rig.package_crc_delta = 1;
  rig.BuildApk();
  const PackResult r = rig.Run();
  CHECK(r.outcome == PackOutcome::kFailed && r.status == PackStatus::kExtractFailed);
  CHECK(!Exists(rig.PackagePath()));
  CHECK(Names(rig.android + "/packages").empty());
  CHECK_EQ(ReadFile(rig.ManifestPath()), kStockManifest);
}

TEST(ADeflatedEntryIsPlacedLikeAStoredOne) {
  Rig rig;
  rig.package_method = 8;
  rig.BuildApk();
  CHECK(rig.Run().outcome == PackOutcome::kPlaced);
  CHECK_EQ(ReadFile(rig.PackagePath()), kPackPackage);
}

// ---------------- a kill, and the next run ----------------

TEST(AKillBetweenThePackageAndTheManifestLeavesATemporaryTheNextRunRemoves) {
  Rig rig;
  rig.BuildApk();
  rig.probe = [&](const char* point, const std::string& path) {
    return !(std::string(point) == "tmp_written" && path == rig.ManifestPath());  // stop as a kill would
  };
  const PackResult killed = rig.Run();
  CHECK(killed.outcome == PackOutcome::kFailed && killed.status == PackStatus::kInterrupted);
  // What a kill leaves: the package whole, the stock manifest, and the manifest's temporary with its marker.
  CHECK_EQ(ReadFile(rig.PackagePath()), kPackPackage);
  CHECK_EQ(ReadFile(rig.ManifestPath()), kStockManifest);
  CHECK(Exists(rig.ManifestPath() + kPackTmpSuffix) && IsOwned(rig.ManifestPath() + kPackTmpSuffix));
  CHECK_EQ(ReadFile(rig.ManifestPath() + kPackTmpSuffix), kPackManifest);

  // The next launch: the temporary is gone, and the pack is completed (the package already in place is not written again).
  rig.probe = nullptr;
  const PackResult next = rig.Run();
  CHECK(next.outcome == PackOutcome::kPlaced);
  const Logged* recover = rig.Find("data_recover");
  CHECK(recover != nullptr && Field(recover->event, "cleaned") == "1" && Field(recover->event, "failed") == "0");
  CHECK_EQ(ReadFile(rig.ManifestPath()), kPackManifest);
  CHECK_EQ(ReadFile(rig.ManifestPath() + ".stock"), kStockManifest);
  CHECK(!Exists(rig.ManifestPath() + kPackTmpSuffix) && !Exists(OwnerMarkerPath(rig.ManifestPath() + kPackTmpSuffix)));
  const Logged* package = rig.Find("pack_place", "path", std::string("packages/") + kManifestId + "_1");
  CHECK(package != nullptr && Field(package->event, "result") == "current");
  CHECK(Names(rig.android + "/manifests") ==
        (std::vector<std::string>{std::string(kManifestId), std::string(kManifestId) + ".stock",
                                  std::string(kManifestId) + ".stock.nevr-owned"}));
}

TEST(AKillWhileThePackageIsBeingWrittenIsCleanedAndRedone) {
  Rig rig;
  rig.BuildApk();
  rig.probe = [](const char* point, const std::string&) { return std::string(point) != "tmp_written"; };
  CHECK(rig.Run().status == PackStatus::kInterrupted);
  const std::string tmp = rig.PackagePath() + kPackTmpSuffix;
  CHECK(Exists(tmp) && IsOwned(tmp));
  CHECK(!Exists(rig.PackagePath()));
  rig.probe = nullptr;
  CHECK(rig.Run().outcome == PackOutcome::kPlaced);
  CHECK(!Exists(tmp) && !Exists(OwnerMarkerPath(tmp)));
  CHECK_EQ(ReadFile(rig.PackagePath()), kPackPackage);
  CHECK_EQ(ReadFile(rig.ManifestPath()), kPackManifest);
}

TEST(AKillAfterTheManifestIsInPlaceIsCurrentOnTheNextRun) {
  Rig rig;
  rig.BuildApk();
  rig.probe = [&](const char* point, const std::string& path) {
    return !(std::string(point) == "renamed" && path == rig.ManifestPath());
  };
  CHECK(rig.Run().status == PackStatus::kInterrupted);
  rig.probe = nullptr;
  CHECK(rig.Run().outcome == PackOutcome::kCurrent);
}

TEST(ATemporaryWithNoMarkerIsNotOursAndIsNotDeleted) {
  Rig rig;
  rig.BuildApk();
  const std::string tmp = rig.PackagePath() + kPackTmpSuffix;
  WriteFile(tmp, "a file that only has our suffix");
  const PackResult r = rig.Run();
  CHECK(r.outcome == PackOutcome::kFailed && r.status == PackStatus::kWriteFailed);
  CHECK_EQ(ReadFile(tmp), std::string("a file that only has our suffix"));
  CHECK(!Exists(rig.PackagePath()));
  CHECK_EQ(ReadFile(rig.ManifestPath()), kStockManifest);
}

// ---------------- the cleanup routine the data step shares ----------------

TEST(CleanLeftoversCoversThePackToo) {
  Rig rig;
  const std::string tmp = rig.PackagePath() + kPackTmpSuffix;
  WriteFile(tmp, "half a package");
  CHECK(MarkOwned(tmp));
  int failed = 0;
  CHECK_EQ(CleanLeftovers(rig.root, &failed), 1);
  CHECK_EQ(failed, 0);
  CHECK(!Exists(tmp) && !Exists(OwnerMarkerPath(tmp)));
}

TEST(CleanPackLeftoversKeepsEverythingThatIsNotAMarkedPackTemporary) {
  Rig rig;
  const std::string unmarked = rig.PackagePath() + kPackTmpSuffix;
  WriteFile(unmarked, "no marker");
  WriteFile(rig.ManifestPath() + ".stock", "the backup");
  CHECK(MarkOwned(rig.ManifestPath() + ".stock"));
  WriteFile(rig.android + "/packages/x" + kPackTmpSuffix + kOwnerSuffix, "content, not an empty marker");
  WriteFile(rig.android + "/packages/someone-elses.tmp", "other tmp");
  int failed = 0;
  CHECK_EQ(CleanPackLeftovers(rig.root, &failed), 0);
  CHECK_EQ(ReadFile(unmarked), std::string("no marker"));
  CHECK_EQ(ReadFile(rig.ManifestPath() + ".stock"), std::string("the backup"));
  CHECK(IsOwned(rig.ManifestPath() + ".stock"));
  CHECK_EQ(ReadFile(rig.android + "/packages/x" + kPackTmpSuffix + kOwnerSuffix), std::string("content, not an empty marker"));
  CHECK_EQ(ReadFile(rig.android + "/packages/someone-elses.tmp"), std::string("other tmp"));
}

TEST(CleanPackLeftoversRemovesMarkersWhoseItemIsGone) {
  Rig rig;
  CHECK(MarkOwned(rig.ManifestPath() + ".stock"));                  // killed between the marker and the rename
  CHECK(MarkOwned(rig.PackagePath() + kPackTmpSuffix));            // a temporary that was removed by hand
  CHECK_EQ(CleanPackLeftovers(rig.root, nullptr), 0);
  CHECK(!Exists(OwnerMarkerPath(rig.ManifestPath() + ".stock")));
  CHECK(!Exists(OwnerMarkerPath(rig.PackagePath() + kPackTmpSuffix)));
}

TEST(CleanPackLeftoversWalksOnlyManifestsAndPackages) {
  Rig rig;
  MakeDirsBelow(rig.root, std::string("_data/") + kVersionDir + "/rad15/android/scripts");
  const std::string elsewhere = rig.android + "/scripts/x" + kPackTmpSuffix;
  WriteFile(elsewhere, "a temporary of ours, in a directory the pack never writes");
  CHECK(MarkOwned(elsewhere));
  CHECK_EQ(CleanPackLeftovers(rig.root, nullptr), 0);
  CHECK(Exists(elsewhere) && IsOwned(elsewhere));
}

TEST(CleanPackLeftoversDoesNotWalkThroughALink) {
  Rig rig;
  const std::string outside = rig.scratch.Path("outside");
  MakeDirsBelow(rig.scratch.dir, "outside/rad15/android/packages");
  const std::string tmp = outside + "/rad15/android/packages/x" + kPackTmpSuffix;
  WriteFile(tmp, "not ours to touch");
  CHECK(MarkOwned(tmp));
  CHECK_EQ(::symlink(outside.c_str(), (rig.root + "/_data/linked").c_str()), 0);
  CHECK_EQ(CleanPackLeftovers(rig.root, nullptr), 0);
  CHECK_EQ(ReadFile(tmp), std::string("not ours to touch"));
}

// ---------------- pack.json ----------------

namespace {

// Every way the description can be unusable: the step touches nothing and says why.
struct Bad {
  const char* name;
  const char* reason;
  std::function<void(nlohmann::json&)> mutate;
};

std::vector<Bad> BadPacks() {
  return {
      {"schema 2", "bad_schema", [](nlohmann::json& j) { j["schema"] = 2; }},
      {"no id", "bad_id", [](nlohmann::json& j) { j.erase("id"); }},
      {"an id with a slash", "bad_id", [](nlohmann::json& j) { j["id"] = "a/b"; }},
      {"an upper-case id", "bad_id", [](nlohmann::json& j) { j["id"] = "Combat"; }},
      {"a version with two parts", "bad_version", [](nlohmann::json& j) { j["version"] = "1.2"; }},
      {"a version with a letter", "bad_version", [](nlohmann::json& j) { j["version"] = "1.2.x"; }},
      {"another stock data version", "bad_stock_version", [](nlohmann::json& j) { j["stock_data_version"] = "1234567890"; }},
      {"no requires", "bad_requires", [](nlohmann::json& j) { j["requires_stock_manifest_sha256"] = nlohmann::json::array(); }},
      {"three requires", "bad_requires",
       [](nlohmann::json& j) {
         j["requires_stock_manifest_sha256"] = nlohmann::json::array({std::string(64, '1'), std::string(64, '2'), std::string(64, '3')});
       }},
      {"unsorted requires", "bad_requires",
       [](nlohmann::json& j) { j["requires_stock_manifest_sha256"] = nlohmann::json::array({std::string(64, '2'), std::string(64, '1')}); }},
      {"duplicate requires", "bad_requires",
       [](nlohmann::json& j) { j["requires_stock_manifest_sha256"] = nlohmann::json::array({std::string(64, '1'), std::string(64, '1')}); }},
      {"an upper-case hash", "bad_requires",
       [](nlohmann::json& j) { j["requires_stock_manifest_sha256"] = nlohmann::json::array({std::string(64, 'A')}); }},
      {"a path that climbs", "bad_file", [](nlohmann::json& j) { j["files"][0]["path"] = "packages/../../x"; }},
      {"a path in another directory", "bad_file", [](nlohmann::json& j) { j["files"][0]["path"] = "scripts/x"; }},
      {"an absolute path", "bad_file", [](nlohmann::json& j) { j["files"][0]["path"] = "/packages/x"; }},
      {"a path of two components below the directory", "bad_file", [](nlohmann::json& j) { j["files"][0]["path"] = "packages/a/b"; }},
      {"a hidden file", "bad_file", [](nlohmann::json& j) { j["files"][0]["path"] = "packages/.x"; }},
      {"a name of ours", "bad_file", [](nlohmann::json& j) { j["files"][0]["path"] = "packages/x.nevr-pack.tmp"; }},
      {"a backup name", "bad_file", [](nlohmann::json& j) { j["files"][0]["path"] = "packages/x.stock"; }},
      {"a marker name", "bad_file", [](nlohmann::json& j) { j["files"][0]["path"] = "packages/x.nevr-owned"; }},
      {"a bad digest", "bad_sha256", [](nlohmann::json& j) { j["files"][0]["sha256"] = "xyz"; }},
      {"a zero size", "bad_size", [](nlohmann::json& j) { j["files"][0]["size"] = 0; }},
      {"a size over the reader's limit", "bad_size", [](nlohmann::json& j) { j["files"][0]["size"] = 4294967296ull; }},
      {"a negative size", "bad_size", [](nlohmann::json& j) { j["files"][0]["size"] = -5; }},
      {"a duplicate file in another case", "duplicate_file",
       [](nlohmann::json& j) {
         nlohmann::json again = j["files"][0];
         again["path"] = "packages/48037DC70B0ECAB2_1";  // the same name as the first, in other case
         j["files"].push_back(again);
       }},
      {"the same path twice", "duplicate_file", [](nlohmann::json& j) { j["files"].push_back(j["files"][0]); }},
      {"another manifest", "wrong_manifest", [](nlohmann::json& j) { j["files"][1]["path"] = "manifests/0123456789abcdef"; }},
      {"no manifest", "no_manifest", [](nlohmann::json& j) { j["files"] = nlohmann::json::array({j["files"][0]}); }},
      {"too many files", "too_many_files",
       [](nlohmann::json& j) {
         for (int i = 0; i < 8; ++i) {
           nlohmann::json more = j["files"][0];
           more["path"] = "packages/more" + std::to_string(i);
           j["files"].push_back(more);
         }
       }},
      {"no source", "bad_source", [](nlohmann::json& j) { j.erase("source_zip_sha256"); }},
      {"a zero source size", "bad_source", [](nlohmann::json& j) { j["source_zip_size"] = 0; }},
  };
}

}  // namespace

TEST(EveryUnusablePackJsonIsInvalidAndNothingIsTouched) {
  for (const Bad& bad : BadPacks()) {
    Rig rig;
    bad.mutate(rig.pack);
    rig.BuildApk();
    const PackResult r = rig.Run();
    if (r.outcome != PackOutcome::kInvalid) std::fprintf(stderr, "  case '%s': outcome %s\n", bad.name, PackOutcomeName(r.outcome));
    CHECK(r.outcome == PackOutcome::kInvalid);
    const Logged* check = rig.Find("pack_check");
    CHECK(check != nullptr && check->level == EventLevel::kError && Field(check->event, "outcome") == "invalid");
    if (check != nullptr && Field(check->event, "reason") != bad.reason) {
      std::fprintf(stderr, "  case '%s': reason %s, wanted %s\n", bad.name, Field(check->event, "reason").c_str(), bad.reason);
    }
    CHECK(check != nullptr && Field(check->event, "reason") == bad.reason);
    CHECK_EQ(ReadFile(rig.ManifestPath()), kStockManifest);
    CHECK(!Exists(rig.PackagePath()) && !Exists(rig.ManifestPath() + ".stock"));
    CHECK(rig.shown.empty() && rig.probes.empty());
  }
}

TEST(APackJsonThatIsNotJsonOrTooLargeIsInvalid) {
  PackInfo info;
  std::string reason;
  CHECK(!ParsePackJson("not json at all", &info, &reason) && reason == "not_json");
  CHECK(!ParsePackJson("[1,2]", &info, &reason) && reason == "not_json");
  CHECK(!ParsePackJson(std::string(kMaxPackJsonBytes + 1, ' '), &info, &reason) && reason == "too_large");
  CHECK(ParsePackJson(Rig::BasePack().dump(), &info, &reason));
  CHECK_EQ(info.id, std::string("combat-pack"));
  CHECK_EQ(info.manifest_path, std::string("manifests/") + kManifestId);
  CHECK_EQ(info.manifest_sha256, Hex(kPackManifest));
  CHECK_EQ(info.files.size(), std::size_t{2});
}

TEST(APackThatNamesAFileTheApkLacksIsInvalid) {
  Rig rig;
  rig.package_in_apk = false;
  rig.BuildApk();
  const PackResult r = rig.Run();
  CHECK(r.outcome == PackOutcome::kInvalid);
  CHECK_EQ(Field(rig.Find("pack_check")->event, "reason"), std::string("file_missing"));
  CHECK(!Exists(rig.PackagePath()));
}

TEST(AnApkWithManyEntriesIsListedWhereTheDataZipLimitWouldRefuseIt) {
  Rig rig;
  rig.BuildApk();
  std::vector<ZipEntry> entries;
  CHECK_EQ(ReadZipListing(rig.apk, ZipLimits{}, &entries, nullptr), ZipStatus::kTooManyEntries);  // 105 entries, the limit is 64
  CHECK_EQ(ReadZipListing(rig.apk, PackZipLimits(), &entries, nullptr), ZipStatus::kOk);
  CHECK(entries.size() > 64);
  CHECK(PackZipLimits().max_entries > 567);  // a store APK
}

// ---------------- the APK a library came from ----------------

namespace {
bool OnlyBaseApk(const char* path) noexcept { return std::string(path) == "/data/app/~~x/com.pkg-y/base.apk"; }
bool Nothing(const char*) noexcept { return false; }
}  // namespace

TEST(TheApkIsFoundFromWhereTheLinkerMappedTheLibrary) {
  CHECK_EQ(ApkPathForLibrary("/data/app/~~x/com.pkg-y/base.apk!/lib/arm64-v8a/libovrplatformloader.so", &Nothing),
           std::string("/data/app/~~x/com.pkg-y/base.apk"));
  CHECK_EQ(ApkPathForLibrary("/data/app/~~x/com.pkg-y/lib/arm64/libovrplatformloader.so", &OnlyBaseApk),
           std::string("/data/app/~~x/com.pkg-y/base.apk"));
  CHECK_EQ(ApkPathForLibrary("/data/app/~~x/com.pkg-y/lib/arm64/libovrplatformloader.so", &Nothing), std::string());
  CHECK_EQ(ApkPathForLibrary("/data/app/~~x/com.pkg-y/other/arm64/libovrplatformloader.so", &OnlyBaseApk), std::string());
  CHECK_EQ(ApkPathForLibrary("libovrplatformloader.so", &OnlyBaseApk), std::string());
  CHECK_EQ(ApkPathForLibrary("", &OnlyBaseApk), std::string());
  CHECK_EQ(ApkPathForLibrary("!/lib/arm64-v8a/x.so", &OnlyBaseApk), std::string());
  CHECK_EQ(ApkPathForLibrary("/lib/arm64/x.so", &OnlyBaseApk), std::string());
}

int main(int argc, char** argv) { return mini_test::RunAll(argc, argv); }
