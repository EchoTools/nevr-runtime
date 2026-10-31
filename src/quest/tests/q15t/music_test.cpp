// Host test of music (src/quest/plugins/q15t/src/music.cpp): the wrapper installed by the real installer on a fake JUMP_SLOT page, called through the slot,
// against a fake PostSoundEvent that records every call (argument layout of the real function, docs/quest/q15t/modules/music.md); the tick
// against a fake CR15Game gamespace table; the full installer against a fake game image carrying the pinned words and the slot at
// the real ELF vaddrs. Real SafeRead, real mprotect, real files.
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/plugin_interface.h"
#include "module_switch.h"
#include "music.h"
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

// ---- the source build's music bank (the source build's music:54-58), restated here and not taken from the module ----
static const uint32_t kPlay[2] = {0x2f22c384, 0xb1b06cfb};
static const uint32_t kStop[2] = {0xde586538, 0x2acba759};
static const uint32_t kState[20] = {0x045d3c23, 0x215e97af, 0x4a61827d, 0x6a599cee, 0x7a9959a5, 0x98a1b868, 0xa3117938,
                                    0xaee8f745, 0xd3282d2b, 0xd9a48db0, 0xd9a48db1, 0xd9a48db3, 0xdb7c7cf9, 0xe34a79b4,
                                    0xe34a79b5, 0xe34a79b6, 0xe44bd2d0, 0xf0a25298, 0xf7898c0b, 0xffae8834};
static const uint64_t kDyson = 0x43e2da7914642604ull, kGauss = 0x43e2da7a0c623a19ull;  // mpl_combat levels (the source build's music:61-62)
static const uintptr_t kNoObj = 0xFFFFFFFFu;

// ---- the fake PostSoundEvent: records the call as the real function reads its registers ----
struct Call {
  uint64_t self, ev, obj, x3, x4, x5, x6, x7;
  uint32_t flags;  // *x3 as the real function reads it (ldr w8,[x3])
  uint32_t id_in;  // *x7 on entry
};
static std::vector<Call> g_calls;
static uint32_t g_next_id = 1000;
static bool g_write_id = true;
extern "C" __attribute__((noinline)) uint64_t FakeReal(uint64_t self, uint64_t ev, uint64_t obj, uint64_t x3, uint64_t x4, uint64_t x5,
                                                       uint64_t x6, uint64_t x7) {
  Call c{self, ev, obj, x3, x4, x5, x6, x7, 0, 0};
  if (x3 != 0) c.flags = *reinterpret_cast<const uint32_t*>(x3);
  if (x7 != 0 && g_write_id) {
    c.id_in = *reinterpret_cast<uint32_t*>(x7);
    *reinterpret_cast<uint32_t*>(x7) = ++g_next_id;
  }
  g_calls.push_back(c);
  return 0x1307;  // low byte 7: the log shows rc=7 only when it is masked
}
extern "C" __attribute__((noinline)) uint64_t OtherFn(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t) {
  return 9;
}
using Hook = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
template <class T>
static uintptr_t Addr(T p) {
  return reinterpret_cast<uintptr_t>(p);
}

struct Slot {
  uintptr_t* w;
  uintptr_t base;
};
static Slot MakePage(uintptr_t word) {
  void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) std::abort();
  auto* w = static_cast<uintptr_t*>(p);
  w[4] = word;  // slot at 0x20
  mprotect(p, 4096, PROT_READ);
  return {w, reinterpret_cast<uintptr_t>(p)};
}

// A fresh module state with the wrapper installed on a new fake page; the call and line records start empty.
static Hook Fresh() {
  q15t::ResetMusicForTest();
  Slot s = MakePage(Addr(&FakeReal));
  const auto r = q15t::InstallMusicAt(s.base, 0x20, Addr(&FakeReal) - s.base);
  CHECK(r.status == q15t::SwapStatus::kOk, "install: %s", q15t::StatusName(r.status));
  g_calls.clear();
  g_lines.clear();
  g_next_id = 1000;
  g_write_id = true;
  return reinterpret_cast<Hook>(s.w[4]);
}

