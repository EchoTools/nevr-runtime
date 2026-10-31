// Host test for the resource request hook and its loose-file overlay (#871 steps 2 and 3; #1737 C5): the real handler
// (src/quest/sentinel/resource_request_hook.cpp), built without exceptions as the sentinel builds it, driven through the
// thunk's entry with a stand-in for CResourceManager::FillResourceFileRequest that searches a fake manifest, and with
// stand-ins for the four libr15 functions the overlay calls. The manager, the manifest and the request use the layouts
// in pinned_targets.h (resource_manifest_layout, resource_request_layout), pinned below by static_assert. No GOT slot is
// touched: got_pinned_test.cpp resolves the pinned slot and the six call sites in the real library, and tests/quest
// TestHookFramesCarryNoPersonality covers the frames.
//
// Built and run by `just test-quest-hooks`.

#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "hook_log.h"
#include "hook_report.h"
#include "quest/tests/test_check.h"
#include "resource_request_hook.h"

namespace {

namespace rr = nevr_quest::resource_request;
namespace layout = sentinel::pinned::resource_request_layout;
namespace manifest = sentinel::pinned::resource_manifest_layout;
using sentinel::pinned::CResourceManagerOpaque;

// ---- the manifest and the manager, in the layout the game reads -------------------------------------------------------

struct Record {  // 32 bytes: the type and the name are the keys the search compares
  std::int64_t type;
  std::int64_t name;
  unsigned char rest[16];
};
static_assert(sizeof(Record) == (std::size_t{1} << manifest::kRecordShift), "a manifest record is 32 bytes");
static_assert(offsetof(Record, type) == manifest::kRecordType, "the type is the first key");
static_assert(offsetof(Record, name) == manifest::kRecordName, "the name follows the type");

struct Manifest {
  const Record* records;  // +0
  unsigned char pad[manifest::kManifestCount - sizeof(const Record*)];
  std::uint64_t count;  // +0x30
};
static_assert(offsetof(Manifest, records) == manifest::kManifestRecords, "the record array is at +0");
static_assert(offsetof(Manifest, count) == manifest::kManifestCount, "the record count is at +0x30");

struct Manager {
  unsigned char pad[manifest::kManagerManifest];
  const Manifest* manifest;  // +8
};
static_assert(offsetof(Manager, manifest) == manifest::kManagerManifest, "the manifest pointer is at +8");

// ---- the log ---------------------------------------------------------------------------------------------------------

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

// ---- the game's function (FillResourceFileRequest) ----------------------------------------------------------------------

std::mutex g_fakeMutex;  // the fakes run on any thread the concurrency test uses

struct Fill {
  int calls = 0;
  CResourceManagerOpaque* manager = nullptr;
  std::uint64_t type = 0, name = 0, unused = 0, index = 0;
  void* allocator = nullptr;
  void* owner = nullptr;
  unsigned char* request = nullptr;
  bool allHits = false;  // every key is a hit (the big tables): no manifest search
  unsigned char wrote[layout::kRequestBytes] = {};  // the request as the stand-in left it on a hit
  bool wroteAny = false;
};
Fill g_fill;
std::atomic<int> g_threadedCalls{0};

constexpr std::int64_t kHandle = 7, kChunk = 3;
constexpr std::uint64_t kChunkOffset = 0x100000, kChunkCompressed = 0x2000, kChunkDecompressed = 0x4000;
constexpr std::uint64_t kOffsetInChunk = 0x40, kSize = 0x1000, kAlignment = 0x10;
constexpr std::uint64_t kMissReturn = 0xdead0000beefULL;
constexpr std::uint64_t kRegisterStub = 0x351127d67ULL;  // the `unused` argument the calls pass

template <typename T>
void Put(unsigned char* request, std::size_t at, T value) {
  std::memcpy(request + at, &value, sizeof(value));
}
template <typename T>
T Get(const unsigned char* request, std::size_t at) {
  T value{};
  std::memcpy(&value, request + at, sizeof(value));
  return value;
}

// The key is in the manifest by a linear search: the test's own reading of the table, independent of the hook's binary search.
bool ManifestHas(const CResourceManagerOpaque* manager, std::uint64_t type, std::uint64_t name) {
  if (manager == nullptr) return false;
  const Manifest* table = reinterpret_cast<const Manager*>(manager)->manifest;
  if (table == nullptr) return false;
  for (std::uint64_t i = 0; i < table->count; ++i) {
    const Record& r = table->records[i];
    if (r.type == static_cast<std::int64_t>(type) && r.name == static_cast<std::int64_t>(name)) return true;
  }
  return false;
}

// What FillResourceFileRequest does (pinned 0x2543a28-0x2543bf8): clear the request, then on a hit the stores; on a miss it
// returns the error object and leaves the request clear.
std::uint64_t FakeFill(CResourceManagerOpaque* manager, std::uint64_t type, std::uint64_t name, std::uint64_t unused,
                       void* allocator, void* owner, std::uint64_t index, unsigned char* request) {
  std::lock_guard<std::mutex> lock(g_fakeMutex);
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
  if (request != nullptr) std::memset(request, 0, layout::kRequestBytes);
  const bool hit = g_fill.allHits || ManifestHas(manager, type, name);
  if (!hit) return kMissReturn;
  if (request == nullptr) return 0;
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

// ---- the four libr15 functions the overlay calls -----------------------------------------------------------------------

constexpr std::int64_t kOverlayHandle = 0x2a;
constexpr unsigned long long kLooseSize = 0x7d00;
constexpr const char* kLoosePath = "/data/loose/file";

struct Overlay {
  int seq = 0;  // the order the calls happened in
  int clearCalls = 0, pathCalls = 0, openCalls = 0, sizeCalls = 0;
  int clearSeq = 0, pathSeq = 0;
  unsigned char* clearRequest = nullptr;
  const CResourceManagerOpaque* pathManager = nullptr;
  std::uint64_t pathType = 0, pathName = 0;
  unsigned char* pathBuf = nullptr;
  const char* openPath = nullptr;
  unsigned openFlags = 0;
  long* openHandle = nullptr;
  long sizeHandle = 0;
  int pathRet = 0, openRet = 0, sizeRet = 0;
};
Overlay g_ov;

int Total(const Overlay& o) { return o.clearCalls + o.pathCalls + o.openCalls + o.sizeCalls; }

void FakeClear(unsigned char* request) {
  std::lock_guard<std::mutex> lock(g_fakeMutex);
  ++g_ov.clearCalls;
  g_ov.clearSeq = ++g_ov.seq;
  g_ov.clearRequest = request;
}

int FakePath(const CResourceManagerOpaque* manager, std::uint64_t type, std::uint64_t name, unsigned char* path) {
  std::lock_guard<std::mutex> lock(g_fakeMutex);
  ++g_ov.pathCalls;
  g_ov.pathSeq = ++g_ov.seq;
  g_ov.pathManager = manager;
  g_ov.pathType = type;
  g_ov.pathName = name;
  g_ov.pathBuf = path;
  if (g_ov.pathRet == 0) std::memcpy(path, kLoosePath, std::strlen(kLoosePath) + 1);
  return g_ov.pathRet;
}

int FakeOpen(const char* path, unsigned flags, long* handle) {
  std::lock_guard<std::mutex> lock(g_fakeMutex);
  ++g_ov.openCalls;
  ++g_ov.seq;
  g_ov.openPath = path;
  g_ov.openFlags = flags;
  g_ov.openHandle = handle;
  if (g_ov.openRet == 0) *handle = static_cast<long>(kOverlayHandle);
  return g_ov.openRet;
}

int FakeSize(long handle, unsigned long long* size) {
  std::lock_guard<std::mutex> lock(g_fakeMutex);
  ++g_ov.sizeCalls;
  ++g_ov.seq;
  g_ov.sizeHandle = handle;
  if (g_ov.sizeRet == 0) *size = kLooseSize;
  return g_ov.sizeRet;
}

const rr::GameFns kFakeFns = {&FakeClear, &FakePath, &FakeOpen, &FakeSize};

// ---- symbol lookup for the resolve tests -----------------------------------------------------------------------------

struct SymbolPin {
  const char* name;
  std::uint64_t vaddr;
};
const SymbolPin kPins[4] = {
    {sentinel::pinned::kClearFileRequestDataSymbol, sentinel::pinned::kClearFileRequestDataVaddr},
    {sentinel::pinned::kGetUnpackagedResourcePathSymbol, sentinel::pinned::kGetUnpackagedResourcePathVaddr},
    {sentinel::pinned::kCSysFileOpenSymbol, sentinel::pinned::kCSysFileOpenVaddr},
    {sentinel::pinned::kCSysFileSizeSymbol, sentinel::pinned::kCSysFileSizeVaddr},
};
std::uint64_t g_lookupBase = 0;
const char* g_lookupNull = nullptr;   // this symbol does not resolve
const char* g_lookupWrong = nullptr;  // this symbol resolves 8 bytes past its pinned address

void* FakeLookup(const char* symbol) {
  for (const SymbolPin& pin : kPins) {
    if (std::strcmp(pin.name, symbol) != 0) continue;
    if (g_lookupNull != nullptr && std::strcmp(g_lookupNull, symbol) == 0) return nullptr;
    std::uint64_t address = g_lookupBase + pin.vaddr;
    if (g_lookupWrong != nullptr && std::strcmp(g_lookupWrong, symbol) == 0) address += 8;
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(address));
  }
  return nullptr;
}

// ---- the manifest and the calls --------------------------------------------------------------------------------------

struct Key {
  std::int64_t type;
  std::int64_t name;
};

Key K(std::uint64_t type, std::uint64_t name) { return Key{static_cast<std::int64_t>(type), static_cast<std::int64_t>(name)}; }

bool KeyLess(const Key& a, const Key& b) { return a.type != b.type ? a.type < b.type : a.name < b.name; }

std::vector<Record> g_records;
Manifest g_manifest{};
Manager g_manager{};
CResourceManagerOpaque* g_callManager = reinterpret_cast<CResourceManagerOpaque*>(&g_manager);

// Builds the manifest from `keys`, sorted by (type, name) as signed values: the order the game's search needs.
void SetManifest(std::vector<Key> keys) {
  std::sort(keys.begin(), keys.end(), KeyLess);
  g_records.assign(keys.size(), Record{});
  for (std::size_t i = 0; i < keys.size(); ++i) {
    g_records[i].type = keys[i].type;
    g_records[i].name = keys[i].name;
  }
  g_manifest.records = g_records.empty() ? nullptr : g_records.data();
  g_manifest.count = g_records.size();
  g_manager.manifest = &g_manifest;
}

// The game's call, through the thunk's entry. Not inlined, so the entry's return address is inside this function.
__attribute__((noinline)) std::uint64_t Call(std::uint64_t type, std::uint64_t name, std::uint64_t index,
                                             unsigned char* request, void* owner = nullptr, void* allocator = nullptr) {
  const rr::FillThunk::Fn entry = reinterpret_cast<rr::FillThunk::Fn>(rr::FillThunk::EntryAddress());
  const std::uint64_t r =
      entry(g_callManager, type, name, kRegisterStub, allocator, owner, index, request);
  asm volatile("" ::: "memory");  // not a tail call
  return r;
}

constexpr std::uint64_t kBaseType = 0x4a4c32c49300b8a0ULL;
constexpr std::uint64_t kMipType = 0x43f216c5d7d2b47fULL;
constexpr std::uint64_t kMissName = 0x66;  // not in the default manifest

// The default manifest: every name the tests call as a hit. kMissName and 0x33 are the misses.
std::vector<Key> DefaultKeys() {
  return {K(kBaseType, 0x11),   K(kBaseType, 0x22), K(kBaseType, 0x44), K(kBaseType, 0x55), K(kBaseType, 0x56),
          K(kBaseType, 0x77),   K(kBaseType, 0x88), K(kBaseType, 0x99), K(kBaseType, 0x9a), K(kBaseType, 0xaa),
          K(kBaseType, 0xab),   K(kBaseType, 0xbb), K(kBaseType, 0xcc), K(kMipType, 0x55),
          K(kMipType, 0xabcdef0123456789ULL)};
}

void ResetOverlay() { g_ov = Overlay(); }

// Clears every fake and the hook. `base` arms the hook with that load base (0: unarmed); `overlay` installs the overlay's
// game functions. Logging is on (ArmForTest's logRequests) whenever the hook is armed.
void Reset(std::uint64_t base = 0, bool overlay = false) {
  g_fill = Fill();
  g_threadedCalls = 0;
  ResetOverlay();
  {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_lines.clear();
  }
  rr::ResetForTest();
  rr::FillThunk::Reset();
  *rr::FillThunk::OriginalOut() = reinterpret_cast<void*>(&FakeFill);
  SetManifest(DefaultKeys());
  g_callManager = reinterpret_cast<CResourceManagerOpaque*>(&g_manager);
  rr::SetGameFnsForTest(overlay ? &kFakeFns : nullptr);
  if (base != 0) rr::ArmForTest(base, true);
}

long long MyTid() { return static_cast<long long>(::syscall(SYS_gettid)); }

// A hit reaches the game's function once and never the overlay; a miss the overlay serves never reaches the game.
void ExpectHit(std::uint64_t type, std::uint64_t name) {
  unsigned char request[layout::kRequestBytes] = {};
  const int calls = g_fill.calls;
  const int overlayCalls = Total(g_ov);
  QCHECK(Call(type, name, 0, request) == 0);
  QCHECK(g_fill.calls == calls + 1);
  QCHECK(Total(g_ov) == overlayCalls);
}
void ExpectServedMiss(std::uint64_t type, std::uint64_t name) {
  unsigned char request[layout::kRequestBytes] = {};
  const int calls = g_fill.calls;
  const int clears = g_ov.clearCalls;
  QCHECK(Call(type, name, 0, request) == 0);
  QCHECK(g_fill.calls == calls);
  QCHECK(g_ov.clearCalls == clears + 1);
}

// ---- tests -----------------------------------------------------------------------------------------------------------

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
  QCHECK(g_fill.manager == g_callManager);
  QCHECK(g_fill.type == kBaseType && g_fill.name == 0x22 && g_fill.index == 1);
  QCHECK(g_fill.unused == kRegisterStub);
  QCHECK(g_fill.owner == &ownerObject && g_fill.allocator == &allocatorObject && g_fill.request == request);
  // The handler wrote nothing to the request after the game's function filled it.
  QCHECK(g_fill.wroteAny && std::memcmp(request, g_fill.wrote, layout::kRequestBytes) == 0);
  // A miss with no overlay returns the game's error object, unchanged.
  QCHECK(Call(kBaseType, 0x33, 0, request) == kMissReturn);
}

