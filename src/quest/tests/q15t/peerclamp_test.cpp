// Host test of peerclamp (src/quest/plugins/q15t/src/peerclamp.cpp): the wrapper installed by the real installer on a fake JUMP_SLOT page and called
// through the slot, against a fake SBroadcasterData (head, chunk table, per-chunk count, "now") with peer entries of the real
// stride (0x948), key at +0x18 and stamp at +0x228, and a fake SAddress::kInvalid. Real SafeRead, real mprotect.
#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/plugin_interface.h"
#include "peerclamp.h"
#include "pin_check.h"
#include "plugin_log.h"

static int g_fail = 0;
#define CHECK(cond, ...)                                \
  do {                                                  \
    if (!(cond)) {                                      \
      ++g_fail;                                         \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
      std::printf(__VA_ARGS__);                         \
      std::printf("\n");                                \
    }                                                   \
  } while (0)

struct Line {
  uint32_t level;
  std::string text;
};
static std::vector<Line> g_lines;
static void FakeLog(const char* id, uint32_t level, const char* msg, const NevrLogField* f, uint32_t n) {
  std::string t = std::string("plugin=") + id + " " + msg;
  for (uint32_t i = 0; i < n; ++i) t += std::string(" ") + f[i].key + "=" + f[i].value;
  g_lines.push_back({level, t});
}
static int CountLines(const char* needle, int level = -1) {
  int c = 0;
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || (int)l.level == level)) ++c;
  return c;
}

static const uint64_t kNow = 1000000;
static const uint32_t kEnd = 0xffffffffu;
static const size_t kStride = 0x948;

alignas(16) static uint8_t g_bd[0x600];
alignas(16) static uint8_t g_invalid_key[0x80];
static uintptr_t g_invalid_cell = 0;

// a peer list: `n` entries over chunks of `per`; entry i's key and stamp are set by the caller
struct Peers {
  std::vector<uint8_t*> chunks;
  std::vector<uintptr_t> table;  // the chunk pointer table the broadcaster points at
  uint32_t per = 0;
  ~Peers() {
    for (auto* c : chunks) std::free(c);
  }
  uint8_t* Entry(uint32_t idx) { return chunks[idx / per] + (idx % per) * kStride; }
  void Build(uint32_t n, uint32_t per_chunk) {
    per = per_chunk;
    const uint32_t nchunks = (n + per - 1) / per;
    for (uint32_t i = 0; i < nchunks; ++i) chunks.push_back(static_cast<uint8_t*>(std::calloc(per, kStride)));
    for (auto* c : chunks) table.push_back(reinterpret_cast<uintptr_t>(c));
    for (uint32_t i = 0; i < n; ++i) {
      SetNext(i, i + 1 < n ? i + 1 : kEnd);
      SetKey(i, static_cast<uint8_t>(i % 200 + 1));  // never the 0xee of the fake kInvalid
      SetStamp(i, kNow - 10);
    }
  }
  void SetNext(uint32_t i, uint32_t next) { std::memcpy(Entry(i) + 0x8, &next, 4); }
  void SetKey(uint32_t i, uint8_t fill) { std::memset(Entry(i) + 0x18, fill, 0x80); }
  void SetKeyInvalid(uint32_t i) { std::memcpy(Entry(i) + 0x18, g_invalid_key, 0x80); }
  void SetStamp(uint32_t i, uint64_t v) { std::memcpy(Entry(i) + 0x228, &v, 8); }
  uint64_t Stamp(uint32_t i) {
    uint64_t v;
    std::memcpy(&v, Entry(i) + 0x228, 8);
    return v;
  }
};
static void Bd(Peers& p, uint32_t head) {
  std::memset(g_bd, 0, sizeof g_bd);
  const uintptr_t chunks = reinterpret_cast<uintptr_t>(p.table.data());
  std::memcpy(g_bd + 0x518, &chunks, 8);
  std::memcpy(g_bd + 0x560, &p.per, 4);
  std::memcpy(g_bd + 0x554, &head, 4);
  const uint64_t now = kNow;
  std::memcpy(g_bd + 0x478, &now, 8);
  const uint64_t sent = 77;
  std::memcpy(g_bd + 0x470, &sent, 8);
}
static uint64_t BdNow() {
  uint64_t v;
  std::memcpy(&v, g_bd + 0x478, 8);
  return v;
}

