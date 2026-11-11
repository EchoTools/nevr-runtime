// Host test of peerclamp (src/quest/combat/peerclamp.cpp, #2178): the real handler through the thunk, installed by the real GotHook into the JUMP_SLOT
// of a libr15 image built in memory, and called the way the game calls it: through the slot. The game side is a model: a fake SBroadcasterData
// (head, chunk table, per-chunk count, "now") with peer entries of the real stride (0x948), key at +0x18 and stamp at +0x228, a fake
// SAddress::kInvalid reached through its GOT word, and a stand-in for CleanupPeers itself. The levels the guard is scoped by are the real combat level answer
// (combat_scope.h), refreshed through the tick's callback against a model of the game's gamespace tables (combat_game_model.h, combat_scope_world.h).
//
// What it pins (the q15t plugin's peerclamp_test assertions, on the new fixture): the walk lowers a stamp above "now" to "now" and nothing else, the game's
// function runs once with the same argument (after the clamp), nothing to walk and every unreadable read leave the stamps alone, the walk stops at 4096
// entries with one Warning, the clamp lines are capped, a slot that does not hold CleanupPeers is refused and left alone with the pin named, an unreadable
// or null kInvalid refuses the install, every code word flipped in turn refuses the install, and an unreadable word does too. New with the move: the guard
// acts only while a combat map or the combat lobby is loaded (an arena match, the lobbies arena players use, nothing loaded, a look that never ran and a
// combat map with an arena level also loaded leave a future stamp alone, write nothing, read none of the game's memory and scan no level, by count), a true
// answer older than kStaleAfterMs is not believed, the switch, the missing image and build ID, the stats lines, and a scope that cannot change (the install is
// refused).
//
// Built and run by `just test-quest-combat`.

#include "quest/combat/peerclamp.h"

#include <sys/mman.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "quest/combat/combat_level.h"
#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/safe_read.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/combat_log_capture.h"
#include "quest/tests/combat_pin_check.h"
#include "quest/tests/combat_pins.h"
#include "quest/tests/combat_scope_world.h"
#include "quest/tests/test_check.h"

namespace {

namespace pc = nevr_quest::combat::peerclamp;
namespace scope = nevr_quest::combat::scope;
namespace tick = nevr_quest::combat::tick;
namespace pinned = sentinel::pinned;
using combat_test::Has;
using combat_test::Image;
using combat_test::Line;
using combat_test::Lines;
using nevr_quest::combat::ModuleResult;
using nevr_quest::combat::pinned::Word;
// The tables themselves, not the module's own `kWordPins` (which a bug could shorten): the test puts every pinned word in the image and flips every one.
const nevr_quest::combat::pinned::Pins kAllWords = {pc::kWords, pc::kWordCount, nullptr, 0};
namespace world = combat_test::scope_world;

using CleanupFn = void (*)(void*);

constexpr std::uint64_t kNow = 1000000;
constexpr std::uint32_t kEnd = 0xffffffffU;
constexpr std::size_t kStride = 0x948;

alignas(16) std::uint8_t g_bd[0x600];
alignas(16) std::uint8_t g_invalidKey[0x80];

// A peer list: `n` entries over chunks of `per`; entry i's key and stamp are set by the caller.
struct Peers {
  std::vector<std::uint8_t*> chunks;
  std::vector<std::uintptr_t> table;  // the chunk pointer table the broadcaster points at
  std::uint32_t per = 0;
  ~Peers() {
    for (auto* c : chunks) std::free(c);
  }
  std::uint8_t* Entry(std::uint32_t idx) { return chunks[idx / per] + (idx % per) * kStride; }
  void Build(std::uint32_t n, std::uint32_t perChunk) {
    per = perChunk;
    const std::uint32_t nchunks = (n + per - 1) / per;
    for (std::uint32_t i = 0; i < nchunks; ++i) chunks.push_back(static_cast<std::uint8_t*>(std::calloc(per, kStride)));
    for (auto* c : chunks) table.push_back(reinterpret_cast<std::uintptr_t>(c));
    for (std::uint32_t i = 0; i < n; ++i) {
      SetNext(i, i + 1 < n ? i + 1 : kEnd);
      SetKey(i, static_cast<std::uint8_t>(i % 200 + 1));  // never the 0xee of the fake kInvalid
      SetStamp(i, kNow - 10);
    }
  }
  void SetNext(std::uint32_t i, std::uint32_t next) { std::memcpy(Entry(i) + 0x8, &next, 4); }
  void SetKey(std::uint32_t i, std::uint8_t fill) { std::memset(Entry(i) + 0x18, fill, 0x80); }
  void SetKeyInvalid(std::uint32_t i) { std::memcpy(Entry(i) + 0x18, g_invalidKey, 0x80); }
  void SetStamp(std::uint32_t i, std::uint64_t v) { std::memcpy(Entry(i) + 0x228, &v, 8); }
  std::uint64_t Stamp(std::uint32_t i) {
    std::uint64_t v;
    std::memcpy(&v, Entry(i) + 0x228, 8);
    return v;
  }
};
void Bd(Peers& p, std::uint32_t head) {
  std::memset(g_bd, 0, sizeof(g_bd));
  const std::uintptr_t chunks = reinterpret_cast<std::uintptr_t>(p.table.data());
  std::memcpy(g_bd + 0x518, &chunks, 8);
  std::memcpy(g_bd + 0x560, &p.per, 4);
  std::memcpy(g_bd + 0x554, &head, 4);
  const std::uint64_t now = kNow;
  std::memcpy(g_bd + 0x478, &now, 8);
  const std::uint64_t sent = 77;
  std::memcpy(g_bd + 0x470, &sent, 8);
}
std::uint64_t BdNow() {
  std::uint64_t v;
  std::memcpy(&v, g_bd + 0x478, 8);
  return v;
}

int g_realCalls = 0;
void* g_realBd = nullptr;
std::vector<std::uint64_t> g_seen;  // the stamps the real function saw (entries 0..), recorded by RealCleanup
Peers* g_see = nullptr;
std::uint32_t g_seeN = 0;
extern "C" __attribute__((noinline)) void RealCleanup(void* bd) {
  ++g_realCalls;
  g_realBd = bd;
  g_seen.clear();
  if (g_see != nullptr)
    for (std::uint32_t i = 0; i < g_seeN; ++i) g_seen.push_back(g_see->Stamp(i));
}
extern "C" __attribute__((noinline)) void OtherCleanup(void*) {}

// ---- the libr15 image ----------------------------------------------------------------------------------------------------------

Image g_image;
pc::Pins g_pins = pc::kPins;
bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}

