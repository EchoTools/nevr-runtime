// Host test of the cargo core at the scale of a whole game (src/core/cargo_*; docs/adr/0045-cdn-texture-delivery.md): a synthetic
// manifest of 70,000 records, of the on-disk types a game has (the engine's first keys: a CArchiveResource, then everything it
// lists; textures with their descriptor; scripts, meshes, audio, data), including the records of 0 bytes and the objects two names
// share. It plans, fetches into one store file, saves, mounts at a second launch with no request and no hash, and every record
// resolves the way the engine reads it, to the bytes that were stored.
//
// The shape follows the all-assets extraction of the Quest game (54,423 records, 2,562 of 0 bytes, 28,668 objects after
// de-duplication, an 11.5 MB manifest); the count is 70,000 so the margin is stated. Built and run by `just test-quest-data`.

#include <time.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/cargo_run.h"
#include "quest/tests/cargo_fake_env.h"
#include "quest/tests/cargo_zstd_support.h"
#include "quest/tests/test_check.h"

namespace {

namespace nc = nevr_cargo;
using cargo_test::FakeEnv;
using cargo_test::MakeFrame;
using cargo_test::Response;
using cargo_test::Sha256Of;
using json = nlohmann::ordered_json;

constexpr const char* kBase = "https://cargo.echo.taxi/";
constexpr const char* kManifestPath = "v1/allassets/2026-11-12/manifest.json.zst";
constexpr const char* kDir = "/cache";

constexpr std::size_t kRecords = 70000;
constexpr std::size_t kEmpty = 3500;
constexpr std::size_t kEntries = kRecords - kEmpty;
constexpr std::size_t kObjects = 36000;  // two names in about every other entry share an object

struct Type {
  const char* text;
  std::uint64_t value;
  const char* what;
};
// The archive type is the one the engine asks for first (REPORT-4); the pair is the Android texture's two buffers (ADR 0045); the
// others are on-disk types read from the Quest extraction's record table, none of them a texture.
const Type kTypes[] = {
    {"0x2a41cf1c1d9e5d32", 0x2a41cf1c1d9e5d32ULL, "CArchiveResource"},
    {"0xe2efe7289d5985b8", 0xe2efe7289d5985b8ULL, "texture descriptor"},
    {"0x489bb35d53ca50e9", 0x489bb35d53ca50e9ULL, "texture pixels"},
    {"0xd9952dd5b4f69aec", 0xd9952dd5b4f69aecULL, "extraction type d995"},
    {"0x0594ee663256aad3", 0x0594ee663256aad3ULL, "extraction type 0594"},
    {"0x038c44cbea5c59e8", 0x038c44cbea5c59e8ULL, "extraction type 038c"},
    {"0x1396ddab0094e9e2", 0x1396ddab0094e9e2ULL, "extraction type 1396"},
    {"0x004c5273be3eab58", 0x004c5273be3eab58ULL, "extraction type 004c"},
};
constexpr std::size_t kTypeCount = sizeof(kTypes) / sizeof(kTypes[0]);

std::uint64_t NowNs() {
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

struct Synthetic {
  std::vector<std::string> frames;  // by object
  std::vector<std::uint64_t> bytes; // by object
  std::vector<std::string> shas;
  std::vector<std::size_t> object_of_entry;
  std::string text;
  std::map<std::string, std::size_t> by_type;  // records by type
};

Synthetic Build() {
  Synthetic s;
  s.frames.reserve(kObjects);
  for (std::size_t j = 0; j < kObjects; ++j) {
    const std::uint64_t bytes = 64 + (j * 37) % 5000;
    std::string frame = cargo_test::MakeFrame(bytes, 40 + j % 60);
    std::memcpy(&frame[frame.size() - 8], &j, 8);  // every object is its own bytes
    s.bytes.push_back(bytes);
    s.shas.push_back(Sha256Of(frame));
    s.frames.push_back(std::move(frame));
  }
  json entries = json::array();
  for (std::size_t i = 0; i < kEntries; ++i) {
    const std::size_t j = i % kObjects;  // 30,500 entries past the first pass share an object with an earlier one
    const Type& t = kTypes[i % kTypeCount];
    s.object_of_entry.push_back(j);
    entries.push_back(json{{"platform", "android-arm64"},
                           {"type", t.text},
                           {"name", "0x" + [&] {
                              char buf[20];
                              std::snprintf(buf, sizeof(buf), "%016llx", 0x8120755c6da7eb41ull + 0x9e3779b97f4a7c15ull * i);
                              return std::string(buf);
                            }()},
                           {"format", "zstd-frame"},
                           {"sha256", s.shas[j]},
                           {"size", s.frames[j].size()},
                           {"bytes", s.bytes[j]},
                           {"align", 16}});
    ++s.by_type[t.what];
  }
  json empty = json::array();
  for (std::size_t i = 0; i < kEmpty; ++i) {
    const Type& t = kTypes[(i * 3) % kTypeCount];
    char buf[20];
    std::snprintf(buf, sizeof(buf), "%016llx", 0x1111111111111111ull + 0xabcdef12345ull * i);
    empty.push_back(json{{"type", t.text}, {"name", std::string("0x") + buf}, {"align", 4}});
    ++s.by_type[std::string(t.what) + " (0 bytes)"];
  }
  json labels = json::object();
  labels["game"] = json{{"entries", entries}};
  s.text = json{{"schema", "nevr-cargo-manifest"}, {"schema_version", 1}, {"labels", labels}, {"empty", empty}}.dump();
  return s;
}

void AWholeGameOfSeventyThousandRecordsPlansStoresMountsAndResolves() {
  const std::uint64_t t0 = NowNs();
  const Synthetic s = Build();
  const std::uint64_t t1 = NowNs();
  std::printf("cargo_scale_test: manifest %zu bytes, %zu entries + %zu empty = %zu records, %zu objects (built in %llu ms)\n", s.text.size(), kEntries,
              kEmpty, kRecords, kObjects, static_cast<unsigned long long>((t1 - t0) / 1000000));
  for (const auto& kv : s.by_type) std::printf("cargo_scale_test:   %-34s %zu\n", kv.first.c_str(), kv.second);
  // The first design could not have taken this: 2,048 entries, 1 MiB of manifest.
  QCHECK(s.text.size() > (std::size_t{1} << 20));
  QCHECK(kEntries > 2048);

  nc::RunConfig config;
  config.dir = kDir;
  config.base_url = kBase;
  config.manifest_path = kManifestPath;

  const std::string frame = cargo_test::ZstdFrameOf(s.text);
  std::printf("cargo_scale_test: manifest text %zu bytes, published frame %zu bytes (%.1f%%)\n", s.text.size(), frame.size(), 100.0 * static_cast<double>(frame.size()) / static_cast<double>(s.text.size()));
  FakeEnv env;
  env.server[std::string(kBase) + kManifestPath] = Response{200, frame, std::string::npos, ""};
  for (std::size_t j = 0; j < kObjects; ++j) {
    env.server[std::string(kBase) + "blobs/sha256/" + s.shas[j]] = Response{200, s.frames[j], std::string::npos, ""};
  }

  // First launch.
  std::unique_ptr<nc::Mounted> first;
  const std::uint64_t t2 = NowNs();
  const nc::RunResult r = nc::FetchAndMount(env, config, [&](std::unique_ptr<nc::Mounted> m) { first = std::move(m); },
                                            [] { return true; });
  const std::uint64_t t3 = NowNs();
  std::printf("cargo_scale_test: first launch (parse, plan, %zu downloads, hash, append, save, mount) %llu ms\n", r.fetched,
              static_cast<unsigned long long>((t3 - t2) / 1000000));
  QCHECK(r.manifest_loaded && r.published && r.failed == 0 && r.refused == 0 && std::string(r.stopped).empty());
  QCHECK(r.fetched == kObjects && r.cached == 0);
  QCHECK(first != nullptr);
  if (first == nullptr) return;
  QCHECK(first->records == kRecords && first->empty_records == kEmpty && first->shadow->record_count() == kRecords);
  QCHECK(first->shadow->chunk_count() == kObjects);  // one chunk for each distinct object, however many names share it
  QCHECK(first->files == 1 && env.open_blobs() == 1);  // one open file serves seventy thousand records
  QCHECK(first->missing == 0 && first->refused == 0);

  // The log stayed readable: per-object lines are Debug, the rest is a handful.
  std::size_t loud = 0;
  for (const std::string& e : env.events) {
    if (e.find("\"level\":\"debug\"") == std::string::npos) ++loud;
  }
  std::printf("cargo_scale_test: %zu events, %zu above Debug\n", env.events.size(), loud);
  QCHECK(loud < 40);

  // Every record resolves like the engine reads it, to the stored bytes.
  std::uint64_t file_len = env.fs_->files[nc::StoreFilePath(kDir, 0)].size();
  std::size_t resolved = 0, read_back = 0;
  std::int64_t handle = -1;
  for (std::size_t i = 0; i < kEntries; ++i) {
    const std::size_t j = s.object_of_entry[i];
    const Type& t = kTypes[i % kTypeCount];
    const std::uint64_t name = 0x8120755c6da7eb41ull + 0x9e3779b97f4a7c15ull * i;
    nevr_shadow::Found f;
    if (!nevr_shadow::ReadLikeTheEngine(first->shadow->manager(), t.value, name, &f)) continue;
    ++resolved;
    QCHECK(f.size == s.bytes[j] && f.decompressed == s.bytes[j] && f.compressed == s.frames[j].size());
    QCHECK(f.offset_in_chunk == 0 && f.alignment == 16 && f.package == 0);
    QCHECK(f.file_offset + static_cast<std::uint64_t>(f.compressed) <= file_len);
    if (handle < 0) handle = f.handle;
    QCHECK(f.handle == handle);
    if (i % 5 == 0) {  // a fifth of them read back whole: the handle the engine is given reads the stored frame
      std::string got(f.compressed, '\0');
      std::size_t n = 0;
      QCHECK(env.ReadBlob(f.handle, f.file_offset, &got[0], got.size(), &n) && n == got.size() && got == s.frames[j]);
      ++read_back;
    }
  }
  QCHECK(resolved == kEntries);
  std::printf("cargo_scale_test: %zu entries resolved, %zu read back through the handle\n", resolved, read_back);
  // The archive the engine asks for first is among them.
  nevr_shadow::Found archive;
  QCHECK(nevr_shadow::ReadLikeTheEngine(first->shadow->manager(), 0x2a41cf1c1d9e5d32ULL, 0x8120755c6da7eb41ull, &archive));
  // And the records of 0 bytes: size 0, a chunk that exists.
  std::size_t zero_ok = 0;
  for (std::size_t i = 0; i < kEmpty; ++i) {
    const Type& t = kTypes[(i * 3) % kTypeCount];
    nevr_shadow::Found f;
    if (nevr_shadow::ReadLikeTheEngine(first->shadow->manager(), t.value, 0x1111111111111111ull + 0xabcdef12345ull * i, &f) && f.size == 0 &&
        f.chunk >= first->shadow->chunk_base() && f.chunk < first->shadow->chunk_base() + first->shadow->chunk_count() && f.alignment == 4) {
      ++zero_ok;
    }
  }
  QCHECK(zero_ok == kEmpty);

  // Second launch: a new process, the same files, no server. It reads the manifest it saved and the index, requests nothing and
  // reads no object.
  FakeEnv boot(env.fs_);
  std::unique_ptr<nc::Mounted> second;
  const std::uint64_t t4 = NowNs();
  const bool mounted = nc::MountFromCache(boot, config, [&](std::unique_ptr<nc::Mounted> m) { second = std::move(m); });
  const std::uint64_t t5 = NowNs();
  std::printf("cargo_scale_test: boot mount from the saved manifest and the index: %llu ms\n", static_cast<unsigned long long>((t5 - t4) / 1000000));
  QCHECK(mounted && second != nullptr);
  QCHECK(boot.requests.empty() && boot.blob_bytes_read == 0);
  if (second != nullptr) {
    QCHECK(second->records == kRecords && second->shadow->record_count() == kRecords && second->files == 1);
    nevr_shadow::Found a;
    QCHECK(nevr_shadow::ReadLikeTheEngine(second->shadow->manager(), 0x2a41cf1c1d9e5d32ULL, 0x8120755c6da7eb41ull, &a));
    QCHECK(a.file_offset == archive.file_offset && a.compressed == archive.compressed);
  }

  // Where the boot mount's time goes, for the report: decoding and parsing the saved manifest, then building the shadow.
  {
    nc::DeviceFacts f;
    boot.QueryDevice(kDir, &f);
    const nc::Limits lim = nc::ComputeLimits(f, nc::ManifestEntryMemoryBytes());
    nc::StoreOpenInfo oi;
    const std::uint64_t a = NowNs();
    std::unique_ptr<nc::Store> st = nc::Store::Open(boot, kDir, lim, &oi);
    const std::uint64_t b = NowNs();
    nc::Manifest mf;
    const nc::ParseResult pr = nc::ParseManifestFrame(boot.fs_->files[nc::ManifestCopyPath(kDir)], lim, &mf);
    const std::uint64_t c = NowNs();
    const nc::Mounted mm = nc::Mount(boot, *st, mf, config.platform, config.policy);
    const std::uint64_t d = NowNs();
    QCHECK(pr.status == nc::ParseStatus::kOk && mm.records == kRecords);
    std::printf("cargo_scale_test: boot mount parts: store open %llu ms, decode+parse %llu ms, mount %llu ms\n", static_cast<unsigned long long>((b - a) / 1000000),
                static_cast<unsigned long long>((c - b) / 1000000), static_cast<unsigned long long>((d - c) / 1000000));
  }

  // The store verifies whole when somebody asks (never at boot).
  nc::DeviceFacts facts;
  boot.QueryDevice(kDir, &facts);
  nc::StoreOpenInfo info;
  std::unique_ptr<nc::Store> store = nc::Store::Open(boot, kDir, nc::ComputeLimits(facts, nc::ManifestEntryMemoryBytes()), &info);
  QCHECK(store != nullptr && info.blobs == kObjects && info.files == 1 && info.dropped == 0);
  std::size_t checked = 0;
  QCHECK(store != nullptr && store->Verify(&checked) == 0 && checked == kObjects);

  // A second fetch of the same manifest makes one request and stores nothing.
  FakeEnv again(env.fs_);
  again.server[std::string(kBase) + kManifestPath] = Response{200, frame, std::string::npos, ""};
  const nc::RunResult r2 = nc::FetchAndMount(again, config, [](std::unique_ptr<nc::Mounted>) {}, [] { return true; });
  QCHECK(r2.fetched == 0 && r2.cached == kObjects && again.requests.size() == 1 && again.blob_bytes_read == 0);
}

}  // namespace

int main() {
  AWholeGameOfSeventyThousandRecordsPlansStoresMountsAndResolves();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "cargo_scale_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("cargo_scale_test: all checks passed\n");
  return 0;
}
