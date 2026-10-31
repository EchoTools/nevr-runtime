// Host test for the Quest main-menu cards (#966): the real handler (src/quest/sentinel/menu_cards_hook.cpp), built without
// exceptions as the sentinel builds it, driven through the thunk's entry against stand-ins for the game: a callback
// block and the resource it names laid out as the pinned libr15 reads them (menu_cards_swap.h), a CAllocator with the
// virtual table slots FreeResource and the streaming worker use, and the game's callback. No GOT slot is touched:
// got_pinned_test.cpp resolves the pinned slot in the real library, and tests/quest TestHookFramesCarryNoPersonality
// covers the frames.
//
// Built and run by `just test-quest-hooks`.

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "friends_panel_swap.h"
#include "hook_log.h"
#include "menu_cards_hook.h"
#include "menu_cards_swap.h"
#include "pinned_targets.h"
#include "quest/data/sha256.h"
#include "quest/tests/test_check.h"
#include "runtime/patch/menu_cards.h"
#include "runtime/patch/menu_page.h"
#include "runtime/patch/netgun_bullets.h"
#include "runtime/patch/friends_canvas.h"
#include "runtime/patch/friends_level.h"
#include "runtime/patch/ui_canvas.h"

namespace {

namespace mc = nevr_quest::menu_cards;
using Thunk = sentinel::pinned::LibR15AsyncResourceIOCallbackThunk;

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

std::string Field(const std::string& line, const char* key) {
  const std::string needle = std::string("\"") + key + "\":";
  const std::size_t at = line.find(needle);
  if (at == std::string::npos) return "<absent>";
  const std::size_t begin = at + needle.size();
  const std::size_t end = line.find_first_of(",}", begin);
  return line.substr(begin, end - begin);
}

// ---- the game's allocator --------------------------------------------------------------------------

struct FakeAllocator {
  void** table;
};
std::atomic<int> g_allocCalls{0};
std::atomic<int> g_freeCalls{0};
std::uint64_t g_lastAlignment = 0, g_lastAllocSize = 0;
bool g_allocFails = false;
std::mutex g_allocMutex;
std::vector<void*> g_allocated;

void* FakeAlloc(void* /*allocator*/, std::uint64_t alignment, std::uint64_t size) {
  g_allocCalls.fetch_add(1);
  std::lock_guard<std::mutex> lock(g_allocMutex);
  g_lastAlignment = alignment;
  g_lastAllocSize = size;
  if (g_allocFails) return nullptr;
  void* p = ::operator new(static_cast<std::size_t>(size));
  g_allocated.push_back(p);
  return p;
}
void FakeFree(void* /*allocator*/, void* /*pointer*/) { g_freeCalls.fetch_add(1); }

void* g_table[16];
FakeAllocator g_alloc{g_table};       // the allocator the context releases buffers through
FakeAllocator g_otherAlloc{g_table};  // another one (the GPU allocator, a caller's)

// ---- the game's memory context and callback --------------------------------------------------------

unsigned char g_context[0x40];
void* g_contextPointer = g_context;  // CMemoryContext::context: the 8-byte slot holds this

struct Seen {
  int calls = 0;
  unsigned char* buffer = nullptr;
  std::uint64_t size = 0;
  std::vector<unsigned char> bytes;  // the buffer's content as the callback saw it
};
Seen g_seen;
std::mutex g_seenMutex;
constexpr std::uint64_t kCallbackResult = 0x5555aaaa1234ULL;

std::uint64_t FakeCallback(unsigned char* block) {
  std::lock_guard<std::mutex> lock(g_seenMutex);
  ++g_seen.calls;
  g_seen.buffer = mc::Load<unsigned char*>(block, mc::block::kBuffer);
  g_seen.size = mc::Load<std::uint64_t>(block, mc::block::kSize);
  g_seen.bytes.assign(g_seen.buffer, g_seen.buffer + g_seen.size);
  return kCallbackResult;
}

// ---- a block and a resource ------------------------------------------------------------------------

struct Resource {
  unsigned char bytes[0x60] = {};
};
struct Block {
  unsigned char bytes[0x40] = {};
};

void Fill(Block& b, Resource& r, std::uint64_t status, std::uint32_t index, unsigned char* buffer, std::uint64_t size,
          void* allocator, std::uint64_t alignment, std::uint64_t classSymbol, std::uint64_t nameSymbol) {
  b = Block();
  r = Resource();
  mc::Store<std::uint64_t>(r.bytes, mc::resource::kClass, classSymbol);
  mc::Store<std::uint64_t>(r.bytes, mc::resource::kName, nameSymbol);
  mc::Store<std::uint64_t>(b.bytes, mc::block::kStatus, status);
  mc::Store<void*>(b.bytes, mc::block::kResource, r.bytes);
  mc::Store<std::uint32_t>(b.bytes, mc::block::kIndex, index);
  mc::Store<unsigned char*>(b.bytes, mc::block::kBuffer, buffer);
  mc::Store<std::uint64_t>(b.bytes, mc::block::kSize, size);
  mc::Store<void*>(b.bytes, mc::block::kAllocator, allocator);
  mc::Store<std::uint64_t>(b.bytes, mc::block::kAlignment, alignment);
}

// A page resource of the format menu_page.h reads: 25 pages with no element, so that page 24 exists.
void Desc(std::vector<unsigned char>& b, std::size_t at, std::uint64_t count, std::uint64_t stride) {
  std::memset(b.data() + at, 0, 56);
  const std::uint64_t bytes = count * stride, mark = 0x20;
  const std::uint32_t flags = 1;
  std::memcpy(b.data() + at + 0x08, &bytes, 8);
  std::memcpy(b.data() + at + 0x1C, &flags, 4);
  std::memcpy(b.data() + at + 0x20, &mark, 8);
  std::memcpy(b.data() + at + 0x28, &count, 8);
  std::memcpy(b.data() + at + 0x30, &count, 8);
}
std::vector<unsigned char> StockPage() {
  std::vector<unsigned char> b(56 + 25 * 328, 0);
  Desc(b, 0, 25, 328);
  for (std::size_t p = 0; p < 25; ++p) {
    const std::size_t rec = 56 + p * 328;
    Desc(b, rec + 32, 0, 472);
    Desc(b, rec + 88, 0, 16);
    Desc(b, rec + 144, 0, 16);
    Desc(b, rec + 200, 0, 16);
    Desc(b, rec + 272, 0, 16);
  }
  return b;
}

std::string Sha(const std::vector<unsigned char>& v) { return nevr_quest::data::Sha256Hex(v.data(), v.size()); }

// The call the game makes: through the thunk's entry.
__attribute__((noinline)) std::uint64_t Call(Block& block) {
  const Thunk::Fn entry = reinterpret_cast<Thunk::Fn>(Thunk::EntryAddress());
  const std::uint64_t r = entry(block.bytes);
  asm volatile("" ::: "memory");
  return r;
}

std::string g_pinStorage;  // the pin the armed handler reads (it keeps the pointer)

void Reset(const std::string& pin, bool withContext = true) {
  {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_lines.clear();
  }
  {
    std::lock_guard<std::mutex> lock(g_seenMutex);
    g_seen = Seen();
  }
  {
    std::lock_guard<std::mutex> lock(g_allocMutex);
    for (void* p : g_allocated) ::operator delete(p);
    g_allocated.clear();
    g_lastAlignment = g_lastAllocSize = 0;
    g_allocFails = false;
  }
  g_allocCalls = 0;
  g_freeCalls = 0;
  g_table[0x30 / sizeof(void*)] = reinterpret_cast<void*>(&FakeAlloc);
  g_table[0x40 / sizeof(void*)] = reinterpret_cast<void*>(&FakeFree);
  std::memset(g_context, 0, sizeof(g_context));
  void* allocatorField = &g_alloc;
  std::memcpy(g_context + mc::kContextAllocatorOffset, &allocatorField, sizeof(allocatorField));
  mc::ResetForTest();
  Thunk::Reset();
  *Thunk::OriginalOut() = reinterpret_cast<void*>(&FakeCallback);
  g_pinStorage = pin;
  mc::ArmForTest(g_pinStorage.c_str(), withContext ? &g_contextPointer : nullptr);
}

// The Quest page is the resource of class CR15UIPage2CR under the name mnu_master_lowspec. The name is also the name of the
// main-menu level, so about sixty resources of other classes arrive under it (headset run 2); only the class picks the page.
constexpr std::uint64_t kPage = nevr_menu_cards::kPageType, kName = nevr_menu_cards::kQuestPageName;

// ---- tests -----------------------------------------------------------------------------------------

void AnUnarmedThunkPassesThroughAndTouchesNothing() {
  Reset("");
  Thunk::Disarm();
  std::vector<unsigned char> page = StockPage();
  Block block;
  Resource res;
  Fill(block, res, 4, 0, page.data(), page.size(), &g_alloc, 16, kPage, kName);
  const Block before = block;
  QCHECK(Call(block) == kCallbackResult);
  QCHECK(g_seen.calls == 1 && g_seen.buffer == page.data() && g_seen.size == page.size());
  QCHECK(std::memcmp(&before, &block, sizeof(block)) == 0);
  QCHECK(Lines("menu_cards_page").empty());
}

void APinnedPageIsServedWithTheCardsThroughTheGamesAllocator() {
  std::vector<unsigned char> page = StockPage();
  Reset(Sha(page));
  Block block;
  Resource res;
  Fill(block, res, 4, 0, page.data(), page.size(), &g_alloc, 0x10, kPage, kName);
  QCHECK(Call(block) == kCallbackResult);  // the game's result comes back as it came
  QCHECK(g_seen.calls == 1);
  QCHECK(g_seen.buffer != page.data() && g_seen.size > page.size());
  QCHECK(g_allocCalls == 1 && g_lastAllocSize == g_seen.size && g_lastAlignment == 0x10);
  QCHECK(g_freeCalls == 0);  // the game's own buffer is not released here
  // What the game's callback saw is the page with the two cards: it decodes, and holds both owners.
  nevr_menu_page::Decoded decoded;
  QCHECK(nevr_menu_page::Decode(g_seen.bytes.data(), g_seen.bytes.size(), &decoded) == nevr_menu_page::Status::Ok);
  QCHECK(decoded.owners.size() == 2);
  QCHECK(decoded.owners.size() == 2 && decoded.owners[0] == nevr_menu_cards::kCreateOwner &&
         decoded.owners[1] == nevr_menu_cards::kJoinOwner);
  // The original buffer is untouched.
  QCHECK(page == StockPage());
  const std::vector<std::string> lines = Lines("menu_cards_swap");
  QCHECK(lines.size() == 1);
  if (!lines.empty()) {
    QCHECK(Field(lines[0], "bytes_before") == std::to_string(page.size()));
    QCHECK(Field(lines[0], "bytes_after") == std::to_string(g_seen.size));
  }
  QCHECK(mc::CurrentTotals().pages == 1 && mc::CurrentTotals().swapped == 1);
}

void TheDigestIsLoggedWhateverThePinIs() {
  std::vector<unsigned char> page = StockPage();
  const std::vector<std::string> pins = {std::string(""), std::string(64, '0'), Sha(page)};
  for (const std::string& pin : pins) {
    Reset(pin);
    Block block;
    Resource res;
    Fill(block, res, 4, 0, page.data(), page.size(), &g_alloc, 0x10, kPage, kName);
    Call(block);
    const std::vector<std::string> lines = Lines("menu_cards_page");
    QCHECK(lines.size() == 1);
    if (lines.empty()) continue;
    QCHECK(Field(lines[0], "sha256") == "\"" + Sha(page) + "\"");
    QCHECK(Field(lines[0], "bytes") == std::to_string(page.size()));
    const std::string state = pin.empty() ? "unset" : (pin == Sha(page) ? "match" : "mismatch");
    QCHECK(Field(lines[0], "pin") == "\"" + state + "\"");
  }
}

void AnUnsetPinServesTheStockPageAndSaysWhy() {
  std::vector<unsigned char> page = StockPage();
  Reset("");
  Block block;
  Resource res;
  Fill(block, res, 4, 0, page.data(), page.size(), &g_alloc, 0x10, kPage, kName);
  QCHECK(Call(block) == kCallbackResult);
  QCHECK(g_seen.buffer == page.data() && g_seen.size == page.size() && g_allocCalls == 0);
  const std::vector<std::string> lines = Lines("menu_cards_skip");
  QCHECK(lines.size() == 1 && Field(lines[0], "reason") == "\"pin_unset\"");
}

void AnotherPageIsLeftAlone() {
  std::vector<unsigned char> page = StockPage();
  Reset(std::string(64, '0'));
  Block block;
  Resource res;
  Fill(block, res, 4, 0, page.data(), page.size(), &g_alloc, 0x10, kPage, kName);
  QCHECK(Call(block) == kCallbackResult);
  QCHECK(g_seen.buffer == page.data() && g_allocCalls == 0);
  const std::vector<std::string> lines = Lines("menu_cards_skip");
  QCHECK(lines.size() == 1 && Field(lines[0], "reason") == "\"page_not_pinned\"");
}

void OnlyTheMainMenuPageInItsFinishedPrimaryBufferIsOffered() {
  std::vector<unsigned char> page = StockPage();
  struct Case {
    const char* what;
    std::uint64_t status;
    std::uint32_t index;
    std::uint64_t cls, name;
  };
  const Case cases[] = {
      {"status not done", 5, 0, kPage, kName},     {"status zero", 0, 0, kPage, kName},
      {"secondary buffer", 4, 1, kPage, kName},    {"another class", 4, 0, kPage ^ 1, kName},
      {"another name", 4, 0, kPage, kName ^ 1},
      {"the PC page name", 4, 0, kPage, nevr_menu_cards::kPageName},
  };
  for (const Case& c : cases) {
    Reset(Sha(page));
    Block block;
    Resource res;
    Fill(block, res, c.status, c.index, page.data(), page.size(), &g_alloc, 0x10, c.cls, c.name);
    QCHECK(Call(block) == kCallbackResult);
    if (g_seen.buffer != page.data() || g_allocCalls != 0 || !Lines("menu_cards_page").empty()) {
      std::fprintf(stderr, "case '%s' was offered\n", c.what);
      QCHECK(false);
    }
  }
}

void ABlockWithoutBytesOrAResourceIsPassedThrough() {
  Reset("");
  Block block;
  Resource res;
  Fill(block, res, 4, 0, nullptr, 0, &g_alloc, 0x10, kPage, kName);
  QCHECK(Call(block) == kCallbackResult && g_seen.calls == 1);
  Fill(block, res, 4, 0, reinterpret_cast<unsigned char*>(&res), 8, &g_alloc, 0x10, kPage, kName);
  mc::Store<void*>(block.bytes, mc::block::kResource, nullptr);  // no resource: the symbols read as zero
  QCHECK(Call(block) == kCallbackResult && g_seen.calls == 2);
  QCHECK(Lines("menu_cards_page").empty() && g_allocCalls == 0);
}

void AnOversizeBlockIsNeverReadOrHashed() {
  Reset("");
  Block block;
  Resource res;
  // The pointer is bogus: reading it would fault. The size is past anything a page can be.
  Fill(block, res, 4, 0, reinterpret_cast<unsigned char*>(0x8), nevr_menu_page::kMaxResourceBytes + 1, &g_alloc, 0x10,
       kPage, kName);
  Thunk::Reset();
  static std::uint64_t seenSize = 0;
  *Thunk::OriginalOut() = reinterpret_cast<void*>(+[](unsigned char* b) -> std::uint64_t {
    seenSize = mc::Load<std::uint64_t>(b, mc::block::kSize);
    return kCallbackResult;
  });
  mc::ArmForTest("", &g_contextPointer);
  QCHECK(Call(block) == kCallbackResult);
  QCHECK(seenSize == nevr_menu_page::kMaxResourceBytes + 1);
  const std::vector<std::string> lines = Lines("menu_cards_skip");
  QCHECK(lines.size() == 1 && Field(lines[0], "reason") == "\"page_too_large\"");
  QCHECK(Lines("menu_cards_page").empty());
}

void AnAllocatorThatIsNotTheOneFreeResourceUsesIsNotUsed() {
  std::vector<unsigned char> page = StockPage();
  Reset(Sha(page));
  Block block;
  Resource res;
  Fill(block, res, 4, 0, page.data(), page.size(), &g_otherAlloc, 0x10, kPage, kName);
  QCHECK(Call(block) == kCallbackResult);
  QCHECK(g_seen.buffer == page.data() && g_allocCalls == 0);
  const std::vector<std::string> lines = Lines("menu_cards_skip");
  QCHECK(lines.size() == 1 && Field(lines[0], "reason") == "\"allocator_mismatch\"");
}

void WithoutAContextOrAnAllocatorTheStockPageIsKept() {
  std::vector<unsigned char> page = StockPage();
  Reset(Sha(page), /*withContext=*/false);
  Block block;
  Resource res;
  Fill(block, res, 4, 0, page.data(), page.size(), &g_alloc, 0x10, kPage, kName);
  QCHECK(Call(block) == kCallbackResult && g_seen.buffer == page.data() && g_allocCalls == 0);
  QCHECK(Lines("menu_cards_skip").size() == 1 && Field(Lines("menu_cards_skip")[0], "reason") == "\"no_allocator\"");
  // A context whose pointer is still null (the game has not made one).
  Reset(Sha(page));
  g_contextPointer = nullptr;
  Fill(block, res, 4, 0, page.data(), page.size(), &g_alloc, 0x10, kPage, kName);
  QCHECK(Call(block) == kCallbackResult && g_seen.buffer == page.data() && g_allocCalls == 0);
  g_contextPointer = g_context;
  // A block that names no allocator.
  Reset(Sha(page));
  Fill(block, res, 4, 0, page.data(), page.size(), nullptr, 0x10, kPage, kName);
  QCHECK(Call(block) == kCallbackResult && g_seen.buffer == page.data() && g_allocCalls == 0);
}

void AFailedAllocationLeavesTheBlockAsItWas() {
  std::vector<unsigned char> page = StockPage();
  Reset(Sha(page));
  g_allocFails = true;
  Block block;
  Resource res;
  Fill(block, res, 4, 0, page.data(), page.size(), &g_alloc, 0x10, kPage, kName);
  const Block before = block;
  QCHECK(Call(block) == kCallbackResult);
  QCHECK(g_allocCalls == 1 && g_seen.buffer == page.data() && g_seen.size == page.size());
  QCHECK(std::memcmp(&before, &block, sizeof(block)) == 0);
  const std::vector<std::string> lines = Lines("menu_cards_skip");
  QCHECK(lines.size() == 1 && Field(lines[0], "reason") == "\"alloc_failed\"");
  QCHECK(mc::CurrentTotals().swapped == 0);
}

void EveryLoadGetsItsOwnCopyOfThePageBuiltOnce() {
  std::vector<unsigned char> page = StockPage();
  Reset(Sha(page));
  std::vector<unsigned char> first;
  for (int i = 0; i < 3; ++i) {
    Block block;
    Resource res;
    Fill(block, res, 4, 0, page.data(), page.size(), &g_alloc, 0x10, kPage, kName);
    QCHECK(Call(block) == kCallbackResult);
    if (i == 0) {
      first = g_seen.bytes;
    } else {
      QCHECK(g_seen.bytes == first);
    }
  }
  QCHECK(g_allocCalls == 3);
  QCHECK(g_allocated.size() == 3 && g_allocated[0] != g_allocated[1] && g_allocated[1] != g_allocated[2]);
  QCHECK(mc::CurrentTotals().swapped == 3);
}

void ConcurrentLoadsAllGetTheSamePage() {
  std::vector<unsigned char> page = StockPage();
  Reset(Sha(page));
  std::vector<std::thread> threads;
  std::atomic<int> same{0};
  std::vector<unsigned char> expected;
  {
    Block block;
    Resource res;
    Fill(block, res, 4, 0, page.data(), page.size(), &g_alloc, 0x10, kPage, kName);
    Call(block);
    expected = g_seen.bytes;
  }
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&] {
      for (int i = 0; i < 20; ++i) {
        Block block;
        Resource res;
        Fill(block, res, 4, 0, page.data(), page.size(), &g_alloc, 0x10, kPage, kName);
        const Thunk::Fn entry = reinterpret_cast<Thunk::Fn>(Thunk::EntryAddress());
        entry(block.bytes);
        const unsigned char* got = mc::Load<unsigned char*>(block.bytes, mc::block::kBuffer);
        const std::uint64_t n = mc::Load<std::uint64_t>(block.bytes, mc::block::kSize);
        if (n == expected.size() && std::memcmp(got, expected.data(), n) == 0) same.fetch_add(1);
      }
    });
  }
  for (std::thread& t : threads) t.join();
  QCHECK(same == 160);
  QCHECK(mc::CurrentTotals().swapped == 161);
}