static const uint64_t kSelf = 0xabc0;
static uint32_t g_cflags = 0x55;  // the caller's flags word (non-zero: the extra Stops carry flags 0)
static uint32_t g_cid = 0;
// A post as the game makes it: flags pointer, callback, cookie, seek and a playing-id pointer of its own.
static uint64_t Post(Hook h, uint64_t ev, uint64_t obj) {
  g_cid = 0;
  return h(kSelf, ev, obj, Addr(&g_cflags), 0x4444, 0x5555, 250, Addr(&g_cid));
}

// call `i` is the extra Stop (event, object) the wrapper made: flags 0, no callback, no cookie, seek 0, its own id pointer
static void ExpectStop(size_t i, uint32_t ev, uint32_t obj, uint64_t self = kSelf) {
  if (i >= g_calls.size()) {
    CHECK(false, "call %zu missing", i);
    return;
  }
  const Call& c = g_calls[i];
  CHECK(c.ev == ev && c.obj == obj, "call %zu: event %llx obj %llu, want %x obj %u", i, (unsigned long long)c.ev, (unsigned long long)c.obj, ev, obj);
  CHECK(c.self == self, "call %zu: self %llx", i, (unsigned long long)c.self);
  CHECK(c.x3 != 0 && c.flags == 0, "call %zu: flags word %u (want a pointer to 0)", i, c.flags);
  CHECK(c.x4 == 0 && c.x5 == 0 && c.x6 == 0, "call %zu: callback/cookie/seek %llx %llx %llu", i, (unsigned long long)c.x4,
        (unsigned long long)c.x5, (unsigned long long)c.x6);
  CHECK(c.x7 != 0 && c.x7 != Addr(&g_cid) && c.id_in == 0, "call %zu: id pointer %llx, id in %u", i, (unsigned long long)c.x7, c.id_in);
}
// call `i` is the game's own post, forwarded untouched
static void ExpectGame(size_t i, uint64_t ev, uint64_t obj) {
  if (i >= g_calls.size()) {
    CHECK(false, "call %zu missing", i);
    return;
  }
  const Call& c = g_calls[i];
  CHECK(c.ev == ev && c.obj == obj && c.self == kSelf, "game call %zu: event %llx obj %llu self %llx", i, (unsigned long long)c.ev,
        (unsigned long long)c.obj, (unsigned long long)c.self);
  CHECK(c.x3 == Addr(&g_cflags) && c.flags == 0x55 && c.x4 == 0x4444 && c.x5 == 0x5555 && c.x6 == 250 && c.x7 == Addr(&g_cid),
        "game call %zu: arguments changed", i);
}

// ---- the fake CR15Game: one gamespace in the table at +0x1f48 (count at +0x1f78) with its level at gs+0x58 ----
alignas(16) static uint8_t g_game[0x2000];
alignas(16) static uint64_t g_tab[2];
alignas(16) static uint8_t g_gs[0x80];
static void World(uint64_t level) {
  std::memset(g_game, 0, sizeof g_game);
  std::memset(g_gs, 0, sizeof g_gs);
  *reinterpret_cast<uint64_t*>(g_game + 0x1f48) = Addr(g_tab);
  *reinterpret_cast<uint64_t*>(g_game + 0x1f78) = 1;
  g_tab[0] = 0x77;
  g_tab[1] = Addr(g_gs);
  *reinterpret_cast<uint64_t*>(g_gs + 0x8) = Addr(g_game);
  *reinterpret_cast<uint64_t*>(g_gs + 0x58) = level;
}
static void Tick() {
  q15t::ResetMusicScheduleForTest();
  q15t::MusicTick(Addr(g_game));
}