void AHitIsLoggedWithTheKeyTheOwnerTheSiteAndTheFilledFields() {
  Reset(1, true);
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
  Reset(1, true);
  unsigned char request[layout::kRequestBytes] = {};
  for (int i = 0; i < 9; ++i) Call(kBaseType, 0x55, 0, request);
  std::vector<std::string> seen;
  for (const std::string& l : Lines("resource_request")) seen.push_back(Field(l, "seen"));
  QCHECK((seen == std::vector<std::string>{"1", "2", "4", "8"}));
  Reset(1, true);
  Call(kBaseType, 0x55, 0, request);
  Call(kBaseType, 0x55, 1, request);        // another index
  Call(kBaseType, 0x56, 0, request);        // another name
  Call(kMipType, 0x55, 0, request);         // another type
  QCHECK(Lines("resource_request").size() == 4);
  for (const std::string& l : Lines("resource_request")) QCHECK(Field(l, "seen") == "1");
}

void AMissIsLoggedAsAMissWithoutReadingTheRequest() {
  Reset(1);  // no overlay: the game's own error
  // A null request on a miss: the handler must not read it.
  QCHECK(Call(kBaseType, kMissName, 2, nullptr) == kMissReturn);
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
  Call(kBaseType, kMissName, 2, nullptr);
  Call(kBaseType, kMissName, 2, nullptr);
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
  rr::ArmForTest(1, true);
  Call(kBaseType, 0x99, 0, request);
  const std::string first = Field(Lines("resource_request").back(), "caller");
  const unsigned long long off1 = std::strtoull(first.c_str() + 1, nullptr, 16);  // returnAddress - 1
  const std::uint64_t returnAddress = off1 + 1;
  Reset();
  rr::ArmForTest(returnAddress - knownReturn, true);
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
  g_fill.allHits = true;  // every key is a hit: the table is the subject, not the manifest
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
  g_fill.allHits = true;
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

// (1) The manifest search: every present key is a hit, and a key before, between and after the records is a miss.
void TheManifestSearchFindsEveryKeyAndOnlyThoseKeys() {
  const std::size_t sizes[] = {1, 2, 3, 8, 33};
  const std::uint64_t negativeType = 0xe8cc523d0fc9e5feULL;
  for (std::size_t n : sizes) {
    std::vector<Key> keys;
    for (std::size_t i = 0; i < n; ++i) {
      // Negative and positive keys, both types: the names are unique, so no two records are the same key.
      const std::int64_t type = (i % 3 == 0) ? static_cast<std::int64_t>(negativeType) : static_cast<std::int64_t>(i * 7) - 40;
      const std::int64_t name = (i % 2 == 0) ? -static_cast<std::int64_t>(i * 1000 + 5) : static_cast<std::int64_t>(0x100 * i + 3);
      keys.push_back(Key{type, name});
    }
    Reset(1, true);
    SetManifest(keys);
    for (const Key& k : keys) ExpectHit(static_cast<std::uint64_t>(k.type), static_cast<std::uint64_t>(k.name));
    // Probes just below, just above and between the present keys: none is in the table, so each is a miss the overlay serves.
    std::vector<Key> probes;
    for (const Key& k : keys) {
      probes.push_back(Key{k.type, k.name - 1});
      probes.push_back(Key{k.type, k.name + 1});
    }
    probes.push_back(Key{keys.front().type - 1, 0});  // before every record
    probes.push_back(Key{keys.back().type + 1, 0});   // after every record
    std::size_t misses = 0;
    for (const Key& p : probes) {
      if (std::find_if(keys.begin(), keys.end(), [&](const Key& k) { return k.type == p.type && k.name == p.name; }) != keys.end()) {
        continue;
      }
      ExpectServedMiss(static_cast<std::uint64_t>(p.type), static_cast<std::uint64_t>(p.name));
      ++misses;
    }
    QCHECK(misses > 0);
  }
}

// (2) A miss with a loose file present: the overlay serves it, the game's function is not called, and the request is the
// layout the cave writes.
void AMissWithALooseFileIsServedWithTheCavesLayout() {
  Reset(1, true);
  unsigned char request[layout::kRequestBytes];
  std::memset(request, 0xEE, sizeof(request));
  int ownerObject = 0, allocatorObject = 0;
  QCHECK(Call(kBaseType, kMissName, 2, request, &ownerObject, &allocatorObject) == 0);
  QCHECK(g_fill.calls == 0);  // the game's function was not called
  // clearRequest first, then the path, the open and the size, in that order.
  QCHECK(g_ov.clearCalls == 1 && g_ov.pathCalls == 1 && g_ov.openCalls == 1 && g_ov.sizeCalls == 1);
  QCHECK(g_ov.clearRequest == request);
  QCHECK(g_ov.clearSeq < g_ov.pathSeq);
  // The request fields.
  QCHECK(Get<std::uint64_t>(request, layout::kOwner) == reinterpret_cast<std::uintptr_t>(&ownerObject));
  QCHECK(Get<std::uint64_t>(request, layout::kIndex) == 2);
  QCHECK(Get<std::int64_t>(request, layout::kPackageHandle) == kOverlayHandle);
  QCHECK(Get<std::uint64_t>(request, layout::kSize) == kLooseSize);
  QCHECK(Get<std::uint64_t>(request, layout::kUncompressedSize) == kLooseSize);
  QCHECK(Get<std::uint64_t>(request, layout::kMemSpace) == reinterpret_cast<std::uintptr_t>(&allocatorObject));
  QCHECK(Get<std::uint32_t>(request, layout::kValid) == 1);
  QCHECK(std::strcmp(reinterpret_cast<const char*>(request + layout::kLoosePath), kLoosePath) == 0);
  // The calls' arguments.
  QCHECK(g_ov.pathManager == g_callManager);
  QCHECK(g_ov.pathType == kBaseType && g_ov.pathName == kMissName);
  QCHECK(g_ov.pathBuf == request + layout::kLoosePath);
  QCHECK(g_ov.openPath == reinterpret_cast<const char*>(request + layout::kLoosePath));
  QCHECK(g_ov.openFlags == 1);
  QCHECK(g_ov.openHandle == reinterpret_cast<long*>(request + layout::kPackageHandle));
  QCHECK(g_ov.sizeHandle == kOverlayHandle);
  QCHECK(rr::CurrentOverlayTotals().served == 1 && rr::CurrentOverlayTotals().failed == 0);
}

// (3) A stage failure: the game's function runs with the original arguments, its result is returned, the request holds
// what it left (cleared), and the failure is one WARN line naming the stage and the return value.
void AStageFailureFallsThroughToTheGame() {
  struct Case {
    const char* stage;
    int Overlay::*field;
    int ret;
  };
  const Case cases[] = {{"path", &Overlay::pathRet, 5}, {"open", &Overlay::openRet, 6}, {"size", &Overlay::sizeRet, 7}};
  for (const Case& c : cases) {
    Reset(1, true);
    g_ov.*(c.field) = c.ret;
    unsigned char request[layout::kRequestBytes];
    std::memset(request, 0xEE, sizeof(request));
    int ownerObject = 0, allocatorObject = 0;
    QCHECK(Call(kBaseType, kMissName, 2, request, &ownerObject, &allocatorObject) == kMissReturn);
    QCHECK(g_fill.calls == 1);
    QCHECK(g_fill.type == kBaseType && g_fill.name == kMissName && g_fill.index == 2);
    QCHECK(g_fill.owner == &ownerObject && g_fill.allocator == &allocatorObject && g_fill.request == request);
    QCHECK(g_fill.unused == kRegisterStub);
    // The game's function cleared the request; nothing the overlay wrote is left in it.
    unsigned char zero[layout::kRequestBytes] = {};
    QCHECK(std::memcmp(request, zero, sizeof(zero)) == 0);
    QCHECK(rr::CurrentOverlayTotals().failed == 1 && rr::CurrentOverlayTotals().served == 0);
    const std::vector<std::string> lines = Lines("resource_overlay_failed");
    QCHECK(lines.size() == 1);
    if (lines.size() == 1) {
      QCHECK(Field(lines[0], "stage") == std::string("\"") + c.stage + "\"");
      QCHECK(Field(lines[0], "ret") == std::to_string(c.ret));
    }
    QCHECK(Lines("resource_overlay_served").empty());
  }
}

// (4) A hit never touches the overlay.
void AHitNeverTouchesTheOverlay() {
  Reset(1, true);
  ExpectHit(kBaseType, 0x11);
  ExpectHit(kMipType, 0xabcdef0123456789ULL);
  QCHECK(g_ov.clearCalls == 0 && g_ov.pathCalls == 0 && g_ov.openCalls == 0 && g_ov.sizeCalls == 0);
  QCHECK(rr::CurrentOverlayTotals().served == 0 && rr::CurrentOverlayTotals().failed == 0);
}

// (5) The game's functions unavailable: a miss passes through to the game.
void WithoutTheGameFunctionsAMissPassesThrough() {
  Reset(1, false);
  unsigned char request[layout::kRequestBytes] = {};
  QCHECK(Call(kBaseType, kMissName, 0, request) == kMissReturn);
  QCHECK(g_fill.calls == 1);
  QCHECK(g_ov.clearCalls == 0 && g_ov.pathCalls == 0 && g_ov.openCalls == 0 && g_ov.sizeCalls == 0);
}

// (6) A null manager, and a null manifest pointer: the game's function runs, and nothing crashes.
void ANullManagerOrManifestPassesThrough() {
  Reset(1, true);
  unsigned char request[layout::kRequestBytes] = {};
  g_callManager = nullptr;
  QCHECK(Call(kBaseType, kMissName, 0, request) == kMissReturn);
  QCHECK(g_fill.calls == 1);
  g_callManager = reinterpret_cast<CResourceManagerOpaque*>(&g_manager);
  g_manager.manifest = nullptr;
  QCHECK(Call(kBaseType, kMissName, 0, request) == kMissReturn);
  QCHECK(g_fill.calls == 2);
  QCHECK(g_ov.clearCalls == 0);
  g_manager.manifest = &g_manifest;
}

// (7) A miss with no request: the game's function runs; the overlay is not called.
void AMissWithoutARequestPassesThrough() {
  Reset(1, true);
  QCHECK(Call(kBaseType, kMissName, 0, nullptr) == kMissReturn);
  QCHECK(g_fill.calls == 1);
  QCHECK(g_ov.clearCalls == 0);
}

// (8) Logging off: the overlay still serves and counts; no request line is written.
void WithLoggingOffTheOverlayStillServes() {
  Reset(1, true);
  rr::ArmForTest(1, false);
  unsigned char request[layout::kRequestBytes] = {};
  QCHECK(Call(kBaseType, 0x11, 0, request) == 0);  // a hit
  QCHECK(Call(kBaseType, kMissName, 0, request) == 0);  // a miss, served
  QCHECK(g_fill.calls == 1);
  QCHECK(rr::CurrentOverlayTotals().served == 1);
  QCHECK(Lines("resource_request").empty());
  QCHECK(Lines("resource_request_miss").empty());
  QCHECK(Lines("resource_overlay_served").empty());
  QCHECK(rr::CurrentTotals().lines == 0);
}

// (9) Logging on, a hit: the request line is as it was.
void ALoggedHitIsUnchanged() {
  Reset(1, true);
  unsigned char request[layout::kRequestBytes] = {};
  QCHECK(Call(kBaseType, 0x11, 0, request) == 0);
  const std::vector<std::string> lines = Lines("resource_request");
  QCHECK(lines.size() == 1);
  if (!lines.empty()) {
    QCHECK(Field(lines[0], "seen") == "1");
    QCHECK(lines[0].find("\"truncated\"") == std::string::npos);
  }
}

// (10) Served in development: one INFO line with the key, the size, the handle, the owner, the site and the thread; and no
// request line for it.
void AServedMissIsOneOverlayLine() {
  Reset(1, true);
  unsigned char request[layout::kRequestBytes] = {};
  int ownerObject = 0;
  QCHECK(Call(kBaseType, kMissName, 3, request, &ownerObject) == 0);
  QCHECK(Lines("resource_request").empty());
  QCHECK(Lines("resource_request_miss").empty());
  const std::vector<std::string> lines = Lines("resource_overlay_served");
  QCHECK(lines.size() == 1);
  if (lines.size() != 1) return;
  const std::string& l = lines[0];
  char hex[19];
  QCHECK(Field(l, "type") == "\"0x4a4c32c49300b8a0\"");
  QCHECK(Field(l, "name") == "\"0x0000000000000066\"");
  QCHECK(Field(l, "index") == "3");
  QCHECK(Field(l, "owner") == std::string("\"") + sentinel::HexString(hex, reinterpret_cast<std::uint64_t>(&ownerObject)) + "\"");
  QCHECK(Field(l, "size") == std::to_string(kLooseSize));
  QCHECK(Field(l, "handle") == std::to_string(kOverlayHandle));
  QCHECK(Field(l, "site") == "\"unknown\"");
  QCHECK(Field(l, "tid") == std::to_string(MyTid()));
  QCHECK(l.find("\"level\":\"info\"") != std::string::npos);
}

// (11) The overlay's line cap: 32 lines of served and failed in all, then one cap line; the counters keep counting.
void TheOverlayLineCapIsBoundedAndSaysSoOnce() {
  Reset(1, true);
  unsigned char request[layout::kRequestBytes] = {};
  for (int i = 0; i < 100; ++i) {
    g_ov.pathRet = (i % 4 == 3) ? 5 : 0;  // a quarter of the misses fail at the path stage
    Call(kBaseType, kMissName, 0, request);
  }
  QCHECK(rr::CurrentOverlayTotals().served == 75);
  QCHECK(rr::CurrentOverlayTotals().failed == 25);
  QCHECK(Lines("resource_overlay_served").size() + Lines("resource_overlay_failed").size() == rr::kOverlayLines);
  QCHECK(Lines("resource_overlay_cap").size() == 1);
  QCHECK(Lines("resource_overlay_served").size() + Lines("resource_overlay_failed").size() == 32);
}

// (12) The resolve: every symbol at its pinned address; one null; one wrong address.
void TheGameFunctionsResolveOnlyAtTheirPinnedAddresses() {
  const std::uint64_t base = 0x7000000000ULL;
  g_lookupBase = base;
  g_lookupNull = nullptr;
  g_lookupWrong = nullptr;
  rr::GameFns out{};
  Reset();
  QCHECK(rr::ResolveGameFnsForTest(base, &FakeLookup, &out) == nullptr);
  QCHECK(out.clearRequest != nullptr && out.unpackagedPath != nullptr && out.open != nullptr && out.size != nullptr);
  QCHECK(Lines("resource_overlay_resolve").empty());

  Reset();
  g_lookupNull = sentinel::pinned::kCSysFileOpenSymbol;
  const char* refused = rr::ResolveGameFnsForTest(base, &FakeLookup, &out);
  QCHECK(refused != nullptr && std::strcmp(refused, sentinel::pinned::kCSysFileOpenSymbol) == 0);
  std::vector<std::string> lines = Lines("resource_overlay_resolve");
  QCHECK(lines.size() == 1);
  if (lines.size() == 1) {
    QCHECK(Field(lines[0], "result") == "\"refused\"");
    QCHECK(Field(lines[0], "symbol") == std::string("\"") + sentinel::pinned::kCSysFileOpenSymbol + "\"");
    char hex[19];
    QCHECK(Field(lines[0], "expected") == std::string("\"") + sentinel::HexString(hex, base + sentinel::pinned::kCSysFileOpenVaddr) + "\"");
    QCHECK(Field(lines[0], "actual") == "\"0x0000000000000000\"");
  }
  g_lookupNull = nullptr;

  Reset();
  g_lookupWrong = sentinel::pinned::kCSysFileSizeSymbol;
  refused = rr::ResolveGameFnsForTest(base, &FakeLookup, &out);
  QCHECK(refused != nullptr && std::strcmp(refused, sentinel::pinned::kCSysFileSizeSymbol) == 0);
  lines = Lines("resource_overlay_resolve");
  QCHECK(lines.size() == 1);
  if (lines.size() == 1) {
    char expectedHex[19], actualHex[19];
    const std::uint64_t expected = base + sentinel::pinned::kCSysFileSizeVaddr;
    QCHECK(Field(lines[0], "expected") == std::string("\"") + sentinel::HexString(expectedHex, expected) + "\"");
    QCHECK(Field(lines[0], "actual") == std::string("\"") + sentinel::HexString(actualHex, expected + 8) + "\"");
  }
  g_lookupWrong = nullptr;
}

// (13) The counters: four, and a refused registration says how many it got.
void TheCountersAreRegisteredAndARefusedOneIsSaid() {
  sentinel::StopReporter();  // forget anything registered earlier in this process
  Reset();
  QCHECK(rr::RegisterCounters());
  QCHECK(Lines("resource_request_counters").empty());
  sentinel::StopReporter();
  // One slot left of the four it needs: refused, said once, false.
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
    QCHECK(Field(lines[0], "registered") == "1" && Field(lines[0], "wanted") == "4");
  }
  sentinel::StopReporter();
  QCHECK(rr::kCounterCount == 4);
}