// A libr15 holding the pinned words, the slot (which holds `slotWord`, the stand-in when 0) and the GOT word that points at the fake kInvalid
// (`invalidWord`).
void Build(std::uintptr_t slotWord = 0, std::uintptr_t invalidWord = ~static_cast<std::uintptr_t>(0)) {
  g_pins = pc::kPins;
  g_image.Build({{pc::kCleanupSymbol, g_pins.slot, false, false, g_pins.fn}, world::TickRow()});
  world::PutTickSlot(g_image);
  // base + fn is the stand-in's address: the real function is not in the image, so the pin holds the stand-in's offset.
  g_pins.fn = reinterpret_cast<std::uintptr_t>(&RealCleanup) - g_image.Base();
  combat_test::PutPins(g_image, &kAllWords, 1);
  *g_image.Slot(g_pins.slot) = slotWord != 0 ? reinterpret_cast<void*>(slotWord) : reinterpret_cast<void*>(&RealCleanup);
  g_image.Put<std::uintptr_t>(g_pins.invalid, invalidWord == ~static_cast<std::uintptr_t>(0) ? reinterpret_cast<std::uintptr_t>(g_invalidKey) : invalidWord);
}
CleanupFn Slot() { return reinterpret_cast<CleanupFn>(*g_image.Slot(g_pins.slot)); }
bool Hooked() { return *g_image.Slot(g_pins.slot) == pc::CleanupThunk::EntryAddress(); }
bool SlotIsTheGames() { return *g_image.Slot(g_pins.slot) == reinterpret_cast<void*>(&RealCleanup); }

// A fresh world: the module forgotten, a libr15 holding the pinned words, the slot and the tick's slot, and the capture started. `scopeLive`: the combat tick
// is installed on the image and the scope row has registered the refresh (what the shipped table does before this module).
std::atomic<std::uint64_t> g_reads{0};  // every SafeRead the module makes (safe_read.h's test counter)
void Begin(bool scopeLive = true) {
  combat_test::StartCapture();
  pc::ResetForTest();
  world::Reset();
  g_realCalls = 0;
  g_see = nullptr;
  Build();
  g_reads = 0;
  nevr_quest::combat::SetReadCounterForTest(&g_reads);
  if (scopeLive) {
    QCHECK(world::StartScope(&Lookup));
    combat_test::Captured().clear();
  }
}
void End() {
  pc::ResetForTest();
  world::Reset();
  g_image.Drop();
  nevr_quest::combat::SetReadCounterForTest(nullptr);
  combat_test::StopCapture();
}

ModuleResult Install(bool on = true) { return pc::InstallAt(&Lookup, g_pins, on); }

// A fresh world with the guard installed and the named levels loaded; returns the hooked slot.
CleanupFn InstalledIn(std::initializer_list<const char*> levels) {
  End();
  Begin();
  QCHECK(Install().installed);
  world::Levels(levels);
  g_reads = 0;
  return Slot();
}

// ---- the tests ------------------------------------------------------------------------------------------------------------------