void OtherClassesUnderThePagesNameAreLeftAloneAndSilent() {
  // Headset run 2: sixty classes arrive under mnu_master_lowspec. None is the page, none is touched, none is logged as one.
  std::vector<unsigned char> page = StockPage();
  Reset(Sha(page));
  for (std::uint64_t i = 0; i < 60; ++i) {
    Block block;
    Resource res;
    Fill(block, res, 4, 0, page.data(), page.size(), &g_alloc, 0x10, 0xd4ce6a5004d8248cULL + i, kName);
    QCHECK(Call(block) == kCallbackResult);
    QCHECK(g_seen.buffer == page.data() && g_seen.size == page.size());
  }
  QCHECK(g_allocCalls == 0 && Lines("menu_cards_page").empty() && Lines("menu_cards_skip").empty());
  QCHECK(mc::CurrentTotals().pages == 0 && mc::CurrentTotals().swapped == 0 && mc::CurrentTotals().deliveries == 60);
  // The page itself, among them, is served and says which class and name it came as.
  Block block;
  Resource res;
  Fill(block, res, 4, 0, page.data(), page.size(), &g_alloc, 0x10, kPage, kName);
  QCHECK(Call(block) == kCallbackResult && g_seen.buffer != page.data() && mc::CurrentTotals().swapped == 1);
  const std::vector<std::string> lines = Lines("menu_cards_page");
  QCHECK(lines.size() == 1);
  if (!lines.empty()) {
    QCHECK(Field(lines[0], "class") == "\"0x75e5993f80940cd0\"");
    QCHECK(Field(lines[0], "name") == "\"0xf927772b9e2aefb1\"");
  }
}

