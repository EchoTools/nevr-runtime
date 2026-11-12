// Host test for the cargo worker (src/core/cargo_run.*; docs/adr/0045-cdn-texture-delivery.md D3, D4 and T1): a scripted server
// serves a manifest and two blobs, the worker fetches, verifies, stores, writes the index last, mounts and publishes, and the second
// launch makes no blob request. Then every way a fetch can go wrong leaves the cache with an index that names only verified blobs,
// including a kill after each step. The machine is cargo_fake_env.h's; the log lines are the design's T1 lines.
//
// Built and run by `just test-quest-data`.

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/cargo_run.h"
#include "quest/tests/cargo_fake_env.h"
#include "quest/tests/test_check.h"

namespace {

namespace nc = nevr_cargo;
using cargo_test::FakeEnv;
using cargo_test::Response;
using cargo_test::Sha256Of;
using json = nlohmann::ordered_json;

constexpr const char* kBase = "https://cargo.echo.taxi/";
constexpr const char* kManifestPath = "v1/test/manifest.json";
constexpr const char* kDescType = "0xe2efe7289d5985b8";
constexpr const char* kPixType = "0x489bb35d53ca50e9";
constexpr const char* kPoster = "0x3fa6d25bc24a7b01";
constexpr std::uint64_t kPosterName = 0x3fa6d25bc24a7b01ULL;

std::string Bytes(std::size_t n, char seed) {
  std::string s(n, '\0');
  for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>(seed + static_cast<char>(i % 29));
  return s;
}

json EntryJson(const char* type, const char* name, const std::string& blob, std::uint64_t bytes, std::uint64_t stock) {
  return json{{"platform", "android-arm64"}, {"type", type}, {"name", name}, {"format", "zstd-frame"}, {"sha256", Sha256Of(blob)},
              {"size", blob.size()},         {"bytes", bytes}, {"align", 16},  {"stock_size", stock}};
}

struct World {
  FakeEnv env;
  std::string desc = Bytes(40, 'd');    // the descriptor's stored frame (the runtime never decompresses it)
  std::string pix = Bytes(2000, 'p');   // the pixels' stored frame
  nc::RunConfig config;
  std::vector<std::unique_ptr<nc::Mounted>> published;
  nc::Publish publish = [this](std::unique_ptr<nc::Mounted> m) { published.push_back(std::move(m)); };

  World() { Setup(); }
  explicit World(std::shared_ptr<cargo_test::Fs> fs) : env(std::move(fs)) { Setup(); }