void ThePinsAreTheReadelfAndDisassemblyPins() {
  QCHECK(pc::kCleanupSlotVaddr == 0x36efad8 && pc::kCleanupVaddr == 0x24ef9b0 && pc::kInvalidWordVaddr == 0x371d538);
  QCHECK(std::strcmp(pc::kCleanupSymbol, "_ZN10NRadEngine16SBroadcasterData12CleanupPeersEv") == 0);
  QCHECK(pc::kOffHead == 0x554 && pc::kOffPerChunk == 0x560 && pc::kOffChunks == 0x518 && pc::kOffNow == 0x478 && pc::kOffSent == 0x470);
  QCHECK(pc::kStride == 0x948 && pc::kOffNext == 0x8 && pc::kOffKey == 0x18 && pc::kOffStamp == 0x228 && pc::kKeyLen == 0x80 && pc::kEnd == 0xffffffffU);
  static const Word kWant[26] = {
      {"", 0x24ef9b0, 0x6db733ed}, {"", 0x24ef9b4, 0x6d012beb}, {"", 0x24ef9d8, 0xb945541b}, {"", 0x24efa14, 0xd0009177}, {"", 0x24efa18, 0xf9429ef7},
      {"", 0x24efa2c, 0xaa0003f3}, {"", 0x24efa30, 0x5281291c}, {"", 0x24efa68, 0xb9456268}, {"", 0x24efa6c, 0xf9428e69}, {"", 0x24efa74, 0xf9423e78},
      {"", 0x24efa78, 0x1ac80b6a}, {"", 0x24efa7c, 0xf86a5934}, {"", 0x24efa80, 0x1b08ed5a}, {"", 0x24efa84, 0x52801002}, {"", 0x24efa8c, 0x9bbc5348},
      {"", 0x24efa90, 0xb940091b}, {"", 0x24efa94, 0xf9411519}, {"", 0x24efa98, 0x91006116}, {"", 0x24efaa4, 0xb4fffd40}, {"", 0x24efae4, 0xcb190319},
      {"", 0x24eeec8, 0xf9400260}, {"", 0x24eeecc, 0x97a90645}, {"", 0xf307e0, 0xf0013df0},  {"", 0xf307e4, 0xf9456e11},  {"", 0xf307e8, 0x912b6210},
      {"", 0xf307ec, 0xd61f0220}};
  QCHECK(pc::kWordCount == 26);
  for (std::size_t i = 0; i < 26; ++i) QCHECK(pc::kWords[i].vaddr == kWant[i].vaddr && pc::kWords[i].word == kWant[i].word && pc::kWords[i].what[0] != '\0');
  QCHECK(combat_test::Fingerprint(pc::kWords, pc::kWordCount) == combat_test::Fingerprint(kWant, 26));
}

void TheThunkTakesTheSlotAndTheLineSaysSo() {
  Begin();
  const ModuleResult r = Install();
  QCHECK(r.installed && std::strcmp(r.status, "installed") == 0 && !r.off);
  QCHECK(Hooked() && tick::Count() == 1);  // the scope row registered the refresh with the tick, once; the module adds no callback
  const std::vector<Line> install = Lines("combat_peerclamp_install");
  QCHECK(install.size() == 1U);
  if (install.size() == 1U) {
    QCHECK(install[0].level == sentinel::LogLevel::kInfo);
    QCHECK(combat_test::Golden(install[0].text) ==
           "\"event\":\"combat_peerclamp_install\",\"result\":\"installed\",\"reason\":\"ok\",\"hook\":\"ok\",\"scope\":\"combat_map_or_lobby\","
           "\"consequence\":\"a peer's last-received stamp above now is pulled down to now before the game's timeout loop, while a combat map or "
           "the combat lobby is loaded\"}");
  }
  End();
}

// 1. The walk: entries over two chunks (per 4); a stamp above "now" comes down to "now", every other stamp stays, now stays, an entry keyed kInvalid
//    stays, and the game's function runs afterwards and sees the clamped stamps.
void TheWalkLowersAStampAboveNowAndNothingElse() {
  const CleanupFn hook = InstalledIn({"mpl_combat_dyson"});
  Peers p;
  p.Build(8, 4);
  p.SetStamp(0, kNow + 5);       // clamped
  p.SetStamp(1, kNow);           // equal: untouched
  p.SetStamp(2, kNow - 123456);  // a real silence: untouched
  p.SetStamp(3, kNow + 1);       // clamped (the end of chunk 0)
  p.SetStamp(4, ~0ULL);          // in the second chunk: clamped
  p.SetKeyInvalid(5);
  p.SetStamp(5, kNow + 99);  // kInvalid key: the stock loop does not timeout-test it: untouched
  p.SetStamp(6, 0);          // untouched
  p.SetKeyInvalid(7);
  std::memset(p.Entry(7) + 0x18 + 0x7f, 0, 1);  // differs from kInvalid in the last key byte only: a normal peer
  p.SetStamp(7, kNow + 7);                       // clamped
  Bd(p, 0);
  g_see = &p;
  g_seeN = 8;
  hook(g_bd);
  QCHECK(g_realCalls == 1 && g_realBd == g_bd);
  QCHECK(p.Stamp(0) == kNow && p.Stamp(1) == kNow && p.Stamp(2) == kNow - 123456 && p.Stamp(3) == kNow && p.Stamp(4) == kNow &&
         p.Stamp(5) == kNow + 99 && p.Stamp(6) == 0 && p.Stamp(7) == kNow);
  QCHECK(g_seen.size() == 8 && g_seen[0] == kNow && g_seen[4] == kNow);  // the clamp ran before the game's function
  QCHECK(BdNow() == kNow);
  QCHECK(pc::CurrentStats().clamped == 4 && pc::CurrentStats().walked == 8 && pc::CurrentStats().inScope == 1);
  const std::vector<Line> clamped = Lines("combat_peerclamp_clamped");
  QCHECK(clamped.size() == 4U);
  if (clamped.size() == 4U) {
    QCHECK(clamped[0].level == sentinel::LogLevel::kInfo);
    QCHECK(Has(clamped[2].text, "\"n\":3,\"idx\":4,\"stamp\":\"18446744073709551615\",\"now\":\"1000000\",\"diff\":\"18446744073708551615\",\"sent\":\"77\""));
    QCHECK(combat_test::Golden(clamped[0].text) ==
           "\"event\":\"combat_peerclamp_clamped\",\"n\":1,\"idx\":0,\"stamp\":\"1000005\",\"now\":\"1000000\",\"diff\":\"5\",\"sent\":\"77\","
           "\"consequence\":\"a peer's last-received stamp was above now and is now now, so the timeout loop does not read it as 2^64 us of silence\"}");
  }
  // An entry not on the list is not touched: start at entry 2.
  p.SetStamp(0, kNow + 5);
  p.SetStamp(2, kNow + 5);
  Bd(p, 2);
  hook(g_bd);
  QCHECK(p.Stamp(0) == kNow + 5 && p.Stamp(2) == kNow);  // the walk starts at the head index
  g_see = nullptr;
  End();
}