void EachResourceClassIsNamedOnceUpToItsBound() {
  Reset("");
  std::vector<unsigned char> bytes(16, 0);
  for (int round = 0; round < 2; ++round) {
    for (std::uint64_t i = 0; i < mc::kClassLines + 4; ++i) {
      for (std::uint64_t n = 0; n < 3; ++n) {  // three names of the same class: still one class line
        Block block;
        Resource res;
        Fill(block, res, 4, 0, bytes.data(), bytes.size(), &g_alloc, 0x10, 0x1000 + i, 0x2000 + n);
        Call(block);
      }
    }
  }
  const std::vector<std::string> lines = Lines("menu_cards_class");
  QCHECK(lines.size() == mc::kClassLines);
  if (!lines.empty()) {
    QCHECK(Field(lines[0], "class") == "\"0x0000000000001000\"" && Field(lines[0], "name") == "\"0x0000000000002000\"");
    QCHECK(Field(lines[0], "bytes") == "16");
  }
  QCHECK(Lines("menu_cards_resource").empty());  // none of them is as large as a page
}

void AClassOfSevenTexturesDoesNotHideTheNextClass() {
  // The headset run that found the page filled the old eight-line listing with seven textures of one class.
  Reset("");
  std::vector<unsigned char> bytes(16, 0);
  for (std::uint64_t n = 0; n < 7; ++n) {
    Block block;
    Resource res;
    Fill(block, res, 4, 0, bytes.data(), bytes.size(), &g_alloc, 0x10, 0xb3ebe9e0bf0eb81fULL, 0x100 + n);
    Call(block);
  }
  Block block;
  Resource res;
  Fill(block, res, 4, 0, bytes.data(), bytes.size(), &g_alloc, 0x10, 0x857497d08cb4096bULL, 0x1);
  Call(block);
  const std::vector<std::string> lines = Lines("menu_cards_class");
  QCHECK(lines.size() == 2);
  if (lines.size() == 2) QCHECK(Field(lines[1], "class") == "\"0x857497d08cb4096b\"");
}

