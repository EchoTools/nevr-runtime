// Host test for the log-only resource request hook (#871 steps 2 and 3): the real handler
// (src/quest/sentinel/resource_request_hook.cpp), built without exceptions as the sentinel builds it, driven
// through the thunk's entry with a stand-in for CResourceManager::FillResourceFileRequest that fills a request the
// way the pinned function does (the field map in pinned_targets.h, resource_request_layout). No GOT slot is touched:
// got_pinned_test.cpp resolves the pinned slot and the six call sites in the real library, and tests/quest
// TestHookFramesCarryNoPersonality covers the frames.
//
// Built and run by `just test-quest-hooks`.

#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "hook_log.h"
#include "hook_report.h"
#include "quest/tests/test_check.h"
#include "resource_request_hook.h"

namespace {

namespace rr = nevr_quest::resource_request;
namespace layout = sentinel::pinned::resource_request_layout;
using sentinel::pinned::CResourceManagerOpaque;

// ---- the log ---------------------------------------------------------------------------------------

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

// The raw text of a field's value (quotes kept for a string), or "<absent>".
std::string Field(const std::string& line, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const std::size_t at = line.find(needle);
  if (at == std::string::npos) return "<absent>";
  const std::size_t begin = at + needle.size();
  const std::size_t end = line.find_first_of(",}", begin);
  return line.substr(begin, end - begin);
}

// ---- the game's function ---------------------------------------------------------------------------

struct Fill {
  int calls = 0;
  CResourceManagerOpaque* manager = nullptr;
  std::uint64_t type = 0, name = 0, unused = 0, index = 0;
  void* allocator = nullptr;
  void* owner = nullptr;
  unsigned char* request = nullptr;
  std::uint64_t missName = 0;  // a request for this name misses
  unsigned char wrote[layout::kRequestBytes] = {};  // the request as the stand-in left it
  bool wroteAny = false;
};
Fill g_fill;
std::atomic<int> g_threadedCalls{0};

constexpr std::int64_t kHandle = 7, kChunk = 3;
constexpr std::uint64_t kChunkOffset = 0x100000, kChunkCompressed = 0x2000, kChunkDecompressed = 0x4000;
constexpr std::uint64_t kOffsetInChunk = 0x40, kSize = 0x1000, kAlignment = 0x10;
constexpr std::uint64_t kMissReturn = 0xdead0000beefULL;

template <typename T>
void Put(unsigned char* request, std::size_t at, T value) {
  std::memcpy(request + at, &value, sizeof(value));
}

// What FillResourceFileRequest does to the request on a hit (pinned 0x2543a2c-0x2543bf8): clear, then the stores.
std::uint64_t FakeFill(CResourceManagerOpaque* manager, std::uint64_t type, std::uint64_t name, std::uint64_t unused,
                       void* allocator, void* owner, std::uint64_t index, unsigned char* request) {
  g_threadedCalls.fetch_add(1, std::memory_order_relaxed);
  ++g_fill.calls;
  g_fill.manager = manager;
  g_fill.type = type;
  g_fill.name = name;
  g_fill.unused = unused;
  g_fill.allocator = allocator;
  g_fill.owner = owner;
  g_fill.index = index;
  g_fill.request = request;
  if (name == g_fill.missName) return kMissReturn;
  if (request == nullptr) return 0;
  std::memset(request, 0, layout::kRequestBytes);
  Put<std::int64_t>(request, layout::kPackageHandle, kHandle);
  Put<std::int64_t>(request, layout::kChunkIndex, kChunk);
  Put<std::uint64_t>(request, layout::kChunkFileOffset, kChunkOffset);
  Put<std::uint64_t>(request, layout::kChunkCompressed, kChunkCompressed);
  Put<std::uint64_t>(request, layout::kChunkDecompressed, kChunkDecompressed);
  Put<std::uint64_t>(request, layout::kOffsetInChunk, kOffsetInChunk);
  Put<std::uint64_t>(request, layout::kSize, kSize);
  Put<std::uint64_t>(request, layout::kAlignment, kAlignment);
  std::memcpy(g_fill.wrote, request, layout::kRequestBytes);
  g_fill.wroteAny = true;
  return 0;
}

// The call the game makes: through the thunk's entry. Not inlined, so the entry's return address is inside this function.
__attribute__((noinline)) std::uint64_t Call(std::uint64_t type, std::uint64_t name, std::uint64_t index,
                                             unsigned char* request, void* owner = nullptr, void* allocator = nullptr) {
  const rr::FillThunk::Fn entry = reinterpret_cast<rr::FillThunk::Fn>(rr::FillThunk::EntryAddress());
  const std::uint64_t r = entry(reinterpret_cast<CResourceManagerOpaque*>(0x1000), type, name, 0x351127d67ULL, allocator,
                                owner, index, request);
  asm volatile("" ::: "memory");  // not a tail call
  return r;
}

constexpr std::uint64_t kBaseType = 0x4a4c32c49300b8a0ULL;
constexpr std::uint64_t kMipType = 0x43f216c5d7d2b47fULL;

void Reset(std::uint64_t base = 0) {
  g_fill = Fill();
  g_threadedCalls = 0;
  {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_lines.clear();
  }
  rr::ResetForTest();
  rr::FillThunk::Reset();
  *rr::FillThunk::OriginalOut() = reinterpret_cast<void*>(&FakeFill);
  if (base != 0) rr::ArmForTest(base);
}

long long MyTid() { return static_cast<long long>(::syscall(SYS_gettid)); }

// ---- tests -----------------------------------------------------------------------------------------

void AnUnarmedThunkPassesThroughAndLogsNothing() {
  Reset();
  unsigned char request[layout::kRequestBytes] = {};
  QCHECK(Call(kBaseType, 0x11, 0, request) == 0);
  QCHECK(g_fill.calls == 1);
  QCHECK(Lines("resource_request").empty());
  QCHECK(rr::CurrentTotals().lines == 0);
}

void TheGameGetsItsOwnResultArgumentsAndRequestBack() {
  Reset(1);
  unsigned char request[layout::kRequestBytes];
  std::memset(request, 0xAB, sizeof(request));
  int ownerObject = 0, allocatorObject = 0;
  QCHECK(Call(kBaseType, 0x22, 1, request, &ownerObject, &allocatorObject) == 0);
  QCHECK(g_fill.calls == 1);
  QCHECK(g_fill.manager == reinterpret_cast<CResourceManagerOpaque*>(0x1000));
  QCHECK(g_fill.type == kBaseType && g_fill.name == 0x22 && g_fill.index == 1);
  QCHECK(g_fill.unused == 0x351127d67ULL);
  QCHECK(g_fill.owner == &ownerObject && g_fill.allocator == &allocatorObject && g_fill.request == request);
  // The handler wrote nothing to the request after the game's function filled it.
  QCHECK(g_fill.wroteAny && std::memcmp(request, g_fill.wrote, layout::kRequestBytes) == 0);
  // A miss returns the game's error object, unchanged.
  g_fill.missName = 0x33;
  QCHECK(Call(kBaseType, 0x33, 0, request) == kMissReturn);
}

void AHitIsLoggedWithTheKeyTheOwnerTheSiteAndTheFilledFields() {
  Reset(1);
  unsigned char request[layout::kRequestBytes] = {};
  int ownerObject = 0, allocatorObject = 0;
  QCHECK(Call(kMipType, 0xabcdef0123456789ULL, 9, request, &ownerObject, &allocatorObject) == 0);
  const std::vector<std::string> lines = Lines("resource_request");
  QCHECK(lines.size() == 1);
  if (lines.empty()) return;
  const std::string& l = lines[0];
  QCHECK(Field(l, "type") == "\"0x43f216c5d7d2b47f\"");
  QCHECK(Field(l, "name") == "\"0xabcdef0123456789\"");
  QCHECK(Field(l, "index") == "9");
  char hex[19];
  QCHECK(Field(l, "owner") == std::string("\"") + sentinel::HexString(hex, reinterpret_cast<std::uint64_t>(&ownerObject)) + "\"");
  QCHECK(Field(l, "allocator_set") == "1");
  QCHECK(Field(l, "ret") == "\"0x0000000000000000\"");
  QCHECK(Field(l, "seen") == "1");
  QCHECK(Field(l, "tid") == std::to_string(MyTid()));
  QCHECK(Field(l, "handle") == std::to_string(kHandle));
  QCHECK(Field(l, "chunk") == std::to_string(kChunk));
  QCHECK(Field(l, "chunk_offset") == std::to_string(kChunkOffset));
  QCHECK(Field(l, "chunk_compressed") == std::to_string(kChunkCompressed));
  QCHECK(Field(l, "chunk_decompressed") == std::to_string(kChunkDecompressed));
  QCHECK(Field(l, "offset_in_chunk") == std::to_string(kOffsetInChunk));
  QCHECK(Field(l, "size") == std::to_string(kSize));
  QCHECK(Field(l, "alignment") == std::to_string(kAlignment));
  QCHECK(Field(l, "request_null") == "<absent>");
  QCHECK(l.find("\"truncated\"") == std::string::npos);  // the widest line still fits the log line
  // A request with no owner and no allocator says so.
  Call(kBaseType, 0x44, 0, request);
  QCHECK(Field(Lines("resource_request").back(), "allocator_set") == "0");
  QCHECK(Field(Lines("resource_request").back(), "owner") == "\"0x0000000000000000\"");
}

void ARepeatIsLoggedAtEachPowerOfTwoAndADifferentKeyIsLoggedAgain() {
  Reset(1);
  unsigned char request[layout::kRequestBytes] = {};
  for (int i = 0; i < 9; ++i) Call(kBaseType, 0x55, 0, request);
  std::vector<std::string> seen;
  for (const std::string& l : Lines("resource_request")) seen.push_back(Field(l, "seen"));
  QCHECK((seen == std::vector<std::string>{"1", "2", "4", "8"}));
  Reset(1);
  Call(kBaseType, 0x55, 0, request);
  Call(kBaseType, 0x55, 1, request);        // another index
  Call(kBaseType, 0x56, 0, request);        // another name
  Call(kMipType, 0x55, 0, request);         // another type
  QCHECK(Lines("resource_request").size() == 4);
  for (const std::string& l : Lines("resource_request")) QCHECK(Field(l, "seen") == "1");
}

void AMissIsLoggedAsAMissWithoutReadingTheRequest() {
  Reset(1);
  g_fill.missName = 0x66;
  // A null request on a miss: the handler must not read it.
  QCHECK(Call(kBaseType, 0x66, 2, nullptr) == kMissReturn);
  QCHECK(Lines("resource_request").empty());
  const std::vector<std::string> lines = Lines("resource_request_miss");
  QCHECK(lines.size() == 1);
  if (lines.empty()) return;
  QCHECK(Field(lines[0], "name") == "\"0x0000000000000066\"");
  QCHECK(Field(lines[0], "index") == "2");
  QCHECK(Field(lines[0], "ret") == "\"0x0000dead0000beef\"");
  QCHECK(Field(lines[0], "tid") == std::to_string(MyTid()));
  QCHECK(Field(lines[0], "handle") == "<absent>");
  // A second miss of the same key follows the same schedule (1, 2, 4 ...).
  Call(kBaseType, 0x66, 2, nullptr);
  Call(kBaseType, 0x66, 2, nullptr);
  QCHECK(Lines("resource_request_miss").size() == 2);
}

void AHitWithoutARequestIsLoggedWithoutReadingIt() {
  Reset(1);
  QCHECK(Call(kBaseType, 0x77, 0, nullptr) == 0);
  const std::vector<std::string> lines = Lines("resource_request");
  QCHECK(lines.size() == 1);
  if (lines.empty()) return;
  QCHECK(Field(lines[0], "request_null") == "1");
  QCHECK(Field(lines[0], "handle") == "<absent>");
}

void TheSiteIsTheReturnAddressRelativeToTheLoadBase() {
  // The call site is the instruction after the game's bl, as a pinned virtual address: the address minus the load base.
  const std::uint64_t base = reinterpret_cast<std::uint64_t>(&Call);
  Reset(base);
  unsigned char request[layout::kRequestBytes] = {};
  Call(kBaseType, 0x88, 0, request);
  const std::vector<std::string> lines = Lines("resource_request");
  QCHECK(lines.size() == 1);
  if (lines.empty()) return;
  const std::string caller = Field(lines[0], "caller");
  QCHECK(caller.size() == 20 && caller.compare(0, 3, "\"0x") == 0);
  const unsigned long long offset = std::strtoull(caller.c_str() + 1, nullptr, 16);
  QCHECK(offset > 0 && offset < 0x400);  // inside Call, not an absolute address
  QCHECK(Field(lines[0], "site") == "\"unknown\"");
}

void EveryKnownCallSiteIsNamedByItsReturnAddress() {
  std::set<std::string> names;
  for (const sentinel::pinned::FillCallSite& site : sentinel::pinned::kFillCallSites) {
    QCHECK(std::strcmp(rr::SiteName(site.bl + 4), site.name) == 0);  // the instruction after the bl
    QCHECK(std::strcmp(rr::SiteName(site.bl), "unknown") == 0);      // not the bl itself
    QCHECK(std::strcmp(rr::SiteName(site.bl + 8), "unknown") == 0);
    names.insert(site.name);
  }
  QCHECK(names.size() == 6);
  QCHECK(std::strcmp(rr::SiteName(0), "unknown") == 0);
}

void ALoggedSiteNameIsTheNamedSite() {
  // Make the call's return address land on a known site: base chosen so that (address - base) is a known return.
  const std::uint64_t knownReturn = sentinel::pinned::kFillCallSites[4].bl + 4;  // mip_request
  Reset();
  unsigned char request[layout::kRequestBytes] = {};
  // The return address of Call's entry call is unknown until a call is made; make one to learn it, then set the base.
  rr::ArmForTest(1);
  Call(kBaseType, 0x99, 0, request);
  const std::string first = Field(Lines("resource_request").back(), "caller");
  const unsigned long long off1 = std::strtoull(first.c_str() + 1, nullptr, 16);  // returnAddress - 1
  const std::uint64_t returnAddress = off1 + 1;
  Reset();
  rr::ArmForTest(returnAddress - knownReturn);
  Call(kBaseType, 0x9a, 0, request);
  const std::string site = Field(Lines("resource_request").back(), "site");
  QCHECK(site == "\"mip_request\"");
}

void EachThreadIsLoggedWithItsOwnThreadId() {
  Reset(1);
  long long worker = 0;
  std::thread t([&] {
    unsigned char request[layout::kRequestBytes] = {};
    worker = MyTid();
    Call(kBaseType, 0xaa, 0, request);
  });
  t.join();
  unsigned char request[layout::kRequestBytes] = {};
  Call(kBaseType, 0xab, 0, request);
  const std::vector<std::string> lines = Lines("resource_request");
  QCHECK(lines.size() == 2);
  if (lines.size() != 2) return;
  QCHECK(Field(lines[0], "tid") == std::to_string(worker));
  QCHECK(Field(lines[1], "tid") == std::to_string(MyTid()));
  QCHECK(worker != MyTid());
}

void ConcurrentCallsOfOneKeyLogEachPowerOfTwoExactlyOnce() {
  Reset(1);
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([] {
      unsigned char request[layout::kRequestBytes] = {};
      for (int i = 0; i < 100; ++i) Call(kBaseType, 0xbb, 0, request);
    });
  }
  for (std::thread& t : threads) t.join();
  QCHECK(g_threadedCalls.load() == 400);
  std::multiset<std::string> seen;
  for (const std::string& l : Lines("resource_request")) seen.insert(Field(l, "seen"));
  const std::multiset<std::string> want = {"1", "2", "4", "8", "16", "32", "64", "128", "256"};
  QCHECK(seen == want);
}