// 2. Nothing to walk: the game's function still runs.
void NothingToWalkStillCallsTheGamesFunction() {
  const CleanupFn hook = InstalledIn({"mpl_combat_gauss"});
  Peers p;
  p.Build(2, 2);
  p.SetStamp(0, kNow + 5);
  Bd(p, kEnd);
  hook(g_bd);
  QCHECK(g_realCalls == 1 && p.Stamp(0) == kNow + 5);
  Bd(p, 0);
  const std::uint32_t zero = 0;
  std::memcpy(g_bd + 0x560, &zero, 4);  // per-chunk count 0 (would divide by zero)
  hook(g_bd);
  QCHECK(g_realCalls == 2 && p.Stamp(0) == kNow + 5);
  Bd(p, 0);
  const std::uintptr_t nul = 0;
  std::memcpy(g_bd + 0x518, &nul, 8);  // no chunk table
  hook(g_bd);
  QCHECK(g_realCalls == 3 && p.Stamp(0) == kNow + 5);
  End();
}

// 3. Unreadable memory: no crash, the game's function runs.
void UnreadableMemoryLeavesTheStampsAloneAndCallsTheGamesFunction() {
  CleanupFn hook = InstalledIn({"mpl_combat_dyson"});
  hook(reinterpret_cast<void*>(0x20));
  QCHECK(g_realCalls == 1);
  Peers p;
  p.Build(3, 4);
  p.SetStamp(0, kNow + 5);
  Bd(p, 0);
  p.table[0] = 0x30;  // chunk pointer unreadable (entries unreadable)
  hook(g_bd);
  QCHECK(g_realCalls == 2);
  p.table[0] = reinterpret_cast<std::uintptr_t>(p.chunks[0]);
  p.SetStamp(0, kNow + 5);
  Bd(p, 0);
  const std::uintptr_t badtab = 0x40;
  std::memcpy(g_bd + 0x518, &badtab, 8);  // chunk table unreadable
  hook(g_bd);
  QCHECK(g_realCalls == 3 && p.Stamp(0) == kNow + 5);
  End();
  {  // kInvalid's contents unreadable at call time: nothing can be compared, nothing is clamped
    Begin();
    auto* kpage = static_cast<std::uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    std::memset(kpage, 0xee, 0x80);
    Build(0, reinterpret_cast<std::uintptr_t>(kpage));
    QCHECK(Install().installed);
    world::Levels({"mpl_combat_dyson"});
    mprotect(kpage, 4096, PROT_NONE);
    Peers q;
    q.Build(3, 4);
    q.SetStamp(0, kNow + 5);
    Bd(q, 0);
    Slot()(g_bd);
    QCHECK(g_realCalls == 1 && q.Stamp(0) == kNow + 5);
    munmap(kpage, 4096);
    End();
  }
}

