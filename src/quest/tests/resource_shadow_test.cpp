// Host test for the shadow manifest builder (src/core/resource_shadow.{h,cpp}; docs/adr/0045-cdn-texture-delivery.md): the
// bytes it builds are the layout the game's own resolver reads (manager +8, records at +0 sorted as signed, the count at +0x30,
// the chunk table at +0x80, the handle table at +0x128), every chunk index it hands out is above the stock manifest's, and each
// refusal is the one the plugin-level and cargo designs rely on. The reference reading is ReadLikeTheEngine plus this file's own
// pointer arithmetic, which does what the game's code does with the table: `table + chunk * 0x10`.
//
// Built and run by `just test-quest-data`.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "core/resource_shadow.h"
#include "quest/tests/test_check.h"

namespace {

namespace ns = nevr_shadow;

template <typename T>
T LoadAt(const unsigned char* base, std::size_t at) {
  T v{};
  std::memcpy(&v, base + at, sizeof(v));
  return v;
}

// A type with the high bit set: it is the smallest as a signed compare and the largest as an unsigned one.
constexpr std::uint64_t kHighType = 0xe2efe7289d5985b8ULL;
constexpr std::uint64_t kLowType = 0x489bb35d53ca50e9ULL;

ns::Input ThreeRecords() {
  ns::Input in;
  in.handles = {101, 202};
  in.chunks = {{0, 0, 40, 248}, {1, 0, 2000, 4096}, {1, 64, 90, 300}};
  // Deliberately not in signed order.
  in.records = {{kLowType, 0x3fa6d25bc24a7b01ULL, 1, 0, 4096, 16, 7},
                {kHighType, 0x3fa6d25bc24a7b01ULL, 0, 0, 248, 16, 9},
                {kLowType, 0x0000000000000005ULL, 2, 12, 100, 8, 3}};
  return in;
}

// The counter is process-wide, so each test that reads absolute indices starts it from its first value.
void FreshCounter() { ns::Shadow::ResetChunkCounterForTest(); }

std::unique_ptr<ns::Shadow> BuildOk(const ns::Input& in) {
  std::unique_ptr<ns::Shadow> s;
  const ns::Status status = ns::Shadow::Build(in, &s);
  QCHECK(status == ns::Status::kOk);
  return s;
}

void TheManifestIsTheEnginesLayout() {
  FreshCounter();
  const ns::Input in = ThreeRecords();
  const auto s = BuildOk(in);
  if (!s) return;
  const unsigned char* manager = static_cast<const unsigned char*>(s->manager());
  const unsigned char* manifest = reinterpret_cast<const unsigned char*>(LoadAt<std::uintptr_t>(manager, ns::kManagerManifest));
  QCHECK(manifest == s->manifest_for_test());
  QCHECK(LoadAt<std::uint64_t>(manifest, ns::kManifestCount) == 3);
  const unsigned char* records = reinterpret_cast<const unsigned char*>(LoadAt<std::uintptr_t>(manifest, ns::kManifestRecords));
  QCHECK(records != nullptr);
  // Signed order: the high-bit type first, then the low type's two names ascending.
  QCHECK(LoadAt<std::int64_t>(records, 0 * ns::kRecordBytes) == static_cast<std::int64_t>(kHighType));
  QCHECK(LoadAt<std::int64_t>(records, 1 * ns::kRecordBytes) == static_cast<std::int64_t>(kLowType));
  QCHECK(LoadAt<std::int64_t>(records, 1 * ns::kRecordBytes + 8) == 5);
  QCHECK(LoadAt<std::int64_t>(records, 2 * ns::kRecordBytes) == static_cast<std::int64_t>(kLowType));
  QCHECK(LoadAt<std::int64_t>(records, 2 * ns::kRecordBytes + 8) == 0x3fa6d25bc24a7b01LL);
  // The chunk table pointer is the table's address minus base * 0x10.
  const std::uintptr_t chunkTable = LoadAt<std::uintptr_t>(manifest, ns::kManifestChunks);
  const std::uintptr_t handleTable = LoadAt<std::uintptr_t>(manifest, ns::kManifestHandles);
  QCHECK(handleTable != 0);
  const std::int64_t* handles = reinterpret_cast<const std::int64_t*>(handleTable);
  QCHECK(handles[0] == 101 && handles[1] == 202);
  // The engine's arithmetic, in 64 bits and in the 32-bit signed form a compiler might choose: the same entry.
  for (int r = 0; r < 3; ++r) {
    const std::uint32_t word = LoadAt<std::uint32_t>(records, static_cast<std::size_t>(r) * ns::kRecordBytes + 0x10);
    QCHECK(word >= ns::kFirstChunkIndex);
    const unsigned char* viaU64 = reinterpret_cast<const unsigned char*>(chunkTable + static_cast<std::uint64_t>(word) * 0x10);
    const unsigned char* viaS64 =
        reinterpret_cast<const unsigned char*>(chunkTable + static_cast<std::uint64_t>(static_cast<std::int64_t>(word) * 0x10));
    QCHECK(viaU64 == viaS64);
    QCHECK(static_cast<std::uint64_t>(word) * 0x10 < (std::uint64_t{1} << 31));
  }
}

void TheEngineReadingFindsEveryRecordWithItsChunkAndHandle() {
  FreshCounter();
  const auto s = BuildOk(ThreeRecords());
  if (!s) return;
  ns::Found f;
  QCHECK(ns::ReadLikeTheEngine(s->manager(), kHighType, 0x3fa6d25bc24a7b01ULL, &f));
  QCHECK(f.chunk == ns::kFirstChunkIndex + 0);
  QCHECK(f.package == 0 && f.handle == 101);
  QCHECK(f.file_offset == 0 && f.compressed == 40 && f.decompressed == 248);
  QCHECK(f.offset_in_chunk == 0 && f.size == 248 && f.alignment == 16);
  QCHECK(ns::ReadLikeTheEngine(s->manager(), kLowType, 0x3fa6d25bc24a7b01ULL, &f));
  QCHECK(f.chunk == ns::kFirstChunkIndex + 1 && f.package == 1 && f.handle == 202);
  QCHECK(f.compressed == 2000 && f.decompressed == 4096 && f.size == 4096);
  QCHECK(ns::ReadLikeTheEngine(s->manager(), kLowType, 5, &f));
  QCHECK(f.chunk == ns::kFirstChunkIndex + 2 && f.file_offset == 64 && f.compressed == 90 && f.decompressed == 300);
  QCHECK(f.offset_in_chunk == 12 && f.size == 100 && f.alignment == 8);
}

void FindAgreesWithTheEngineReadingAndGivesTheCallersTag() {
  const auto s = BuildOk(ThreeRecords());
  if (!s) return;
  ns::Found a, b;
  std::uint32_t user = 0;
  QCHECK(s->Find(kLowType, 5, &a, &user));
  QCHECK(ns::ReadLikeTheEngine(s->manager(), kLowType, 5, &b));
  QCHECK(std::memcmp(&a, &b, sizeof(a)) == 0);
  QCHECK(user == 3);
  QCHECK(s->Find(kHighType, 0x3fa6d25bc24a7b01ULL, &a, &user));
  QCHECK(user == 9);
  QCHECK(s->Find(kLowType, 0x3fa6d25bc24a7b01ULL, &a, &user));
  QCHECK(user == 7);
}

void AKeyThatIsNotThereIsAMissOnEveryEdge() {
  const auto s = BuildOk(ThreeRecords());
  if (!s) return;
  ns::Found f;
  QCHECK(!s->Find(kLowType, 6, &f));                    // between two names of one type
  QCHECK(!s->Find(kLowType, 0, &f));                    // below the first name of its type
  QCHECK(!s->Find(kLowType, ~std::uint64_t{0}, &f));    // above the last
  QCHECK(!s->Find(0x1234, 5, &f));                      // a type that is not there
  QCHECK(!ns::ReadLikeTheEngine(s->manager(), kLowType, 6, &f));
  QCHECK(!ns::ReadLikeTheEngine(nullptr, kLowType, 5, &f));
}

void EveryChunkWordIsAboveTheStockManifestsAndUnique() {
  FreshCounter();
  ns::Input in;
  in.handles = {1};
  for (std::uint32_t i = 0; i < 200; ++i) {
    in.chunks.push_back({0, i * 100, 100, 100});
    in.records.push_back({kLowType, 1000u + i, i, 0, 100, 16, i});
  }
  const auto s = BuildOk(in);
  if (!s) return;
  std::vector<std::uint32_t> words;
  for (std::uint32_t i = 0; i < 200; ++i) {
    ns::Found f;
    QCHECK(s->Find(kLowType, 1000u + i, &f));
    QCHECK(f.chunk >= ns::kStockChunkCount);  // stock has 10,304 chunks
    QCHECK(f.chunk == ns::kFirstChunkIndex + i);
    words.push_back(f.chunk);
  }
  for (std::size_t i = 1; i < words.size(); ++i) QCHECK(words[i] != words[i - 1]);
}

void AShadowThatDoesNotFitTheIndexSpaceIsRefusedAndTakesNoIndex() {
  ns::Input in = ThreeRecords();  // three chunks
  std::unique_ptr<ns::Shadow> s;
  ns::Shadow::ResetChunkCounterForTest(static_cast<std::uint32_t>(ns::kChunkIndexLimit) - 2);  // two left: not enough
  QCHECK(ns::Shadow::Build(in, &s) == ns::Status::kIndexSpace && !s);
  QCHECK(ns::Shadow::NextChunkIndex() == ns::kChunkIndexLimit - 2);  // a refusal took none
  ns::Shadow::ResetChunkCounterForTest(static_cast<std::uint32_t>(ns::kChunkIndexLimit) - 3);  // exactly three left
  QCHECK(ns::Shadow::Build(in, &s) == ns::Status::kOk && s);
  QCHECK(s && s->chunk_base() == ns::kChunkIndexLimit - 3);
  QCHECK(ns::Shadow::NextChunkIndex() == ns::kChunkIndexLimit);  // the space is spent
  QCHECK(ns::Shadow::Build(in, &s) == ns::Status::kIndexSpace && !s);
  // The counter does not wrap into an index already given out.
  QCHECK(ns::Shadow::NextChunkIndex() == ns::kChunkIndexLimit);
  QCHECK(std::string(ns::StatusName(ns::Status::kIndexSpace)) == "index_space");
}

// design 8.1 (plugin levels): shadows built in turn, then a republish, never share an index; each takes fresh indices above the last.
void ShadowsBuiltInTurnNeverShareAnIndex() {
  FreshCounter();
  const auto cargo = BuildOk(ThreeRecords());         // the cargo shadow: indices [first, first + 3)
  const auto packages = BuildOk(ThreeRecords());      // the package shadow: the next three
  const auto republish = BuildOk(ThreeRecords());     // a cargo republish after a fetch: three more
  if (!cargo || !packages || !republish) return;
  QCHECK(cargo->chunk_base() == ns::kFirstChunkIndex);
  QCHECK(packages->chunk_base() == cargo->chunk_base() + cargo->chunk_count());
  QCHECK(republish->chunk_base() == packages->chunk_base() + packages->chunk_count());
  QCHECK(ns::Shadow::NextChunkIndex() == republish->chunk_base() + republish->chunk_count());
  std::set<std::uint32_t> seen;
  for (const ns::Shadow* shadow : {cargo.get(), packages.get(), republish.get()}) {
    for (std::uint64_t name : {0x3fa6d25bc24a7b01ULL}) {
      ns::Found f;
      QCHECK(shadow->Find(kHighType, name, &f));
      QCHECK(seen.insert(f.chunk).second);  // no chunk word repeats across shadows
    }
    ns::Found g;
    QCHECK(shadow->Find(kLowType, 5, &g) && seen.insert(g.chunk).second);
    QCHECK(shadow->Find(kLowType, 0x3fa6d25bc24a7b01ULL, &g) && seen.insert(g.chunk).second);
  }
  QCHECK(seen.size() == 9);
  // And each still reads its own handle: the second shadow's table is not the first's.
  ns::Found a, b;
  QCHECK(cargo->Find(kLowType, 5, &a) && republish->Find(kLowType, 5, &b) && a.chunk != b.chunk && a.handle == b.handle);
}

void AFailedBuildBetweenTwoGoodOnesLeavesNoGap() {
  FreshCounter();
  const auto first = BuildOk(ThreeRecords());
  ns::Input bad = ThreeRecords();
  bad.records[0].chunk = 9;  // refused: a chunk that is not there
  std::unique_ptr<ns::Shadow> none;
  QCHECK(ns::Shadow::Build(bad, &none) == ns::Status::kChunkIndex && !none);
  const auto second = BuildOk(ThreeRecords());
  QCHECK(first && second && second->chunk_base() == first->chunk_base() + first->chunk_count());
}

void EachRefusalIsNamedAndLeavesNothing() {
  std::unique_ptr<ns::Shadow> s;
  ns::Input in;
  QCHECK(ns::Shadow::Build(in, &s) == ns::Status::kEmpty && !s);

  in = ThreeRecords();
  in.records[0].chunk = 3;  // there are chunks 0..2
  QCHECK(ns::Shadow::Build(in, &s) == ns::Status::kChunkIndex && !s);

  in = ThreeRecords();
  in.chunks[1].package = 2;  // there are handles 0..1
  QCHECK(ns::Shadow::Build(in, &s) == ns::Status::kPackageIndex && !s);

  in = ThreeRecords();
  in.records[2] = in.records[0];  // the same (type, name) twice
  in.records[2].user = 99;
  QCHECK(ns::Shadow::Build(in, &s) == ns::Status::kDuplicate && !s);
}

void TheStatusNamesAreDistinct() {
  const ns::Status all[] = {ns::Status::kOk,         ns::Status::kEmpty,        ns::Status::kIndexSpace,
                            ns::Status::kChunkIndex, ns::Status::kPackageIndex, ns::Status::kDuplicate};
  for (std::size_t i = 0; i < sizeof(all) / sizeof(all[0]); ++i) {
    for (std::size_t j = i + 1; j < sizeof(all) / sizeof(all[0]); ++j) {
      QCHECK(std::strcmp(ns::StatusName(all[i]), ns::StatusName(all[j])) != 0);
    }
  }
}

void TwoShadowsDoNotShareStorage() {
  const auto a = BuildOk(ThreeRecords());
  ns::Input other = ThreeRecords();
  other.handles = {7, 8};
  const auto b = BuildOk(other);
  if (!a || !b) return;
  ns::Found fa, fb;
  QCHECK(a->Find(kLowType, 5, &fa) && b->Find(kLowType, 5, &fb));
  QCHECK(fa.handle == 202 && fb.handle == 8);
  QCHECK(a->manager() != b->manager());
}

}  // namespace

int main() {
  TheManifestIsTheEnginesLayout();
  TheEngineReadingFindsEveryRecordWithItsChunkAndHandle();
  FindAgreesWithTheEngineReadingAndGivesTheCallersTag();
  AKeyThatIsNotThereIsAMissOnEveryEdge();
  EveryChunkWordIsAboveTheStockManifestsAndUnique();
  AShadowThatDoesNotFitTheIndexSpaceIsRefusedAndTakesNoIndex();
  ShadowsBuiltInTurnNeverShareAnIndex();
  AFailedBuildBetweenTwoGoodOnesLeavesNoGap();
  EachRefusalIsNamedAndLeavesNothing();
  TheStatusNamesAreDistinct();
  TwoShadowsDoNotShareStorage();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "resource_shadow_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("resource_shadow_test: all checks passed\n");
  return 0;
}