void APageSizedResourceIsNamedWhateverItsNameUpToItsBound() {
  Reset("");
  std::vector<unsigned char> big(81160, 0);
  for (int round = 0; round < 2; ++round) {
    for (std::uint64_t i = 0; i < mc::kBandLines + 4; ++i) {
      Block block;
      Resource res;
      Fill(block, res, 4, 0, big.data(), big.size(), &g_alloc, 0x10, 0x77, 0x9000 + i);
      Call(block);
    }
  }
  const std::vector<std::string> lines = Lines("menu_cards_resource");
  QCHECK(lines.size() == mc::kBandLines);
  if (!lines.empty()) {
    QCHECK(Field(lines[0], "class") == "\"0x0000000000000077\"" && Field(lines[0], "name") == "\"0x0000000000009000\"");
    QCHECK(Field(lines[0], "bytes") == "81160");
  }
  // The band's two edges are listed; just outside it is not.
  Reset("");
  std::vector<unsigned char> atMin(mc::kBandMin, 0), atMax(mc::kBandMax, 0);
  Block edge;
  Resource edgeRes;
  Fill(edge, edgeRes, 4, 0, atMin.data(), atMin.size(), &g_alloc, 0x10, 0x1, 0x2);
  Call(edge);
  Fill(edge, edgeRes, 4, 0, atMax.data(), atMax.size(), &g_alloc, 0x10, 0x1, 0x3);
  Call(edge);
  QCHECK(Lines("menu_cards_resource").size() == 2);
  Reset("");
  std::vector<unsigned char> small(mc::kBandMin - 1, 0), large(mc::kBandMax + 1, 0);
  Block block;
  Resource res;
  Fill(block, res, 4, 0, small.data(), small.size(), &g_alloc, 0x10, 0x1, 0x2);
  Call(block);
  Fill(block, res, 4, 0, large.data(), large.size(), &g_alloc, 0x10, 0x1, 0x3);
  Call(block);
  QCHECK(Lines("menu_cards_resource").empty());
}