// 3b. "now" unreadable while the rest is readable: nothing is clamped (a "now" of 0 would zero every stamp).
void AnUnreadableNowClampsNothing() {
  const CleanupFn hook = InstalledIn({"mpl_combat_dyson"});
  auto* two = static_cast<std::uint8_t*>(mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  std::uint8_t* bd = two + 4096 - 0x500;  // bd+0x478 (now) is in the PROT_NONE page; bd+0x518.. is readable
  Peers p;
  p.Build(2, 2);
  p.SetStamp(0, kNow + 5);
  p.SetStamp(1, kNow - 5);
  const std::uintptr_t chunks = reinterpret_cast<std::uintptr_t>(p.table.data());
  const std::uint32_t head = 0;
  std::memcpy(bd + 0x518, &chunks, 8);
  std::memcpy(bd + 0x560, &p.per, 4);
  std::memcpy(bd + 0x554, &head, 4);
  mprotect(two, 4096, PROT_NONE);
  hook(bd);
  QCHECK(g_realCalls == 1 && p.Stamp(0) == kNow + 5 && p.Stamp(1) == kNow - 5);
  munmap(two, 8192);
  End();
}

// 3c. An entry whose stamp is unreadable ends the walk: the entries behind it are not touched.
void AnEntryWhoseStampIsUnreadableEndsTheWalk() {
  const CleanupFn hook = InstalledIn({"mpl_combat_dyson"});
  auto* two = static_cast<std::uint8_t*>(mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  mprotect(two + 4096, 4096, PROT_NONE);
  std::uint8_t* a = two + 4096 - 0x100;  // next and key readable, the stamp at +0x228 is in the PROT_NONE page
  const std::uint32_t next = 1;
  std::memcpy(a + 0x8, &next, 4);
  std::memset(a + 0x18, 0x11, 0x80);
  Peers p;
  p.Build(2, 1);  // idx 1 = chunk 1, a normal entry
  p.SetStamp(1, kNow + 5);
  p.table[0] = reinterpret_cast<std::uintptr_t>(a);
  Bd(p, 0);
  hook(g_bd);
  QCHECK(g_realCalls == 1 && p.Stamp(1) == kNow + 5);
  munmap(two, 8192);
  End();
}

// 4. A stale `next` that loops: the walk stops at 4096 entries, one Warning, the game's function runs; exactly 4096 entries is not capped.
void AWalkIsCappedAt4096EntriesWithOneWarning() {
  CleanupFn hook = InstalledIn({"mpl_combat_dyson"});
  Peers loop;
  loop.Build(2, 2);
  loop.SetNext(1, 0);  // 0 -> 1 -> 0 -> ...
  loop.SetStamp(0, kNow + 5);
  Bd(loop, 0);
  hook(g_bd);
  hook(g_bd);
  QCHECK(g_realCalls == 2);
  const std::vector<Line> capped = Lines("combat_peerclamp_walk_capped");
  QCHECK(capped.size() == 1U && capped[0].level == sentinel::LogLevel::kWarn && Has(capped[0].text, "\"entries\":4096"));  // one, ever
  QCHECK(pc::CurrentStats().clamped == 1 && pc::CurrentStats().capped == 2);  // a clamped stamp is not clamped again; both walks were capped
  hook = InstalledIn({"mpl_combat_dyson"});
  Peers full;
  full.Build(4096, 4096);
  for (std::uint32_t i = 0; i < 4096; ++i) full.SetStamp(i, kNow + 1);
  Bd(full, 0);
  hook(g_bd);
  QCHECK(Lines("combat_peerclamp_walk_capped").empty() && pc::CurrentStats().clamped == 4096 && pc::CurrentStats().capped == 0);
  hook = InstalledIn({"mpl_combat_dyson"});
  Peers over;
  over.Build(4097, 4097);
  for (std::uint32_t i = 0; i < 4097; ++i) over.SetStamp(i, kNow + 1);
  Bd(over, 0);
  hook(g_bd);
  QCHECK(Lines("combat_peerclamp_walk_capped").size() == 1U && pc::CurrentStats().clamped == 4096 && over.Stamp(4096) == kNow + 1);
  End();
}

// 5. Log caps: 16 clamp lines, then a total at every 100th clamp, at most 8 totals.
void TheClampLinesAreCappedAndTotalsAreWrittenEvery100th() {
  const CleanupFn hook = InstalledIn({"mpl_combat_dyson"});
  Peers p;
  p.Build(1, 1);
  for (int i = 0; i < 99; ++i) {
    p.SetStamp(0, kNow + 5);
    Bd(p, 0);
    hook(g_bd);
  }
  QCHECK(Lines("combat_peerclamp_clamped").size() == 16U && Lines("combat_peerclamp_clamped_total").empty());
  p.SetStamp(0, kNow + 5);
  Bd(p, 0);
  hook(g_bd);
  const std::vector<Line> first = Lines("combat_peerclamp_clamped_total");
  QCHECK(first.size() == 1U && first[0].level == sentinel::LogLevel::kInfo && Has(first[0].text, "\"clamps\":100"));  // none before the 100th
  for (int i = 0; i < 900; ++i) {
    p.SetStamp(0, kNow + 5);
    Bd(p, 0);
    hook(g_bd);
  }
  QCHECK(Lines("combat_peerclamp_clamped_total").size() == 8U && pc::CurrentStats().clamped == 1000 && Lines("combat_peerclamp_clamped").size() == 16U);
  const std::vector<Line> caps = Lines("combat_peerclamp_log_cap");
  QCHECK(caps.size() == 2U);  // the 17th clamp said the first-clamp lines stopped, the 9th total said the totals stopped, each once
  if (caps.size() == 2U) {
    QCHECK(caps[0].level == sentinel::LogLevel::kWarn && Has(caps[0].text, "\"lines\":\"clamped\",\"cap\":16") && Has(caps[0].text, "combat_peerclamp_stats"));
    QCHECK(Has(caps[1].text, "\"lines\":\"clamped_total\",\"cap\":8") && Has(caps[1].text, "no further"));
  }
  End();
}

// The scope: only a combat map or the combat lobby. Each of these leaves a future stamp alone, calls the game's function and counts the call as not in scope;
// arena pays nothing, by count: no read of the game's memory and no level scan.
void AnArenaMatchAndEveryOtherLevelLeaveAFutureStampAloneAndWriteNothing() {
  struct Case {
    const char* what;
    std::initializer_list<const char*> levels;
  };
  const Case cases[] = {{"an Echo Arena level", {"mpl_arena_a"}},
                        {"the lowspec arena level", {"mpl_arena_a_lowspec"}},
                        {"the arena's other level", {"mpl_arena_b"}},
                        {"the tutorial", {"mpl_tutorial_a"}},
                        {"the lobby arena players use", {"mpl_lobby_b2"}},
                        {"the arena social lobby", {"mpl_lobby_b_social"}},
                        {"the lobby of the arena", {"mpl_lobby_b_arena"}},
                        {"a level nobody names", {"mpl_somewhere_else"}},
                        {"nothing loaded", {}},
                        {"a combat map with an arena level still loaded (arena wins)", {"mpl_combat_dyson", "mpl_arena_a"}},
                        {"the combat lobby with an arena level still loaded", {"mpl_lobby_b_combat", "mpl_arena_b"}}};
  for (const Case& c : cases) {
    const CleanupFn hook = InstalledIn(c.levels);
    Peers p;
    p.Build(3, 4);
    p.SetStamp(0, kNow + 5);
    Bd(p, 0);
    const std::uint32_t scans = nevr_quest::combat::level::ScanCalls();
    hook(g_bd);
    if (p.Stamp(0) != kNow + 5) std::fprintf(stderr, "peerclamp scope: %s clamped\n", c.what);
    QCHECK(p.Stamp(0) == kNow + 5 && g_realCalls == 1 && g_realBd == g_bd);
    QCHECK(pc::CurrentStats().inScope == 0 && pc::CurrentStats().walked == 0 && pc::CurrentStats().clamped == 0 && pc::CurrentStats().calls == 1);
    QCHECK(Lines("combat_peerclamp_clamped").empty());
    QCHECK(g_reads.load() == 0 && nevr_quest::combat::level::ScanCalls() == scans);  // nothing of the game's was read, no level was asked for
    End();
  }
  {  // reads nothing: the broadcaster is unreadable and the call still goes to the game's function without a counted read
    const CleanupFn hook = InstalledIn({"mpl_arena_a"});
    hook(reinterpret_cast<void*>(0x20));
    QCHECK(g_realCalls == 1 && pc::CurrentStats().inScope == 0 && g_reads.load() == 0);
    End();
  }
}

// Before the scope's first look nothing is clamped (the answer starts false); the scope has not looked, and nothing is read.
void BeforeTheFirstLookNothingIsClamped() {
  Begin();
  QCHECK(Install().installed);
  g_reads = 0;  // the install's own pin reads are not the call's
  Peers p;
  p.Build(2, 2);
  p.SetStamp(0, kNow + 5);
  Bd(p, 0);
  Slot()(g_bd);
  QCHECK(g_realCalls == 1 && p.Stamp(0) == kNow + 5 && pc::CurrentStats().clamped == 0 && scope::Looks() == 0 && g_reads.load() == 0);
  End();
}

// A true answer older than kStaleAfterMs is not believed: the tick stopped, so a future stamp is left alone and nothing is read, and the first look after the
// tick is back makes the guard act again.
void ATrueAnswerFromAStoppedTickIsNotBelieved() {
  const CleanupFn hook = InstalledIn({"mpl_combat_dyson"});
  Peers p;
  p.Build(2, 2);
  p.SetStamp(0, kNow + 5);
  Bd(p, 0);
  hook(g_bd);
  QCHECK(p.Stamp(0) == kNow && pc::CurrentStats().clamped == 1);  // fresh: acts
  world::TheTickStops();
  QCHECK(scope::SeenCombatOrLobby() && !scope::LookIsFresh());
  p.SetStamp(0, kNow + 5);
  Bd(p, 0);
  g_reads = 0;
  hook(g_bd);
  QCHECK(p.Stamp(0) == kNow + 5 && pc::CurrentStats().clamped == 1 && pc::CurrentStats().inScope == 1 && g_reads.load() == 0 && g_realCalls == 2);
  world::Levels({"mpl_combat_dyson"});
  hook(g_bd);
  QCHECK(p.Stamp(0) == kNow && pc::CurrentStats().clamped == 2);
  End();
}

// The combat lobby and a combat map clamp; the look follows the level change at the next look.
void TheCombatLobbyAndACombatMapClampAndTheGuardFollowsTheLevel() {
  const CleanupFn hook = InstalledIn({"mpl_lobby_b_combat"});
  Peers p;
  p.Build(2, 2);
  p.SetStamp(0, kNow + 5);
  Bd(p, 0);
  hook(g_bd);
  QCHECK(p.Stamp(0) == kNow && pc::CurrentStats().clamped == 1);  // the combat lobby is enough
  world::Levels({"mpl_lobby_b2"});  // an arena lobby: off again
  p.SetStamp(0, kNow + 5);
  Bd(p, 0);
  hook(g_bd);
  QCHECK(p.Stamp(0) == kNow + 5 && pc::CurrentStats().clamped == 1);
  world::Levels({"mpl_combat_fission"});  // a combat map
  Bd(p, 0);
  hook(g_bd);
  QCHECK(p.Stamp(0) == kNow && pc::CurrentStats().clamped == 2);
  world::Levels({"mpl_combat_fission", "mpl_arena_a"});  // an arena level loads while the map is still there: arena wins
  p.SetStamp(0, kNow + 5);
  Bd(p, 0);
  hook(g_bd);
  QCHECK(p.Stamp(0) == kNow + 5 && pc::CurrentStats().clamped == 2 && pc::CurrentStats().inScope == 2);
  // Between looks the answer is the last look's: the level changes in the game, nothing has looked yet.
  world::LevelsWithoutALook({"mpl_combat_dyson"});
  hook(g_bd);
  QCHECK(p.Stamp(0) == kNow + 5 && scope::Looks() == 4);
  End();
}

// 6. Install refusals.
void ASlotThatDoesNotHoldCleanupPeersIsRefusedAndLeftAlone() {
  Begin();
  Build(0x4242420);
  ModuleResult r = Install();
  QCHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0);
  QCHECK(*g_image.Slot(g_pins.slot) == reinterpret_cast<void*>(0x4242420));
  const std::vector<Line> install = Lines("combat_peerclamp_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"result\":\"refused\"") &&
         Has(install[0].text, "\"reason\":\"slot_refused\"") && Has(install[0].text, "\"hook\":\"original_mismatch\""));
  End();
  Begin();
  Build(reinterpret_cast<std::uintptr_t>(&OtherCleanup));  // real executable code, but not CleanupPeers: no acceptance by plausibility
  r = Install();
  QCHECK(!r.installed && std::strcmp(r.status, "original_mismatch") == 0);
  QCHECK(*g_image.Slot(g_pins.slot) == reinterpret_cast<void*>(&OtherCleanup));
  End();
}

