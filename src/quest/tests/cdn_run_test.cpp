// Host test for the CDN worker (src/cdn/cdn_run.*; docs/adr/0045-cdn-texture-delivery.md D3 and D4): a scripted server serves a
// manifest and its objects; the worker computes its limits from the device, plans, hashes each object once while it downloads,
// appends it to the store, saves the manifest, mounts and publishes. The second launch mounts with no request and no hash. Every
// way a fetch can go wrong leaves a store that names only whole objects, including a kill after each step.
//
// Built and run by `just test-quest-data`.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "cdn/cdn_run.h"
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
    return json{{"schema", "nevr-cargo-manifest"}, {"schema_version", 1}, {"labels", labels}, {"empty", empty}}.dump();
  }
  void Serve(FakeEnv& env) const {
    env.server[std::string(kBase) + kManifestPath] = Response{200, cdn_test::ZstdFrameOf(ManifestText()), std::string::npos, ""};
    for (const Obj& o : objs) env.server[std::string(kBase) + "blobs/sha256/" + o.sha] = Response{200, o.frame, std::string::npos, ""};
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
    if (std::string(failure) == "garbage") m.body = "<html>";  // not a zstd frame
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

void AManifestOverTheComputedTextLimitIsRefusedByName() {
  World w;
  FakeEnv env;
  w.Serve(env);
  env.device.memory_available = 20000;  // text limit: 20000 / (8 * 8) = 312 bytes
  Captured cap;
  const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  QCHECK(!r.manifest_loaded && std::string(r.stopped) == "manifest_failed");
  const auto events = env.EventsNamed("cdn_manifest");
  QCHECK(events.size() == 1 && FakeEnv::Value(events[0], "reason") == "too_large");
}

void APlainJsonManifestIsRefusedByName() {
  World w;
  FakeEnv env;
  w.Serve(env);
  env.server[std::string(kBase) + kManifestPath].body = w.ManifestText();  // nothing uncompressed is published, so none is read
  Captured cap;
  const nc::RunResult r = nc::FetchAndMount(env, w.config, cap.Sink(), kGo);
  QCHECK(!r.manifest_loaded && std::string(r.stopped) == "manifest_refused" && GetsOfBlobs(env) == 0);
  const auto events = env.EventsNamed("cdn_manifest");
  QCHECK(events.size() == 1 && FakeEnv::Value(events[0], "reason") == "not_zstd");
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
  World w(40);
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

int main() {
  TheFirstLaunchFetchesStoresSavesMountsAndPublishes();
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
  APlainJsonManifestIsRefusedByName();
  AFrameWithNoContentSizeIsRefusedByName();
  ACorruptFrameIsRefusedByName();
  ADeclaredSizeOverTheMemoryLimitEndsTheDownloadAtTheHeader();
  ARefusedManifestNamesTheLabelAndIndexNeverTheText();
  TheProgressCallbackCountsEveryObjectAndEndsAtTheTotal();
  AShutdownBetweenObjectsEndsTheRunAndKeepsWhatWasStored();
  AKillAfterAnyStepLeavesAnHonestStoreAndTheNextLaunchFinishes();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "cdn_run_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("cdn_run_test: all checks passed\n");
  return 0;
}