void ARepeatedResourceIsNamedOnce() {
  Reset("");
  std::vector<unsigned char> bytes(16, 0);
  for (int i = 0; i < 5; ++i) {
    Block block;
    Resource res;
    Fill(block, res, 4, 0, bytes.data(), bytes.size(), &g_alloc, 0x10, 0x1234, 0x5678);
    Call(block);
  }
  QCHECK(Lines("menu_cards_class").size() == 1);
  QCHECK(mc::CurrentTotals().deliveries == 5);
}

void TheQuestPinIsTheDigestTheHeadsetLogged() {
  // Headset run 2 (APK 5.16.2-dev.13+9b381cae) logged this digest for class 0x75e5993f80940cd0, name 0xf927772b9e2aefb1,
  // 81160 bytes: the same digest as the PC stock page (kStockPageSha256), so the two pages are byte-identical.
  const std::string pin = nevr_menu_cards::kQuestStockPageSha256;
  QCHECK(pin == "af396701cfe27cb5d9fd512943b1ad732477b6399c4df5a2b36971375a6919eb");
  QCHECK(pin == nevr_menu_cards::kStockPageSha256);
  QCHECK(nevr_menu_cards::kPageType == 0x75e5993f80940cd0ULL && nevr_menu_cards::kQuestPageName == 0xf927772b9e2aefb1ULL);
}