// kInvalid's GOT word null or unreadable: refused, slot untouched, the tick not asked for.
void ANullOrUnreadableInvalidWordRefusesTheInstall() {
  Begin();
  Build(0, 0);
  ModuleResult r = Install();
  QCHECK(!r.installed && std::strcmp(r.status, "invalid_unreadable") == 0 && SlotIsTheGames() && tick::Count() == 1);
  std::vector<Line> install = Lines("combat_peerclamp_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"reason\":\"invalid_unreadable\""));
  End();
  Begin();
  const std::uintptr_t page = (g_image.Base() + static_cast<std::uintptr_t>(g_pins.invalid)) & ~static_cast<std::uintptr_t>(0xfff);
  QCHECK(mprotect(reinterpret_cast<void*>(page), 0x1000, PROT_NONE) == 0);
  r = Install();
  mprotect(reinterpret_cast<void*>(page), 0x1000, PROT_READ | PROT_WRITE);
  QCHECK(!r.installed && std::strcmp(r.status, "invalid_unreadable") == 0 && SlotIsTheGames() && tick::Count() == 1);
  install = Lines("combat_peerclamp_install");
  QCHECK(install.size() == 1U && Has(install[0].text, "\"reason\":\"invalid_unreadable\""));
  End();
}

// 7. combat.peerclamp: false installs nothing and says so.
void TheSwitchOffInstallsNothingAndSaysSo() {
  Begin();
  const ModuleResult r = Install(false);
  QCHECK(!r.installed && r.off && std::strcmp(r.status, "off_config") == 0);
  QCHECK(SlotIsTheGames() && tick::Count() == 1 && g_reads.load() == 0);  // the scope row's refresh is the only callback; the off module read nothing
  const std::vector<Line> install = Lines("combat_peerclamp_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kInfo);
  if (install.size() == 1U) {
    QCHECK(combat_test::Golden(install[0].text) ==
           "\"event\":\"combat_peerclamp_install\",\"result\":\"off\",\"reason\":\"off_config\",\"consequence\":\"combat.peerclamp is false: "
           "the game's timeout loop reads every stamp as it is, and a stamp above now drops the peer\"}");
  }
  End();
}

// The image: not loaded, no build ID, another build.
void AMissingOrOtherImageInstallsNothing() {
  Begin();
  QCHECK(std::strcmp(pc::InstallAt(nullptr, g_pins, true).status, "module_not_loaded") == 0);
  g_image.Drop();
  QCHECK(std::strcmp(Install().status, "module_not_loaded") == 0);
  g_image.Build({}, pinned::kLibR15StockBuildId);
  QCHECK(std::strcmp(Install().status, "build_id_mismatch") == 0);
  g_image.Build({}, nullptr, /*note=*/false);
  QCHECK(std::strcmp(Install().status, "no_build_id") == 0);
  const std::vector<Line> lines = Lines("combat_peerclamp_install");
  QCHECK(lines.size() == 4U);
  for (const Line& l : lines) QCHECK(l.level == sentinel::LogLevel::kError && Has(l.text, "\"result\":\"refused\""));
  QCHECK(tick::Count() == 1);  // the module asked for no callback of its own
  End();
}

// 8. The pinned words: the table is pinned, every word flipped in turn (and an unreadable one) refuses the install before the slot or the tick.
void EveryPinnedWordGatesTheInstall() {
  Begin();
  combat_test::EveryPinIsRefused(
      "combat_peerclamp_install", kAllWords, g_image,
      [] {
        combat_test::StartCapture();
        pc::ResetForTest();
        world::Reset();
        Build();
        QCHECK(world::StartScope(&Lookup));
        combat_test::Captured().clear();
      },
      [] { return Install(); }, [] { return SlotIsTheGames(); });
  End();
}

// A scope that cannot change refuses the install: the guard would never be in a combat map or the combat lobby, so no hook is put in (the same refusal as the
// other readers of the answer). Either the scope row never registered its refresh, or the tick is not installed, or the tick table has no room.
void AScopeThatCannotChangeRefusesTheInstall() {
  Begin(/*scopeLive=*/false);  // the tick is not installed, the scope row did not register
  ModuleResult r = Install();
  QCHECK(!r.installed && !r.off && std::strcmp(r.status, "scope_not_live") == 0 && SlotIsTheGames());
  std::vector<Line> install = Lines("combat_peerclamp_install");
  QCHECK(install.size() == 1U && install[0].level == sentinel::LogLevel::kError && Has(install[0].text, "\"reason\":\"scope_not_live\""));
  QCHECK(g_reads.load() == 0 && tick::Count() == 0);
  End();
  Begin(/*scopeLive=*/false);  // the tick is installed, but no row registered the refresh
  QCHECK(tick::InstallAt(&Lookup));
  r = Install();
  QCHECK(!r.installed && std::strcmp(r.status, "scope_not_live") == 0 && SlotIsTheGames() && !scope::Live());
  End();
  Begin(/*scopeLive=*/false);  // the tick table has no room, so the scope row cannot register
  QCHECK(tick::InstallAt(&Lookup));
  for (std::size_t i = tick::Count(); i < tick::kMaxCallbacks; ++i) QCHECK(tick::Add([](std::uintptr_t) noexcept {}));
  QCHECK(!scope::InstallNormal().installed);
  r = Install();
  QCHECK(!r.installed && std::strcmp(r.status, "scope_not_live") == 0 && SlotIsTheGames());
  End();
  Begin(/*scopeLive=*/false);  // off by the key wins over a scope that cannot change: nothing is asked of the scope
  r = Install(false);
  QCHECK(!r.installed && r.off && std::strcmp(r.status, "off_config") == 0);
  End();
}

// A second install: the slot already holds the hook, nothing is installed again.
void ASecondInstallDoesNothing() {
  Begin();
  QCHECK(Install().installed);
  const ModuleResult again = Install();
  QCHECK(!again.installed && Hooked());
  End();
}

// The stats: a Debug line every kStatsEvery in-scope calls, the Info line at the end of the process only when the guard is installed.
void TheStatsAreWrittenEveryKStatsEveryCallsAndAtTheEnd() {
  const CleanupFn hook = InstalledIn({"mpl_combat_dyson"});
  Peers p;
  p.Build(2, 2);
  Bd(p, 0);  // healthy peers: only the calls are counted
  for (unsigned i = 0; i < pc::kStatsEvery - 1; ++i) hook(g_bd);
  QCHECK(Lines("combat_peerclamp_stats").empty());
  hook(g_bd);
  std::vector<Line> stats = Lines("combat_peerclamp_stats");
  QCHECK(stats.size() == 1U && stats[0].level == sentinel::LogLevel::kDebug && Has(stats[0].text, "\"final\":0") &&
         Has(stats[0].text, "\"calls\":4096,\"in_scope\":4096,\"walked\":8192,\"clamped\":0,\"capped\":0,\"final\":0"));
  p.SetStamp(1, kNow + 5);  // one clamp and one more call before the end
  Bd(p, 0);
  hook(g_bd);
  pc::NoteEnd();
  stats = Lines("combat_peerclamp_stats");
  QCHECK(stats.size() == 2U && stats[1].level == sentinel::LogLevel::kInfo && Has(stats[1].text, "\"final\":1") &&
         Has(stats[1].text, "\"calls\":4097,\"in_scope\":4097,\"walked\":8194,\"clamped\":1,\"capped\":0,\"final\":1"));
  End();
  Begin();  // not installed: no final line
  pc::NoteEnd();
  QCHECK(Lines("combat_peerclamp_stats").empty());
  End();
}

}  // namespace