void TheKeyTableIsBoundedAndSaysSoOnce() {
  Reset(1);
  unsigned char request[layout::kRequestBytes] = {};
  unsigned long long keys = 0;
  while (keys < 30000 && Lines("resource_request_table_full").empty()) {
    Call(kBaseType, 0x10000 + keys, 0, request);
    ++keys;
  }
  QCHECK(Lines("resource_request_table_full").size() == 1);
  // The limit is the probe window, not a full table: some key found no slot among kProbeLimit while slots were free.
  QCHECK(keys > rr::kProbeLimit && keys < rr::kKeySlots);
  const unsigned long long linesAtFull = rr::CurrentTotals().lines;
  QCHECK(linesAtFull >= rr::kProbeLimit);
  // More new keys: still not logged, counted, and the game's calls still reach the original.
  const int callsBefore = g_fill.calls;
  for (int i = 0; i < 2000; ++i) Call(kBaseType, 0x90000 + static_cast<std::uint64_t>(i), 0, request);
  QCHECK(g_fill.calls == callsBefore + 2000);
  QCHECK(Lines("resource_request_table_full").size() == 1);  // said once
  QCHECK(rr::CurrentTotals().unlogged > 0);
  // The keys that did get a slot keep their schedule.
  QCHECK(rr::CurrentTotals().lines >= linesAtFull);
}

