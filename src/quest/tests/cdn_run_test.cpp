// Host test for the CDN worker (src/cdn/cdn_run.*; docs/adr/0045-cdn-texture-delivery.md D3 and D4): a scripted server serves a
// manifest and its objects; the worker computes its limits from the device, plans, hashes each object once while it downloads,
// appends it to the store, saves the manifest, mounts and publishes. The second launch mounts with no request and no hash. Every
// way a fetch can go wrong leaves a store that names only whole objects, including a kill after each step.
//
// Built and run by `just test-quest-data`.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cdn/cdn_run.h"
#include "cdn/cdn_script_gate.h"
#include "quest/tests/cdn_fake_env.h"
#include "quest/tests/cdn_zstd_support.h"
#include "quest/tests/test_check.h"

namespace {

namespace nc = nevr_cdn;
using cdn_test::FakeEnv;
using cdn_test::MakeFrame;
using cdn_test::Response;
using cdn_test::Sha256Of;
using json = nlohmann::ordered_json;

constexpr const char* kBase = "https://cargo.echo.taxi/";
constexpr const char* kManifestPath = "v1/allassets/test/manifest.json.zst";
constexpr const char* kDir = "/cache";
constexpr const char* kArchive = "0x2a41cf1c1d9e5d32";
// A device whose memory gives a manifest text limit of 20000 / (8 * 8) = 312 bytes, so a test can go over it.
constexpr std::uint64_t kSmallDeviceMemory = 20000;
// A set large enough that its manifest takes several 97-byte deliveries, so a refusal at the header leaves most of it unsent.
constexpr int kManyObjects = 40;

struct Obj {
  std::string frame;
  std::uint64_t bytes = 0;
  std::string sha;
  std::string type;
  std::string name;
};

struct World {
  std::vector<Obj> objs;
  std::string label = "game";
  std::vector<json> extra_entries;  // entries that point at no object
  json empty = json::array();
  std::string other_label;      // a second label, when set, holding other_objs (the gate turns off one label, never its neighbour)
  std::vector<Obj> other_objs;
  nc::RunConfig config;