// (14) Without libr15 the install is refused and says why; the counters being refused does not stop the install's own line.
void WithoutLibr15TheInstallIsRefusedAndSaysWhy() {
  Reset();
  QCHECK(!rr::Install(true));
  const std::vector<std::string> lines = Lines("resource_request_install");
  QCHECK(lines.size() == 1);
  if (!lines.empty()) {
    QCHECK(Field(lines[0], "result") == "\"failed\"");
    QCHECK(Field(lines[0], "slot") == "\"module_not_loaded\"");
    QCHECK(Field(lines[0], "load_base") == "\"0x0000000000000000\"");
    QCHECK(Field(lines[0], "overlay") == "\"module_not_loaded\"");
    QCHECK(Field(lines[0], "log") == "1");
    QCHECK(Field(lines[0], "counters") == "\"registered\"");
  }
  Reset();
  QCHECK(!rr::InstallCounted(false, false));
  const std::vector<std::string> refused = Lines("resource_request_install");
  QCHECK(refused.size() == 1);
  if (!refused.empty()) {
    QCHECK(Field(refused[0], "counters") == "\"refused\"");
    QCHECK(Field(refused[0], "log") == "0");
  }
}

// (15) Concurrency: four threads, a hit and a miss each time: no crash, and the counts add up.
void ConcurrentHitsAndServedMissesAddUp() {
  Reset(1, true);
  std::atomic<int> nonzero{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&nonzero] {
      unsigned char request[layout::kRequestBytes] = {};
      for (int i = 0; i < 100; ++i) {
        const std::uint64_t name = (i % 2 == 0) ? 0xbb : kMissName;
        if (Call(kBaseType, name, 0, request) != 0) nonzero.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }
  for (std::thread& t : threads) t.join();
  QCHECK(nonzero.load() == 0);
  QCHECK(g_threadedCalls.load() == 200);                  // the hits reached the game
  QCHECK(rr::CurrentOverlayTotals().served == 200);       // the misses were served
  QCHECK(g_ov.clearCalls == 200);
  QCHECK(g_threadedCalls.load() + static_cast<int>(rr::CurrentOverlayTotals().served) == 400);
}

// (16) A served request returns exactly 0, and the game's function (which would return nonzero) is not reached.
void AServedRequestReturnsZeroAndTheGameIsNotCalled() {
  Reset(1, true);
  unsigned char request[layout::kRequestBytes] = {};
  const std::uint64_t r = Call(kBaseType, kMissName, 0, request);
  QCHECK(r == 0);
  QCHECK(g_fill.calls == 0);
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
  TheManifestSearchFindsEveryKeyAndOnlyThoseKeys();
  AMissWithALooseFileIsServedWithTheCavesLayout();
  AStageFailureFallsThroughToTheGame();
  AHitNeverTouchesTheOverlay();
  WithoutTheGameFunctionsAMissPassesThrough();
  ANullManagerOrManifestPassesThrough();
  AMissWithoutARequestPassesThrough();
  WithLoggingOffTheOverlayStillServes();
  ALoggedHitIsUnchanged();
  AServedMissIsOneOverlayLine();
  TheOverlayLineCapIsBoundedAndSaysSoOnce();
  TheGameFunctionsResolveOnlyAtTheirPinnedAddresses();
  ConcurrentHitsAndServedMissesAddUp();
  AServedRequestReturnsZeroAndTheGameIsNotCalled();
  sentinel::SetLogSink(nullptr);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "resource_request_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("resource_request_hook_test: all checks passed\n");
  return 0;
}