// ---- the net-gun resources (#1478) -----------------------------------------------------------------

// The five combat levels' net-gun resources, of either class, whose bytes are not the pinned stock ones (a test has no game
// bytes, and the pins are the PC digests): the buffer is passed through, nothing is allocated, and the skip says not_pinned.
void ANetgunResourceOfAFiveLevelIsLeftAloneWhenItsBytesAreNotPinned() {
  for (const nevr_netgun_bullets::Level& level : nevr_netgun_bullets::kLevels) {
    for (const std::uint64_t cls : {nevr_netgun_bullets::kParticleType, nevr_netgun_bullets::kBulletType}) {
      Reset("");
      std::vector<unsigned char> buf(200, 0x11);
      Block block;
      Resource res;
      Fill(block, res, 4, 0, buf.data(), buf.size(), &g_alloc, 0x10, cls, level.hash);
      QCHECK(Call(block) == kCallbackResult);
      QCHECK(g_seen.buffer == buf.data() && g_seen.size == buf.size() && g_allocCalls == 0);
      const std::vector<std::string> lines = Lines("netgun_bullets_skip");
      QCHECK(lines.size() == 1 && Field(lines[0], "reason") == "\"not_pinned\"");
      QCHECK(Lines("netgun_bullets_swap").empty());
    }
  }
}