  explicit World(int n = 4) {
    config.dir = kDir;
    config.base_url = kBase;
    config.manifest_path = kManifestPath;
    for (int i = 0; i < n; ++i) Add(300 + i, 120 + 7 * i, static_cast<char>('a' + i), kArchive, "0x" + std::to_string(100 + i));
  }
  void Add(std::uint64_t bytes, std::size_t size, char fill, const std::string& type, const std::string& name) {
    Obj o;
    o.bytes = bytes;
    o.frame = MakeFrame(bytes, size, fill);
    o.sha = Sha256Of(o.frame);
    o.type = type;
    o.name = name;
    objs.push_back(o);
  }
  static json EntryOf(const Obj& o, const std::string& label) {
    (void)label;
    return json{{"platform", "android-arm64"}, {"type", o.type}, {"name", o.name}, {"format", "zstd-frame"}, {"sha256", o.sha},
                {"size", o.frame.size()},      {"bytes", o.bytes}, {"align", 16}};
  }
  std::string ManifestText() const {
    json entries = json::array();
    for (const Obj& o : objs) entries.push_back(EntryOf(o, label));
    for (const json& e : extra_entries) entries.push_back(e);
    json labels = json::object();
    labels[label] = json{{"entries", entries}};
    if (!other_label.empty()) {
      json others = json::array();
      for (const Obj& o : other_objs) others.push_back(EntryOf(o, other_label));
      labels[other_label] = json{{"entries", others}};
    }
    return json{{"schema", "nevr-cargo-manifest"}, {"schema_version", 1}, {"labels", labels}, {"empty", empty}}.dump();
  }
  void Serve(FakeEnv& env) const {
    env.server[std::string(kBase) + kManifestPath] = Response{200, cdn_test::ZstdFrameOf(ManifestText()), std::string::npos, ""};
    for (const Obj& o : objs) env.server[std::string(kBase) + "blobs/sha256/" + o.sha] = Response{200, o.frame, std::string::npos, ""};
    for (const Obj& o : other_objs) env.server[std::string(kBase) + "blobs/sha256/" + o.sha] = Response{200, o.frame, std::string::npos, ""};
  }
};

struct Captured {
  std::vector<const nc::Mounted*> sets;
  nc::Publish Sink() {
    return [this](std::unique_ptr<nc::Mounted> m) { sets.push_back(m.release()); };
  }
  ~Captured() {
    for (const nc::Mounted* m : sets) delete m;
  }
};

const auto kGo = [] { return true; };

std::unique_ptr<nc::Store> OpenStore(FakeEnv& env, const nc::RunConfig& c) {
  nc::DeviceFacts facts;
  env.QueryDevice(c.dir, &facts);
  const nc::Limits limits = nc::ComputeLimits(facts, nc::ManifestEntryMemoryBytes());
  nc::StoreOpenInfo info;
  return nc::Store::Open(env, c.dir, limits, &info);
}

std::size_t GetsOfBlobs(const FakeEnv& env) {
  std::size_t n = 0;
  for (const std::string& r : env.requests) {
    if (r.find("blobs/sha256/") != std::string::npos) ++n;
  }
  return n;
}

// ── a first launch ───────────────────────────────────────────────────────────────────────────────────────────────────────
void TheFirstLaunchFetchesStoresSavesMountsAndPublishes() {
  World w;
  FakeEnv env;
  w.Serve(env);
  Captured cap;
  const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  QCHECK(r.manifest_loaded && r.fetched == 4 && r.cached == 0 && r.failed == 0 && r.refused == 0 && r.published);
  QCHECK(std::string(r.stopped).empty());
  QCHECK(cap.sets.size() == 1 && cap.sets[0]->records == 4 && cap.sets[0]->files == 1 && cap.sets[0]->missing == 0);
  QCHECK(GetsOfBlobs(env) == 4);
  QCHECK(env.fs_->files[nc::ManifestCopyPath(kDir)] == cdn_test::ZstdFrameOf(w.ManifestText()));  // the frame as published: the second launch reads this
  std::unique_ptr<nc::Store> store = OpenStore(env, w.config);
  for (const Obj& o : w.objs) QCHECK(store->Has(o.sha));
  // The objects lie back to back in one file.
  std::string all;
  for (const Obj& o : w.objs) all += o.frame;
  QCHECK(env.fs_->files[nc::StoreFilePath(kDir, 0)] == all);
  std::size_t checked = 0;
  QCHECK(store->Verify(&checked) == 0 && checked == 4);
  // The engine resolves each.
  for (const Obj& o : w.objs) {
    nevr_shadow::Found f;
    QCHECK(cap.sets[0]->shadow->Find(0x2a41cf1c1d9e5d32ULL, std::stoull(o.name.substr(2), nullptr, 16), &f));
    QCHECK(f.size == o.bytes && f.decompressed == o.bytes && f.compressed == o.frame.size());
  }
}

// The PC holds a mounted store file deny-write and its append handle deny-write too (src/cdn/env_windows.cpp), so a mount made while
// the fetch's append handle is still open fails with a sharing violation. The fetch closes its handles before it mounts.
void AFirstLaunchUnderWindowsShareModesMountsWhatItFetched() {
  World w;
  FakeEnv env;
  env.windows_sharing = true;
  w.Serve(env);
  Captured cap;
  const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  QCHECK(r.fetched == 4 && r.failed == 0 && r.published);
  QCHECK(cap.sets.size() == 1 && cap.sets[0]->records == 4 && cap.sets[0]->files == 1 && cap.sets[0]->refused == 0);
  for (const cdn_test::Record& rec : env.records) QCHECK(!(rec.name == "cdn_mount" && rec.Get("reason") == "open_failed"));
}

// The next launch mounts the store at boot, and that mount holds store file 0 for the life of the process; a fetch of a new
// object then cannot append to file 0 under the PC's share modes, so it stores the object in file 1 and mounts both.
void AnUpdateLaunchUnderWindowsShareModesStoresIntoTheNextFileAndMounts() {
  World w;
  FakeEnv first;
  first.windows_sharing = true;
  w.Serve(first);
  Captured cap1;
  QCHECK(nc::FetchAndMount(first, w.config, cap1.Sink(), kGo).published);
  w.Add(900, 150, 'n', kArchive, "0x999");
  FakeEnv second(first.fs_);
  second.windows_sharing = true;
  w.Serve(second);
  Captured cap2;
  QCHECK(nc::MountFromCache(second, w.config, cap2.Sink()));
  const nc::RunResult r = nc::FetchAndMount(second, w.config, cap2.Sink(), kGo);
  QCHECK(r.fetched == 1 && r.failed == 0 && std::string(r.stopped).empty() && r.published);
  QCHECK(cap2.sets.size() == 2 && cap2.sets[1]->records == 5 && cap2.sets[1]->files == 2 && cap2.sets[1]->refused == 0);
  QCHECK(second.fs_->files[nc::StoreFilePath(kDir, 1)] == w.objs.back().frame);
}

void TheLogHasTheLimitsTheStoreThePlanTheObjectsAndTheOutcome() {
  World w;
  FakeEnv env;
  w.Serve(env);
  Captured cap;
  nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  const std::string order = env.EventOrder();
  QCHECK(order.rfind("cdn_limits,cdn_store,cdn_start,cdn_manifest,cdn_plan,", 0) == 0);
  QCHECK(order.find("cdn_mounted,cdn_done") != std::string::npos);
  const auto limits = env.EventsNamed("cdn_limits");
  QCHECK(limits.size() == 1);
  if (!limits.empty()) {
    QCHECK(FakeEnv::Value(limits[0], "result") == "ok");
    QCHECK(FakeEnv::Value(limits[0], "max_entries") != "<absent>" && FakeEnv::Value(limits[0], "storage_budget") != "<absent>");
    QCHECK(FakeEnv::Value(limits[0], "max_store_files") != "<absent>" && FakeEnv::Value(limits[0], "manifest_text_bytes") != "<absent>");
  }
  const auto done = env.EventsNamed("cdn_done");
  QCHECK(done.size() == 1 && FakeEnv::Value(done[0], "fetched") == "4" && FakeEnv::Value(done[0], "failed") == "0");
  for (const std::string& b : env.EventsNamed("cdn_blob")) QCHECK(b.find("\"level\":\"debug\"") != std::string::npos);  // per-object lines are Debug
}

void TheSecondLaunchMountsFromTheStoreWithNoRequestAndNoHash() {
  World w;
  FakeEnv first;
  w.Serve(first);
  Captured cap1;
  nc::FetchAndMount(first, w.config, cap1.Sink(), kGo);
  FakeEnv second(first.fs_);  // a new process on the same files, with no server
  Captured cap2;
  QCHECK(nc::MountFromCache(second, w.config, cap2.Sink()));
  QCHECK(second.requests.empty());
  QCHECK(second.blob_bytes_read == 0);  // no object was read at boot, so none was hashed
  QCHECK(cap2.sets.size() == 1 && cap2.sets[0]->records == 4);
  const auto mounted = second.EventsNamed("cdn_mounted");
  QCHECK(mounted.size() == 1 && FakeEnv::Value(mounted[0], "source") == "boot");
  QCHECK(second.EventsNamed("cdn_store").size() == 1);
}

void AFetchOfWhatTheStoreHoldsMakesOnlyTheManifestRequestAndPublishesNothing() {
  World w;
  FakeEnv first;
  w.Serve(first);
  Captured cap1;
  nc::FetchAndMount(first, w.config, cap1.Sink(), kGo);
  FakeEnv second(first.fs_);
  w.Serve(second);
  Captured cap2;
  const nc::RunResult r = nc::FetchAndMount(second, w.config, cap2.Sink(), kGo);
  QCHECK(r.fetched == 0 && r.cached == 4 && !r.published && cap2.sets.empty());
  QCHECK(GetsOfBlobs(second) == 0 && second.requests.size() == 1);
  QCHECK(second.blob_bytes_read == 0);
}

void ABootWithNothingStoredMountsNothingAndSaysNothingWrong() {
  World w;
  FakeEnv env;
  Captured cap;
  QCHECK(!nc::MountFromCache(env, w.config, cap.Sink()));
  QCHECK(cap.sets.empty());
  for (const std::string& e : env.events) QCHECK(e.find("\"level\":\"warn\"") == std::string::npos && e.find("\"level\":\"error\"") == std::string::npos);
  // An empty store is the normal first launch: it is said, at Info, with its reason.
  const auto mounted = env.EventsNamed("cdn_mounted");
  QCHECK(mounted.size() == 1 && FakeEnv::Value(mounted[0], "source") == "boot" && FakeEnv::Value(mounted[0], "result") == "none" &&
         FakeEnv::Value(mounted[0], "reason") == "store_empty" && mounted[0].find("\"level\":\"info\"") != std::string::npos);
}

void ABootWithObjectsButNoSavedManifestSaysSoAtWarn() {
  World w;
  FakeEnv first;
  w.Serve(first);
  Captured cap1;
  nc::FetchAndMount(first, w.config, cap1.Sink(), kGo);
  first.fs_->files.erase(nc::ManifestCopyPath(kDir));  // the run was killed before it saved the manifest
  FakeEnv second(first.fs_);
  Captured cap2;
  QCHECK(!nc::MountFromCache(second, w.config, cap2.Sink()) && cap2.sets.empty());
  const auto events = second.EventsNamed("cdn_manifest");
  QCHECK(events.size() == 1 && FakeEnv::Value(events[0], "source") == "saved" && FakeEnv::Value(events[0], "result") == "absent" &&
         FakeEnv::Value(events[0], "reason") == "no_saved_manifest" && events[0].find("\"level\":\"warn\"") != std::string::npos &&
         events[0].find("consequence") != std::string::npos);
}

// The saved manifest names objects the store does not hold (a store file lost, a manifest saved by a run whose objects were never
// stored): nothing mounts, and the line says that names were missing, not merely that nothing was left to mount.
void ABootWhoseEveryNameIsMissingSaysAllRefusedOrMissing() {
  World w;
  FakeEnv first;
  w.Serve(first);
  Captured cap1;
  nc::FetchAndMount(first, w.config, cap1.Sink(), kGo);
  World other(2);
  other.objs[0].name = "0x9001";
  other.objs[1].name = "0x9002";
  for (std::size_t i = 0; i < other.objs.size(); ++i) {
    other.objs[i].frame = MakeFrame(900 + i, 150 + i, static_cast<char>('m' + i));  // objects no run ever stored
    other.objs[i].bytes = 900 + i;
    other.objs[i].sha = Sha256Of(other.objs[i].frame);
  }
  first.fs_->files[nc::ManifestCopyPath(kDir)] = cdn_test::ZstdFrameOf(other.ManifestText());
  FakeEnv second(first.fs_);
  Captured cap2;
  QCHECK(!nc::MountFromCache(second, w.config, cap2.Sink()) && cap2.sets.empty());
  const auto mounted = second.EventsNamed("cdn_mounted");
  QCHECK(mounted.size() == 1 && FakeEnv::Value(mounted[0], "result") == "none" &&
         FakeEnv::Value(mounted[0], "reason") == "all_refused_or_missing" && FakeEnv::Value(mounted[0], "missing") == "2");
  QCHECK(mounted[0].find("consequence") != std::string::npos);
}

void ABootWhereNothingMountsSaysSoWithItsReason() {
  World w;
  FakeEnv first;
  w.Serve(first);
  Captured cap1;
  nc::FetchAndMount(first, w.config, cap1.Sink(), kGo);
  FakeEnv second(first.fs_);
  w.config.policy.overrides["game"] = nc::LabelState::kOff;  // config turned the only label off: nothing is refused, nothing is missing
  Captured cap2;
  QCHECK(!nc::MountFromCache(second, w.config, cap2.Sink()) && cap2.sets.empty());
  const auto mounted = second.EventsNamed("cdn_mounted");
  QCHECK(mounted.size() == 1 && FakeEnv::Value(mounted[0], "result") == "none" && FakeEnv::Value(mounted[0], "reason") == "nothing_mounted" &&
         FakeEnv::Value(mounted[0], "refused") == "0" && FakeEnv::Value(mounted[0], "missing") == "0");
  QCHECK(mounted[0].find("consequence") != std::string::npos);
}

void ANewManifestAddsObjectsAndPublishesAShadowWithFreshChunkIndices() {
  World w;
  FakeEnv first;
  w.Serve(first);
  Captured cap1;
  nc::FetchAndMount(first, w.config, cap1.Sink(), kGo);
  w.Add(900, 150, 'n', kArchive, "0x999");
  FakeEnv second(first.fs_);
  w.Serve(second);
  Captured cap2;
  const nc::RunResult r = nc::FetchAndMount(second, w.config, cap2.Sink(), kGo);
  QCHECK(r.fetched == 1 && r.cached == 4 && r.published && cap2.sets.size() == 1 && cap2.sets[0]->records == 5);
  QCHECK(cap2.sets[0]->shadow->chunk_base() >= cap1.sets[0]->shadow->chunk_base() + cap1.sets[0]->shadow->chunk_count());
}

void ABlobTwoEntriesNameIsFetchedOnce() {
  World w(2);
  w.objs.push_back(w.objs[0]);
  w.objs.back().name = "0x777";  // same bytes, another name
  FakeEnv env;
  w.Serve(env);
  Captured cap;
  const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  QCHECK(r.fetched == 2 && GetsOfBlobs(env) == 2);
  QCHECK(cap.sets[0]->records == 3 && cap.sets[0]->shadow->chunk_count() == 2);
}

void ZeroByteRecordsInTheManifestAreMounted() {
  World w;
  w.empty.push_back(json{{"type", kArchive}, {"name", "0x5000"}, {"align", 4}});
  FakeEnv env;
  w.Serve(env);
  Captured cap;
  nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  QCHECK(cap.sets.size() == 1 && cap.sets[0]->empty_records == 1 && cap.sets[0]->records == 5);
  nevr_shadow::Found f;
  QCHECK(cap.sets[0]->shadow->Find(0x2a41cf1c1d9e5d32ULL, 0x5000, &f) && f.size == 0);
}

// What the store holds under a name is held at one size; an entry that names the same SHA-256 at another size is not served by it and
// is refused by name, with no request (the store's bytes are the ones that hash to the name).
void AHeldObjectOfAnotherSizeThanTheManifestStatesIsRefusedByName() {
  World w(2);
  FakeEnv first;
  w.Serve(first);
  Captured cap1;
  nc::FetchAndMount(first, w.config, cap1.Sink(), kGo);
  json lie = World::EntryOf(w.objs[0], "game");
  lie["name"] = "0x777";
  lie["size"] = w.objs[0].frame.size() + 1;  // the same object, declared one byte longer
  w.extra_entries.push_back(lie);
  FakeEnv second(first.fs_);
  w.Serve(second);
  Captured cap2;
  const nc::RunResult r = nc::FetchAndMount(second, w.config, cap2.Sink(), kGo);
  QCHECK(r.failed == 1 && GetsOfBlobs(second) == 0);
  bool named = false;
  for (const std::string& e : second.EventsNamed("cdn_blob")) {
    if (FakeEnv::Value(e, "result") == "failed" && FakeEnv::Value(e, "reason") == "held_size_mismatch" && e.find("\"level\":\"error\"") != std::string::npos) named = true;
  }
  QCHECK(named);
  nc::StoreOpenInfo info;
  const std::unique_ptr<nc::Store> store = nc::Store::Open(second, kDir, nc::ComputeLimits(second.device, nc::ManifestEntryMemoryBytes()), &info);
  QCHECK(store != nullptr && store->Find(w.objs[0].sha) != nullptr && store->Find(w.objs[0].sha)->size == w.objs[0].frame.size());
}

// ── the limits ───────────────────────────────────────────────────────────────────────────────────────────────────────────
void ADeviceThatGivesNoMemoryFactStopsTheRunBeforeAnyRequest() {
  World w;
  FakeEnv env;
  w.Serve(env);
  env.device.have_memory = false;
  Captured cap;
  const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  QCHECK(std::string(r.stopped) == "limits" && env.requests.empty() && !r.manifest_loaded);
  const auto limits = env.EventsNamed("cdn_limits");
  QCHECK(limits.size() == 1 && FakeEnv::Value(limits[0], "result") == "refused" && FakeEnv::Value(limits[0], "reason") == "no_memory_fact");
  QCHECK(limits[0].find("\"level\":\"warn\"") != std::string::npos);
  QCHECK(limits[0].find("consequence") != std::string::npos);
  Captured cap2;
  QCHECK(!nc::MountFromCache(env, w.config, cap2.Sink()));
}

void ALabelThatDoesNotFitTheFreeSpaceIsSkippedAndNothingIsRequested() {
  World w;
  FakeEnv env;
  w.Serve(env);
  env.device.free_bytes = 500;
  Captured cap;
  const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  QCHECK(GetsOfBlobs(env) == 0 && r.fetched == 0 && cap.sets.empty());
  const auto plans = env.EventsNamed("cdn_plan");
  QCHECK(!plans.empty() && FakeEnv::Value(plans[0], "skipped") == "over_storage" && plans[0].find("\"level\":\"warn\"") != std::string::npos);
}

void ARunThatFindsItsDeviceGivesTheStoreTheComputedBounds() {
  World w;
  FakeEnv env;
  w.Serve(env);
  env.device.fd_soft_limit = 66;  // 64 open: 2 free, one store file may be held
  env.device.fds_open = 64;
  Captured cap;
  const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  QCHECK(r.fetched == 4);
  const auto limits = env.EventsNamed("cdn_limits");
  QCHECK(!limits.empty() && FakeEnv::Value(limits[0], "max_store_files") == "1");
}

// ── refusals by rule ─────────────────────────────────────────────────────────────────────────────────────────────────────
void ABadRecordInTheManifestIsRefusedByRuleAndTheRestIsFetched() {
  World w;
  json bad = World::EntryOf(w.objs[0], "game");
  bad["name"] = "0x4242";
  bad["align"] = 3;
  w.extra_entries.push_back(bad);
  FakeEnv env;
  w.Serve(env);
  Captured cap;
  const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  QCHECK(r.fetched == 4 && r.refused == 1 && cap.sets.size() == 1 && cap.sets[0]->records == 4);
  const auto refused = env.EventsNamed("cdn_refused");
  QCHECK(refused.size() >= 1);
  if (!refused.empty()) {
    QCHECK(FakeEnv::Value(refused[0], "rule") == "alignment" && FakeEnv::Value(refused[0], "label") == "game" &&
           FakeEnv::Value(refused[0], "index") == "4");
    QCHECK(refused[0].find("\"level\":\"warn\"") != std::string::npos && refused[0].find("consequence") != std::string::npos);
  }
}

void AFloodOfRefusalsIsSummarisedAfterTheFirstFew() {
  World w(1);
  for (int i = 0; i < 100; ++i) {
    json bad = World::EntryOf(w.objs[0], "game");
    bad["name"] = "0x" + std::to_string(5000 + i);
    bad["align"] = 3;
    w.extra_entries.push_back(bad);
  }
  FakeEnv env;
  w.Serve(env);
  Captured cap;
  const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  QCHECK(r.refused == 100);
  const auto refused = env.EventsNamed("cdn_refused");
  QCHECK(refused.size() < 30);  // the first few singly, then one line per rule with the count
  bool summary = false;
  for (const std::string& e : refused) {
    if (FakeEnv::Value(e, "count") == "100" && FakeEnv::Value(e, "rule") == "alignment") summary = true;
  }
  QCHECK(summary);
}

void ADownloadWhoseFrameIsNotAZstdFrameIsNeverStored() {
  World w(2);
  Obj bad;
  bad.bytes = 500;
  bad.frame = std::string(100, 'x');  // hashes to whatever it hashes to: the manifest names that hash
  bad.sha = Sha256Of(bad.frame);
  bad.type = kArchive;
  bad.name = "0x666";
  w.objs.push_back(bad);
  FakeEnv env;
  w.Serve(env);
  Captured cap;
  const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  QCHECK(r.fetched == 2 && r.failed == 1);
  std::unique_ptr<nc::Store> store = OpenStore(env, w.config);
  QCHECK(!store->Has(bad.sha));
  QCHECK(env.fs_->files[nc::StoreFilePath(kDir, 0)].size() == w.objs[0].frame.size() + w.objs[1].frame.size());  // the bytes were given back
  bool named = false;
  for (const std::string& e : env.EventsNamed("cdn_blob")) {
    if (FakeEnv::Value(e, "result") == "failed" && FakeEnv::Value(e, "reason") == "frame_magic") named = true;
  }
  QCHECK(named);
}

// The header is checked when its first bytes arrive: a frame that cannot be the object's is not downloaded to the end.
void AnObjectWhoseHeaderIsWrongEndsTheDownloadAtTheHeader() {
  World w(1);
  Obj bad;
  bad.bytes = 500;
  bad.frame = std::string(5000, 'x');
  bad.sha = Sha256Of(bad.frame);
  bad.type = kArchive;
  bad.name = "0x668";
  w.objs.push_back(bad);
  FakeEnv env;
  w.Serve(env);
  Captured cap;
  nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  std::size_t at = env.requests.size();
  for (std::size_t i = 0; i < env.requests.size(); ++i) {
    if (env.requests[i].find(bad.sha) != std::string::npos) at = i;
  }
  QCHECK(at < env.requests.size() && env.body_bytes_delivered[at] < 500);  // 5,000 bytes are served; the transfer ends at the first slice
}

void ADownloadWhoseHeaderStatesAnotherSizeThanTheManifestIsNeverStored() {
  World w(1);
  Obj bad;
  bad.bytes = 777;                    // the manifest says 777 ...
  bad.frame = MakeFrame(778, 100);    // ... the frame's header says 778
  bad.sha = Sha256Of(bad.frame);
  bad.type = kArchive;
  bad.name = "0x667";
  w.objs.push_back(bad);
  FakeEnv env;
  w.Serve(env);
  Captured cap;
  const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  QCHECK(r.fetched == 1 && r.failed == 1);
  bool named = false;
  for (const std::string& e : env.EventsNamed("cdn_blob")) {
    if (FakeEnv::Value(e, "reason") == "content_size_mismatch") named = true;
  }
  QCHECK(named);
  QCHECK(!OpenStore(env, w.config)->Has(bad.sha));
}

void ABodyThatIsNotWhatTheManifestSaidIsNeverStoredWhateverTheWay() {
  struct Case {
    const char* reason;
    std::function<void(FakeEnv&, const Obj&)> mutate;
  };
  const std::vector<Case> cases = {
      {"sha256_mismatch",
       [](FakeEnv& env, const Obj& o) {
         std::string body = o.frame;
         body[body.size() - 1] ^= 1;  // same length, one bit flipped
         env.server[std::string(kBase) + "blobs/sha256/" + o.sha].body = body;
       }},
      {"too_large", [](FakeEnv& env, const Obj& o) { env.server[std::string(kBase) + "blobs/sha256/" + o.sha].extra = "more"; }},
      {"size_mismatch", [](FakeEnv& env, const Obj& o) { env.server[std::string(kBase) + "blobs/sha256/" + o.sha].body.pop_back(); }},
      {"http_status", [](FakeEnv& env, const Obj& o) { env.server[std::string(kBase) + "blobs/sha256/" + o.sha].status = 404; }},
      {"transport", [](FakeEnv& env, const Obj& o) { env.server[std::string(kBase) + "blobs/sha256/" + o.sha].drop_after = 50; }},
  };
  for (const Case& c : cases) {
    World w(3);
    FakeEnv env;
    w.Serve(env);
    c.mutate(env, w.objs[1]);
    Captured cap;
    const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
    QCHECK(r.fetched == 2 && r.failed == 1);
    QCHECK(!OpenStore(env, w.config)->Has(w.objs[1].sha));
    QCHECK(env.fs_->files[nc::StoreFilePath(kDir, 0)] == w.objs[0].frame + w.objs[2].frame);
    bool named = false;
    for (const std::string& e : env.EventsNamed("cdn_blob")) {
      if (FakeEnv::Value(e, "result") == "failed" && FakeEnv::Value(e, "reason") == c.reason) named = true;
    }
    if (!named) std::fprintf(stderr, "no cdn_blob failed reason=%s\n", c.reason);
    QCHECK(named);
  }
}

void AFailedObjectOfARequiredLabelIsAnErrorAndOfAnOptionalOneAWarning() {
  for (const bool required : {true, false}) {
    World w(2);
    w.label = required ? "game" : "extra";
    w.config.policy.overrides[w.label] = required ? nc::LabelState::kRequired : nc::LabelState::kOn;
    FakeEnv env;
    w.Serve(env);
    env.server[std::string(kBase) + "blobs/sha256/" + w.objs[0].sha].status = 404;
    Captured cap;
    nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
    bool seen = false;
    for (const std::string& e : env.EventsNamed("cdn_blob")) {
      if (FakeEnv::Value(e, "result") != "failed") continue;
      seen = true;
      QCHECK(e.find(required ? "\"level\":\"error\"" : "\"level\":\"warn\"") != std::string::npos);
      QCHECK(e.find("consequence") != std::string::npos && FakeEnv::Value(e, "http_status") == "404");
    }
    QCHECK(seen);
  }
}

void AFullDiskStopsTheRunWithTheReasonAndGivesTheBytesBack() {
  World w(4);
  FakeEnv env;
  w.Serve(env);
  // Room for the first object, its index record and a little of the second: the second fails mid-write.
  env.disk_bytes_left = w.objs[0].frame.size() + nc::kIndexHeaderBytes + nc::kIndexRecordBytes + 50;
  Captured cap;
  const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  QCHECK(std::string(r.stopped) == "write_failed");
  QCHECK(r.fetched == 1 && r.failed == 1);
  QCHECK(GetsOfBlobs(env) == 2);  // it did not go on to the third and fourth
  QCHECK(env.fs_->files[nc::StoreFilePath(kDir, 0)] == w.objs[0].frame);
  bool named = false;
  for (const std::string& e : env.EventsNamed("cdn_blob")) {
    if (FakeEnv::Value(e, "reason") == "write_failed") named = true;
  }
  QCHECK(named);
  const auto done = env.EventsNamed("cdn_done");
  QCHECK(done.size() == 1 && FakeEnv::Value(done[0], "result") == "write_failed" && done[0].find("\"level\":\"warn\"") != std::string::npos);
}

// ── the manifest ─────────────────────────────────────────────────────────────────────────────────────────────────────────
void AManifestThatCannotBeFetchedChangesNothingAndWhatIsStoredStaysMounted() {
  World w;
  FakeEnv first;
  w.Serve(first);
  Captured cap1;
  nc::FetchAndMount(first, w.config, cap1.Sink(), kGo);
  for (const char* failure : {"404", "garbage", "drop"}) {
    FakeEnv env(first.fs_);
    w.Serve(env);
    Response& m = env.server[std::string(kBase) + kManifestPath];
    if (std::string(failure) == "404") m.status = 404;
    if (std::string(failure) == "garbage") m.body = "<html>";  // not a manifest: neither a zstd frame nor the manifest's JSON
    if (std::string(failure) == "drop") m.drop_after = 10;
    Captured cap;
    const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
    QCHECK(!r.manifest_loaded && r.fetched == 0 && cap.sets.empty());
    QCHECK(std::string(r.stopped) == (std::string(failure) == "garbage" ? "manifest_refused" : "manifest_failed"));
    QCHECK(GetsOfBlobs(env) == 0);
    QCHECK(env.fs_->files[nc::ManifestCopyPath(kDir)] == cdn_test::ZstdFrameOf(w.ManifestText()));  // the saved manifest is the last good one
    const auto events = env.EventsNamed("cdn_manifest");
    QCHECK(events.size() == 1 && events[0].find("\"level\":\"warn\"") != std::string::npos && events[0].find("consequence") != std::string::npos);
    if (events.size() == 1) QCHECK(FakeEnv::Value(events[0], "url") == std::string(kBase) + kManifestPath);  // a failure names what it asked for
  }
}

// The manifest is read by its bytes, never by its name: a path with no .json suffix that holds a zstd frame loads, and so does a
// body of plain JSON; the plain body is saved as fetched and the next boot mounts from it.
void AManifestIsReadByItsBytesNotByItsName() {
  for (const bool zstd : {true, false}) {
    World w;
    w.config.manifest_path = zstd ? "v1/test/manifest" : "v1/test/plain";
    FakeEnv env;
    w.Serve(env);
    const std::string text = w.ManifestText();
    env.server[std::string(kBase) + w.config.manifest_path].body = zstd ? cdn_test::ZstdFrameOf(text) : text;  // the default answer: 200
    Captured cap;
    const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
    QCHECK(r.manifest_loaded && r.fetched == w.objs.size() && r.failed == 0 && std::string(r.stopped).empty());
    const auto loaded = env.EventsNamed("cdn_manifest");
    QCHECK(loaded.size() == 1);
    if (loaded.size() == 1) {
      QCHECK(FakeEnv::Value(loaded[0], "result") == "loaded" && FakeEnv::Value(loaded[0], "format") == (zstd ? "zstd" : "json"));
      QCHECK(FakeEnv::Value(loaded[0], "url") == std::string(kBase) + w.config.manifest_path);
      QCHECK(FakeEnv::Value(loaded[0], "bytes") == std::to_string(text.size()));
    }
    QCHECK(env.fs_->files[nc::ManifestCopyPath(kDir)] == (zstd ? cdn_test::ZstdFrameOf(text) : text));  // saved as fetched
    FakeEnv boot(env.fs_);
    Captured atBoot;
    QCHECK(nc::MountFromCache(boot, w.config, atBoot.Sink()));
    QCHECK(atBoot.sets.size() == 1 && atBoot.sets[0]->records == w.objs.size() && atBoot.sets[0]->missing == 0);
  }
}

// A plain body is held to the text limit as it arrives, just over it and far over it: the transfer ends at the limit as a failed
// fetch (manifest_failed, too_large, as an oversized frame), the record names the limit, and nothing is mounted.
void APlainManifestOverTheTextLimitEndsAtTheLimitAsItArrives() {
  constexpr std::size_t kFarOver = 100000;        // a body many times the limit
  for (const std::size_t extra : {std::size_t{1}, kFarOver}) {
    World w;
    FakeEnv env;
    w.Serve(env);
    env.device.memory_available = kSmallDeviceMemory;
    nc::DeviceFacts facts;
    env.QueryDevice(kDir, &facts);
    const std::uint64_t limit = nc::ComputeLimits(facts, nc::ManifestEntryMemoryBytes()).manifest_text_bytes;
    const std::string body(static_cast<std::size_t>(limit) + extra, '{');  // not the zstd magic: read as text
    env.server[std::string(kBase) + kManifestPath].body = body;
    Captured cap;
    const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
    QCHECK(!r.manifest_loaded && std::string(r.stopped) == "manifest_failed" && cap.sets.empty() && GetsOfBlobs(env) == 0);
    const auto events = env.EventsNamed("cdn_manifest");
    QCHECK(events.size() == 1);
    if (events.size() == 1) {
      QCHECK(FakeEnv::Value(events[0], "reason") == "too_large");
      QCHECK(FakeEnv::Value(events[0], "limit_bytes") == std::to_string(limit));
    }
    if (extra == kFarOver) {  // far over: the transfer ended near the limit, the rest was never downloaded
      QCHECK(!env.body_bytes_delivered.empty() && env.body_bytes_delivered[0] < body.size());
    }
  }
}

// A body with the zstd magic and a header no frame may have (the reserved descriptor bit set) is refused when its header arrives, as
// a refused manifest, before the rest of it is downloaded.
void AFrameWithABrokenHeaderIsRefusedBeforeTheBody() {
  World w(kManyObjects);
  FakeEnv env;
  w.Serve(env);
  std::string& body = env.server[std::string(kBase) + kManifestPath].body;
  body[nc::kZstdDescriptorAt] = static_cast<char>(static_cast<unsigned char>(body[nc::kZstdDescriptorAt]) | nc::kZstdReservedBit);
  Captured cap;
  const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  QCHECK(!r.manifest_loaded && std::string(r.stopped) == "manifest_refused" && GetsOfBlobs(env) == 0);
  const auto events = env.EventsNamed("cdn_manifest");
  QCHECK(events.size() == 1 && FakeEnv::Value(events[0], "reason") == "not_zstd");
  QCHECK(!env.body_bytes_delivered.empty() && env.body_bytes_delivered[0] < body.size());
}

// A body shorter than the magic, or text that is not the manifest's JSON, is read as text and refused as no manifest; a body that is
// the zstd magic alone, or the magic and the start of a header, is a frame with no whole header and is refused too.
void AShortOrForeignBodyIsRefusedAsNoManifest() {
  const std::string magic(reinterpret_cast<const char*>(nc::kZstdMagic), nc::kZstdMagicBytes);
  const std::string magicAndDescriptor = magic + std::string(1, '\0');  // a descriptor that asks for more header than follows
  for (const std::string& text : {std::string(), std::string("{}"), std::string("ab"), std::string("<html>not found</html>"), magic,
                                  magicAndDescriptor}) {
    World w;
    FakeEnv env;
    w.Serve(env);
    env.server[std::string(kBase) + kManifestPath].body = text;
    Captured cap;
    const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
    QCHECK(!r.manifest_loaded && std::string(r.stopped) == "manifest_refused" && GetsOfBlobs(env) == 0);
    const auto events = env.EventsNamed("cdn_manifest");
    QCHECK(events.size() == 1 && FakeEnv::Value(events[0], "result") == "refused");
  }
}

void AManifestOverTheComputedTextLimitIsRefusedByName() {
  World w;
  FakeEnv env;
  w.Serve(env);
  env.device.memory_available = kSmallDeviceMemory;
  Captured cap;
  const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  QCHECK(!r.manifest_loaded && std::string(r.stopped) == "manifest_failed");
  const auto events = env.EventsNamed("cdn_manifest");
  QCHECK(events.size() == 1 && FakeEnv::Value(events[0], "reason") == "too_large");
}

void AFrameWithNoContentSizeIsRefusedByName() {
  World w;
  FakeEnv env;
  w.Serve(env);
  env.server[std::string(kBase) + kManifestPath].body = cdn_test::ZstdStreamFrameOf(w.ManifestText());
  Captured cap;
  const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  QCHECK(!r.manifest_loaded && std::string(r.stopped) == "manifest_refused" && GetsOfBlobs(env) == 0);
  const auto events = env.EventsNamed("cdn_manifest");
  QCHECK(events.size() == 1 && FakeEnv::Value(events[0], "reason") == "no_content_size");
}

void ACorruptFrameIsRefusedByName() {
  World w(kManyObjects);
  FakeEnv env;
  w.Serve(env);
  std::string& body = env.server[std::string(kBase) + kManifestPath].body;
  body = body.substr(0, body.size() - 9);
  Captured cap;
  const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  QCHECK(!r.manifest_loaded && GetsOfBlobs(env) == 0);
  const auto events = env.EventsNamed("cdn_manifest");
  QCHECK(events.size() == 1 && (FakeEnv::Value(events[0], "reason") == "decompress" || FakeEnv::Value(events[0], "reason") == "transport"));
}

// The size the frame's header states is read from the first bytes; a manifest too large for the device's memory is refused there, and
// the rest of its body is never downloaded.
void ADeclaredSizeOverTheMemoryLimitEndsTheDownloadAtTheHeader() {
  World w(400);
  FakeEnv env;
  w.Serve(env);
  const std::size_t text = w.ManifestText().size();
  env.device.memory_available = text * 4;  // the limit: text * 4 / (8 * 8): far below the text
  Captured cap;
  const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  QCHECK(!r.manifest_loaded && std::string(r.stopped) == "manifest_failed" && GetsOfBlobs(env) == 0);
  QCHECK(env.body_bytes_delivered.size() == 1 && env.body_bytes_delivered[0] < 200);  // the first slice, no more
  const auto events = env.EventsNamed("cdn_manifest");
  QCHECK(events.size() == 1 && FakeEnv::Value(events[0], "reason") == "too_large" && FakeEnv::Value(events[0], "declared_bytes") == std::to_string(text));
}

void ARefusedManifestNamesTheLabelAndIndexNeverTheText() {
  World w;
  w.extra_entries.push_back(json{{"platform", "android-arm64"}, {"type", kArchive}, {"name", "0x1"}, {"format", "zstd-frame"},
                                 {"sha256", "SECRETTEXT"}, {"size", 1}, {"bytes", 1}, {"align", 16}});
  FakeEnv env;
  w.Serve(env);
  Captured cap;
  nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  const auto events = env.EventsNamed("cdn_manifest");
  QCHECK(events.size() == 1 && FakeEnv::Value(events[0], "reason") == "bad_hash" && FakeEnv::Value(events[0], "label") == "game" &&
         FakeEnv::Value(events[0], "index") == "4");
  for (const std::string& e : env.events) QCHECK(e.find("SECRETTEXT") == std::string::npos);
}

// ── progress, shutdown, and kills ────────────────────────────────────────────────────────────────────────────────────────
void TheProgressCallbackCountsEveryObjectAndEndsAtTheTotal() {
  World w(5);
  FakeEnv env;
  w.Serve(env);
  std::vector<nc::FetchProgress> seen;
  w.config.progress = [&](const nc::FetchProgress& p) { seen.push_back(p); };
  Captured cap;
  nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  QCHECK(seen.size() == 6);  // after the plan, then after each of the five objects
  std::uint64_t total = 0;
  for (const Obj& o : w.objs) total += o.frame.size();
  QCHECK(seen.front().total_bytes == total && seen.front().done_bytes == 0 && seen.front().total_blobs == 5);
  for (std::size_t i = 1; i < seen.size(); ++i) QCHECK(seen[i].done_bytes > seen[i - 1].done_bytes && seen[i].done_blobs == i);
  QCHECK(seen.back().done_bytes == total && seen.back().done_blobs == 5 && seen.back().failed_blobs == 0);
}

void AShutdownBetweenObjectsEndsTheRunAndKeepsWhatWasStored() {
  World w(4);
  FakeEnv env;
  w.Serve(env);
  int calls = 0;
  Captured cap;
  const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), [&] { return ++calls < 12; });
  QCHECK(std::string(r.stopped) == "cancelled" && !r.published);
  FakeEnv env2(env.fs_);
  w.Serve(env2);
  Captured cap2;
  const nc::RunResult again = nc::FetchAndMount(env2, w.config, cap2.Sink(), kGo);
  QCHECK(again.fetched + again.cached == 4 && again.failed == 0 && again.cached >= 1);  // the objects stored before the stop are not fetched again
  QCHECK(GetsOfBlobs(env2) == again.fetched);
}

