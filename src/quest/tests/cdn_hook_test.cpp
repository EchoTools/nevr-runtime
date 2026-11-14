// Host test for the CDN decision in the resource request hook (src/quest/sentinel/resource_request_hook.cpp;
// docs/adr/0045-cdn-texture-delivery.md D1 and T1): the real handler through the thunk's entry, against a stand-in for
// FillResourceFileRequest that reads the manager it is given the way the game does (nevr_shadow::ReadLikeTheEngine) and fills the
// request from the record, the chunk and the handle. The live manager is itself a shadow of "stock" keys, so the stand-in is the same
// code for both.
//
// Built and run by `just test-quest-hooks`.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "cdn_read_keys.h"
#include "cdn/cdn_store.h"
#include "core/resource_shadow.h"
#include "hook_log.h"
#include "quest/tests/test_check.h"
#include "resource_request_hook.h"

namespace {

namespace rr = nevr_quest::resource_request;
namespace layout = sentinel::pinned::resource_request_layout;
using sentinel::pinned::CResourceManagerOpaque;

constexpr std::uint64_t kDescType = 0xe2efe7289d5985b8ULL, kPixType = 0x489bb35d53ca50e9ULL, kPoster = 0x3fa6d25bc24a7b01ULL;
constexpr std::uint64_t kStockOnly = 0x1234, kBoth = 0x5678, kCdnOnly = 0x7777;
constexpr std::uint64_t kMiss = 0xdead0000beefULL;
constexpr std::int64_t kStockHandle = 7, kCdnDescHandle = 41, kCdnPixHandle = 42;

std::mutex g_logMutex;
std::vector<std::string> g_lines;
void Sink(sentinel::LogLevel, const char* line) {
  std::lock_guard<std::mutex> lock(g_logMutex);
  g_lines.emplace_back(line);
}
std::vector<std::string> Lines(const char* event) {
  const std::string needle = std::string("\"event\":\"") + event + "\"";
  std::vector<std::string> out;
  std::lock_guard<std::mutex> lock(g_logMutex);
  for (const std::string& l : g_lines) {
    if (l.find(needle) != std::string::npos) out.push_back(l);
  }
  return out;
}
std::string Field(const std::string& line, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const std::size_t at = line.find(needle);
  if (at == std::string::npos) return "<absent>";
  const std::size_t begin = at + needle.size();
  if (line[begin] == '"') return line.substr(begin + 1, line.find('"', begin + 1) - begin - 1);
  return line.substr(begin, line.find_first_of(",}", begin) - begin);
}

template <typename T>
T Get(const unsigned char* request, std::size_t at) {
  T v{};
  std::memcpy(&v, request + at, sizeof(v));
  return v;
}
template <typename T>
void Put(unsigned char* request, std::size_t at, T value) {
  std::memcpy(request + at, &value, sizeof(value));
}

struct Fill {
  int calls = 0;
  const void* manager = nullptr;
  std::uint64_t index = 0;
  bool failOnCdn = false;
  const void* cdnManager = nullptr;
};
Fill g_fill;

// FillResourceFileRequest, reading whatever manager it is given as the game does.
std::uint64_t FakeFill(CResourceManagerOpaque* manager, std::uint64_t type, std::uint64_t name, std::uint64_t, void*, void*,
                       std::uint64_t index, unsigned char* request) {
  ++g_fill.calls;
  g_fill.manager = manager;
  g_fill.index = index;
  if (request != nullptr) std::memset(request, 0, layout::kRequestBytes);
  nevr_shadow::Found f;
  if (!nevr_shadow::ReadLikeTheEngine(manager, type, name, &f)) return kMiss;
  if (g_fill.failOnCdn && manager == g_fill.cdnManager) return 1;
  if (request != nullptr) {
    Put<std::int64_t>(request, layout::kPackageHandle, f.handle);
    Put<std::int64_t>(request, layout::kChunkIndex, f.chunk);
    Put<std::uint64_t>(request, layout::kChunkFileOffset, f.file_offset);
    Put<std::uint64_t>(request, layout::kChunkCompressed, f.compressed);
    Put<std::uint64_t>(request, layout::kChunkDecompressed, f.decompressed);
    Put<std::uint64_t>(request, layout::kOffsetInChunk, f.offset_in_chunk);
    Put<std::uint64_t>(request, layout::kSize, f.size);
    Put<std::uint64_t>(request, layout::kAlignment, f.alignment);
  }
  return 0;
}

// The "stock" manager: a shadow with a few keys, chunk numbers well inside the stock range for the test's purposes.
std::unique_ptr<nevr_shadow::Shadow> g_stock;
std::unique_ptr<nevr_cdn::Mounted> g_cdnSet;
std::unique_ptr<nevr_cdn::Mounted> g_cdnSet2;

std::unique_ptr<nevr_shadow::Shadow> BuildShadow(const std::vector<nevr_shadow::Record>& records, const std::vector<nevr_shadow::Chunk>& chunks,
                                                 const std::vector<std::int64_t>& handles) {
  nevr_shadow::Input in;
  in.records = records;
  in.chunks = chunks;
  in.handles = handles;
  std::unique_ptr<nevr_shadow::Shadow> s;
  QCHECK(nevr_shadow::Shadow::Build(in, &s) == nevr_shadow::Status::kOk);
  return s;
}

std::unique_ptr<nevr_cdn::Mounted> PosterSet(std::int64_t descHandle, std::int64_t pixHandle, bool withBoth = true) {
  std::vector<nevr_shadow::Record> records = {{kDescType, kPoster, 0, 0, 248, 16, 0}, {kPixType, kPoster, 1, 0, 8388608, 16, 0}};
  std::vector<nevr_shadow::Chunk> chunks = {{0, 0, 40, 248}, {1, 0, 2000, 8388608}};
  if (withBoth) {
    records.push_back({kPixType, kBoth, 2, 0, 4096, 16, 1});
    records.push_back({kPixType, kCdnOnly, 2, 0, 4096, 16, 1});  // a name the stock manifest does not hold
    chunks.push_back({1, 0, 90, 4096});
  }
  std::unique_ptr<nevr_cdn::Mounted> m(new nevr_cdn::Mounted());
  m->shadow = BuildShadow(records, chunks, {descHandle, pixHandle});
  m->labels = {"lobby_posters", "other"};
  return m;
}

__attribute__((noinline)) std::uint64_t Call(std::uint64_t type, std::uint64_t name, std::uint64_t index, unsigned char* request) {
  const rr::FillThunk::Fn entry = reinterpret_cast<rr::FillThunk::Fn>(rr::FillThunk::EntryAddress());
  const std::uint64_t r = entry(reinterpret_cast<CResourceManagerOpaque*>(const_cast<void*>(g_stock->manager())), type, name, 0, nullptr,
                                nullptr, index, request);
  asm volatile("" ::: "memory");
  return r;
}

void Reset(bool log = true) {
  g_fill = Fill();
  {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_lines.clear();
  }
  rr::ResetForTest();
  rr::FillThunk::Reset();
  *rr::FillThunk::OriginalOut() = reinterpret_cast<void*>(&FakeFill);
  g_stock = BuildShadow({{kStockOnly, 1, 0, 0, 100, 16, 0}, {kBoth, 2, 1, 0, 100, 16, 0}, {kPixType, kBoth, 1, 0, 777, 16, 0},
                         {kDescType, kPoster, 2, 0, 256, 16, 0}, {kPixType, kPoster, 3, 0, 8388756, 16, 0}},
                        {{0, 0, 1, 100}, {0, 1, 1, 100}, {0, 2, 1, 256}, {0, 3, 1, 8388756}}, {kStockHandle});
  g_cdnSet = PosterSet(kCdnDescHandle, kCdnPixHandle);
  g_cdnSet->shadow.get();
  g_fill.cdnManager = g_cdnSet->shadow->manager();
  rr::ArmForTest(0x1000, log);
}

const void* Live() { return g_stock->manager(); }

void ACdnKeyIsFilledFromTheCdnShadowAndNeverTheLiveManager() {
  Reset();
  rr::PublishCdn(g_cdnSet.get());
  unsigned char request[layout::kRequestBytes] = {};
  QCHECK(Call(kDescType, kPoster, 0, request) == 0);
  QCHECK(g_fill.manager == g_cdnSet->shadow->manager() && g_fill.calls == 1);
  QCHECK(Get<std::int64_t>(request, layout::kPackageHandle) == kCdnDescHandle);
  QCHECK(Get<std::int64_t>(request, layout::kChunkIndex) >= static_cast<std::int64_t>(nevr_shadow::kFirstChunkIndex));
  QCHECK(Get<std::uint64_t>(request, layout::kChunkFileOffset) == 0);
  QCHECK(Get<std::uint64_t>(request, layout::kChunkCompressed) == 40 && Get<std::uint64_t>(request, layout::kChunkDecompressed) == 248);
  QCHECK(Get<std::uint64_t>(request, layout::kSize) == 248 && Get<std::uint64_t>(request, layout::kAlignment) == 16);
  std::memset(request, 0, sizeof(request));
  QCHECK(Call(kPixType, kPoster, 1, request) == 0);
  QCHECK(g_fill.manager == g_cdnSet->shadow->manager() && g_fill.calls == 2);
  QCHECK(Get<std::int64_t>(request, layout::kPackageHandle) == kCdnPixHandle);
  QCHECK(Get<std::uint64_t>(request, layout::kChunkCompressed) == 2000 && Get<std::uint64_t>(request, layout::kChunkDecompressed) == 8388608);
  QCHECK(Get<std::uint64_t>(request, layout::kSize) == 8388608);
  QCHECK(rr::CdnServedCount() == 2);
}

void AnyOtherKeyReachesTheGameWithTheLiveManagerUnchanged() {
  Reset();
  rr::PublishCdn(g_cdnSet.get());
  unsigned char request[layout::kRequestBytes] = {};
  QCHECK(Call(kStockOnly, 1, 0, request) == 0);
  QCHECK(g_fill.manager == Live() && g_fill.calls == 1);
  QCHECK(Get<std::int64_t>(request, layout::kPackageHandle) == kStockHandle);
  QCHECK(Call(0x9999, 0x1, 0, request) == kMiss);  // a stock miss stays the game's miss
  QCHECK(g_fill.manager == Live());
  QCHECK(rr::CdnServedCount() == 0);
}

void CdnReplacesAStockKeyOfTheSameName() {
  Reset();
  unsigned char request[layout::kRequestBytes] = {};
  QCHECK(Call(kPixType, kPoster, 1, request) == 0);   // before the mount: stock
  QCHECK(g_fill.manager == Live() && Get<std::uint64_t>(request, layout::kSize) == 8388756);
  rr::PublishCdn(g_cdnSet.get());
  QCHECK(Call(kDescType, kPoster, 0, request) == 0);  // after: CDN for both, though stock holds both too
  QCHECK(g_fill.manager == g_cdnSet->shadow->manager());
  QCHECK(Call(kPixType, kPoster, 1, request) == 0);
  QCHECK(g_fill.manager == g_cdnSet->shadow->manager() && Get<std::uint64_t>(request, layout::kSize) == 8388608);
}

void ACdnKeyTheStockLacksIsServedToo() {
  Reset();
  rr::PublishCdn(g_cdnSet.get());
  unsigned char request[layout::kRequestBytes] = {};
  // kBoth/kPixType is in stock (777); the CDN set holds it as well and wins. A name only the CDN set holds:
  QCHECK(Call(kPixType, kBoth, 0, request) == 0);
  QCHECK(g_fill.manager == g_cdnSet->shadow->manager() && Get<std::uint64_t>(request, layout::kSize) == 4096);
}

void ASetMountedBetweenTheTwoReadsDoesNotSplitThePair() {
  Reset();
  unsigned char request[layout::kRequestBytes] = {};
  QCHECK(Call(kDescType, kPoster, 0, request) == 0);   // no CDN yet: the descriptor comes from stock
  QCHECK(g_fill.manager == Live());
  rr::PublishCdn(g_cdnSet.get());                  // the set mounts between the two reads
  QCHECK(Call(kPixType, kPoster, 1, request) == 0);
  QCHECK(g_fill.manager == Live());                    // the pixels come from stock too
  QCHECK(Get<std::uint64_t>(request, layout::kSize) == 8388756);
  QCHECK(Call(kDescType, kPoster, 0, request) == 0);   // the next load takes the CDN
  QCHECK(g_fill.manager == g_cdnSet->shadow->manager());
}

void ANewerSetDoesNotSplitAPairAlreadyStartedFromTheOlder() {
  Reset();
  rr::PublishCdn(g_cdnSet.get());
  g_cdnSet2 = PosterSet(61, 62);
  unsigned char request[layout::kRequestBytes] = {};
  QCHECK(Call(kDescType, kPoster, 0, request) == 0 && g_fill.manager == g_cdnSet->shadow->manager());
  rr::PublishCdn(g_cdnSet2.get());
  QCHECK(Call(kPixType, kPoster, 1, request) == 0);
  QCHECK(g_fill.manager == g_cdnSet->shadow->manager());
  QCHECK(Get<std::int64_t>(request, layout::kPackageHandle) == kCdnPixHandle);
  QCHECK(Call(kDescType, kPoster, 0, request) == 0 && g_fill.manager == g_cdnSet2->shadow->manager());
}

void AnOtherThreadHasItsOwnPin() {
  Reset();
  rr::PublishCdn(g_cdnSet.get());
  g_cdnSet2 = PosterSet(61, 62);
  unsigned char request[layout::kRequestBytes] = {};
  QCHECK(Call(kDescType, kPoster, 0, request) == 0);   // this thread starts a pair from the first set
  std::thread other([&] {
    rr::PublishCdn(g_cdnSet2.get());
    unsigned char r2[layout::kRequestBytes] = {};
    Call(kDescType, kPoster, 0, r2);
    QCHECK(Get<std::int64_t>(r2, layout::kPackageHandle) == 61);
    Call(kPixType, kPoster, 1, r2);
    QCHECK(Get<std::int64_t>(r2, layout::kPackageHandle) == 62);
  });
  other.join();
  QCHECK(Call(kPixType, kPoster, 1, request) == 0);
  QCHECK(Get<std::int64_t>(request, layout::kPackageHandle) == kCdnPixHandle);  // this thread still finishes its pair from the first set
}

// The loose-file overlay's four game functions, counted: a stock miss that a CDN shadow holds is the CDN's to serve, never the overlay's.
int g_overlayCalls = 0;
void OverlayClear(unsigned char*) { ++g_overlayCalls; }
int OverlayPath(const CResourceManagerOpaque*, std::uint64_t, std::uint64_t, unsigned char*) { ++g_overlayCalls; return 0; }
int OverlayOpen(const char*, unsigned, long*) { ++g_overlayCalls; return 0; }
int OverlaySize(long, unsigned long long*) { ++g_overlayCalls; return 0; }

void AStockMissThatCdnHoldsIsNotTheOverlaysToServe() {
  Reset();
  const rr::GameFns fns = {&OverlayClear, &OverlayPath, &OverlayOpen, &OverlaySize};
  rr::SetGameFnsForTest(&fns);
  rr::PublishCdn(g_cdnSet.get());
  g_overlayCalls = 0;
  unsigned char request[layout::kRequestBytes] = {};
  QCHECK(Call(kPixType, kCdnOnly, 0, request) == 0);
  QCHECK(g_overlayCalls == 0);                                                   // the overlay was never asked
  QCHECK(g_fill.manager == g_cdnSet->shadow->manager() && Get<std::uint64_t>(request, layout::kSize) == 4096);
  // A stock miss that CDN does not hold still reaches the overlay, which serves it (the existing behaviour).
  QCHECK(Call(kPixType, 0x9999, 0, request) == 0);
  QCHECK(g_overlayCalls > 0);
}

void ANullRequestIsAnsweredFromTheShadowToo() {
  Reset();
  rr::PublishCdn(g_cdnSet.get());
  QCHECK(Call(kPixType, kPoster, 1, nullptr) == 0);
  QCHECK(g_fill.manager == g_cdnSet->shadow->manager());
}

void TheServedLineIsWrittenAtTheKeysPowersOfTwo() {
  Reset();
  rr::PublishCdn(g_cdnSet.get());
  unsigned char request[layout::kRequestBytes] = {};
  for (int i = 0; i < 5; ++i) {
    Call(kDescType, kPoster, 0, request);
    Call(kPixType, kPoster, 1, request);
  }
  const std::vector<std::string> served = Lines("cdn_served");
  QCHECK(served.size() == 6);  // each key at its 1st, 2nd and 4th serve
  if (served.size() == 6) {
    QCHECK(Field(served[0], "label") == "lobby_posters" && Field(served[0], "type") == "0xe2efe7289d5985b8");
    QCHECK(Field(served[0], "name") == "0x3fa6d25bc24a7b01" && Field(served[0], "index") == "0" && Field(served[0], "bytes") == "248");
    QCHECK(Field(served[0], "seen") == "1" && Field(served[1], "type") == "0x489bb35d53ca50e9" && Field(served[1], "bytes") == "8388608");
    QCHECK(Field(served[2], "seen") == "2" && Field(served[4], "seen") == "4");
  }
  QCHECK(rr::CdnServedCount() == 10);
  // The request line of a development build is written for a CDN request as for any other.
  const std::vector<std::string> reqs = Lines("resource_request");
  QCHECK(!reqs.empty());
  if (!reqs.empty()) QCHECK(std::stoll(Field(reqs[0], "chunk")) >= static_cast<long long>(nevr_shadow::kFirstChunkIndex));
}

void ALabelIsTheSecondSetsOwnNotTheFirsts() {
  Reset();
  rr::PublishCdn(g_cdnSet.get());
  g_cdnSet2 = PosterSet(61, 62);
  g_cdnSet2->labels = {"second_set_label", "other"};
  rr::PublishCdn(g_cdnSet2.get());
  unsigned char request[layout::kRequestBytes] = {};
  Call(kDescType, kPoster, 0, request);
  const std::vector<std::string> served = Lines("cdn_served");
  QCHECK(served.size() == 1 && Field(served[0], "label") == "second_set_label");
}

void AGameFailureWithTheShadowIsAWarningNotASilentServe() {
  Reset();
  rr::PublishCdn(g_cdnSet.get());
  g_fill.failOnCdn = true;
  unsigned char request[layout::kRequestBytes] = {};
  QCHECK(Call(kDescType, kPoster, 0, request) == 1);  // the game's own result is returned untouched
  QCHECK(Lines("cdn_served").empty());
  const std::vector<std::string> failed = Lines("cdn_failed");
  QCHECK(failed.size() == 1 && Field(failed[0], "label") == "lobby_posters");
}

void AReleaseBuildServesTheSameAndLogsNoRequestLine() {
  Reset(false);
  rr::PublishCdn(g_cdnSet.get());
  unsigned char request[layout::kRequestBytes] = {};
  QCHECK(Call(kDescType, kPoster, 0, request) == 0);
  QCHECK(g_fill.manager == g_cdnSet->shadow->manager());
  QCHECK(Lines("resource_request").empty());
  QCHECK(Lines("cdn_served").size() == 1);
}

void NothingPublishedChangesNothing() {
  Reset();
  rr::PublishCdn(nullptr);
  nevr_cdn::Mounted empty;
  rr::PublishCdn(&empty);  // a set with no shadow is ignored
  unsigned char request[layout::kRequestBytes] = {};
  QCHECK(Call(kDescType, kPoster, 0, request) == 0);
  QCHECK(g_fill.manager == Live() && Get<std::uint64_t>(request, layout::kSize) == 256);
  QCHECK(Lines("cdn_served").empty() && rr::CdnServedCount() == 0);
}

// T2c (#2135): each served (name, index) is recorded for the read-finished callback, which writes `cdn_read` for it.
void AServedKeyIsRecordedForTheReadCallbackAndAFailedOneIsNot() {
  Reset();
  rr::PublishCdn(g_cdnSet.get());
  unsigned char request[layout::kRequestBytes] = {};
  QCHECK(nevr_quest::cdn_read::TouchRead(kPoster, 1) == 0);  // nothing served yet
  Call(kPixType, kPoster, 1, request);
  Call(kPixType, kPoster, 1, request);  // a second serve records nothing new
  QCHECK(nevr_quest::cdn_read::TouchRead(kPoster, 1) == 1);
  QCHECK(nevr_quest::cdn_read::TouchRead(kPoster, 1) == 2);
  QCHECK(nevr_quest::cdn_read::TouchRead(kPoster, 0) == 0);  // index 0 was not served in this test
  Reset();
  rr::PublishCdn(g_cdnSet.get());
  g_fill.failOnCdn = true;
  Call(kDescType, kPoster, 0, request);
  QCHECK(nevr_quest::cdn_read::TouchRead(kPoster, 0) == 0);  // the game's function failed: no read will finish
}

void AKeyThatIsNotCdnIsNotRecorded() {
  Reset();
  rr::PublishCdn(g_cdnSet.get());
  unsigned char request[layout::kRequestBytes] = {};
  Call(kPixType, kPoster + 1, 1, request);  // not in the CDN set: the game's own manager answers
  QCHECK(nevr_quest::cdn_read::TouchRead(kPoster + 1, 1) == 0);
}

void TheCounterCountIsUnchanged() { QCHECK(rr::kCounterCount == 4); }

}  // namespace