int main() {
  std::memset(g_invalidKey, 0xee, sizeof(g_invalidKey));
  ThePinsAreTheReadelfAndDisassemblyPins();
  TheThunkTakesTheSlotAndTheLineSaysSo();
  TheWalkLowersAStampAboveNowAndNothingElse();
  NothingToWalkStillCallsTheGamesFunction();
  UnreadableMemoryLeavesTheStampsAloneAndCallsTheGamesFunction();
  AnUnreadableNowClampsNothing();
  AnEntryWhoseStampIsUnreadableEndsTheWalk();
  AWalkIsCappedAt4096EntriesWithOneWarning();
  TheClampLinesAreCappedAndTotalsAreWrittenEvery100th();
  AnArenaMatchAndEveryOtherLevelLeaveAFutureStampAloneAndWriteNothing();
  BeforeTheFirstLookNothingIsClamped();
  ATrueAnswerFromAStoppedTickIsNotBelieved();
  TheCombatLobbyAndACombatMapClampAndTheGuardFollowsTheLevel();
  ASlotThatDoesNotHoldCleanupPeersIsRefusedAndLeftAlone();
  ANullOrUnreadableInvalidWordRefusesTheInstall();
  TheSwitchOffInstallsNothingAndSaysSo();
  AMissingOrOtherImageInstallsNothing();
  EveryPinnedWordGatesTheInstall();
  AScopeThatCannotChangeRefusesTheInstall();
  ASecondInstallDoesNothing();
  TheStatsAreWrittenEveryKStatsEveryCallsAndAtTheEnd();
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "peerclamp_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::puts("peerclamp_test: all checks passed");
  return 0;
}