void AKillAfterAnyStepLeavesAnHonestStoreAndTheNextLaunchFinishes() {
  for (int kill = 0; kill < 90; ++kill) {
    World w(4);
    FakeEnv dying;
    w.Serve(dying);
    dying.stop_after = kill;
    Captured cap;
    nc::FetchAndMount(dying, w.config, cap.Sink(), kGo);
    // The next process finds the files as the dead one left them.
    FakeEnv next(dying.fs_);
    w.Serve(next);
    {
      std::unique_ptr<nc::Store> store = OpenStore(next, w.config);
      QCHECK(store != nullptr);
      std::size_t checked = 0;
      QCHECK(store->Verify(&checked) == 0);  // whatever the index names is whole
    }
    Captured cap2;
    const nc::RunResult r = nc::FetchAndMount(next, w.config, cap2.Sink(), kGo);
    QCHECK(r.failed == 0 && r.fetched + r.cached == 4);
    std::unique_ptr<nc::Store> store = OpenStore(next, w.config);
    for (const Obj& o : w.objs) QCHECK(store->Has(o.sha));
    std::size_t checked = 0;
    QCHECK(store->Verify(&checked) == 0 && checked == 4);
    std::string all;
    for (const Obj& o : w.objs) all += o.frame;
    // The store holds exactly the four frames, in some order of arrival: no orphan bytes, no gaps.
    QCHECK(next.fs_->files[nc::StoreFilePath(kDir, 0)].size() == all.size());
  }
}

}  // namespace