void TheLineCountIsBoundedAndSaysSoOnce() {
  Reset(1);
  unsigned char request[layout::kRequestBytes] = {};
  int calls = 0;
  // 3000 keys, each called 32 times: 6 lines per key (1, 2, 4, 8, 16, 32) would be 18000 lines, over the cap.
  for (int rep = 0; rep < 32; ++rep) {
    for (std::uint64_t k = 0; k < 3000; ++k) {
      Call(kBaseType, 0x200000 + k, 0, request);
      ++calls;
    }
  }
  QCHECK(g_fill.calls == calls);  // every call reached the game's function
  QCHECK(Lines("resource_request").size() == rr::kMaxLines);
  QCHECK(rr::CurrentTotals().lines == rr::kMaxLines);
  QCHECK(Lines("resource_request_cap").size() == 1);
  QCHECK(rr::CurrentTotals().unlogged > 0);
  QCHECK(Lines("resource_request_table_full").empty());
}

void TheCountersAreRegisteredAndARefusedOneIsSaid() {
  sentinel::StopReporter();  // forget anything registered earlier in this process
  Reset();
  QCHECK(rr::RegisterCounters());
  QCHECK(Lines("resource_request_counters").empty());
  sentinel::StopReporter();
  // One slot left of the two it needs: refused, said once, false.
  static std::atomic<std::uint64_t> spare[sentinel::kMaxReportCounters];
  for (unsigned i = 0; i + 1 < sentinel::kMaxReportCounters; ++i) {
    QCHECK(sentinel::RegisterReportCounter("spare", &spare[i]));
  }
  Reset();
  QCHECK(!rr::RegisterCounters());
  const std::vector<std::string> lines = Lines("resource_request_counters");
  QCHECK(lines.size() == 1);
  if (!lines.empty()) {
    QCHECK(Field(lines[0], "result") == "\"refused\"");
    QCHECK(Field(lines[0], "registered") == "1" && Field(lines[0], "wanted") == "2");
  }
  sentinel::StopReporter();
  QCHECK(rr::kCounterCount == 2);
}

