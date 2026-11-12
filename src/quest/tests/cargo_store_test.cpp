// Host test for the cargo cache (src/core/cargo_store.*; docs/adr/0045-cdn-texture-delivery.md D3): the index v2 file, what a damaged
// index is, and the mount, which opens and hashes every blob again through the handle it keeps and mounts a name's buffers together
// or not at all. The machine is cargo_fake_env.h's.
//
// Built and run by `just test-quest-data`.

#include <cstdint>
#include <cstdio>
#include <string>

#include <nlohmann/json.hpp>

#include "core/cargo_store.h"
#include "quest/tests/cargo_fake_env.h"
#include "quest/tests/test_check.h"

namespace {

namespace nc = nevr_cargo;
using cargo_test::FakeEnv;
using cargo_test::Sha256Of;

constexpr const char* kDir = "/cache";
constexpr std::uint64_t kDescType = 0xe2efe7289d5985b8ULL, kPixType = 0x489bb35d53ca50e9ULL, kPoster = 0x3fa6d25bc24a7b01ULL;

std::string Bytes(std::size_t n, char seed) {
  std::string s(n, '\0');
  for (std::size_t i = 0; i < n; ++i) s[i] = static_cast<char>(seed + static_cast<char>(i % 31));
  return s;
}

nc::IndexEntry Entry(std::uint64_t type, std::uint64_t name, const std::string& blob, std::uint64_t bytes, const char* label = "lobby_posters") {
  nc::IndexEntry e;
  e.type = type;
  e.name = name;
  e.sha256 = Sha256Of(blob);
  e.label = label;
  e.format = "zstd-frame";
  e.bytes = bytes;
  e.align = 16;
  return e;
}

// Puts both blobs of the poster on disk and returns the index that names them.
nc::Index PosterOnDisk(FakeEnv& env, const std::string& desc, const std::string& pix) {
  env.fs_->files[nc::BlobPath(kDir, Sha256Of(desc))] = desc;
  env.fs_->files[nc::BlobPath(kDir, Sha256Of(pix))] = pix;
  nc::Index index;
  index.entries = {Entry(kDescType, kPoster, desc, 248), Entry(kPixType, kPoster, pix, 8388608)};
  return index;
}

void TheIndexRoundTripsAndIsPlainJson() {
  FakeEnv env;
  nc::Index in;
  in.entries = {Entry(kPixType, kPoster, "b", 100), Entry(kDescType, kPoster, "a", 248), Entry(kDescType, 7, "c", 12, "aaa")};
  QCHECK(nc::WriteIndex(env, kDir, in));
  nc::Index out;
  QCHECK(nc::ReadIndex(env, kDir, &out) == nc::IndexStatus::kOk);
  nc::Normalize(&in);
  QCHECK(out == in);
  QCHECK(out.entries.size() == 3 && out.entries[0].label == "aaa");  // sorted by label, then type, then name
  const nlohmann::json doc = nlohmann::json::parse(env.fs_->files[nc::IndexPath(kDir)], nullptr, false);
  QCHECK(!doc.is_discarded() && doc["version"] == 2 && doc["entries"].size() == 3);
  QCHECK(doc["entries"][1]["type"] == "0x489bb35d53ca50e9" && doc["entries"][1]["name"] == "0x3fa6d25bc24a7b01");  // by label, then type as unsigned
}

void AnIndexThatIsNotExactlyTheShapeIsInvalidWhole() {
  FakeEnv env;
  nc::Index out;
  QCHECK(nc::ReadIndex(env, kDir, &out) == nc::IndexStatus::kAbsent);
  const std::string good = [&] {
    nc::Index in;
    in.entries = {Entry(kPixType, kPoster, "b", 100)};
    nc::WriteIndex(env, kDir, in);
    return env.fs_->files[nc::IndexPath(kDir)];
  }();
  const auto invalid = [&](const std::string& text) {
    env.fs_->files[nc::IndexPath(kDir)] = text;
    return nc::ReadIndex(env, kDir, &out) == nc::IndexStatus::kInvalid && out.entries.empty();
  };
  QCHECK(invalid("not json"));
  QCHECK(invalid("[]"));
  QCHECK(invalid("{\"version\":1,\"entries\":[]}"));  // v1 is the callback path's index
  QCHECK(invalid("{\"entries\":[]}"));
  QCHECK(invalid("{\"version\":2}"));
  nlohmann::json doc = nlohmann::json::parse(good);
  for (const char* key : {"type", "name", "sha256", "label", "format", "bytes", "align"}) {
    nlohmann::json bad = doc;
    bad["entries"][0].erase(key);
    QCHECK(invalid(bad.dump()));
  }
  nlohmann::json bad = doc;
  bad["entries"][0]["sha256"] = std::string(64, 'Z');
  QCHECK(invalid(bad.dump()));
  bad = doc;
  bad["entries"][0]["format"] = "raw";
  QCHECK(invalid(bad.dump()));
  bad = doc;
  bad["entries"][0]["bytes"] = 0;
  QCHECK(invalid(bad.dump()));
  bad = doc;
  bad["entries"][0]["label"] = "Bad Label";
  QCHECK(invalid(bad.dump()));
  bad = doc;
  bad["entries"].push_back(bad["entries"][0]);  // a duplicate (label, type, name)
  QCHECK(invalid(bad.dump()));
  env.fs_->files[nc::IndexPath(kDir)] = good;
  QCHECK(nc::ReadIndex(env, kDir, &out) == nc::IndexStatus::kOk);
  env.fs_->files[nc::IndexPath(kDir)] = good + std::string(nc::kMaxIndexBytes, ' ');
  QCHECK(nc::ReadIndex(env, kDir, &out) == nc::IndexStatus::kAbsent);  // over the cap: not read
}

void HashFileIsTheSha256OfTheFile() {
  FakeEnv env;
  const std::string big = Bytes(200000, 'k');  // more than one read chunk
  env.fs_->files["/cache/x"] = big;
  QCHECK(nc::HashFile(env, "/cache/x") == Sha256Of(big));
  QCHECK(nc::HashFile(env, "/cache/missing").empty());
  QCHECK(env.open_blobs() == 0);
}

void AMountOpensEveryBlobAndBuildsTheShadow() {
  nevr_shadow::Shadow::ResetChunkCounterForTest();
  FakeEnv env;
  const std::string desc = Bytes(40, 'd'), pix = Bytes(2000, 'p');
  const nc::Index index = PosterOnDisk(env, desc, pix);
  const nc::Mounted m = nc::Mount(env, kDir, index, nc::Policy());
  QCHECK(m.shadow != nullptr);
  if (!m.shadow) return;
  QCHECK(m.names == 1 && m.records == 2 && m.refused == 0 && m.labels.size() == 1 && m.labels[0] == "lobby_posters");
  QCHECK(m.shadow->chunk_base() == nevr_shadow::kFirstChunkIndex);
  nevr_shadow::Found f;
  std::uint32_t user = 99;
  QCHECK(m.shadow->Find(kDescType, kPoster, &f, &user));
  QCHECK(f.chunk >= nevr_shadow::kFirstChunkIndex && f.file_offset == 0 && f.compressed == 40 && f.decompressed == 248);
  QCHECK(f.offset_in_chunk == 0 && f.size == 248 && f.alignment == 16 && user == 0);
  QCHECK(env.PathOfHandle(f.handle) == nc::BlobPath(kDir, Sha256Of(desc)));
  QCHECK(m.shadow->Find(kPixType, kPoster, &f));
  QCHECK(f.compressed == 2000 && f.decompressed == 8388608 && f.size == 8388608);
  QCHECK(env.PathOfHandle(f.handle) == nc::BlobPath(kDir, Sha256Of(pix)));
  QCHECK(env.open_blobs() == 2);  // held for the life of the process
}

void AFlippedByteIsRefusedAtMountAndNothingOfTheNameMounts() {
  FakeEnv env;
  const std::string desc = Bytes(40, 'd'), pix = Bytes(2000, 'p');
  const nc::Index index = PosterOnDisk(env, desc, pix);
  std::string& blob = env.fs_->files[nc::BlobPath(kDir, Sha256Of(pix))];
  blob[1000] = static_cast<char>(blob[1000] ^ 0x01);
  const nc::Mounted m = nc::Mount(env, kDir, index, nc::Policy());
  QCHECK(m.shadow == nullptr);  // the descriptor is intact, but the name mounts whole or not at all
  QCHECK(m.names == 0 && m.records == 0 && m.refused == 1);
  QCHECK(env.open_blobs() == 0);  // the handles opened for the refused name are closed
  const auto lines = env.EventsNamed("cargo_mount");
  QCHECK(lines.size() == 1 && FakeEnv::Value(lines[0], "reason") == "sha256_mismatch" && FakeEnv::Value(lines[0], "label") == "lobby_posters");
  QCHECK(FakeEnv::Value(lines[0], "level") == "<absent>" || lines[0].find("\"level\":\"warn\"") != std::string::npos);
}

void ADamagedDescriptorTakesItsPayloadWithIt() {
  FakeEnv env;
  const std::string desc = Bytes(40, 'd'), pix = Bytes(2000, 'p');
  const nc::Index index = PosterOnDisk(env, desc, pix);
  env.fs_->files[nc::BlobPath(kDir, Sha256Of(desc))] += "x";
  const nc::Mounted m = nc::Mount(env, kDir, index, nc::Policy());
  QCHECK(m.shadow == nullptr && m.refused == 1);
  nevr_shadow::Found f;
  QCHECK(m.shadow == nullptr || !m.shadow->Find(kPixType, kPoster, &f));
}

void OneBadNameDoesNotTakeAnotherNameDown() {
  FakeEnv env;
  const std::string desc = Bytes(40, 'd'), pix = Bytes(2000, 'p'), other = Bytes(500, 'o');
  nc::Index index = PosterOnDisk(env, desc, pix);
  env.fs_->files[nc::BlobPath(kDir, Sha256Of(other))] = other;
  index.entries.push_back(Entry(kPixType, 0x77, other, 300));
  env.fs_->files[nc::BlobPath(kDir, Sha256Of(pix))][5] ^= 1;  // the poster is bad, name 0x77 is fine
  const nc::Mounted m = nc::Mount(env, kDir, index, nc::Policy());
  QCHECK(m.shadow != nullptr && m.names == 1 && m.records == 1 && m.refused == 1);
  nevr_shadow::Found f;
  QCHECK(m.shadow && m.shadow->Find(kPixType, 0x77, &f) && !m.shadow->Find(kPixType, kPoster, &f));
}

void AMissingBlobIsRefusedByName() {
  FakeEnv env;
  const std::string desc = Bytes(40, 'd'), pix = Bytes(2000, 'p');
  const nc::Index index = PosterOnDisk(env, desc, pix);
  env.fs_->files.erase(nc::BlobPath(kDir, Sha256Of(desc)));
  const nc::Mounted m = nc::Mount(env, kDir, index, nc::Policy());
  QCHECK(m.shadow == nullptr && m.refused == 1);
  const auto lines = env.EventsNamed("cargo_mount");
  QCHECK(lines.size() == 1 && FakeEnv::Value(lines[0], "reason") == "open_failed");
}

void ALabelThatIsOffIsNotMountedAndNotRefused() {
  FakeEnv env;
  const nc::Index index = PosterOnDisk(env, Bytes(40, 'd'), Bytes(2000, 'p'));
  nc::Policy off;
  off.overrides["lobby_posters"] = nc::LabelState::kOff;
  const nc::Mounted m = nc::Mount(env, kDir, index, off);
  QCHECK(m.shadow == nullptr && m.refused == 0 && m.records == 0 && env.open_blobs() == 0);
  QCHECK(env.requests.empty());
}

void AnEmptyBlobIsRefused() {
  FakeEnv env;
  env.fs_->files[nc::BlobPath(kDir, Sha256Of(""))] = "";
  nc::Index index;
  index.entries = {Entry(kPixType, 5, "", 100)};
  const nc::Mounted m = nc::Mount(env, kDir, index, nc::Policy());
  QCHECK(m.shadow == nullptr && m.refused == 1 && env.open_blobs() == 0);
  const auto lines = env.EventsNamed("cargo_mount");
  QCHECK(lines.size() == 1 && FakeEnv::Value(lines[0], "reason") == "size");
}

void TwoNamesThatShareABlobBothMount() {
  FakeEnv env;
  const std::string blob = Bytes(300, 's');
  env.fs_->files[nc::BlobPath(kDir, Sha256Of(blob))] = blob;
  nc::Index index;
  index.entries = {Entry(kPixType, 1, blob, 4096), Entry(kPixType, 2, blob, 4096)};
  const nc::Mounted m = nc::Mount(env, kDir, index, nc::Policy());
  QCHECK(m.shadow != nullptr && m.records == 2 && m.names == 2);
  nevr_shadow::Found a, b;
  QCHECK(m.shadow && m.shadow->Find(kPixType, 1, &a) && m.shadow->Find(kPixType, 2, &b));
  QCHECK(a.chunk != b.chunk);  // one chunk each, so the block cache keys differ
}

void TwoLabelsKeepTheirOwnTags() {
  FakeEnv env;
  const std::string a = Bytes(30, 'a'), b = Bytes(31, 'b');
  env.fs_->files[nc::BlobPath(kDir, Sha256Of(a))] = a;
  env.fs_->files[nc::BlobPath(kDir, Sha256Of(b))] = b;
  nc::Index index;
  index.entries = {Entry(kPixType, 1, a, 100, "lobby_posters"), Entry(kPixType, 2, b, 100, "other_label")};
  nc::Policy p;
  p.overrides["other_label"] = nc::LabelState::kOn;
  const nc::Mounted m = nc::Mount(env, kDir, index, p);
  QCHECK(m.shadow != nullptr && m.labels.size() == 2);
  std::uint32_t u1 = 9, u2 = 9;
  nevr_shadow::Found f;
  QCHECK(m.shadow && m.shadow->Find(kPixType, 1, &f, &u1) && m.shadow->Find(kPixType, 2, &f, &u2));
  QCHECK(u1 < m.labels.size() && u2 < m.labels.size() && m.labels[u1] == "lobby_posters" && m.labels[u2] == "other_label");
}

void TheStatusNamesAreDistinct() {
  QCHECK(std::string(nc::IndexStatusName(nc::IndexStatus::kOk)) != nc::IndexStatusName(nc::IndexStatus::kInvalid));
  QCHECK(nc::FormatHex64(0) == "0x0" && nc::FormatHex64(0x3fa6d25bc24a7b01ULL) == "0x3fa6d25bc24a7b01");
  QCHECK(nc::BlobPath("/d", std::string(64, 'a')) == "/d/" + std::string(64, 'a') + ".blob");
}

// Evict: the blobs of /cache, a: oldest, b, c newest, by write order; `named` is the set the index names.
struct EvictRig {
  FakeEnv env;
  std::string a = Bytes(100, 'a'), b = Bytes(100, 'b'), c = Bytes(100, 'c');
  EvictRig() {
    for (const std::string* blob : {&a, &b, &c}) {
      env.fs_->files[nc::BlobPath(kDir, Sha256Of(*blob))] = *blob;
      env.fs_->modified[nc::BlobPath(kDir, Sha256Of(*blob))] = ++env.fs_->write_tick;
    }
  }
  bool Has(const std::string& blob) const { return env.fs_->files.count(nc::BlobPath(kDir, Sha256Of(blob))) != 0; }
};

void EvictRemovesUnreferencedBlobsOldestFirstUntilTheyFit() {
  EvictRig r;
  const nc::EvictReport rep = nc::Evict(r.env, kDir, 200, {});
  QCHECK(rep.bytes_before == 300 && rep.bytes_after == 200 && rep.evicted == 1 && rep.failed == 0 && !rep.over_cap_all_referenced);
  QCHECK(!r.Has(r.a) && r.Has(r.b) && r.Has(r.c));  // the oldest went, and only as many as needed
  const nc::EvictReport again = nc::Evict(r.env, kDir, 200, {});
  QCHECK(again.evicted == 0 && again.bytes_after == 200);
}

void EvictNeverRemovesABlobTheIndexNames() {
  EvictRig r;
  const nc::EvictReport rep = nc::Evict(r.env, kDir, 100, {Sha256Of(r.a)});  // the oldest is the one in use
  QCHECK(rep.evicted == 2 && r.Has(r.a) && !r.Has(r.b) && !r.Has(r.c));
  QCHECK(rep.bytes_after == 100 && !rep.over_cap_all_referenced);
  const nc::EvictReport all = nc::Evict(r.env, kDir, 10, {Sha256Of(r.a)});  // only the named one is left, and it is over the cap
  QCHECK(all.evicted == 0 && r.Has(r.a) && all.over_cap_all_referenced);
}

void EvictUnderTheCapRemovesNothingAndTouchesOnlyBlobFiles() {
  EvictRig r;
  r.env.fs_->files["/cache/index.json"] = std::string(1000, 'x');
  r.env.fs_->files["/cache/notablob.blob"] = std::string(1000, 'y');
  r.env.fs_->files["/cache/" + std::string(64, 'A') + ".blob"] = std::string(1000, 'z');  // upper case is not a key
  const nc::EvictReport rep = nc::Evict(r.env, kDir, 300, {});  // a, b, c are the only blobs counted: 300 bytes fit a 300 cap
  QCHECK(rep.blobs_before == 3 && rep.bytes_before == 300 && rep.evicted == 0 && r.Has(r.a) && r.Has(r.b) && r.Has(r.c));
  QCHECK(nc::Evict(r.env, kDir, 0, {}).evicted == 3);  // a cap of nothing takes every blob, and still only blobs
  QCHECK(r.env.fs_->files.count("/cache/index.json") == 1 && r.env.fs_->files.count("/cache/notablob.blob") == 1);
  QCHECK(r.env.fs_->files.count("/cache/" + std::string(64, 'A') + ".blob") == 1);
}

void EvictCountsAFailedRemovalAndDoesNotClaimTheCapIsMet() {
  EvictRig r;
  r.env.remove_fails_for_blobs = 1;  // the oldest blob cannot be removed
  const nc::EvictReport rep = nc::Evict(r.env, kDir, 50, {});
  QCHECK(rep.failed == 1 && rep.evicted == 2 && rep.bytes_after == 100 && r.Has(r.a));
  QCHECK(!rep.over_cap_all_referenced);  // over the cap because a removal failed, not because the index names the rest
}

}  // namespace

int main() {
  TheIndexRoundTripsAndIsPlainJson();
  AnIndexThatIsNotExactlyTheShapeIsInvalidWhole();
  HashFileIsTheSha256OfTheFile();
  AMountOpensEveryBlobAndBuildsTheShadow();
  AFlippedByteIsRefusedAtMountAndNothingOfTheNameMounts();
  ADamagedDescriptorTakesItsPayloadWithIt();
  OneBadNameDoesNotTakeAnotherNameDown();
  AMissingBlobIsRefusedByName();
  AnEmptyBlobIsRefused();
  ALabelThatIsOffIsNotMountedAndNotRefused();
  TwoNamesThatShareABlobBothMount();
  TwoLabelsKeepTheirOwnTags();
  TheStatusNamesAreDistinct();
  EvictRemovesUnreferencedBlobsOldestFirstUntilTheyFit();
  EvictNeverRemovesABlobTheIndexNames();
  EvictUnderTheCapRemovesNothingAndTouchesOnlyBlobFiles();
  EvictCountsAFailedRemovalAndDoesNotClaimTheCapIsMet();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "cargo_store_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("cargo_store_test: all checks passed\n");
  return 0;
}