namespace {

// ── the script gate (src/cdn/cdn_script_gate.h; PLAN-v5 5.2) ────────────────────────────────────────────────────────────────
constexpr const char* kScriptTypeText = "0x991421b41de58370";
constexpr std::uint64_t kScriptType = 0x991421b41de58370ULL;
constexpr const char* kScriptName = "0x94ba7ca39fc97e1a";
constexpr std::uint64_t kScriptHash = 0x94ba7ca39fc97e1aULL;
constexpr const char* kScriptDir = "/game/scripts/";
constexpr const char* kScriptFile = "/game/scripts/94ba7ca39fc97e1a.dll";
constexpr const char* kScriptHash16Text = "94ba7ca39fc97e1a";
constexpr const char* kCombat = "combat_content";
constexpr std::uint64_t kScriptRecordBytes = 4;  // the 6.18.1 shape: a CScriptResource record is a name, not a file
constexpr std::size_t kScriptFrameBytes = 40;

// The real 6.18.1 shape: combat_content holds a 4-byte entry of the script type and four ordinary objects, and lists no `scripts`.
World CombatWorld() {
  World w;
  w.label = kCombat;
  w.Add(kScriptRecordBytes, kScriptFrameBytes, 's', kScriptTypeText, kScriptName);
  return w;
}

bool Serves(const nc::Mounted* set, std::uint64_t type, std::uint64_t name) {
  nevr_shadow::Found f;
  return set != nullptr && set->shadow != nullptr && set->shadow->Find(type, name, &f);
}

// What a player has who fetched before the gate existed: every object stored, the manifest saved, the set mounted.
std::shared_ptr<cdn_test::Fs> StoredBeforeTheGate(const World& w) {
  FakeEnv first;
  w.Serve(first);
  Captured cap;
  nc::FetchAndMount(first, w.config, cap.Sink(), kGo);
  QCHECK(cap.sets.size() == 1 && Serves(cap.sets[0], kScriptType, kScriptHash));  // an ungated run mounts the script record
  return first.fs_;
}

nc::RunConfig GatedConfig(const World& w) {
  nc::RunConfig c = w.config;
  c.script_dir = kScriptDir;
  return c;
}

// Case 1: the boot mount, from the saved manifest, with no file under scripts\.
void ABootRefusesALabelWhoseFourByteScriptRecordNamesNoFileOnDisk() {
  const World w = CombatWorld();
  const std::shared_ptr<cdn_test::Fs> fs = StoredBeforeTheGate(w);
  FakeEnv env(fs);
  Captured cap;
  QCHECK(!nc::MountFromCache(env, GatedConfig(w), cap.Sink()) && cap.sets.empty());
  const auto refused = env.EventsNamed("cdn_mount");
  QCHECK(refused.size() == 1);
  if (refused.size() == 1) {
    QCHECK(FakeEnv::Value(refused[0], "label") == kCombat && FakeEnv::Value(refused[0], "result") == "refused" &&
           FakeEnv::Value(refused[0], "reason") == "script_undelivered" && FakeEnv::Value(refused[0], "scripts") == kScriptHash16Text &&
           FakeEnv::Value(refused[0], "source") == "boot" && refused[0].find("\"level\":\"warn\"") != std::string::npos &&
           refused[0].find("consequence") != std::string::npos);
  }
  QCHECK(env.requests.empty());
}

// The gate turns off the label that names the script, and only that label.
void TheGateTurnsOffOneLabelAndMountsItsNeighbour() {
  World w = CombatWorld();
  w.other_label = "game";
  World neighbour(0);
  neighbour.Add(777, 200, 'q', kArchive, "0x7000");
  w.other_objs = neighbour.objs;
  const std::shared_ptr<cdn_test::Fs> fs = StoredBeforeTheGate(w);
  FakeEnv env(fs);
  Captured cap;
  QCHECK(nc::MountFromCache(env, GatedConfig(w), cap.Sink()) && cap.sets.size() == 1);
  QCHECK(cap.sets[0]->labels.size() == 1 && cap.sets[0]->labels[0] == "game");
  QCHECK(!Serves(cap.sets[0], kScriptType, kScriptHash) && Serves(cap.sets[0], 0x2a41cf1c1d9e5d32ULL, 0x7000));
  const auto refused = env.EventsNamed("cdn_mount");
  QCHECK(refused.size() == 1 && FakeEnv::Value(refused[0], "label") == kCombat);
}

// Case 2: the same through FetchAndMount, with every blob already stored (only the manifest changed, so the fetch mounts).
void AFetchRefusesTheSameLabelWithEveryBlobAlreadyStored() {
  const World w = CombatWorld();
  const std::shared_ptr<cdn_test::Fs> fs = StoredBeforeTheGate(w);
  World changed = w;
  changed.empty.push_back(json{{"type", kArchive}, {"name", "0x5000"}, {"align", 4}});  // a new manifest text, no new object
  FakeEnv env(fs);
  changed.Serve(env);
  Captured cap;
  const nc::RunResult r = nc::FetchAndMount(env, GatedConfig(changed), cap.Sink(), kGo);
  QCHECK(r.manifest_loaded && r.fetched == 0 && !r.published && cap.sets.empty());
  const auto refused = env.EventsNamed("cdn_mount");
  QCHECK(refused.size() == 1);
  if (refused.size() == 1) {
    QCHECK(FakeEnv::Value(refused[0], "reason") == "script_undelivered" && FakeEnv::Value(refused[0], "source") == "fetch" &&
           FakeEnv::Value(refused[0], "scripts") == kScriptHash16Text);
  }
}

// Case 3: the file is on disk under the name, and the label lists no `scripts` (a stock script, or one a content pack serves): mounted.
void AScriptOnDiskUnderANameOutsideTheLabelsListMountsTheLabel() {
  const World w = CombatWorld();
  const std::shared_ptr<cdn_test::Fs> fs = StoredBeforeTheGate(w);
  fs->files[kScriptFile] = "a stock script";
  FakeEnv env(fs);
  Captured cap;
  QCHECK(nc::MountFromCache(env, GatedConfig(w), cap.Sink()) && cap.sets.size() == 1);
  QCHECK(Serves(cap.sets[0], kScriptType, kScriptHash));
  QCHECK(env.EventsNamed("cdn_mount").empty());
  QCHECK(env.open_blobs() == 1);  // the store file; the existence probe closed its handle
}

// Case 4: a record of 0 bytes of the script type is refused like an entry.
void AScriptRecordOfZeroBytesIsRefusedLikeAnEntry() {
  World w(4);
  w.label = kCombat;
  w.empty.push_back(json{{"type", kScriptTypeText}, {"name", kScriptName}, {"align", 4}});
  const std::shared_ptr<cdn_test::Fs> fs = StoredBeforeTheGate(w);
  FakeEnv env(fs);
  Captured cap;
  QCHECK(!nc::MountFromCache(env, GatedConfig(w), cap.Sink()) && cap.sets.empty());
  const auto refused = env.EventsNamed("cdn_mount");
  QCHECK(refused.size() == 1 && FakeEnv::Value(refused[0], "reason") == "script_undelivered" &&
         FakeEnv::Value(refused[0], "scripts") == kScriptHash16Text);
  fs->files[kScriptFile] = "a stock script";
  FakeEnv present(fs);
  Captured cap2;
  QCHECK(nc::MountFromCache(present, GatedConfig(w), cap2.Sink()) && cap2.sets.size() == 1 && cap2.sets[0]->empty_records == 1);
}

// With no script folder (the Quest's) the gate is not applied, and one Info line says so for the half that mounted.
void NoScriptDirLeavesTheGateOffAndSaysSo() {
  const World w = CombatWorld();
  const std::shared_ptr<cdn_test::Fs> fs = StoredBeforeTheGate(w);
  FakeEnv env(fs);
  Captured cap;
  QCHECK(nc::MountFromCache(env, w.config, cap.Sink()) && cap.sets.size() == 1);
  const auto off = env.EventsNamed("cdn_scripts_gate");
  QCHECK(off.size() == 1 && FakeEnv::Value(off[0], "result") == "off" && FakeEnv::Value(off[0], "reason") == "no_script_dir" &&
         off[0].find("\"level\":\"info\"") != std::string::npos && off[0].find("consequence") != std::string::npos);
  QCHECK(env.EventsNamed("cdn_mount").empty());
}

nc::Manifest ScriptManifest() {
  nc::Manifest m;
  m.labels.push_back(kCombat);
  nc::Entry e;
  e.label = kCombat;
  e.platform = nc::kAndroidArm64.name;
  e.type = kScriptType;
  e.name = kScriptHash;
  m.entries.push_back(e);
  return m;
}

// Case 6 (X8): a file on disk under a name the label's own `scripts` list names is not ours, so the label is refused, not mounted.
// `listed` is a fake here: the manifest field that answers it is another piece's (PLAN-v5 R3).
void AFileOnDiskUnderAListedNameIsShadowingAndRefusesTheLabel() {
  const nc::Manifest m = ScriptManifest();
  const auto yes = [](const std::string&, std::uint64_t) { return true; };
  const auto no = [](const std::string&, std::uint64_t) { return false; };
  const auto onDisk = [](std::uint64_t) { return true; };
  const auto absent = [](std::uint64_t) { return false; };

  std::vector<nc::GatedLabel> g = nc::GateScripts(m, nc::kAndroidArm64, onDisk, no, yes);  // listed, on disk, not held
  QCHECK(g.size() == 1 && g[0].label == kCombat && g[0].reason == "script_shadowed" && g[0].scripts.size() == 1 && g[0].scripts[0] == kScriptHash);

  FakeEnv env;
  nc::SayGated(env, g, nc::Policy{}, kScriptDir, "boot");
  QCHECK(env.records.size() == 1);
  if (env.records.size() == 1) {
    const cdn_test::Record& r = env.records[0];
    QCHECK(r.name == "cdn_mount" && r.level == nc::EventLevel::kWarn && r.Get("reason") == "script_shadowed" &&
           r.Get("path") == kScriptFile && r.Get("label") == kCombat && !r.Get("consequence").empty());
  }
  // A label the player turned off has nothing to refuse: no line.
  FakeEnv quiet;
  nc::Policy off;
  off.overrides[kCombat] = nc::LabelState::kOff;
  nc::SayGated(quiet, g, off, kScriptDir, "boot");
  QCHECK(quiet.records.empty());

  QCHECK(nc::GateScripts(m, nc::kAndroidArm64, onDisk, no, no).empty());     // not listed: present counts (case 3)
  QCHECK(nc::GateScripts(m, nc::kAndroidArm64, onDisk, yes, yes).empty());   // listed and held: ours
  QCHECK(nc::GateScripts(m, nc::kAndroidArm64, absent, yes, yes).empty());
  g = nc::GateScripts(m, nc::kAndroidArm64, absent, no, yes);                // listed, nothing on disk, not held
  QCHECK(g.size() == 1 && g[0].reason == "script_undelivered");
  g = nc::GateScripts(m, nc::kAndroidArm64, absent, yes, no);                // held, not listed: counts as delivered
  QCHECK(g.empty());
  // Both at once: the label is reported as shadowed, with the shadowed name only.
  nc::Manifest two = m;
  nc::Entry second = two.entries[0];
  second.name = kScriptHash + 1;
  two.entries.push_back(second);
  g = nc::GateScripts(two, nc::kAndroidArm64, [](std::uint64_t h) { return h == kScriptHash; }, no, yes);
  QCHECK(g.size() == 1 && g[0].reason == "script_shadowed" && g[0].scripts.size() == 1 && g[0].scripts[0] == kScriptHash);
  nc::Manifest other = m;
  other.entries[0].platform = nc::kWindowsX64.name;                          // another platform's record is not this plan's
  QCHECK(nc::GateScripts(other, nc::kAndroidArm64, absent, no, no).empty());
}

// The registrar seam (PLAN-v5 5.3, wired by R10b): it runs before the gate, what it holds counts as delivered, and a refusal turns its
// whole label off, naming the content pack when that is the reason (5.2 cases 5 and 7, driven here with a fake registrar).
void TheRegistrarSeamRunsBeforeTheGateAndItsRefusalsTurnTheLabelOff() {
  const World w = CombatWorld();
  const std::shared_ptr<cdn_test::Fs> fs = StoredBeforeTheGate(w);

  nc::RunConfig held = GatedConfig(w);
  held.registrar = [](const nc::Manifest&, const nc::Platform&) {
    nc::ScriptRegistration r;
    r.held.push_back(std::make_pair(std::string(kCombat), kScriptHash));
    return r;
  };
  FakeEnv heldEnv(fs);
  Captured heldCap;
  QCHECK(nc::MountFromCache(heldEnv, held, heldCap.Sink()) && heldCap.sets.size() == 1 && heldEnv.EventsNamed("cdn_mount").empty());

  fs->files[kScriptFile] = "a file the registrar refused to shadow";  // present, yet the refusal still turns the label off
  nc::RunConfig refused = GatedConfig(w);
  refused.registrar = [](const nc::Manifest&, const nc::Platform&) {
    nc::ScriptRegistration r;
    r.refused.push_back(nc::ScriptRefusal{kCombat, kScriptHash, "stock_hash", ""});
    return r;
  };
  FakeEnv refusedEnv(fs);
  Captured refusedCap;
  QCHECK(!nc::MountFromCache(refusedEnv, refused, refusedCap.Sink()) && refusedCap.sets.empty());
  auto lines = refusedEnv.EventsNamed("cdn_mount");
  QCHECK(lines.size() == 1 && FakeEnv::Value(lines[0], "reason") == "stock_hash" && FakeEnv::Value(lines[0], "pack") == "<absent>");

  nc::RunConfig pack = GatedConfig(w);
  pack.registrar = [](const nc::Manifest&, const nc::Platform&) {
    nc::ScriptRegistration r;
    r.refused.push_back(nc::ScriptRefusal{kCombat, kScriptHash, "pack_hash", "cat-pack"});
    return r;
  };
  FakeEnv packEnv(fs);
  Captured packCap;
  QCHECK(!nc::MountFromCache(packEnv, pack, packCap.Sink()) && packCap.sets.empty());
  lines = packEnv.EventsNamed("cdn_mount");
  QCHECK(lines.size() == 1 && FakeEnv::Value(lines[0], "reason") == "pack_hash" && FakeEnv::Value(lines[0], "pack") == "cat-pack" &&
         lines[0].find("content pack") != std::string::npos);
}

// A registrar refusal turns its label off by itself, even when no record of the label is of the script type (PLAN-v5 5.3 item 2): the
// refusal withdrew the label's other copies, which its other records may use.
void ARegistrarRefusalTurnsOffALabelWithNoScriptRecord() {
  const World w;  // label "game", four ordinary objects, no record of the script type
  FakeEnv first;
  w.Serve(first);
  Captured stored;
  nc::FetchAndMount(first, w.config, stored.Sink(), kGo);
  QCHECK(stored.sets.size() == 1);
  nc::RunConfig c = GatedConfig(w);
  c.registrar = [](const nc::Manifest&, const nc::Platform&) {
    nc::ScriptRegistration r;
    r.refused.push_back(nc::ScriptRefusal{"game", kScriptHash, "duplicate", ""});
    return r;
  };
  FakeEnv env(first.fs_);
  Captured cap;
  QCHECK(!nc::MountFromCache(env, c, cap.Sink()) && cap.sets.empty());
  const auto lines = env.EventsNamed("cdn_mount");
  QCHECK(lines.size() == 1 && FakeEnv::Value(lines[0], "label") == "game" && FakeEnv::Value(lines[0], "reason") == "duplicate" &&
         FakeEnv::Value(lines[0], "scripts") == kScriptHash16Text);
}
}  // namespace