// ---- a fake game image: the pinned words at base + ELF vaddr and the slot at the real vaddr, on a read-only page ----
struct Image {
  uint8_t* mem;
  size_t len;
  uintptr_t base() const { return Addr(mem); }
  uintptr_t slot() const { return *reinterpret_cast<uintptr_t*>(mem + q15t::kMusicSlot); }
};
static Image MakeImage(bool good_text, uintptr_t slot_word_vaddr) {
  Image im{nullptr, 0x36d8000};
  void* m = mmap(nullptr, im.len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (m == MAP_FAILED) std::abort();
  im.mem = static_cast<uint8_t*>(m);
  if (good_text)
    for (size_t i = 0; i < q15t::kMusicWordPinCount; ++i)
      *reinterpret_cast<uint32_t*>(im.mem + q15t::kMusicWordPins[i].va) = q15t::kMusicWordPins[i].want;
  *reinterpret_cast<uintptr_t*>(im.mem + q15t::kMusicSlot) = im.base() + slot_word_vaddr;
  mprotect(im.mem + 0x36d7000, 0x1000, PROT_READ);  // the slot's page, like RELRO
  return im;
}

int main() {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);
  char tmpl[] = "/tmp/q15t-music-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  const std::string dir = tmpl;
  q15t::SetModuleSwitchDirForTest(dir.c_str());

  // the pinned addresses (R0_readelf_r.txt line 118641; disassembly 0x2649264 - 0x100000)
  CHECK(q15t::kMusicSlot == 0x36d76e8 && q15t::kMusicReal == 0x2549264, "pinned addresses changed");

  // ---- 1. install on a fake slot: swapped, one INFO line ----
  {
    q15t::ResetMusicForTest();
    Slot s = MakePage(Addr(&FakeReal));
    const auto r = q15t::InstallMusicAt(s.base, 0x20, Addr(&FakeReal) - s.base);
    CHECK(r.status == q15t::SwapStatus::kOk, "install: %s", q15t::StatusName(r.status));
    CHECK(s.w[4] != Addr(&FakeReal) && s.w[4] != 0, "slot not replaced");
    CHECK(CountLines("music on", 1) == 1 && CountLines("status=ok") == 1, "no INFO install line");
  }

  // ---- 2. an event that is not in the music bank: forwarded untouched, nothing else, no log, no counter ----
  {
    const Hook h = Fresh();
    const uint32_t others[] = {0x12345678, kPlay[0] ^ 1u, kPlay[1] + 1u, kStop[0] ^ 0x80000000u, kStop[1] - 1u, kState[0] ^ 1u,
                               kState[19] + 1u, 0, 0xffffffffu};
    for (const uint32_t ev : others) {
      g_calls.clear();
      const uint64_t r = Post(h, ev, 9);
      CHECK(r == 0x1307 && g_calls.size() == 1, "event %08x: %zu calls, result %llx", ev, g_calls.size(), (unsigned long long)r);
      ExpectGame(0, ev, 9);
    }
    CHECK(g_lines.empty(), "a non-music event logged %zu lines", g_lines.size());
    const auto st = q15t::GetMusicStats();
    CHECK(st.bank_posts == 0 && st.extra_stops == 0 && st.tick_stops == 0, "counters moved for non-music events");
  }

  // ---- 3. every one of the 24 bank ids is a bank event and logged; only the 2 Plays make extra Stops ----
  {
    const Hook h = Fresh();
    for (const uint32_t ev : kState) {
      g_calls.clear();
      g_lines.clear();
      Post(h, ev, 31);
      char want[96];
      std::snprintf(want, sizeof want, "music post event=%08x kind=state obj=31 rc=7 playing_id=%u", ev, g_next_id);
      CHECK(g_calls.size() == 1 && CountLines(want, 1) == 1 && g_lines.size() == 1, "SetState %08x: %zu calls, %zu lines", ev, g_calls.size(),
            g_lines.size());
      ExpectGame(0, ev, 31);
    }
    for (const uint32_t ev : kStop) {
      g_calls.clear();
      g_lines.clear();
      Post(h, ev, 31);
      char want[96];
      std::snprintf(want, sizeof want, "music post event=%08x kind=stop obj=31 rc=7 playing_id=%u", ev, g_next_id);
      CHECK(g_calls.size() == 1 && CountLines(want, 1) == 1 && g_lines.size() == 1, "Stop %08x: %zu calls, %zu lines", ev, g_calls.size(),
            g_lines.size());
      ExpectGame(0, ev, 31);
    }
    for (const uint32_t ev : kPlay) {
      g_calls.clear();
      g_lines.clear();
      Post(h, ev, 31);
      CHECK(g_calls.size() >= 3 && g_calls.back().ev == ev, "Play %08x: %zu calls", ev, g_calls.size());
      CHECK(CountLines("music post event=", 1) == 1 && CountLines("kind=play") == 1 && CountLines("music play: both music Stops posted first", 1) == 1,
            "Play %08x: lines", ev);
    }
    CHECK(q15t::GetMusicStats().bank_posts == 24, "bank posts %llu", (unsigned long long)q15t::GetMusicStats().bank_posts);
    // only the low 32 bits of the event register name the event (the real function passes w20 on)
    g_calls.clear();
    g_lines.clear();
    const uint64_t sym = (1ull << 32) | kState[3];
    Post(h, sym, 31);
    CHECK(g_calls.size() == 1 && g_calls[0].ev == sym && CountLines("music post event=6a599cee kind=state", 1) == 1 && g_lines.size() == 1,
          "high bits of the event register");
  }

  // ---- 4. a Play: both Stops on its object come first, then the Play itself with the game's own arguments ----
  {
    const Hook h = Fresh();
    const uint64_t r = Post(h, kPlay[0], 17);
    CHECK(r == 0x1307, "result of the Play not returned: %llx", (unsigned long long)r);
    CHECK(g_calls.size() == 3, "first Play: %zu calls", g_calls.size());
    ExpectStop(0, kStop[0], 17);
    ExpectStop(1, kStop[1], 17);
    ExpectGame(2, kPlay[0], 17);
    CHECK(g_cid == 1003, "the game's playing-id slot holds %u, want the Play's own 1003", g_cid);
    CHECK(CountLines("music play: both music Stops posted first event=2f22c384 obj=17 prev_obj=4294967295 stops= de586538 obj 17 -> 7 id 1001; 2acba759 obj 17 -> 7 id 1002;", 1) == 1,
          "Play line");
    CHECK(CountLines("music post event=2f22c384 kind=play obj=17 rc=7 playing_id=1003", 1) == 1, "post line of the Play");
    CHECK(g_lines.size() == 2, "lines: %zu", g_lines.size());

    // a Play on another object: its own Stops, then the previous Play's object, then the Play
    g_calls.clear();
    Post(h, kPlay[1], 23);
    CHECK(g_calls.size() == 5, "second Play: %zu calls", g_calls.size());
    ExpectStop(0, kStop[0], 23);
    ExpectStop(1, kStop[1], 23);
    ExpectStop(2, kStop[0], 17);
    ExpectStop(3, kStop[1], 17);
    ExpectGame(4, kPlay[1], 23);
    CHECK(CountLines("prev_obj=17") == 1, "second Play line does not name the previous object");

    // the same object again: its Stops only (the previous object is the same one)
    g_calls.clear();
    Post(h, kPlay[1], 23);
    CHECK(g_calls.size() == 3, "third Play: %zu calls", g_calls.size());
    ExpectStop(0, kStop[0], 23);
    ExpectStop(1, kStop[1], 23);
    ExpectGame(2, kPlay[1], 23);

    // the source skips the no-object sentinel as a target (the source build's music:102): only the previous object is stopped
    g_calls.clear();
    Post(h, kPlay[0], kNoObj);
    CHECK(g_calls.size() == 3, "Play on the no-object sentinel: %zu calls", g_calls.size());
    ExpectStop(0, kStop[0], 23);
    ExpectStop(1, kStop[1], 23);
    ExpectGame(2, kPlay[0], kNoObj);

    const auto st = q15t::GetMusicStats();
    CHECK(st.extra_stops == 2 + 4 + 2 + 2 && st.bank_posts == 4, "counters: %llu extra, %llu bank", (unsigned long long)st.extra_stops,
          (unsigned long long)st.bank_posts);
  }

  // ---- 5. the playing-id pointer: none, or unreadable: logged as 0, never a fault; the game's arguments untouched ----
  {
    const Hook h = Fresh();
    g_write_id = false;
    h(kSelf, kState[0], 5, Addr(&g_cflags), 1, 2, 3, 0);
    CHECK(g_calls.size() == 1 && g_calls[0].x7 == 0 && CountLines("playing_id=0") == 1, "no id pointer");
    g_calls.clear();
    g_lines.clear();
    h(kSelf, kState[0], 5, Addr(&g_cflags), 1, 2, 3, 0x20);
    CHECK(g_calls.size() == 1 && g_calls[0].x7 == 0x20 && CountLines("playing_id=0") == 1, "unreadable id pointer");
  }

  // ---- 6. the line budget: 4000 lines, then one WARNING, then silence; counters go on ----
  {
    const Hook h = Fresh();
    for (int i = 0; i < 4005; ++i) Post(h, kState[i % 20], 5);
    CHECK(CountLines("music post", 1) == 4000, "post lines %d", CountLines("music post", 1));
    CHECK(CountLines("line budget used up", 2) == 1 && CountLines("max_lines=4000") == 1, "budget warning: %d", CountLines("line budget used up", 2));
    CHECK(g_lines.size() == 4001, "lines %zu", g_lines.size());
    CHECK(q15t::GetMusicStats().bank_posts == 4005 && g_calls.size() == 4005, "a post was lost after the budget");
  }

  // ---- 7. the tick: both Stops once, on the last Play's object, when the combat map is gone after a Play ----
  {
    const Hook h = Fresh();
    World(kDyson);
    Tick();  // a combat map loaded, nothing played yet
    CHECK(g_calls.empty(), "tick before a Play posted");
    Post(h, kPlay[0], 40);
    g_calls.clear();
    g_lines.clear();
    Tick();  // still loaded
    CHECK(g_calls.empty(), "tick with the combat map still loaded posted");
    World(0x1234);  // the lobby
    Tick();
    CHECK(g_calls.size() == 2, "tick after the map went: %zu calls", g_calls.size());
    ExpectStop(0, kStop[0], 40);
    ExpectStop(1, kStop[1], 40);
    CHECK(CountLines("music: combat map gone, both music Stops posted obj=40 stops= de586538 obj 40 -> 7 id", 1) == 1 &&
              CountLines("2acba759 obj 40 -> 7 id") == 1,
          "tick line");
    CHECK(q15t::GetMusicStats().tick_stops == 1, "tick_stops %llu", (unsigned long long)q15t::GetMusicStats().tick_stops);
    // once: a second look at the lobby, and a second leave without a new Play, post nothing
    g_calls.clear();
    Tick();
    World(kGauss);
    Tick();
    World(0x1234);
    Tick();
    CHECK(g_calls.empty(), "tick posted twice for one Play: %zu calls", g_calls.size());
    // a new Play re-arms it (payload map level), on the new object
    Post(h, kPlay[1], 41);
    g_calls.clear();
    World(kGauss);
    Tick();
    World(0x1234);
    Tick();
    CHECK(g_calls.size() == 2, "re-armed tick: %zu calls", g_calls.size());
    ExpectStop(0, kStop[0], 41);
    ExpectStop(1, kStop[1], 41);
  }

  // ---- 8. the tick: a combat map never seen, no Play, a look inside 0.5 s: no Stops ----
  {
    const Hook h = Fresh();
    World(0x1234);
    Post(h, kPlay[0], 40);
    g_calls.clear();
    Tick();
    Tick();
    CHECK(g_calls.empty(), "a lobby that was never a combat map posted Stops");
    World(kDyson);
    Tick();
    World(0x1234);
    Tick();  // the Play is still pending: now it fires
    CHECK(g_calls.size() == 2, "pending Play did not stop after the map went: %zu", g_calls.size());

    Fresh();
    World(kDyson);
    Tick();
    World(0x1234);
    Tick();
    CHECK(g_calls.empty(), "Stops posted although no music Play was made");

    const Hook h2 = Fresh();
    World(kDyson);
    Tick();
    Post(h2, kPlay[0], 40);
    g_calls.clear();
    World(0x1234);
    q15t::ResetMusicScheduleForTest();
    q15t::MusicTick(Addr(g_game));  // looks (schedule forgotten): now 0.5 s ahead
    CHECK(g_calls.size() == 2, "first look did not post: %zu", g_calls.size());
    // the schedule: a second look at once is skipped, even though the map came back and went again between
    Post(h2, kPlay[0], 40);
    g_calls.clear();
    World(kDyson);
    q15t::MusicTick(Addr(g_game));  // inside 0.5 s of the last look: not looked at
    World(0x1234);
    q15t::MusicTick(Addr(g_game));
    CHECK(g_calls.empty(), "a look inside the 0.5 s period posted: %zu", g_calls.size());
    World(kDyson);
    Tick();
    World(0x1234);
    Tick();
    CHECK(g_calls.size() == 2, "looks after the period did not post: %zu", g_calls.size());
  }

  // ---- 9. the tick without a real function (not installed): no fault, nothing posted ----
  {
    q15t::ResetMusicForTest();
    g_calls.clear();
    g_lines.clear();
    q15t::MusicForceStateForTest(true, kSelf, 40);
    World(kDyson);
    Tick();
    World(0x1234);
    Tick();
    CHECK(g_calls.empty() && g_lines.empty() && q15t::GetMusicStats().tick_stops == 0, "tick without a real function posted");
    // the wrapper itself, called after the real function was forgotten: answers 0, calls nothing, never faults
    const Hook h = Fresh();
    q15t::ResetMusicForTest();
    g_calls.clear();
    CHECK(Post(h, kState[0], 5) == 0 && Post(h, 0x12345678, 5) == 0 && g_calls.empty(), "wrapper without a real function");
  }

  // ---- 10. refusals on a fake slot: nothing written, an ERROR naming pin / found / expected ----
  {
    q15t::ResetMusicForTest();
    g_lines.clear();
    Slot bad = MakePage(0x4242420);
    const auto rb = q15t::InstallMusicAt(bad.base, 0x20, Addr(&FakeReal) - bad.base);
    CHECK(rb.status == q15t::SwapStatus::kUnexpectedOriginal && bad.w[4] == 0x4242420, "foreign word: %s", q15t::StatusName(rb.status));
    char named[128];
    std::snprintf(named, sizeof named, "pin=0x20 found=0x4242420 expected=0x%llx", static_cast<unsigned long long>(Addr(&FakeReal)));
    CHECK(CountLines("music refused", 3) == 1 && CountLines(named) == 1 && CountLines("music on") == 0, "refusal not loud / does not name pin, found, expected");
    // a word that points at real executable code, but not at the pinned function, is refused too (no by-name acceptance, no chaining)
    g_lines.clear();
    Slot other = MakePage(Addr(&OtherFn));
    const auto rc = q15t::InstallMusicAt(other.base, 0x20, Addr(&FakeReal) - other.base);
    CHECK(rc.status == q15t::SwapStatus::kUnexpectedOriginal && other.w[4] == Addr(&OtherFn), "another wrapper's word taken: %s", q15t::StatusName(rc.status));
    CHECK(CountLines("music refused", 3) == 1, "foreign function refusal not loud");
    // the slot is not in the image at all
    const auto rn = q15t::InstallMusicAt(0, 0x20, Addr(&FakeReal));
    CHECK(rn.status != q15t::SwapStatus::kOk, "a null base was accepted");
  }

  // ---- 11. music.off in the data directory: nothing swapped, one INFO line ----
  {
    q15t::ResetMusicForTest();
    g_lines.clear();
    FILE* m = std::fopen((dir + "/music.off").c_str(), "w");
    std::fclose(m);
    Slot off = MakePage(Addr(&FakeReal));
    const auto ro = q15t::InstallMusicAt(off.base, 0x20, Addr(&FakeReal) - off.base);
    CHECK(ro.status != q15t::SwapStatus::kOk && std::strcmp(ro.reason, "off_file") == 0, "off file ignored: %s", q15t::StatusName(ro.status));
    CHECK(off.w[4] == Addr(&FakeReal), "slot swapped although music.off exists");
    CHECK(CountLines("music off", 1) == 1 && CountLines("music on") == 0, "no INFO off line");
    // another module's switch file does not turn this one off
    unlink((dir + "/music.off").c_str());
    m = std::fopen((dir + "/fxtrail.off").c_str(), "w");
    std::fclose(m);
    const auto rk = q15t::InstallMusicAt(off.base, 0x20, Addr(&FakeReal) - off.base);
    CHECK(rk.status == q15t::SwapStatus::kOk, "another module's off file refused music: %s", q15t::StatusName(rk.status));
    unlink((dir + "/fxtrail.off").c_str());
  }

  // ---- 12. the real installer on a fake game image: gate order, no write when refused ----
  {
    // right build: pins match, the slot holds the stock function -> swapped
    Image good = MakeImage(true, q15t::kMusicReal);
    g_lines.clear();
    q15t::ResetMusicForTest();
    const auto r = q15t::InstallMusic(good.base());
    CHECK(r.status == q15t::SwapStatus::kOk, "install on the right image: %s %s", q15t::StatusName(r.status), r.reason);
    CHECK(good.slot() != good.base() + q15t::kMusicReal && good.slot() != 0, "slot not swapped on the right image");
    CHECK(CountLines("music on", 1) == 1 && CountLines("slot_vaddr=0x36d76e8 real_vaddr=0x2549264 status=ok") == 1, "install line");

    // wrong build: the words differ -> refused before the slot is looked at, one ERROR naming the first pin, nothing written
    Image wrong = MakeImage(false, q15t::kMusicReal);
    g_lines.clear();
    q15t::ResetMusicForTest();
    const auto rw = q15t::InstallMusic(wrong.base());
    CHECK(rw.status != q15t::SwapStatus::kOk && std::strcmp(rw.reason, "word_pin_mismatch") == 0, "wrong build: %s %s", q15t::StatusName(rw.status), rw.reason);
    CHECK(wrong.slot() == wrong.base() + q15t::kMusicReal, "slot written although a pin differs");
    CHECK(CountLines("music refused: code word pin mismatch", 3) == 1 && CountLines("pin=0x2549264 found=0x00000000 expected=0xd10183ff") == 1 &&
              CountLines("music on") == 0,
          "wrong build refusal not loud / does not name the pin");

    // right words, but the slot holds something else -> refused, the slot word untouched, ERROR naming the slot, found and expected
    Image foreign = MakeImage(true, q15t::kMusicReal + 4);
    g_lines.clear();
    q15t::ResetMusicForTest();
    const auto rf = q15t::InstallMusic(foreign.base());
    CHECK(rf.status == q15t::SwapStatus::kUnexpectedOriginal && foreign.slot() == foreign.base() + q15t::kMusicReal + 4, "foreign slot: %s", q15t::StatusName(rf.status));
    char want[160];
    std::snprintf(want, sizeof want, "pin=0x36d76e8 found=0x%llx expected=0x%llx", static_cast<unsigned long long>(foreign.base() + q15t::kMusicReal + 4),
                  static_cast<unsigned long long>(foreign.base() + q15t::kMusicReal));
    CHECK(CountLines("music refused", 3) == 1 && CountLines(want) == 1, "foreign slot refusal does not name pin / found / expected");

    // music.off with the right image: nothing written
    Image off = MakeImage(true, q15t::kMusicReal);
    FILE* m = std::fopen((dir + "/music.off").c_str(), "w");
    std::fclose(m);
    g_lines.clear();
    q15t::ResetMusicForTest();
    const auto ro = q15t::InstallMusic(off.base());
    CHECK(std::strcmp(ro.reason, "off_file") == 0 && off.slot() == off.base() + q15t::kMusicReal && CountLines("music off", 1) == 1, "off file on the image");
    unlink((dir + "/music.off").c_str());

    // an unreadable game library: refused, the pin named as unreadable
    g_lines.clear();
    const auto ru = q15t::InstallMusic(0x1000);
    CHECK(ru.status != q15t::SwapStatus::kOk && CountLines("found=unreadable", 3) == 1, "unreadable library accepted");
    q15t::ResetMusicForTest();
  }

  rmdir(dir.c_str());

  // ---- code-word pins: every word read, a changed or unreadable one refused loudly, no file hash anywhere ----
  {
    auto count = [](const char* needle, int level) { return CountLines(needle, level); };
    auto clear = []() { g_lines.clear(); };
    g_fail += pincheck::Run("music", q15t::kMusicWordPins, q15t::kMusicWordPinCount, 29, 0xde869a4ebfff2bb8ull,
                            [](std::uintptr_t b) { return q15t::MusicWordPinsMatch(b); }, count, clear);
  }

  if (g_fail == 0) std::printf("music_test: all checks passed\n");
  return g_fail == 0 ? 0 : 1;
}