  void Setup() {
    config.dir = "/cache";
    config.base_url = kBase;
    config.manifest_path = kManifestPath;
    ServeManifest(Doc());
    ServeBlobs();
  }
  json Doc() const {
    return json{{"schema", "nevr-cargo-manifest"},
                {"schema_version", 1},
                {"labels", {{"lobby_posters", {{"entries", {EntryJson(kDescType, kPoster, desc, 248, 248),
                                                           EntryJson(kPixType, kPoster, pix, 8388608, 8388608)}}}}}}};
  }
  void ServeManifest(const json& doc) { env.server[std::string(kBase) + kManifestPath] = Response{200, doc.dump(), std::string::npos, ""}; }
  void ServeBlob(const std::string& blob) { env.server[std::string(kBase) + "blobs/sha256/" + Sha256Of(blob)] = Response{200, blob, std::string::npos, ""}; }
  void ServeBlobs() {
    ServeBlob(desc);
    ServeBlob(pix);
  }
  std::string BlobUrl(const std::string& blob) const { return std::string(kBase) + "blobs/sha256/" + Sha256Of(blob); }
  nc::RunResult Run(const std::function<bool()>& keepGoing = [] { return true; }) {
    return nc::FetchAndMount(env, config, publish, keepGoing);
  }
  bool HasBlob(const std::string& blob) const { return env.fs_->files.count(nc::BlobPath("/cache", Sha256Of(blob))) != 0; }
  std::size_t TempFiles() const {
    std::size_t n = 0;
    for (const auto& kv : env.fs_->files) {
      if (kv.first.find("/.tmp") != std::string::npos) ++n;
    }
    return n;
  }
  std::size_t BlobGets() const {
    std::size_t n = 0;
    for (const std::string& r : env.requests) {
      if (r.find("/blobs/sha256/") != std::string::npos) ++n;
    }
    return n;
  }
};

bool Level(const std::string& line, const char* level) { return line.find(std::string("\"level\":\"") + level + "\"") != std::string::npos; }

// The index on disk names only blobs that are in place and hash to their names.
bool IndexIsHonest(FakeEnv& env) {
  nc::Index index;
  const nc::IndexStatus status = nc::ReadIndex(env, "/cache", &index);
  if (status == nc::IndexStatus::kAbsent) return true;
  if (status != nc::IndexStatus::kOk) return false;
  for (const nc::IndexEntry& e : index.entries) {
    if (nc::HashFile(env, nc::BlobPath("/cache", e.sha256)) != e.sha256) return false;
  }
  return true;
}

void TheFirstLaunchFetchesVerifiesStoresIndexesMountsAndPublishes() {
  World w;
  const nc::RunResult r = w.Run();
  QCHECK(r.manifest_loaded && r.fetched == 2 && r.cached == 0 && r.failed == 0 && r.published);
  QCHECK(r.bytes == 2040);
  // The requests: the manifest, then the descriptor and the pixels.
  QCHECK(w.env.requests.size() == 3);
  QCHECK(w.env.requests[0] == std::string(kBase) + kManifestPath);
  QCHECK(w.env.requests[1] == w.BlobUrl(w.desc) && w.env.requests[2] == w.BlobUrl(w.pix));
  // The files: two blobs named by their SHA-256, the index, no temp file left.
  QCHECK(w.HasBlob(w.desc) && w.HasBlob(w.pix) && w.TempFiles() == 0);
  nc::Index index;
  QCHECK(nc::ReadIndex(w.env, "/cache", &index) == nc::IndexStatus::kOk && index.entries.size() == 2);
  QCHECK(index.entries.size() == 2 && index.entries[0].label == "lobby_posters" && index.entries[1].label == "lobby_posters");
  std::uint64_t byteSum = 0;
  for (const nc::IndexEntry& e : index.entries) byteSum += e.bytes;
  QCHECK(byteSum == 248 + 8388608);
  // The published shadow serves both keys from the held blobs.
  QCHECK(w.published.size() == 1 && w.published[0]->shadow != nullptr);
  if (w.published.empty() || !w.published[0]->shadow) return;
  nevr_shadow::Found f;
  QCHECK(w.published[0]->shadow->Find(0xe2efe7289d5985b8ULL, kPosterName, &f));
  QCHECK(f.chunk >= nevr_shadow::kFirstChunkIndex && f.compressed == 40 && f.decompressed == 248 && f.size == 248);
  QCHECK(w.published[0]->shadow->Find(0x489bb35d53ca50e9ULL, kPosterName, &f));
  QCHECK(f.chunk >= nevr_shadow::kFirstChunkIndex && f.compressed == 2000 && f.decompressed == 8388608 && f.size == 8388608);
  QCHECK(w.env.open_blobs() == 2);
}

void TheLogLinesAreTheDesignsInTheDesignsOrder() {
  nevr_shadow::Shadow::ResetChunkCounterForTest();  // a fresh process: the first shadow starts at the first index
  World w;
  w.Run();
  QCHECK(w.env.EventOrder() == "cargo_start,cargo_manifest,cargo_plan,cargo_blob,cargo_blob,cargo_index,cargo_mounted,cargo_done");
  const std::vector<std::string>& e = w.env.events;
  if (e.size() != 8) return;
  QCHECK(FakeEnv::Value(e[0], "source") == "default" && FakeEnv::Value(e[0], "host") == "cargo.echo.taxi");
  QCHECK(FakeEnv::Value(e[0], "platform") == "android-arm64" && FakeEnv::Value(e[0], "labels") == "lobby_posters=on");
  QCHECK(FakeEnv::Value(e[0], "required_only") == "0");
  QCHECK(e[0].find("v1/") == std::string::npos && e[0].find("https") == std::string::npos);  // never the path or the scheme
  QCHECK(FakeEnv::Value(e[1], "result") == "loaded" && FakeEnv::Value(e[1], "http_status") == "200");
  QCHECK(FakeEnv::Value(e[1], "labels") == "1" && FakeEnv::Value(e[1], "entries") == "2" && FakeEnv::Value(e[1], "platform_entries") == "2");
  QCHECK(FakeEnv::Value(e[1], "sha256").size() == 64);
  QCHECK(FakeEnv::Value(e[2], "label") == "lobby_posters" && FakeEnv::Value(e[2], "state") == "on" && FakeEnv::Value(e[2], "entries") == "2");
  QCHECK(FakeEnv::Value(e[2], "cached") == "0" && FakeEnv::Value(e[2], "to_fetch") == "2" && FakeEnv::Value(e[2], "fetch_bytes") == "2040");
  QCHECK(FakeEnv::Value(e[3], "result") == "stored" && FakeEnv::Value(e[3], "type") == kDescType && FakeEnv::Value(e[3], "name") == kPoster);
  QCHECK(FakeEnv::Value(e[3], "sha256") == Sha256Of(w.desc) && FakeEnv::Value(e[3], "size") == "40");
  QCHECK(FakeEnv::Value(e[4], "result") == "stored" && FakeEnv::Value(e[4], "type") == kPixType && FakeEnv::Value(e[4], "size") == "2000");
  QCHECK(FakeEnv::Value(e[5], "result") == "written" && FakeEnv::Value(e[5], "entries") == "2");
  QCHECK(FakeEnv::Value(e[6], "source") == "fetch" && FakeEnv::Value(e[6], "labels") == "1" && FakeEnv::Value(e[6], "records") == "2");
  QCHECK(FakeEnv::Value(e[6], "chunk_base") == "0x01000000" && FakeEnv::Value(e[6], "refused") == "0");
  QCHECK(FakeEnv::Value(e[7], "fetched") == "2" && FakeEnv::Value(e[7], "cached") == "0" && FakeEnv::Value(e[7], "failed") == "0");
  QCHECK(FakeEnv::Value(e[7], "bytes") == "2040");
  for (const std::string& line : e) QCHECK(Level(line, "info"));
}

// A republish after a fetch is a second shadow in the same process: it takes indices above the first's, so the engine's block cache
// (keyed by the chunk index alone) never serves one shadow's decompressed block for the other's chunk.
void ARepublishTakesFreshChunkIndicesAboveTheFirstShadows() {
  nevr_shadow::Shadow::ResetChunkCounterForTest();
  World w;
  w.Run();
  QCHECK(w.published.size() == 1 && w.published[0]->shadow != nullptr);
  // The manifest now names a second texture; the worker fetches it and publishes a second set.
  const std::string other = Bytes(60, 'o');
  json doc = w.Doc();
  doc["labels"]["lobby_posters"]["entries"].push_back(EntryJson(kPixType, "0x77", other, 4096, 4096));
  w.ServeManifest(doc);
  w.ServeBlob(other);
  const nc::RunResult r = w.Run();
  QCHECK(r.fetched == 1 && r.published && w.published.size() == 2 && w.published[1]->shadow != nullptr);
  if (w.published.size() != 2 || !w.published[0]->shadow || !w.published[1]->shadow) return;
  const nevr_shadow::Shadow& a = *w.published[0]->shadow;
  const nevr_shadow::Shadow& b = *w.published[1]->shadow;
  QCHECK(b.chunk_base() == a.chunk_base() + a.chunk_count());  // the second range starts where the first ended
  nevr_shadow::Found fa, fb;
  QCHECK(a.Find(0x489bb35d53ca50e9ULL, kPosterName, &fa) && b.Find(0x489bb35d53ca50e9ULL, kPosterName, &fb));  // the same texture in both
  QCHECK(fa.chunk != fb.chunk);                                                                              // never the same cache key
  QCHECK(fa.chunk < b.chunk_base() && fb.chunk >= b.chunk_base());
  // The mount line says which range each shadow took.
  const std::vector<std::string> mounts = w.env.EventsNamed("cargo_mounted");
  char want[16];
  QCHECK(mounts.size() == 2);
  std::snprintf(want, sizeof(want), "0x%08x", static_cast<unsigned>(a.chunk_base()));
  QCHECK(mounts.size() == 2 && FakeEnv::Value(mounts[0], "chunk_base") == want);
  std::snprintf(want, sizeof(want), "0x%08x", static_cast<unsigned>(b.chunk_base()));
  QCHECK(mounts.size() == 2 && FakeEnv::Value(mounts[1], "chunk_base") == want);
}

void TheSecondLaunchMountsFromTheCacheAndRequestsNoBlob() {
  World first;
  first.Run();
  World second(first.env.fs_);
  QCHECK(nc::MountFromCache(second.env, second.config, second.publish));
  QCHECK(second.published.size() == 1 && second.published[0]->shadow != nullptr);
  QCHECK(second.env.EventOrder() == "cargo_mounted" && FakeEnv::Value(second.env.events[0], "source") == "boot");
  // The boot mount runs in the library's constructor and hashes every blob: the line says how long it took. The fake clock
  // moves 3 ms per read, so a mount that read the clock before and after reports a multiple of 3 above 0.
  {
    const std::string ms = second.env.events.empty() ? "" : FakeEnv::Value(second.env.events[0], "ms");
    const bool number = !ms.empty() && ms.find_first_not_of("0123456789") == std::string::npos;
    QCHECK(number && ms != "0" && std::stoull(ms) % 3 == 0);
  }
  QCHECK(second.env.requests.empty());  // the boot half touches no network
  const nc::RunResult r = second.Run();
  QCHECK(r.fetched == 0 && r.cached == 2 && r.failed == 0 && !r.published);
  QCHECK(second.BlobGets() == 0);  // zero GetRange calls for blobs
  QCHECK(second.env.requests.size() == 1);
  QCHECK(second.env.EventOrder() == "cargo_mounted,cargo_start,cargo_manifest,cargo_plan,cargo_done");  // no new index, no second mount
  const auto plan = second.env.EventsNamed("cargo_plan");
  QCHECK(plan.size() == 1 && FakeEnv::Value(plan[0], "cached") == "2" && FakeEnv::Value(plan[0], "to_fetch") == "0" && FakeEnv::Value(plan[0], "fetch_bytes") == "0");
  const auto done = second.env.EventsNamed("cargo_done");
  QCHECK(FakeEnv::Value(done[0], "fetched") == "0" && FakeEnv::Value(done[0], "cached") == "2");
}

void ABootWithNoIndexMountsNothingAndSaysNothing() {
  World w;
  QCHECK(!nc::MountFromCache(w.env, w.config, w.publish));
  QCHECK(w.published.empty() && w.env.events.empty() && w.env.requests.empty());
  w.env.fs_->files["/cache/index.json"] = "garbage";
  QCHECK(!nc::MountFromCache(w.env, w.config, w.publish));
  const auto lines = w.env.EventsNamed("cargo_index");
  QCHECK(lines.size() == 1 && FakeEnv::Value(lines[0], "result") == "invalid" && Level(lines[0], "warn"));
}

void AManifestThatCannotBeUsedChangesNothing() {
  struct Case {
    const char* name;
    Response resp;
    const char* reason;
  };
  const Case cases[] = {
      {"404", {404, "", std::string::npos, ""}, "http_status"},
      {"500", {500, "oops", std::string::npos, ""}, "http_status"},
      {"not json", {200, "this is not json", std::string::npos, ""}, nullptr},
      {"too large", {200, std::string(nc::kMaxManifestBytes * 3, ' '), std::string::npos, ""}, "too_large"},
      {"dropped", {200, "{\"schema\":", 5, ""}, "transport"},
  };
  for (const Case& c : cases) {
    World w;
    w.env.server[std::string(kBase) + kManifestPath] = c.resp;
    const nc::RunResult r = w.Run();
    QCHECK(!r.manifest_loaded && r.fetched == 0 && !r.published);
    QCHECK(w.BlobGets() == 0 && w.published.empty());
    if (std::string(c.name) == "too large") QCHECK(w.env.body_bytes_delivered[0] <= nc::kMaxManifestBytes + 97);  // ended at the cap
    QCHECK(w.env.fs_->files.count("/cache/index.json") == 0);
    const auto lines = w.env.EventsNamed("cargo_manifest");
    QCHECK(lines.size() == 1 && Level(lines[0], "warn"));
    if (c.reason != nullptr) QCHECK(FakeEnv::Value(lines[0], "reason") == c.reason);
    if (c.reason == nullptr) QCHECK(FakeEnv::Value(lines[0], "reason") == "not_json");
  }
}

void APartialContentAnswerToTheWholeRangeIsTheWholeObject() {
  // The transport sends `Range: bytes=0-`; Cloudflare answers 206 and the body is the whole object.
  World w;
  w.env.server[std::string(kBase) + kManifestPath].status = 206;
  w.env.server[w.BlobUrl(w.desc)].status = 206;
  w.env.server[w.BlobUrl(w.pix)].status = 206;
  const nc::RunResult r = w.Run();
  QCHECK(r.manifest_loaded && r.fetched == 2 && r.failed == 0 && r.published);
  QCHECK(FakeEnv::Value(w.env.EventsNamed("cargo_manifest")[0], "http_status") == "206");
}

void ARefusedManifestNamesTheLabelAndIndexNeverTheText() {
  World w;
  json doc = w.Doc();
  doc["labels"]["lobby_posters"]["entries"][1]["sha256"] = "NOT-A-HASH-with-remote-text";
  w.ServeManifest(doc);
  w.Run();
  const auto lines = w.env.EventsNamed("cargo_manifest");
  QCHECK(lines.size() == 1 && FakeEnv::Value(lines[0], "result") == "refused" && FakeEnv::Value(lines[0], "reason") == "bad_hash");
  QCHECK(FakeEnv::Value(lines[0], "label") == "lobby_posters" && FakeEnv::Value(lines[0], "index") == "1");
  QCHECK(lines[0].find("NOT-A-HASH") == std::string::npos);
}

void ABlobThatIsNotWhatTheManifestSaidIsNeverStored() {
  struct Case {
    const char* name;
    Response resp;
    const char* reason;
  };
  World probe;
  const std::string good = probe.pix;
  std::string flipped = good;
  flipped[100] = static_cast<char>(flipped[100] ^ 0x40);
  const Case cases[] = {
      {"flipped byte", {200, flipped, std::string::npos, ""}, "sha256_mismatch"},
      {"short", {200, good.substr(0, good.size() - 1), std::string::npos, ""}, "size_mismatch"},
      {"long", {200, good, std::string::npos, "EXTRA"}, "too_large"},
      {"404", {404, "", std::string::npos, ""}, "http_status"},
      {"dropped", {200, good, 700, ""}, "transport"},
  };
  for (const Case& c : cases) {
    World w;
    w.env.server[w.BlobUrl(w.pix)] = c.resp;
    const nc::RunResult r = w.Run();
    if (std::string(c.name) == "long") {
      // The transfer is ended at the first byte past `size`: the worker never takes the extra bytes.
      QCHECK(w.env.body_bytes_delivered.back() <= good.size() + 97);
    }
    QCHECK(r.fetched == 1 && r.failed == 1 && r.manifest_loaded);
    QCHECK(w.HasBlob(w.desc) && !w.HasBlob(w.pix) && w.TempFiles() == 0);
    QCHECK(w.env.fs_->files.count("/cache/index.json") == 0);  // the label is incomplete: no index names half a poster
    QCHECK(w.published.empty());
    const auto lines = w.env.EventsNamed("cargo_blob");
    QCHECK(lines.size() == 2 && FakeEnv::Value(lines[1], "result") == "failed");
    if (lines.size() == 2) QCHECK(FakeEnv::Value(lines[1], "reason") == c.reason);
    if (lines.size() == 2) QCHECK(Level(lines[1], "warn"));
    const auto done = w.env.EventsNamed("cargo_done");
    QCHECK(done.size() == 1 && FakeEnv::Value(done[0], "failed") == "1" && Level(done[0], "warn"));
  }
}

void ARequiredLabelsFailureIsAnError() {
  World w;
  w.config.policy.overrides["lobby_posters"] = nc::LabelState::kRequired;
  w.env.server[w.BlobUrl(w.pix)] = Response{500, "", std::string::npos, ""};
  w.Run();
  const auto lines = w.env.EventsNamed("cargo_blob");
  QCHECK(lines.size() == 2 && Level(lines[1], "error"));
}

void ARunAfterAFailureFetchesOnlyWhatIsMissing() {
  World w;
  w.env.server[w.BlobUrl(w.pix)] = Response{503, "", std::string::npos, ""};
  w.Run();
  QCHECK(w.HasBlob(w.desc) && !w.HasBlob(w.pix));
  w.ServeBlobs();
  w.env.requests.clear();
  w.env.events.clear();
  const nc::RunResult r = w.Run();
  QCHECK(r.fetched == 1 && r.cached == 1 && r.failed == 0 && r.published);
  QCHECK(w.BlobGets() == 1 && w.env.requests[1] == w.BlobUrl(w.pix));
  QCHECK(IndexIsHonest(w.env) && w.published.size() == 1);
}

void AFailedLabelKeepsWhatTheOldIndexSaid() {
  World first;
  first.Run();
  const std::string oldIndex = first.env.fs_->files["/cache/index.json"];
  // The manifest changes the poster's pixels; the new blob cannot be fetched.
  World w(first.env.fs_);
  const std::string newPix = Bytes(2100, 'q');
  json doc = w.Doc();
  doc["labels"]["lobby_posters"]["entries"][1] = EntryJson(kPixType, kPoster, newPix, 8388608, 8388608);
  w.ServeManifest(doc);
  w.env.server[w.BlobUrl(newPix)] = Response{500, "", std::string::npos, ""};
  const nc::RunResult r = w.Run();
  QCHECK(r.failed == 1 && !r.published);
  QCHECK(w.env.fs_->files["/cache/index.json"] == oldIndex);  // the old, honest index stays
  QCHECK(IndexIsHonest(w.env));
}

// A name that moves to another label while its old label fails: the failed label keeps its old entries, but not one whose
// (type, name) a label written fresh now holds, so the index never names a key twice and the shadow, which refuses a key seen
// twice, still mounts. Run 1: lobby_posters = {P}, second = {0x11}. Run 2: P moves to second, and lobby_posters' only
// entry is a new name whose blob 404s.
void ANameMovedToAnotherLabelWhileItsOldLabelFailsIsIndexedOnceAndMounts() {
  constexpr const char* kOther = "0x0000000000000011";
  const std::string otherDesc = Bytes(40, 'e');
  const std::string otherPix = Bytes(2000, 'f');
  World first;
  first.config.policy.overrides["second"] = nc::LabelState::kOn;
  json doc1 = first.Doc();
  doc1["labels"]["second"] = {{"entries", {EntryJson(kDescType, kOther, otherDesc, 248, 248),
                                           EntryJson(kPixType, kOther, otherPix, 8388608, 8388608)}}};
  first.ServeManifest(doc1);
  first.ServeBlob(otherDesc);
  first.ServeBlob(otherPix);
  QCHECK(first.Run().published);

  World w(first.env.fs_);
  w.config.policy.overrides["second"] = nc::LabelState::kOn;
  const std::string lostPix = Bytes(2100, 'q');
  json doc2 = w.Doc();
  doc2["labels"]["lobby_posters"]["entries"] = json::array({EntryJson(kPixType, "0x0000000000000022", lostPix, 8388608, 8388608)});
  doc2["labels"]["second"] = {{"entries", {EntryJson(kDescType, kPoster, w.desc, 248, 248),
                                           EntryJson(kPixType, kPoster, w.pix, 8388608, 8388608),
                                           EntryJson(kDescType, kOther, otherDesc, 248, 248),
                                           EntryJson(kPixType, kOther, otherPix, 8388608, 8388608)}}};
  w.ServeManifest(doc2);
  w.ServeBlob(otherDesc);
  w.ServeBlob(otherPix);
  w.env.server[w.BlobUrl(lostPix)] = Response{404, "", std::string::npos, ""};
  const nc::RunResult r = w.Run();
  QCHECK(r.failed == 1);
  nc::Index index;
  QCHECK(nc::ReadIndex(w.env, "/cache", &index) == nc::IndexStatus::kOk);
  QCHECK(index.entries.size() == 4);  // second's four; lobby_posters' old P entries are dropped, P being second's now
  for (const nc::IndexEntry& e : index.entries) QCHECK(e.label == "second");
  const auto mounts = w.env.EventsNamed("cargo_mounted");
  QCHECK(!mounts.empty() && FakeEnv::Value(mounts.back(), "records") == "4" && FakeEnv::Value(mounts.back(), "refused") == "0");
  QCHECK(!mounts.empty() && FakeEnv::Value(mounts.back(), "source") == "fetch" && FakeEnv::Value(mounts.back(), "ms") != "0" &&
         FakeEnv::Value(mounts.back(), "ms") != "<absent>");  // the fetch mount says its time too
  QCHECK(r.published && !w.published.empty() && w.published.back()->shadow != nullptr);
  nevr_shadow::Found found;
  if (!w.published.empty() && w.published.back()->shadow) {
    QCHECK(w.published.back()->shadow->Find(0x489bb35d53ca50e9ULL, kPosterName, &found));
  }

  // And the next launch, offline, mounts the same index.
  World third(first.env.fs_);
  third.config.policy.overrides["second"] = nc::LabelState::kOn;
  QCHECK(nc::MountFromCache(third.env, third.config, third.publish));
  const auto boot = third.env.EventsNamed("cargo_mounted");
  QCHECK(boot.size() == 1 && FakeEnv::Value(boot[0], "records") == "4");
}

void ALabelThatIsOffMakesNoBlobRequest() {
  World w;
  w.config.policy.overrides["lobby_posters"] = nc::LabelState::kOff;
  const nc::RunResult r = w.Run();
  QCHECK(r.manifest_loaded && r.fetched == 0 && w.BlobGets() == 0 && w.published.empty());
  QCHECK(w.env.fs_->files.count("/cache/index.json") == 0 || w.env.fs_->files["/cache/index.json"].find("lobby_posters") == std::string::npos);
  const auto plan = w.env.EventsNamed("cargo_plan");
  QCHECK(plan.size() == 1 && FakeEnv::Value(plan[0], "skipped") == "off");
}

void AnUnlistedLabelIsOffEvenWhenTheManifestListsIt() {
  World w;
  json doc = w.Doc();
  doc["labels"]["brand_new"] = doc["labels"]["lobby_posters"];
  doc["labels"]["brand_new"]["entries"][0]["name"] = "0x99";
  doc["labels"]["brand_new"]["entries"][1]["name"] = "0x99";
  doc["labels"]["brand_new"]["state"] = "required";  // the manifest cannot turn a label on
  w.ServeManifest(doc);
  const nc::RunResult r = w.Run();
  QCHECK(r.fetched == 2);  // only lobby_posters' two blobs; brand_new shares the same blobs but is off
  const auto plans = w.env.EventsNamed("cargo_plan");
  QCHECK(plans.size() == 2);
  if (plans.size() == 2) QCHECK(FakeEnv::Value(plans[1], "label") == "brand_new" && FakeEnv::Value(plans[1], "skipped") == "off");
  QCHECK(FakeEnv::Value(w.env.EventsNamed("cargo_manifest")[0], "unknown_fields") == "1");
}

void AShutdownBetweenBlobsEndsTheRunWithoutAnIndex() {
  World w;
  // keep_going turns false once the first blob is stored, so the second never starts.
  const nc::RunResult r = w.Run([&w] { return w.env.EventsNamed("cargo_blob").empty(); });
  QCHECK(r.fetched == 1 && !r.published && w.HasBlob(w.desc) && !w.HasBlob(w.pix));
  QCHECK(w.BlobGets() == 1);
  QCHECK(w.env.fs_->files.count("/cache/index.json") == 0);
  QCHECK(w.TempFiles() == 0);
  const auto done = w.env.EventsNamed("cargo_done");
  QCHECK(done.size() == 1 && FakeEnv::Value(done[0], "result") == "cancelled");
}

// Kill the process after each mutating step of a first launch. At every point the cache must hold an honest index (or none), and the next
// launch must finish the job.
void AKillAfterAnyStepLeavesAnHonestCacheAndTheNextLaunchFinishes() {
  int total = 0;
  {
    World w;
    w.Run();
    total = w.env.mutating_ops;
  }
  QCHECK(total >= 5);
  for (int k = 0; k <= total; ++k) {
    World killed;
    killed.env.stop_after = k;
    killed.Run();
    QCHECK(IndexIsHonest(killed.env));
    // The next launch, with the same files: a fresh environment that is not killed.
    World next(killed.env.fs_);
    next.env.fs_->next_temp += 1000;
    nc::MountFromCache(next.env, next.config, next.publish);  // the boot half, as a launch does it
    next.Run();
    QCHECK(IndexIsHonest(next.env));
    nc::Index index;
    QCHECK(nc::ReadIndex(next.env, "/cache", &index) == nc::IndexStatus::kOk && index.entries.size() == 2);
    QCHECK(!next.published.empty() && next.published.back()->shadow != nullptr && next.published.back()->records == 2);
    if (k < total) {
      // A leftover temp file from the kill is not a blob and not in the index.
      nc::Index idx;
      nc::ReadIndex(next.env, "/cache", &idx);
      for (const nc::IndexEntry& e : idx.entries) QCHECK(next.env.fs_->files.count(nc::BlobPath("/cache", e.sha256)) == 1);
    }
  }
}

void AFlippedByteInAHeldBlobIsFetchedAgain() {
  World first;
  first.Run();
  first.env.fs_->files[nc::BlobPath("/cache", Sha256Of(first.pix))][10] ^= 1;  // the cached copy rots
  World w(first.env.fs_);
  const nc::RunResult r = w.Run();
  QCHECK(r.fetched == 1 && r.cached == 1 && r.failed == 0);
  QCHECK(w.BlobGets() == 1 && w.env.requests[1] == w.BlobUrl(w.pix));
  QCHECK(r.published && w.published.size() == 1);  // the blob was replaced, so the set is mounted again
  QCHECK(IndexIsHonest(w.env));
}

// The size cap (#873): after the index is written a run removes unreferenced blobs, oldest first, and never one the index names.
void AnOverCapCacheLosesItsUnreferencedBlobAndKeepsTheNamedOnes() {
  World w;
  const std::string stale = Bytes(500, 's');
  const std::string staleKey = nc::BlobPath("/cache", Sha256Of(stale));
  w.env.fs_->files[staleKey] = stale;  // modified time 0: older than anything this run writes
  w.config.cap_bytes = w.desc.size() + w.pix.size() + 10;  // the two named blobs fit; with the stale one the cache does not
  const nc::RunResult r = w.Run();
  QCHECK(r.fetched == 2 && r.failed == 0);
  QCHECK(w.env.fs_->files.count(staleKey) == 0 && w.HasBlob(w.desc) && w.HasBlob(w.pix));
  const auto ev = w.env.EventsNamed("cargo_evict");
  QCHECK(ev.size() == 1 && FakeEnv::Value(ev[0], "evicted") == "1" && FakeEnv::Value(ev[0], "result") == "ok");
  QCHECK(Level(ev[0], "info") && IndexIsHonest(w.env));
}

void ACapBelowTheNamedBlobsStillKeepsThemAndSaysSo() {
  World w;
  w.config.cap_bytes = 10;
  w.Run();
  QCHECK(w.HasBlob(w.desc) && w.HasBlob(w.pix));  // the index names both: neither is evictable
  const auto ev = w.env.EventsNamed("cargo_evict");
  QCHECK(ev.size() == 1 && FakeEnv::Value(ev[0], "over_cap_all_referenced") == "true" && Level(ev[0], "warn"));
  World second(w.env.fs_);
  second.config.cap_bytes = 10;
  second.Run();
  QCHECK(second.BlobGets() == 0);  // nothing was removed, so nothing is fetched again
}

void AnUnderCapRunWritesNoEvictEvent() {
  World w;
  w.Run();
  QCHECK(w.env.EventsNamed("cargo_evict").empty());
}

}  // namespace