// ── the boot-only labels (docs/adr/0045 decision 7; R6-b) ───────────────────────────────────────────────────────────────────

nc::Entry Ent(const std::string& label, std::uint64_t name, const std::string& sha, const char* platform = "android-arm64") {
  nc::Entry e;
  e.label = label;
  e.platform = platform;
  e.type = 0x2a;
  e.name = name;
  e.sha256 = sha;
  e.size = 10;
  e.bytes = 20;
  e.align = 16;
  return e;
}
nc::EmptyRecord Empty(const std::string& label, std::uint64_t name) {
  nc::EmptyRecord z;
  z.label = label;
  z.platform = "android-arm64";
  z.type = 0x2a;
  z.name = name;
  z.align = 16;
  return z;
}
nc::ScriptEntry Script(const std::string& label, std::uint64_t name, const char* platform = "android-arm64") {
  nc::ScriptEntry s;
  s.label = label;
  s.platform = platform;
  s.name = name;
  s.stock_source = 0x77;
  s.stock_sha256 = std::string(64, 'c');
  return s;
}
bool HasEntry(const nc::Manifest& m, const std::string& label, const std::string& sha) {
  for (const nc::Entry& e : m.entries) {
    if (e.label == label && e.sha256 == sha) return true;
  }
  return false;
}
bool HasEmpty(const nc::Manifest& m, const std::string& label, std::uint64_t name) {
  for (const nc::EmptyRecord& z : m.empties) {
    if (z.label == label && z.name == name) return true;
  }
  return false;
}
bool HasScript(const nc::Manifest& m, const std::string& label, std::uint64_t name) {
  for (const nc::ScriptEntry& s : m.scripts) {
    if (s.label == label && s.name == name) return true;
  }
  return false;
}
// World with `n` objects, their bytes, sizes and fills from the given bases, named by `name_base`.
World WithObjects(int n, std::uint64_t bytes_base, std::size_t size_base, char fill, std::uint64_t name_base) {
  World w(0);
  for (int i = 0; i < n; ++i) {
    w.Add(bytes_base + static_cast<std::uint64_t>(i), size_base + 7 * static_cast<std::size_t>(i), static_cast<char>(fill + i), kArchive,
          "0x" + std::to_string(name_base + static_cast<std::uint64_t>(i)));
  }
  return w;
}
// The newest cdn_mounted record of a source, or null.
const cdn_test::Record* LastMounted(const FakeEnv& env, const std::string& source) {
  const cdn_test::Record* found = nullptr;
  for (const cdn_test::Record& r : env.records) {
    if (r.name == "cdn_mounted" && r.Get("source") == source) found = &r;
  }
  return found;
}
nc::Limits LimitsOf(FakeEnv& env) {
  nc::DeviceFacts facts;
  env.QueryDevice(kDir, &facts);
  return nc::ComputeLimits(facts, nc::ManifestEntryMemoryBytes());
}