static int g_real_calls = 0;
static void* g_real_bd = nullptr;
static std::vector<uint64_t> g_seen;  // the stamps the real function saw (entries 0..), recorded by SeeStamps
static Peers* g_see = nullptr;
static uint32_t g_see_n = 0;
extern "C" __attribute__((noinline)) void RealCleanup(void* bd) {
  ++g_real_calls;
  g_real_bd = bd;
  g_seen.clear();
  if (g_see != nullptr)
    for (uint32_t i = 0; i < g_see_n; ++i) g_seen.push_back(g_see->Stamp(i));
}
extern "C" __attribute__((noinline)) void OtherCleanup(void*) {}
using Fn = void (*)(void*);

struct Slot {
  uintptr_t* w;
  uintptr_t base;
};
static Slot MakePage(uintptr_t word) {
  void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) std::abort();
  auto* w = static_cast<uintptr_t*>(p);
  w[4] = word;
  mprotect(p, 4096, PROT_READ);
  return {w, reinterpret_cast<uintptr_t>(p)};
}
static Fn Install() {
  q15t::ResetPeerClampForTest();
  Slot s = MakePage(reinterpret_cast<uintptr_t>(&RealCleanup));
  g_lines.clear();
  g_real_calls = 0;
  g_see = nullptr;
  g_invalid_cell = reinterpret_cast<uintptr_t>(g_invalid_key);
  const auto r = q15t::InstallPeerClampAt(s.base, 0x20, reinterpret_cast<uintptr_t>(&RealCleanup) - s.base,
                                          reinterpret_cast<uintptr_t>(&g_invalid_cell) - s.base);
  CHECK(r.status == q15t::SwapStatus::kOk, "install: %s", q15t::StatusName(r.status));
  return reinterpret_cast<Fn>(s.w[4]);
}