int main() {
  TheFirstLaunchFetchesVerifiesStoresIndexesMountsAndPublishes();
  TheLogLinesAreTheDesignsInTheDesignsOrder();
  ARepublishTakesFreshChunkIndicesAboveTheFirstShadows();
  TheSecondLaunchMountsFromTheCacheAndRequestsNoBlob();
  ABootWithNoIndexMountsNothingAndSaysNothing();
  AManifestThatCannotBeUsedChangesNothing();
  APartialContentAnswerToTheWholeRangeIsTheWholeObject();
  ARefusedManifestNamesTheLabelAndIndexNeverTheText();
  ABlobThatIsNotWhatTheManifestSaidIsNeverStored();
  ARequiredLabelsFailureIsAnError();
  ARunAfterAFailureFetchesOnlyWhatIsMissing();
  AFailedLabelKeepsWhatTheOldIndexSaid();
  ANameMovedToAnotherLabelWhileItsOldLabelFailsIsIndexedOnceAndMounts();
  ALabelThatIsOffMakesNoBlobRequest();
  AnUnlistedLabelIsOffEvenWhenTheManifestListsIt();
  AShutdownBetweenBlobsEndsTheRunWithoutAnIndex();
  AKillAfterAnyStepLeavesAnHonestCacheAndTheNextLaunchFinishes();
  AFlippedByteInAHeldBlobIsFetchedAgain();
  AnOverCapCacheLosesItsUnreferencedBlobAndKeepsTheNamedOnes();
  ACapBelowTheNamedBlobsStillKeepsThemAndSaysSo();
  AnUnderCapRunWritesNoEvictEvent();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "cargo_run_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("cargo_run_test: all checks passed\n");
  return 0;
}