// Pure: the fetched records of a boot-only label are replaced by the boot set's, its scripts and empties too.
void KeepBootOnlyReplacesTheBootOnlyLabelsRecordsWithTheBootSets() {
  nc::Manifest fetched;
  fetched.entries = {Ent("combat_content", 1, "b1"), Ent("lobby_posters", 2, "p2")};
  fetched.empties = {Empty("combat_content", 5)};
  fetched.scripts = {Script("combat_content", 7)};
  nc::Manifest boot;
  boot.entries = {Ent("combat_content", 1, "a1"), Ent("lobby_posters", 2, "p1")};
  boot.empties = {Empty("combat_content", 6)};
  boot.scripts = {Script("combat_content", 8)};
  const nc::Manifest out = nc::KeepBootOnly(fetched, boot, {"combat_content"});
  QCHECK(HasEntry(out, "lobby_posters", "p2"));
  QCHECK(HasEntry(out, "combat_content", "a1"));
  QCHECK(HasEmpty(out, "combat_content", 6));
  QCHECK(HasScript(out, "combat_content", 8));
  QCHECK(!HasEntry(out, "combat_content", "b1"));
  QCHECK(!HasEmpty(out, "combat_content", 5));
  QCHECK(!HasScript(out, "combat_content", 7));
}