void WithoutLibr15TheInstallIsRefusedAndSaysWhy() {
  Reset();
  QCHECK(!rr::Install());
  const std::vector<std::string> lines = Lines("resource_request_install");
  QCHECK(lines.size() == 1);
  if (!lines.empty()) {
    QCHECK(Field(lines[0], "result") == "\"failed\"");
    QCHECK(Field(lines[0], "slot") == "\"module_not_loaded\"");
    QCHECK(Field(lines[0], "load_base") == "\"0x0000000000000000\"");
  }
  // The counters were refused: nothing is installed or armed, and the line says why.
  Reset();
  QCHECK(!rr::InstallIfCounted(false));
  const std::vector<std::string> skipped = Lines("resource_request_install");
  QCHECK(skipped.size() == 1);
  if (!skipped.empty()) {
    QCHECK(Field(skipped[0], "result") == "\"skipped\"");
    QCHECK(Field(skipped[0], "why") == "\"counters_refused\"");
  }
  unsigned char request[layout::kRequestBytes] = {};
  Call(kBaseType, 0xcc, 0, request);
  QCHECK(Lines("resource_request").empty());  // the skipped install armed nothing
}

}  // namespace

int main() {
  sentinel::SetLogSink(&Sink);
  AnUnarmedThunkPassesThroughAndLogsNothing();
  TheGameGetsItsOwnResultArgumentsAndRequestBack();
  AHitIsLoggedWithTheKeyTheOwnerTheSiteAndTheFilledFields();
  ARepeatIsLoggedAtEachPowerOfTwoAndADifferentKeyIsLoggedAgain();
  AMissIsLoggedAsAMissWithoutReadingTheRequest();
  AHitWithoutARequestIsLoggedWithoutReadingIt();
  TheSiteIsTheReturnAddressRelativeToTheLoadBase();
  EveryKnownCallSiteIsNamedByItsReturnAddress();
  ALoggedSiteNameIsTheNamedSite();
  EachThreadIsLoggedWithItsOwnThreadId();
  ConcurrentCallsOfOneKeyLogEachPowerOfTwoExactlyOnce();
  TheKeyTableIsBoundedAndSaysSoOnce();
  TheLineCountIsBoundedAndSaysSoOnce();
  TheCountersAreRegisteredAndARefusedOneIsSaid();
  WithoutLibr15TheInstallIsRefusedAndSaysWhy();
  sentinel::SetLogSink(nullptr);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "resource_request_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("resource_request_hook_test: all checks passed\n");
  return 0;
}