int main() {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);
  std::memset(g_invalid_key, 0xee, sizeof g_invalid_key);

  // the pinned addresses (R0_readelf_r.txt line 131055; disassembly 0x25ef9b0 - 0x100000; kInvalid GOT word from the source, disassembly 0x24efa14/18)
  CHECK(q15t::kPeerClampSlot == 0x36efad8 && q15t::kPeerClampReal == 0x24ef9b0 && q15t::kPeerClampInvalid == 0x371d538, "pinned addresses changed");

  Fn hook = Install();
  CHECK(CountLines("peerclamp on (TEST ONLY)", 1) == 1, "no INFO install line");

  // 1. the walk: entries over two chunks (per 4); a stamp above "now" comes down to "now", every other stamp stays, now stays,
  //    an entry keyed kInvalid stays, and the real function runs afterwards and sees the clamped stamps
  {
    Peers p;
    p.Build(8, 4);
    p.SetStamp(0, kNow + 5);        // clamped
    p.SetStamp(1, kNow);            // equal: untouched
    p.SetStamp(2, kNow - 123456);   // a real silence: untouched
    p.SetStamp(3, kNow + 1);        // clamped (first entry of chunk 0's end)
    p.SetStamp(4, ~0ull);           // in the second chunk: clamped
    p.SetKeyInvalid(5);
    p.SetStamp(5, kNow + 99);       // kInvalid key: the stock loop does not timeout-test it: untouched
    p.SetStamp(6, 0);               // untouched
    p.SetKeyInvalid(7);
    std::memset(p.Entry(7) + 0x18 + 0x7f, 0, 1);  // differs from kInvalid in the last key byte only: a normal peer
    p.SetStamp(7, kNow + 7);        // clamped
    Bd(p, 0);
    g_see = &p;
    g_see_n = 8;
    hook(g_bd);
    CHECK(g_real_calls == 1 && g_real_bd == g_bd, "real function not called once with the broadcaster");
    CHECK(p.Stamp(0) == kNow && p.Stamp(1) == kNow && p.Stamp(2) == kNow - 123456 && p.Stamp(3) == kNow && p.Stamp(4) == kNow &&
              p.Stamp(5) == kNow + 99 && p.Stamp(6) == 0 && p.Stamp(7) == kNow,
          "stamps: %llu %llu %llu %llu %llu %llu %llu", (unsigned long long)p.Stamp(0), (unsigned long long)p.Stamp(1), (unsigned long long)p.Stamp(2),
          (unsigned long long)p.Stamp(3), (unsigned long long)p.Stamp(4), (unsigned long long)p.Stamp(5), (unsigned long long)p.Stamp(6));
    CHECK(g_seen.size() == 8 && g_seen[0] == kNow && g_seen[4] == kNow, "the real function ran before the clamp");
    CHECK(BdNow() == kNow, "now was written");
    CHECK(q15t::PeerClampCount() == 4, "clamps %u", q15t::PeerClampCount());
    CHECK(CountLines("peerclamp pulled", 1) == 4 && CountLines("idx=4 stamp=18446744073709551615 now=1000000 diff=18446744073708551615 c470=77 clamp=3") == 1,
          "clamp records");
    // an entry not on the list is not touched: start at entry 2
    p.SetStamp(0, kNow + 5);
    p.SetStamp(2, kNow + 5);
    Bd(p, 2);
    hook(g_bd);
    CHECK(p.Stamp(0) == kNow + 5 && p.Stamp(2) == kNow, "the walk must start at the head index");
    g_see = nullptr;
  }

  // 2. nothing to walk: the real function still runs
  {
    Peers p;
    p.Build(2, 2);
    p.SetStamp(0, kNow + 5);
    Bd(p, kEnd);
    g_real_calls = 0;
    hook(g_bd);
    CHECK(g_real_calls == 1 && p.Stamp(0) == kNow + 5, "empty list: real %d", g_real_calls);
    Bd(p, 0);
    const uint32_t zero = 0;
    std::memcpy(g_bd + 0x560, &zero, 4);  // per-chunk count 0 (would divide by zero)
    hook(g_bd);
    CHECK(g_real_calls == 2 && p.Stamp(0) == kNow + 5, "per-chunk 0: real %d", g_real_calls);
    Bd(p, 0);
    const uintptr_t nul = 0;
    std::memcpy(g_bd + 0x518, &nul, 8);  // no chunk table
    hook(g_bd);
    CHECK(g_real_calls == 3 && p.Stamp(0) == kNow + 5, "no chunk table: real %d", g_real_calls);
  }

  // 3. unreadable memory: no crash, the real function runs
  {
    g_real_calls = 0;
    hook(reinterpret_cast<void*>(0x20));
    CHECK(g_real_calls == 1, "unreadable broadcaster: real %d", g_real_calls);
    Peers p;
    p.Build(3, 4);
    p.SetStamp(0, kNow + 5);
    Bd(p, 0);
    p.table[0] = 0x30;  // chunk pointer unreadable (entries unreadable)
    hook(g_bd);
    CHECK(g_real_calls == 2, "unreadable entries: real %d", g_real_calls);
    p.table[0] = reinterpret_cast<uintptr_t>(p.chunks[0]);
    p.SetStamp(0, kNow + 5);
    Bd(p, 0);
    const uintptr_t badtab = 0x40;
    std::memcpy(g_bd + 0x518, &badtab, 8);  // chunk table unreadable
    hook(g_bd);
    CHECK(g_real_calls == 3 && p.Stamp(0) == kNow + 5, "unreadable chunk table: real %d", g_real_calls);
    // kInvalid's contents unreadable at call time: nothing can be compared, nothing is clamped
    auto* kpage = static_cast<uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    std::memset(kpage, 0xee, 0x80);
    Slot ks = MakePage(reinterpret_cast<uintptr_t>(&RealCleanup));
    g_invalid_cell = reinterpret_cast<uintptr_t>(kpage);
    q15t::ResetPeerClampForTest();
    q15t::InstallPeerClampAt(ks.base, 0x20, reinterpret_cast<uintptr_t>(&RealCleanup) - ks.base, reinterpret_cast<uintptr_t>(&g_invalid_cell) - ks.base);
    const Fn khook = reinterpret_cast<Fn>(ks.w[4]);
    mprotect(kpage, 4096, PROT_NONE);
    p.SetStamp(0, kNow + 5);
    Bd(p, 0);
    khook(g_bd);
    CHECK(g_real_calls == 4 && p.Stamp(0) == kNow + 5, "unreadable kInvalid: real %d stamp %llu", g_real_calls, (unsigned long long)p.Stamp(0));
    munmap(kpage, 4096);
    g_invalid_cell = reinterpret_cast<uintptr_t>(g_invalid_key);
  }

  // 3b. 'now' unreadable while the rest is readable: nothing is clamped (a 'now' of 0 would zero every stamp)
  {
    hook = Install();
    auto* two = static_cast<uint8_t*>(mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    mprotect(two, 4096, PROT_NONE);
    uint8_t* bd = two + 4096 - 0x500;  // bd+0x478 (now) is in the PROT_NONE page; bd+0x518.. is readable
    Peers p;
    p.Build(2, 2);
    p.SetStamp(0, kNow + 5);
    p.SetStamp(1, kNow - 5);
    mprotect(two, 4096, PROT_READ | PROT_WRITE);
    const uintptr_t chunks = reinterpret_cast<uintptr_t>(p.table.data());
    const uint32_t head = 0;
    std::memcpy(bd + 0x518, &chunks, 8);
    std::memcpy(bd + 0x560, &p.per, 4);
    std::memcpy(bd + 0x554, &head, 4);
    mprotect(two, 4096, PROT_NONE);
    g_real_calls = 0;
    hook(bd);
    CHECK(g_real_calls == 1 && p.Stamp(0) == kNow + 5 && p.Stamp(1) == kNow - 5, "unreadable now: stamps %llu %llu", (unsigned long long)p.Stamp(0),
          (unsigned long long)p.Stamp(1));
    munmap(two, 8192);
  }

  // 3c. an entry whose stamp is unreadable ends the walk: the entries behind it are not touched
  {
    hook = Install();
    auto* two = static_cast<uint8_t*>(mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    mprotect(two + 4096, 4096, PROT_NONE);
    uint8_t* a = two + 4096 - 0x100;  // next and key readable, the stamp at +0x228 is in the PROT_NONE page
    const uint32_t next = 1;
    std::memcpy(a + 0x8, &next, 4);
    std::memset(a + 0x18, 0x11, 0x80);
    Peers p;
    p.Build(2, 1);  // idx 1 = chunk 1, a normal entry
    p.SetStamp(1, kNow + 5);
    p.table[0] = reinterpret_cast<uintptr_t>(a);
    Bd(p, 0);
    g_real_calls = 0;
    hook(g_bd);
    CHECK(g_real_calls == 1 && p.Stamp(1) == kNow + 5, "the walk went past an entry it could not read");
    munmap(two, 8192);
  }

  // 4. a stale 'next' that loops: the walk stops at 4096 entries, one line, the real function runs; exactly 4096 entries is not capped
  {
    hook = Install();
    Peers loop;
    loop.Build(2, 2);
    loop.SetNext(1, 0);  // 0 -> 1 -> 0 -> ...
    loop.SetStamp(0, kNow + 5);
    Bd(loop, 0);
    hook(g_bd);
    hook(g_bd);
    CHECK(g_real_calls == 2 && CountLines("peerclamp walk capped", 2) == 1, "cap lines %d (want one, ever)", CountLines("peerclamp walk capped"));
    CHECK(q15t::PeerClampCount() == 1, "a clamped stamp clamped again: %u", q15t::PeerClampCount());
    hook = Install();
    Peers full;
    full.Build(4096, 4096);
    for (uint32_t i = 0; i < 4096; ++i) full.SetStamp(i, kNow + 1);
    Bd(full, 0);
    hook(g_bd);
    CHECK(CountLines("walk capped") == 0 && q15t::PeerClampCount() == 4096, "4096 entries: cap line %d clamps %u", CountLines("walk capped"), q15t::PeerClampCount());
    hook = Install();
    Peers over;
    over.Build(4097, 4097);
    for (uint32_t i = 0; i < 4097; ++i) over.SetStamp(i, kNow + 1);
    Bd(over, 0);
    hook(g_bd);
    CHECK(CountLines("walk capped") == 1 && q15t::PeerClampCount() == 4096 && over.Stamp(4096) == kNow + 1,
          "4097 entries: cap line %d clamps %u", CountLines("walk capped"), q15t::PeerClampCount());
  }

  // 5. log caps: 16 clamp lines, then a total at every 100th clamp, at most 8 totals
  {
    hook = Install();
    Peers p;
    p.Build(1, 1);
    for (int i = 0; i < 99; ++i) {
      p.SetStamp(0, kNow + 5);
      Bd(p, 0);
      hook(g_bd);
    }
    CHECK(CountLines("peerclamp pulled") == 16 && CountLines("clamps so far") == 0, "after 99: %d / %d", CountLines("peerclamp pulled"), CountLines("clamps so far"));
    p.SetStamp(0, kNow + 5);
    Bd(p, 0);
    hook(g_bd);
    CHECK(CountLines("clamps so far") == 1 && CountLines("clamps=100") == 1, "no total at the 100th clamp");
    for (int i = 0; i < 900; ++i) {
      p.SetStamp(0, kNow + 5);
      Bd(p, 0);
      hook(g_bd);
    }
    CHECK(CountLines("clamps so far") == 8 && q15t::PeerClampCount() == 1000, "totals %d clamps %u", CountLines("clamps so far"), q15t::PeerClampCount());
  }

  // 6. install refusals
  {
    q15t::ResetPeerClampForTest();
    g_lines.clear();
    Slot bad = MakePage(0x4242420);
    g_invalid_cell = reinterpret_cast<uintptr_t>(g_invalid_key);
    const auto rb = q15t::InstallPeerClampAt(bad.base, 0x20, reinterpret_cast<uintptr_t>(&RealCleanup) - bad.base, reinterpret_cast<uintptr_t>(&g_invalid_cell) - bad.base);
    CHECK(rb.status == q15t::SwapStatus::kUnexpectedOriginal && bad.w[4] == 0x4242420, "foreign word: %s", q15t::StatusName(rb.status));
    {
    char named[128];
    std::snprintf(named, sizeof named, "pin=0x20 found=0x4242420 expected=0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&RealCleanup)));
    CHECK(CountLines("peerclamp refused", 3) == 1 && CountLines(named) == 1, "refusal not loud / does not name pin, found, expected");
  }
    Slot other = MakePage(reinterpret_cast<uintptr_t>(&OtherCleanup));
    const auto rc = q15t::InstallPeerClampAt(other.base, 0x20, reinterpret_cast<uintptr_t>(&RealCleanup) - other.base, reinterpret_cast<uintptr_t>(&g_invalid_cell) - other.base);
    CHECK(rc.status == q15t::SwapStatus::kUnexpectedOriginal && other.w[4] == reinterpret_cast<uintptr_t>(&OtherCleanup), "other function's word accepted: %s",
          q15t::StatusName(rc.status));
    // kInvalid pointer null / unreadable: refused, slot untouched
    g_lines.clear();
    Slot s = MakePage(reinterpret_cast<uintptr_t>(&RealCleanup));
    g_invalid_cell = 0;
    const auto rn = q15t::InstallPeerClampAt(s.base, 0x20, reinterpret_cast<uintptr_t>(&RealCleanup) - s.base, reinterpret_cast<uintptr_t>(&g_invalid_cell) - s.base);
    CHECK(std::strcmp(rn.reason, "invalid_unreadable") == 0 && s.w[4] == reinterpret_cast<uintptr_t>(&RealCleanup), "null kInvalid installed");
    CHECK(CountLines("peerclamp refused", 3) == 1, "null kInvalid not loud");
    g_lines.clear();
    const auto ru = q15t::InstallPeerClampAt(s.base, 0x20, reinterpret_cast<uintptr_t>(&RealCleanup) - s.base, 0x30 - s.base);  // reads address 0x30
    CHECK(std::strcmp(ru.reason, "invalid_unreadable") == 0 && s.w[4] == reinterpret_cast<uintptr_t>(&RealCleanup) && CountLines("peerclamp refused", 3) == 1,
          "unreadable kInvalid installed");
    g_invalid_cell = reinterpret_cast<uintptr_t>(g_invalid_key);
  }

  // ---- code-word pins: every word read, a changed or unreadable one refused loudly, no file hash anywhere ----
  {
    auto count = [](const char* needle, int level) { return CountLines(needle, level); };
    auto clear = []() { g_lines.clear(); };
    g_fail += pincheck::Run("peerclamp", q15t::kPeerClampWordPins, q15t::kPeerClampWordPinCount, 26, 0xbfc915ef85869c5full,
                            [](std::uintptr_t b) { return q15t::PeerClampWordPinsMatch(b); }, count, clear);
    // the real installer refuses on a changed word, before anything else (reason word_pin_mismatch, one ERROR line, nothing swapped)
    pincheck::Text t = pincheck::Make(q15t::kPeerClampWordPins, q15t::kPeerClampWordPinCount);
    *t.word(q15t::kPeerClampWordPins[0].va) ^= 1u;
    g_lines.clear();
    const auto pr = q15t::InstallPeerClamp(t.base());
    CHECK(std::strcmp(pr.reason, "word_pin_mismatch") == 0 && CountLines("code word pin mismatch", 3) == 1, "installer ignored a changed word");
    CHECK(CountLines("peerclamp on", -1) == 0, "installer went on after a changed word");
    pincheck::Free(&t);
  }

  if (g_fail == 0) std::printf("peerclamp_test: all checks passed\n");
  return g_fail == 0 ? 0 : 1;
}