// Pure: a boot-only label the boot set lacks keeps no record; another label is kept as fetched.
void KeepBootOnlyDropsALabelTheBootSetLacks() {
  nc::Manifest fetched;
  fetched.entries = {Ent("combat_content", 1, "b1"), Ent("lobby_posters", 2, "p2")};
  const nc::Manifest out = nc::KeepBootOnly(fetched, nc::Manifest(), {"combat_content"});
  QCHECK(out.entries.size() == 1 && HasEntry(out, "lobby_posters", "p2"));
}

// Pure: the scripted labels of this platform join the fixed list; another platform's do not; no fixed list means none.
void BootOnlyLabelsAddsEveryLabelWithScriptsOfThisPlatform() {
  nc::Manifest fetched;
  fetched.scripts = {Script("x", 1), Script("z", 2, "windows-x86_64")};
  nc::Manifest boot;
  boot.scripts = {Script("y", 3)};
  const std::vector<std::string> got = nc::BootOnlyLabels({"combat_content"}, boot, fetched, nc::kAndroidArm64);
  QCHECK((got == std::vector<std::string>{"combat_content", "x", "y"}));
  QCHECK(nc::BootOnlyLabels({}, boot, fetched, nc::kAndroidArm64).empty());
}

// Pure: SameMountRecords compares what Mount reads of this platform, not order, labels, refusals or stock_size.
void SameMountRecordsComparesWhatMountReads() {
  nc::Manifest a;
  a.entries = {Ent("combat_content", 1, "a1"), Ent("lobby_posters", 2, "p1")};
  a.empties = {Empty("combat_content", 6)};
  a.scripts = {Script("combat_content", 8)};
  nc::Manifest reordered = a;
  std::reverse(reordered.entries.begin(), reordered.entries.end());
  QCHECK(nc::SameMountRecords(a, reordered, nc::kAndroidArm64));
  nc::Manifest other = a;
  other.labels = {"x"};
  other.refused.push_back(nc::Refusal{"x", 0, 0, 0, nc::Rule::kNone});
  other.entries[0].has_stock_size = true;
  other.entries[0].stock_size = 99;
  QCHECK(nc::SameMountRecords(a, other, nc::kAndroidArm64));
  nc::Manifest sha = a;
  sha.entries[0].sha256 = "zz";
  QCHECK(!nc::SameMountRecords(a, sha, nc::kAndroidArm64));
  nc::Manifest extra = a;
  extra.empties.push_back(Empty("combat_content", 9));
  QCHECK(!nc::SameMountRecords(a, extra, nc::kAndroidArm64));
  nc::Manifest script = a;
  script.scripts[0].stock_sha256 = std::string(64, 'd');
  QCHECK(!nc::SameMountRecords(a, script, nc::kAndroidArm64));
  nc::Manifest windows = a;
  windows.entries.push_back(Ent("combat_content", 3, "w3", "windows-x86_64"));
  QCHECK(nc::SameMountRecords(a, windows, nc::kAndroidArm64));
}

// A boot-only label keeps the boot set when the fetch brings a new set of it: the objects are stored, nothing is published, and the
// saved manifest is the new one.
void ABootOnlyLabelKeepsTheBootSetWhenTheFetchBringsANewOne() {
  World a = WithObjects(4, 300, 120, 'a', 100);
  a.label = "combat_content";
  FakeEnv env;
  a.Serve(env);
  Captured run1;
  nc::FetchAndMount(env, a.config, run1.Sink(), kGo);
  QCHECK(run1.sets.size() == 1);
  World b = WithObjects(4, 500, 130, 'p', 200);
  b.label = "combat_content";
  b.Serve(env);
  nc::RunConfig c = b.config;
  c.boot_only = {"combat_content"};
  Captured cap;
  nc::Manifest boot;
  QCHECK(nc::MountFromCache(env, c, cap.Sink(), &boot));
  QCHECK(cap.sets.size() == 1);
  const std::size_t getsBefore = GetsOfBlobs(env);
  c.boot = boot;
  const nc::RunResult r = nc::FetchAndMount(env, c, cap.Sink(), kGo);
  QCHECK(r.fetched == 4 && !r.published);
  QCHECK(GetsOfBlobs(env) == getsBefore + 4);
  QCHECK(cap.sets.size() == 1);
  const cdn_test::Record* unchanged = LastMounted(env, "fetch");
  QCHECK(unchanged != nullptr && unchanged->Get("result") == "unchanged" && unchanged->Get("reason") == "boot_only" &&
         unchanged->Get("boot_only_kept") == "1");
  std::string saved;
  QCHECK(env.ReadSmallFile(nc::ManifestCopyPath(kDir), &saved, std::size_t{1} << 20));
  nc::Manifest savedManifest;
  QCHECK(nc::ParseManifestBody(saved, LimitsOf(env), &savedManifest).status == nc::ParseStatus::kOk);
  QCHECK(!savedManifest.entries.empty() && savedManifest.entries[0].sha256 == b.objs[0].sha);
}

// A boot-only label with no boot set: its objects are stored and the manifest saved, and no set is published.
void ABootOnlyLabelWithNoBootSetIsStoredNotPublished() {
  World w = WithObjects(4, 300, 120, 'a', 100);
  w.label = "combat_content";
  FakeEnv env;
  w.Serve(env);
  Captured cap;
  nc::RunConfig c = w.config;
  c.boot_only = {"combat_content"};
  const nc::RunResult r = nc::FetchAndMount(env, c, cap.Sink(), kGo);
  QCHECK(r.fetched == 4 && !r.published && cap.sets.empty());
  QCHECK(GetsOfBlobs(env) == 4);
  std::string saved;
  QCHECK(env.ReadSmallFile(nc::ManifestCopyPath(kDir), &saved, std::size_t{1} << 20));
  const cdn_test::Record* unchanged = LastMounted(env, "fetch");
  QCHECK(unchanged != nullptr && unchanged->Get("reason") == "boot_only" && unchanged->Get("boot_only_kept") == "1");
}

// A non-boot-only label still publishes beside a kept one: the set holds the boot set's combat records and the new lobby record.
void ANonBootOnlyLabelStillPublishesBesideAKeptOne() {
  World a = WithObjects(4, 300, 120, 'a', 100);
  a.label = "combat_content";
  a.other_label = "lobby_posters";
  a.other_objs = WithObjects(1, 700, 140, 'm', 300).objs;
  FakeEnv env;
  a.Serve(env);
  Captured run1;
  nc::FetchAndMount(env, a.config, run1.Sink(), kGo);
  nc::RunConfig c = a.config;
  c.boot_only = {"combat_content"};
  Captured cap;
  nc::Manifest boot;
  QCHECK(nc::MountFromCache(env, c, cap.Sink(), &boot));
  World b = WithObjects(4, 500, 130, 'p', 200);
  b.label = "combat_content";
  b.other_label = "lobby_posters";
  b.other_objs = WithObjects(1, 800, 150, 'q', 400).objs;
  b.Serve(env);
  nc::RunConfig c2 = b.config;
  c2.boot_only = {"combat_content"};
  c2.boot = boot;
  const nc::RunResult r = nc::FetchAndMount(env, c2, cap.Sink(), kGo);
  QCHECK(r.published && cap.sets.size() == 2);
  if (cap.sets.size() == 2) {
    QCHECK(cap.sets[1]->records == 5 && cap.sets[1]->labels.size() == 2);
  }
  const cdn_test::Record* published = LastMounted(env, "fetch");
  QCHECK(published != nullptr && published->Get("boot_only_kept") == "1");
}