int main() {
  sentinel::SetLogSink(&Sink);
  ACdnKeyIsFilledFromTheCdnShadowAndNeverTheLiveManager();
  AnyOtherKeyReachesTheGameWithTheLiveManagerUnchanged();
  CdnReplacesAStockKeyOfTheSameName();
  ACdnKeyTheStockLacksIsServedToo();
  ASetMountedBetweenTheTwoReadsDoesNotSplitThePair();
  ANewerSetDoesNotSplitAPairAlreadyStartedFromTheOlder();
  AnOtherThreadHasItsOwnPin();
  AStockMissThatCdnHoldsIsNotTheOverlaysToServe();
  ANullRequestIsAnsweredFromTheShadowToo();
  TheServedLineIsWrittenAtTheKeysPowersOfTwo();
  ALabelIsTheSecondSetsOwnNotTheFirsts();
  AGameFailureWithTheShadowIsAWarningNotASilentServe();
  AReleaseBuildServesTheSameAndLogsNoRequestLine();
  NothingPublishedChangesNothing();
  AServedKeyIsRecordedForTheReadCallbackAndAFailedOneIsNot();
  AKeyThatIsNotCdnIsNotRecorded();
  TheCounterCountIsUnchanged();
  sentinel::SetLogSink(nullptr);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "cdn_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("cdn_hook_test: all checks passed\n");
  return 0;
}