// A net-gun class under another level's name, another class under a level's name, and the page's class under a level's
// name, are not offered to the net-gun rewrite and are not logged as one.
void OnlyTheNetgunClassUnderAFiveLevelNameIsOffered() {
  const std::uint64_t level = nevr_netgun_bullets::kLevels[0].hash;
  struct Case {
    const char* what;
    std::uint64_t cls, name;
  };
  const Case cases[] = {
      {"a net-gun class under another name", nevr_netgun_bullets::kParticleType, 0x1234},
      {"another class under a level", nevr_netgun_bullets::kParticleType ^ 1, level},
      {"the page's class under a level", kPage, level},
  };
  for (const Case& c : cases) {
    Reset("");
    std::vector<unsigned char> buf(200, 0x11);
    Block block;
    Resource res;
    Fill(block, res, 4, 0, buf.data(), buf.size(), &g_alloc, 0x10, c.cls, c.name);
    QCHECK(Call(block) == kCallbackResult);
    if (g_seen.buffer != buf.data() || g_allocCalls != 0 || !Lines("netgun_bullets_skip").empty() ||
        !Lines("netgun_bullets_swap").empty()) {
      std::fprintf(stderr, "case '%s' was offered\n", c.what);
      QCHECK(false);
    }
  }
}

// ---- the FRIENDS panel (#1535) ---------------------------------------------------------------------

namespace fc = nevr_friends_canvas;
namespace uc = nevr_ui_canvas;
constexpr std::uint64_t kCanvasClass = nevr_quest::friends_panel::kCanvasType;

// A delivered SEASON INFO canvas as far as the panel takes from it: the button and picture behaviours.
std::vector<unsigned char> SeasonCanvas() {
  uc::Canvas c;
  c.behaviours.push_back(uc::MakeBehaviour(fc::kButtonStates, 0x0E7F320C56265C6CULL, 0, std::vector<std::uint8_t>(304, 7)));
  c.behaviours.push_back(uc::MakeBehaviour(fc::kPointerInteract, fc::kPointerInteract, 0, uc::Words({fc::kPointerInteract, 0})));
  c.behaviours.push_back(uc::MakeBehaviour(fc::kDynLoadTexture, fc::kDynLoadTexture, 0, uc::Words({fc::kDynLoadTexture, 0xBEEF})));
  return uc::Encode(c);
}