// A new object of a label that is not boot-only publishes even when the boot-only label is unchanged; boot_only_kept is 0.
void AnUnchangedBootOnlyLabelWithNewObjectsOfAnotherLabelPublishes() {
  World a = WithObjects(4, 300, 120, 'a', 100);
  a.label = "combat_content";
  a.other_label = "lobby_posters";
  a.other_objs = WithObjects(1, 700, 140, 'm', 300).objs;
  FakeEnv env;
  a.Serve(env);
  env.server.erase(std::string(kBase) + "blobs/sha256/" + a.other_objs[0].sha);  // the lobby object is not had at the first run
  Captured cap;
  nc::FetchAndMount(env, a.config, cap.Sink(), kGo);
  nc::RunConfig c = a.config;
  c.boot_only = {"combat_content"};
  nc::Manifest boot;
  QCHECK(nc::MountFromCache(env, c, cap.Sink(), &boot));
  QCHECK(cap.sets.size() == 2);
  a.Serve(env);  // the object is served now
  c.boot = boot;
  const nc::RunResult r = nc::FetchAndMount(env, c, cap.Sink(), kGo);
  QCHECK(r.fetched == 1 && r.published);
  QCHECK(cap.sets.size() == 3);
  const cdn_test::Record* published = LastMounted(env, "fetch");
  QCHECK(published != nullptr && published->Get("boot_only_kept") == "0");
}

// No boot-only label: the fetch publishes what it fetched, as the Quest's path does, and the line carries no boot_only_kept.
void AnEmptyListFetchesAndPublishesAsBefore() {
  World a = WithObjects(4, 300, 120, 'a', 100);
  a.label = "combat_content";
  FakeEnv env;
  a.Serve(env);
  Captured cap;
  nc::FetchAndMount(env, a.config, cap.Sink(), kGo);
  QCHECK(nc::MountFromCache(env, a.config, cap.Sink()));
  World b = WithObjects(4, 500, 130, 'p', 200);
  b.label = "combat_content";
  b.Serve(env);
  const nc::RunResult r = nc::FetchAndMount(env, b.config, cap.Sink(), kGo);
  QCHECK(r.published && cap.sets.size() == 3);
  const cdn_test::Record* fetch = LastMounted(env, "fetch");
  QCHECK(fetch != nullptr && fetch->Get("boot_only_kept").empty());
}

// MountFromCache gives back the manifest it published, and nothing when it published none.
void MountFromCacheHandsBackTheManifestItPublished() {
  World a = WithObjects(4, 300, 120, 'a', 100);
  a.label = "combat_content";
  FakeEnv env;
  a.Serve(env);
  Captured run1;
  nc::FetchAndMount(env, a.config, run1.Sink(), kGo);
  Captured cap;
  nc::Manifest boot;
  QCHECK(nc::MountFromCache(env, a.config, cap.Sink(), &boot));
  QCHECK(boot.labels == std::vector<std::string>{"combat_content"} && boot.entries.size() == 4);
  nc::Manifest parsed;
  QCHECK(nc::ParseManifestBody(cdn_test::ZstdFrameOf(a.ManifestText()), LimitsOf(env), &parsed).status == nc::ParseStatus::kOk);
  QCHECK(nc::SameMountRecords(boot, parsed, nc::kAndroidArm64));

  // Every name missing: nothing is published, and the handed-back manifest is empty (the junk label is cleared).
  World w = WithObjects(4, 300, 120, 'a', 100);
  w.label = "combat_content";
  FakeEnv first;
  w.Serve(first);
  Captured cap1;
  nc::FetchAndMount(first, w.config, cap1.Sink(), kGo);
  World other(2);
  other.label = "combat_content";
  other.objs[0].name = "0x9001";
  other.objs[1].name = "0x9002";
  for (std::size_t i = 0; i < other.objs.size(); ++i) {
    other.objs[i].frame = MakeFrame(900 + i, 150 + i, static_cast<char>('m' + i));
    other.objs[i].bytes = 900 + i;
    other.objs[i].sha = Sha256Of(other.objs[i].frame);
  }
  first.fs_->files[nc::ManifestCopyPath(kDir)] = cdn_test::ZstdFrameOf(other.ManifestText());
  FakeEnv second(first.fs_);
  Captured cap2;
  nc::Manifest none;
  none.labels.push_back("junk");
  QCHECK(!nc::MountFromCache(second, w.config, cap2.Sink(), &none) && cap2.sets.empty());
  QCHECK(none.labels.empty() && none.entries.empty());

  // An empty store: nothing to hand back either.
  FakeEnv empty;
  Captured cap3;
  nc::Manifest nothing;
  nothing.labels.push_back("junk");
  QCHECK(!nc::MountFromCache(empty, w.config, cap3.Sink(), &nothing) && nothing.labels.empty());
}

int main() {
  AManifestIsReadByItsBytesNotByItsName();
  APlainManifestOverTheTextLimitEndsAtTheLimitAsItArrives();
  AFrameWithABrokenHeaderIsRefusedBeforeTheBody();
  AShortOrForeignBodyIsRefusedAsNoManifest();
  TheFirstLaunchFetchesStoresSavesMountsAndPublishes();
  AFirstLaunchUnderWindowsShareModesMountsWhatItFetched();
  AnUpdateLaunchUnderWindowsShareModesStoresIntoTheNextFileAndMounts();
  TheLogHasTheLimitsTheStoreThePlanTheObjectsAndTheOutcome();
  TheSecondLaunchMountsFromTheStoreWithNoRequestAndNoHash();
  AFetchOfWhatTheStoreHoldsMakesOnlyTheManifestRequestAndPublishesNothing();
  ABootWithNothingStoredMountsNothingAndSaysNothingWrong();
  ABootWithObjectsButNoSavedManifestSaysSoAtWarn();
  ABootWhoseEveryNameIsMissingSaysAllRefusedOrMissing();
  ABootWhereNothingMountsSaysSoWithItsReason();
  ANewManifestAddsObjectsAndPublishesAShadowWithFreshChunkIndices();
  ABlobTwoEntriesNameIsFetchedOnce();
  ZeroByteRecordsInTheManifestAreMounted();
  AHeldObjectOfAnotherSizeThanTheManifestStatesIsRefusedByName();
  ADeviceThatGivesNoMemoryFactStopsTheRunBeforeAnyRequest();
  ALabelThatDoesNotFitTheFreeSpaceIsSkippedAndNothingIsRequested();
  ARunThatFindsItsDeviceGivesTheStoreTheComputedBounds();
  ABadRecordInTheManifestIsRefusedByRuleAndTheRestIsFetched();
  AFloodOfRefusalsIsSummarisedAfterTheFirstFew();
  ADownloadWhoseFrameIsNotAZstdFrameIsNeverStored();
  AnObjectWhoseHeaderIsWrongEndsTheDownloadAtTheHeader();
  ADownloadWhoseHeaderStatesAnotherSizeThanTheManifestIsNeverStored();
  ABodyThatIsNotWhatTheManifestSaidIsNeverStoredWhateverTheWay();
  AFailedObjectOfARequiredLabelIsAnErrorAndOfAnOptionalOneAWarning();
  AFullDiskStopsTheRunWithTheReasonAndGivesTheBytesBack();
  AManifestThatCannotBeFetchedChangesNothingAndWhatIsStoredStaysMounted();
  AManifestOverTheComputedTextLimitIsRefusedByName();
  AFrameWithNoContentSizeIsRefusedByName();
  ACorruptFrameIsRefusedByName();
  ADeclaredSizeOverTheMemoryLimitEndsTheDownloadAtTheHeader();
  ARefusedManifestNamesTheLabelAndIndexNeverTheText();
  TheProgressCallbackCountsEveryObjectAndEndsAtTheTotal();
  AShutdownBetweenObjectsEndsTheRunAndKeepsWhatWasStored();
  AKillAfterAnyStepLeavesAnHonestStoreAndTheNextLaunchFinishes();
  ABootRefusesALabelWhoseFourByteScriptRecordNamesNoFileOnDisk();
  TheGateTurnsOffOneLabelAndMountsItsNeighbour();
  AFetchRefusesTheSameLabelWithEveryBlobAlreadyStored();
  AScriptOnDiskUnderANameOutsideTheLabelsListMountsTheLabel();
  AScriptRecordOfZeroBytesIsRefusedLikeAnEntry();
  NoScriptDirLeavesTheGateOffAndSaysSo();
  AFileOnDiskUnderAListedNameIsShadowingAndRefusesTheLabel();
  TheRegistrarSeamRunsBeforeTheGateAndItsRefusalsTurnTheLabelOff();
  ARegistrarRefusalTurnsOffALabelWithNoScriptRecord();
  KeepBootOnlyReplacesTheBootOnlyLabelsRecordsWithTheBootSets();
  KeepBootOnlyDropsALabelTheBootSetLacks();
  BootOnlyLabelsAddsEveryLabelWithScriptsOfThisPlatform();
  SameMountRecordsComparesWhatMountReads();
  ABootOnlyLabelKeepsTheBootSetWhenTheFetchBringsANewOne();
  ABootOnlyLabelWithNoBootSetIsStoredNotPublished();
  ANonBootOnlyLabelStillPublishesBesideAKeptOne();
  AnUnchangedBootOnlyLabelWithNewObjectsOfAnotherLabelPublishes();
  AnEmptyListFetchesAndPublishesAsBefore();
  MountFromCacheHandsBackTheManifestItPublished();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "cdn_run_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("cdn_run_test: all checks passed\n");
  return 0;
}