void ThePanelCanvasIsServedBuiltFromTheDeliveredOne() {
  Reset("");
  std::vector<unsigned char> season = SeasonCanvas();
  const std::vector<unsigned char> stock = season;
  Block block;
  Resource res;
  Fill(block, res, 4, 0, season.data(), season.size(), &g_alloc, 0x10, kCanvasClass, fc::kSeasonCanvas);
  QCHECK(Call(block) == kCallbackResult);
  QCHECK(g_seen.buffer != season.data() && g_allocCalls == 1 && g_freeCalls == 0);
  uc::Canvas served;
  QCHECK(uc::Decode(g_seen.bytes.data(), g_seen.bytes.size(), &served, nullptr));
  QCHECK(served.Find(EchoVR::CSymbol64Hash("friend4_name")) >= 0 && served.Find(EchoVR::CSymbol64Hash("play_btn")) >= 0);
  QCHECK(season == stock);  // the game's own buffer is not written
  const std::vector<std::string> lines = Lines("friends_panel_swap");
  QCHECK(lines.size() == 1);
  if (!lines.empty()) QCHECK(Field(lines[0], "resource") == "\"panel_canvas\"");
}

void APanelTableThatIsNotStockIsLeftAndSaysWhy() {
  Reset("");
  std::vector<unsigned char> junk(120, 0x11);
  Block block;
  Resource res;
  Fill(block, res, 4, 0, junk.data(), junk.size(), &g_alloc, 0x10, nevr_friends_level::kPointerMeshType,
       nevr_friends_level::kQuestLevel);
  QCHECK(Call(block) == kCallbackResult);
  QCHECK(g_seen.buffer == junk.data() && g_allocCalls == 0);
  const std::vector<std::string> lines = Lines("friends_panel_skip");
  QCHECK(lines.size() == 1);
  if (!lines.empty()) QCHECK(Field(lines[0], "resource") == "\"pointer_meshes\"");
}

void OnlyThePanelsClassesAndNamesAreOffered() {
  std::vector<unsigned char> season = SeasonCanvas();
  struct Case {
    const char* what;
    std::uint64_t cls, name;
  };
  const Case cases[] = {
      {"a canvas of another name", kCanvasClass, fc::kSeasonCanvas ^ 1},
      {"the season canvas's name under another class", kCanvasClass ^ 1, fc::kSeasonCanvas},
      {"a panel table under the PC level's name", nevr_friends_level::kScriptType, nevr_friends_level::kLevel},
  };
  for (const Case& c : cases) {
    Reset("");
    Block block;
    Resource res;
    Fill(block, res, 4, 0, season.data(), season.size(), &g_alloc, 0x10, c.cls, c.name);
    QCHECK(Call(block) == kCallbackResult);
    if (g_seen.buffer != season.data() || g_allocCalls != 0 || !Lines("friends_panel_swap").empty() ||
        !Lines("friends_panel_skip").empty()) {
      std::fprintf(stderr, "case '%s' was offered\n", c.what);
      QCHECK(false);
    }
  }
}

}  // namespace

int main() {
  sentinel::SetLogSink(&Sink);
  g_table[0] = nullptr;
  AnUnarmedThunkPassesThroughAndTouchesNothing();
  APinnedPageIsServedWithTheCardsThroughTheGamesAllocator();
  TheDigestIsLoggedWhateverThePinIs();
  AnUnsetPinServesTheStockPageAndSaysWhy();
  AnotherPageIsLeftAlone();
  OnlyTheMainMenuPageInItsFinishedPrimaryBufferIsOffered();
  ABlockWithoutBytesOrAResourceIsPassedThrough();
  AnOversizeBlockIsNeverReadOrHashed();
  AnAllocatorThatIsNotTheOneFreeResourceUsesIsNotUsed();
  WithoutAContextOrAnAllocatorTheStockPageIsKept();
  AFailedAllocationLeavesTheBlockAsItWas();
  EveryLoadGetsItsOwnCopyOfThePageBuiltOnce();
  ConcurrentLoadsAllGetTheSamePage();
  OtherClassesUnderThePagesNameAreLeftAloneAndSilent();
  EachResourceClassIsNamedOnceUpToItsBound();
  AClassOfSevenTexturesDoesNotHideTheNextClass();
  APageSizedResourceIsNamedWhateverItsNameUpToItsBound();
  ARepeatedResourceIsNamedOnce();
  TheQuestPinIsTheDigestTheHeadsetLogged();
  ANetgunResourceOfAFiveLevelIsLeftAloneWhenItsBytesAreNotPinned();
  OnlyTheNetgunClassUnderAFiveLevelNameIsOffered();
  ThePanelCanvasIsServedBuiltFromTheDeliveredOne();
  APanelTableThatIsNotStockIsLeftAndSaysWhy();
  OnlyThePanelsClassesAndNamesAreOffered();
  sentinel::SetLogSink(nullptr);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "menu_cards_hook_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("menu_cards_hook_test: all checks passed\n");
  return 0;
}
