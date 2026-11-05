// Host test of equiprefire (src/quest/plugins/q15t/src/equiprefire.cpp, issue #2121) against a fake game: the six setter words of the pinned build
// are swapped through the real install path (real mprotect, real /proc/self/maps), the "setters" are host functions behind x86 trampolines at the
// pinned ELF vaddrs, the component, its records, its actor links and the net game are laid out as the game's (the offsets are the shared rule
// header's, src/runtime/patch/equipment_refire_rules.h), and the wrappers are called through the swapped words as the game does. Real SafeRead, a fake clock.
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "equiprefire.h"
#include "module_switch.h"
#include "nevr/plugin_interface.h"
#include "pbhud_image.h"
#include "pin_check.h"
#include "plugin_log.h"
#include "runtime/patch/equipment_refire_rules.h"

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

namespace rules = nevr_equipment_refire;
using u64 = std::uint64_t;
using u32 = std::uint32_t;

// ---- log capture ----
struct Line {
  uint32_t level;
  std::string text;
};
static std::vector<Line> g_lines;
static void FakeLog(const char* id, uint32_t level, const char* msg, const NevrLogField* f, uint32_t n) {
  std::string t = std::string("plugin=") + id + " " + msg;
  for (uint32_t i = 0; i < n; ++i) t += std::string(" ") + f[i].key + "=" + f[i].value;
  if (std::getenv("PBHUD_LOG")) std::printf("LOG level=%u %s\n", level, t.c_str());
  g_lines.push_back({level, t});
}
static int Count(const std::string& needle, int level = -1) {
  int n = 0;
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || static_cast<int>(l.level) == level)) ++n;
  return n;
}
static std::string Find(const std::string& needle) {
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos) return l.text;
  return "";
}

// ---- the fake game ----
// One arena, page aligned, so a test can unmap the component alone. Layout (offsets into the arena):
constexpr size_t kPage = 0x1000;
constexpr size_t kCompOff = 0, kCtxOff = kPage, kIndexOff = 2 * kPage, kRecOff = 3 * kPage, kLinkOff = 4 * kPage, kNetArgOff = 5 * kPage,
                 kNetArgSize = 0x9000, kGameOff = kNetArgOff + kNetArgSize, kArenaSize = kGameOff + kPage;

constexpr u32 kLocalHandle = (5u << 16) | 0;   // sparse 0, generation 5, dense 0: the local player's record
constexpr u32 kRemoteHandle = (7u << 16) | 1;  // sparse 1, generation 7, dense 1: another player's record
constexpr u64 kLocalLink = 0x1111, kRemoteLink = 0x2222, kKey0 = 0xabcdef;
constexpr u32 kLocalSlot = (9u << 16) | 0;       // the net game's slot 0: generation 9, field 0
constexpr u32 kRemoteSlot = (3u << 16) | 1;
constexpr u64 kVtable = 0x7e570000;

struct World {
  std::uint8_t* mem = nullptr;
  std::uint8_t* comp() const { return mem + kCompOff; }
  std::uint8_t* rec(int dense) const { return mem + kRecOff + dense * rules::kRecordStride; }
  template <typename T>
  void Put(size_t off, T v) const {
    std::memcpy(mem + off, &v, sizeof v);
  }
  u64& Value(int dense, int slot) const { return *reinterpret_cast<u64*>(rec(dense) + rules::kRecordValueOffset[slot]); }
  std::uintptr_t cs() const { return reinterpret_cast<std::uintptr_t>(comp()); }
};

static World Make() {
  World w;
  void* p = mmap(nullptr, kArenaSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) std::abort();
  w.mem = static_cast<std::uint8_t*>(p);
  const auto at = [&](size_t off) { return reinterpret_cast<std::uintptr_t>(w.mem + off); };
  // the component: vtable, context, index table, record array, link table, record count
  w.Put<std::uintptr_t>(kCompOff, kVtable);
  w.Put<std::uintptr_t>(kCompOff + rules::kCtxOffset, at(kCtxOff));
  w.Put<std::uintptr_t>(kCompOff + rules::kIndexTableOffset, at(kIndexOff));
  w.Put<std::uintptr_t>(kCompOff + rules::kRecordsOffset, at(kRecOff));
  w.Put<std::uintptr_t>(kCompOff + rules::kLinkTableOffset, at(kLinkOff));
  w.Put<std::uint16_t>(kCompOff + rules::kCountOffset, 2);
  // the context: the net game getter's argument and the first half of the actor key
  w.Put<std::uintptr_t>(kCtxOff + rules::kCtxNetGameArgOffset, at(kNetArgOff));
  w.Put<u64>(kCtxOff + rules::kCtxActorKeyOffset, kKey0);
  // index table: sparse 0 -> dense 0 generation 5, sparse 1 -> dense 1 generation 7
  w.Put<std::uint16_t>(kIndexOff + 0, 0);
  w.Put<std::uint16_t>(kIndexOff + 2, 5);
  w.Put<std::uint16_t>(kIndexOff + 4, 1);
  w.Put<std::uint16_t>(kIndexOff + 6, 7);
  w.Put<u64>(kLinkOff + 0 * rules::kLinkStride, kLocalLink);
  w.Put<u64>(kLinkOff + 1 * rules::kLinkStride, kRemoteLink);
  for (int d = 0; d < 2; ++d)
    for (int s = 0; s < 3; ++s) w.Value(d, s) = 0xA0 + s;
  // the net game: two slots in use, slot 0 (field 0) with generation 9
  w.Put<std::uint16_t>(kGameOff + rules::kGameSlotCountOffset, 2);
  w.Put<std::uint16_t>(kGameOff + rules::kGameLocalFieldOffset, 0);
  w.Put<std::uint16_t>(kGameOff + rules::kGameSlotGenerationOffset, 9);
  return w;
}
static void Free(World* w) {
  if (w->mem != nullptr) munmap(w->mem, kArenaSize);
  w->mem = nullptr;
}

static World* g_world = nullptr;

// The stock setters: the record write the game does only when the value differs; every call is recorded in full.
struct SetterCall {
  std::uintptr_t cs;
  u64 handle, value;
};
static std::vector<SetterCall> g_setter_calls[3];
template <int S>
static void FakeSetter(void* cs, u64 handle, u64 value) {
  g_setter_calls[S].push_back({reinterpret_cast<std::uintptr_t>(cs), handle, value});
  const u32 sparse = static_cast<u32>(handle) & 0xffff;
  std::uint16_t dense;
  std::memcpy(&dense, g_world->mem + kIndexOff + sparse * 4, 2);
  g_world->Value(dense, S) = value;
}
struct HandlerCall {
  std::uintptr_t cs;
  u64 handle;
};
static std::vector<HandlerCall> g_handler_calls[3];
template <int S>
static void FakeHandler(void* cs, u64 handle) {
  g_handler_calls[S].push_back({reinterpret_cast<std::uintptr_t>(cs), handle});
}
static int g_netgame_calls = 0, g_userid_calls = 0;
static std::uintptr_t FakeNetGame(std::uintptr_t arg) {
  ++g_netgame_calls;
  return arg == reinterpret_cast<std::uintptr_t>(g_world->mem + kNetArgOff) ? reinterpret_cast<std::uintptr_t>(g_world->mem + kGameOff) : 0;
}
static u32 FakeUserId(void* game, u64 key0, u64 key1) {
  ++g_userid_calls;
  if (game != g_world->mem + kGameOff || key0 != kKey0) return 0xffffffffu;
  return key1 == kLocalLink ? kLocalSlot : kRemoteSlot;
}

static u64 g_now = 0;
static u64 Clock() { return g_now; }

using SetterFn = void (*)(void*, u64, u64);
static const q15t::EquipRefireAddrs& A = q15t::kEquipRefirePinned;

static q15t::EquipRefireFns HostFns() {
  q15t::EquipRefireFns f{};
  f.handler[0] = reinterpret_cast<std::uintptr_t>(&FakeHandler<0>);
  f.handler[1] = reinterpret_cast<std::uintptr_t>(&FakeHandler<1>);
  f.handler[2] = reinterpret_cast<std::uintptr_t>(&FakeHandler<2>);
  f.net_game = reinterpret_cast<std::uintptr_t>(&FakeNetGame);
  f.user_id = reinterpret_cast<std::uintptr_t>(&FakeUserId);
  return f;
}

// An image whose six words hold the stock setters (trampolines to the fake setters at the pinned vaddrs).
static void Fresh(pbhud::Image& img, World& w) {
  g_lines.clear();
  for (auto& v : g_setter_calls) v.clear();
  for (auto& v : g_handler_calls) v.clear();
  g_netgame_calls = g_userid_calls = 0;
  g_now = 1000;
  g_world = &w;
  q15t::ResetEquipRefireForTest();
  q15t::SetEquipRefireClockForTest(&Clock);
  q15t::SetModuleSwitchDirForTest("/nonexistent-equiprefire-test-dir");
  img.Real(A.setter[0], reinterpret_cast<void*>(&FakeSetter<0>));
  img.Real(A.setter[1], reinterpret_cast<void*>(&FakeSetter<1>));
  img.Real(A.setter[2], reinterpret_cast<void*>(&FakeSetter<2>));
  for (int s = 0; s < 3; ++s) {
    img.Slot(A.jump_slot[s], A.setter[s]);
    img.Slot(A.glob_dat[s], A.setter[s]);
  }
}
static SetterFn Word(pbhud::Image& img, int slot, int word) {
  return reinterpret_cast<SetterFn>(img.Get(word == 0 ? A.jump_slot[slot] : A.glob_dat[slot]));
}
static bool Swapped(pbhud::Image& img, int slot, int word) {
  return img.Get(word == 0 ? A.jump_slot[slot] : A.glob_dat[slot]) != img.base + A.setter[slot];
}
static size_t Handlers() { return g_handler_calls[0].size() + g_handler_calls[1].size() + g_handler_calls[2].size(); }

// the arguments a stock caller passes: garbage in the handle's upper half (the callee masks it)
static u64 Wide(u32 handle) { return 0xdead000000000000ull | handle; }

int main() {
  NevrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);

  // (a) three changes within 300 ms through the installed wrappers, then ticks on a fake clock: exactly one handler call after kSettleMs,
  //     carrying the last handle; the first two changes are not re-fired; it fires once and then not again.
  {
    pbhud::Image img;
    World w = Make();
    Fresh(img, w);
    const q15t::InstallResult r = q15t::InstallEquipRefireAt(img.base, A, HostFns());
    CHECK(r.status == q15t::SwapStatus::kOk, "install status %s", q15t::StatusName(r.status));
    for (int s = 0; s < 3; ++s)
      for (int k = 0; k < 2; ++k) CHECK(Swapped(img, s, k), "slot %d word %d not swapped", s, k);
    CHECK(Count("equiprefire: slot swapped", 1) == 6, "want 6 INFO slot lines, got %d", Count("equiprefire: slot swapped", 1));
    CHECK(Count("equiprefire: on", 1) == 1 && Count("settle_ms=2500") == 1, "summary line");
    g_now = 10000;
    Word(img, 0, 0)(w.comp(), Wide(kLocalHandle), 0x11);
    g_now = 10100;
    Word(img, 0, 1)(w.comp(), Wide(kLocalHandle), 0x22);
    g_now = 10300;
    Word(img, 0, 0)(w.comp(), Wide(kLocalHandle), 0x33);
    CHECK(g_setter_calls[0].size() == 3, "the original ran %zu times, want 3", g_setter_calls[0].size());
    CHECK(w.Value(0, 0) == 0x33, "the original did not write the value");
    q15t::EquipRefireTick(0);
    CHECK(Handlers() == 0, "a handler call before the slot was still");
    g_now = 10300 + rules::kSettleMs - 1;
    q15t::EquipRefireTick(0);
    CHECK(Handlers() == 0, "a handler call 1 ms before kSettleMs");
    g_now = 10300 + rules::kSettleMs;
    q15t::EquipRefireTick(0);
    CHECK(g_handler_calls[0].size() == 1 && Handlers() == 1, "want exactly one weapon handler call at kSettleMs, got %zu/%zu", g_handler_calls[0].size(),
          Handlers());
    if (!g_handler_calls[0].empty()) {
      CHECK(g_handler_calls[0][0].cs == w.cs(), "handler component");
      CHECK(g_handler_calls[0][0].handle == kLocalHandle, "handler handle 0x%llx, want the last handle 0x%x", (unsigned long long)g_handler_calls[0][0].handle,
            kLocalHandle);
    }
    const std::string l = Find("equiprefire: equipment re-fired");
    CHECK(l.find("slot=weapon") != std::string::npos && l.find("value=0x0000000000000033") != std::string::npos &&
              l.find("handle=0x00050000") != std::string::npos && l.find("changes=3") != std::string::npos &&
              l.find("quiet_ms=2500") != std::string::npos,
          "re-fire line: %s", l.c_str());
    CHECK(Count("equiprefire: equipment re-fired", 1) == 1, "the re-fire line is not one INFO line");
    g_now += 60000;
    q15t::EquipRefireTick(0);
    CHECK(Handlers() == 1, "a second re-fire without a new change");
    // the Debug record of a setter call carries slot, word, verdict, handle, value, read/changed/local
    const std::string d = Find("equiprefire: equipment change");
    CHECK(d.find("slot=weapon") != std::string::npos && d.find("verdict=tracked") != std::string::npos && d.find("read=yes") != std::string::npos &&
              d.find("changed=yes") != std::string::npos && d.find("local=yes") != std::string::npos,
          "setter record: %s", d.c_str());
    CHECK(Count("equiprefire: equipment change", 0) == 3, "want 3 Debug records, got %d", Count("equiprefire: equipment change", 0));
    const auto st = q15t::GetEquipRefireStats();
    CHECK(st.tracked == 3 && st.refired == 1 && st.skipped == 0 && st.on, "stats %u/%u/%u", st.tracked, st.refired, st.skipped);
    Free(&w);
  }

  // (b) another player's record: the original runs, nothing is tracked, no handler call ever
  {
    pbhud::Image img;
    World w = Make();
    Fresh(img, w);
    q15t::InstallEquipRefireAt(img.base, A, HostFns());
    for (int s = 0; s < 3; ++s) Word(img, s, 0)(w.comp(), Wide(kRemoteHandle), 0x55);
    CHECK(g_setter_calls[0].size() == 1 && g_setter_calls[1].size() == 1 && g_setter_calls[2].size() == 1, "originals not called for a remote record");
    g_now += 100000;
    q15t::EquipRefireTick(0);
    CHECK(Handlers() == 0, "a remote record was re-fired");
    const auto st = q15t::GetEquipRefireStats();
    CHECK(st.not_local == 3 && st.tracked == 0, "stats not_local=%u tracked=%u", st.not_local, st.tracked);
    CHECK(Count("verdict=not_local", 0) == 3, "no not_local Debug records");
    Free(&w);
  }

  // (c) a value equal to the record's: the original runs, nothing is tracked, the lookups are not even asked
  {
    pbhud::Image img;
    World w = Make();
    Fresh(img, w);
    q15t::InstallEquipRefireAt(img.base, A, HostFns());
    for (int s = 0; s < 3; ++s) Word(img, s, 1)(w.comp(), Wide(kLocalHandle), 0xA0 + s);
    g_now += 100000;
    q15t::EquipRefireTick(0);
    CHECK(Handlers() == 0, "an unchanged value was re-fired");
    CHECK(g_userid_calls == 0, "an unchanged value asked the net game (%d calls)", g_userid_calls);
    const auto st = q15t::GetEquipRefireStats();
    CHECK(st.unchanged == 3 && st.tracked == 0, "stats unchanged=%u tracked=%u", st.unchanged, st.tracked);
    // an unreadable record (a handle of a generation that is gone) is not a change either
    Word(img, 0, 0)(w.comp(), Wide((1u << 16) | 0), 0x77);
    g_now += 100000;
    q15t::EquipRefireTick(0);
    CHECK(Handlers() == 0, "a handle whose generation is gone was re-fired");
    CHECK(Count("read=no", 0) == 1, "the unreadable record is not logged as read=no");
    const auto st2 = q15t::GetEquipRefireStats();
    CHECK(st2.tracked == 0 && st2.not_local == 0 && st2.unchanged == 4, "unreadable record: tracked=%u not_local=%u unchanged=%u", st2.tracked,
          st2.not_local, st2.unchanged);
    Free(&w);
  }

  // (d) both word kinds of one setter feed the same slot, and each calls ITS stored original with all three original arguments
  {
    pbhud::Image img;
    World w = Make();
    Fresh(img, w);
    q15t::InstallEquipRefireAt(img.base, A, HostFns());
    for (int s = 0; s < 3; ++s) {
      g_now = 20000;
      Word(img, s, 0)(w.comp(), Wide(kLocalHandle), 0x100 + s);
      Word(img, s, 1)(w.comp(), Wide(kLocalHandle), 0x200 + s);
      CHECK(g_setter_calls[s].size() == 2, "slot %d: %zu original calls, want 2", s, g_setter_calls[s].size());
      for (size_t i = 0; i < g_setter_calls[s].size() && i < 2; ++i) {
        CHECK(g_setter_calls[s][i].cs == w.cs() && g_setter_calls[s][i].handle == Wide(kLocalHandle) &&
                  g_setter_calls[s][i].value == (i == 0 ? 0x100u : 0x200u) + s,
              "slot %d call %zu: cs/handle/value 0x%llx/0x%llx/0x%llx", s, i, (unsigned long long)g_setter_calls[s][i].cs,
              (unsigned long long)g_setter_calls[s][i].handle, (unsigned long long)g_setter_calls[s][i].value);
      }
    }
    g_now = 20000 + rules::kSettleMs;
    q15t::EquipRefireTick(0);
    for (int s = 0; s < 3; ++s)
      CHECK(g_handler_calls[s].size() == 1, "slot %d: %zu handler calls from the two word kinds, want 1", s, g_handler_calls[s].size());
    CHECK(Find("slot=tac_mod value=0x0000000000000201").empty() == false, "tac mod: the later word's value was not the one re-fired: %s",
          Find("slot=tac_mod").c_str());
    CHECK(Find("slot=weapon value=0x0000000000000200 handle=0x00050000 changes=2").empty() == false, "weapon line: %s", Find("slot=weapon").c_str());
    Free(&w);
  }

  // (e) a word that does not hold the stock address is refused alone; the others install and work; a refused word clears only its own original
  {
    pbhud::Image img;
    World w = Make();
    Fresh(img, w);
    img.SetSlot(A.glob_dat[0], img.base + 0x1234);  // weapon GLOB_DAT: not the stock setter
    img.SetSlot(A.jump_slot[1], img.base + 0x1234);  // tac mod JUMP_SLOT: not the stock setter
    const q15t::InstallResult r = q15t::InstallEquipRefireAt(img.base, A, HostFns());
    CHECK(r.status == q15t::SwapStatus::kOk, "four words installed: status %s", q15t::StatusName(r.status));
    CHECK(img.Get(A.glob_dat[0]) == img.base + 0x1234 && img.Get(A.jump_slot[1]) == img.base + 0x1234, "a refused word was written");
    CHECK(Swapped(img, 0, 0) && Swapped(img, 1, 1) && Swapped(img, 2, 0) && Swapped(img, 2, 1), "an acceptable word was not swapped");
    CHECK(Count("equiprefire: slot refused, its calls stay stock", 2) == 2, "want 2 WARNING refusals, got %d",
          Count("equiprefire: slot refused, its calls stay stock", 2));
    CHECK(Count("equiprefire: slot swapped", 1) == 4 && Count("slots=4 of=6") == 1, "4 of 6 words: %s", Find("equiprefire: on").c_str());
    // the weapon JUMP_SLOT wrapper (installed before the refused GLOB_DAT of the same setter) still calls its original
    Word(img, 0, 0)(w.comp(), Wide(kLocalHandle), 0x41);
    CHECK(g_setter_calls[0].size() == 1 && g_setter_calls[0][0].value == 0x41, "the surviving wrapper lost its original");
    // the tac mod GLOB_DAT wrapper (installed after its refused JUMP_SLOT)
    Word(img, 1, 1)(w.comp(), Wide(kLocalHandle), 0x42);
    CHECK(g_setter_calls[1].size() == 1 && g_setter_calls[1][0].value == 0x42, "the tac mod wrapper lost its original");
    // every word refused: OFF, reason from the first refusal, no tick work
    pbhud::Image img2;
    World w2 = Make();
    Fresh(img2, w2);
    for (int s = 0; s < 3; ++s) {
      img2.SetSlot(A.jump_slot[s], img2.base + 0x1234);
      img2.SetSlot(A.glob_dat[s], img2.base + 0x1234);
    }
    const q15t::InstallResult r2 = q15t::InstallEquipRefireAt(img2.base, A, HostFns());
    CHECK(r2.status == q15t::SwapStatus::kUnexpectedOriginal, "all refused: %s", q15t::StatusName(r2.status));
    CHECK(Count("equiprefire: OFF; no slot installed", 3) == 1, "no OFF ERROR");
    CHECK(!q15t::GetEquipRefireStats().on, "on after no word installed");
    Free(&w);
    Free(&w2);
  }

  // (f) the pins: each word of the table, a changed word refuses the whole module, nothing is swapped, the kill file, no base
  {
    auto count = [](const char* needle, int level) { return Count(needle, level); };
    auto clear = []() { g_lines.clear(); };
    g_fail += pincheck::Run("equiprefire", q15t::kEquipRefireWordPins, q15t::kEquipRefireWordPinCount, 24, 0x067e042ef33837e7ull,
                            [](std::uintptr_t b) { return q15t::EquipRefireWordPinsMatch(b); }, count, clear);
    // the addresses of the pinned build, as ELF vaddrs (ReVault minus 0x100000), written out here again so a changed table is a failure
    const u64 want_setter[3] = {0x217ca84, 0x217cde4, 0x217cf90}, want_handler[3] = {0x217afb4, 0x217b124, 0x217b1dc};
    const u64 want_jump[3] = {0x36f3d00, 0x36fa900, 0x36d8e10}, want_glob[3] = {0x37233e8, 0x3727b38, 0x3711db8};
    for (int s = 0; s < 3; ++s)
      CHECK(A.setter[s] == want_setter[s] && A.handler[s] == want_handler[s] && A.jump_slot[s] == want_jump[s] && A.glob_dat[s] == want_glob[s],
            "slot %d: pinned address table changed", s);
    CHECK(A.net_game == 0x12027d4 && A.user_id == 0x125a9b8, "net game / UserId addresses changed");
    const q15t::EquipRefireFns at = q15t::EquipRefireFnsAt(0x100000000ull, A);
    CHECK(at.handler[0] == 0x100000000ull + 0x217afb4 && at.handler[1] == 0x100000000ull + 0x217b124 && at.handler[2] == 0x100000000ull + 0x217b1dc &&
              at.net_game == 0x100000000ull + 0x12027d4 && at.user_id == 0x100000000ull + 0x125a9b8,
          "EquipRefireFnsAt is not base + the pinned vaddrs");
    // the installer on an image that carries the pins and the six stock words
    pbhud::Image img;
    World w = Make();
    Fresh(img, w);
    for (const auto& p : q15t::kEquipRefireWordPins) {
      std::uint32_t* word = &img.Word32(p.va);
      // the setters' and handlers' trampolines already occupy their first 12 bytes; the pins at an entry word are written over them
      *word = p.want;
    }
    // restore the setters the pins overwrote at their entries (the test calls none of them here)
    img.Word32(A.setter[0]) ^= 1u;  // a changed first word of SetWeaponSlot
    g_lines.clear();
    const q15t::InstallResult bad = q15t::InstallEquipRefire(img.base);
    CHECK(bad.status == q15t::SwapStatus::kUnexpectedOriginal && std::strcmp(bad.reason, "word_pin_mismatch") == 0, "pin mismatch: %s/%s",
          q15t::StatusName(bad.status), bad.reason);
    CHECK(Count("code word pin mismatch", 3) == 1, "no ERROR pin line");
    for (int s = 0; s < 3; ++s)
      for (int k = 0; k < 2; ++k) CHECK(!Swapped(img, s, k), "pin mismatch but slot %d word %d was swapped", s, k);
    CHECK(Count("equiprefire: slot swapped") == 0 && !q15t::GetEquipRefireStats().on, "a slot was swapped after a pin mismatch");
    img.Word32(A.setter[0]) ^= 1u;
    // the same image with every pin right: installs all six words
    g_lines.clear();
    const q15t::InstallResult good = q15t::InstallEquipRefire(img.base);
    CHECK(good.status == q15t::SwapStatus::kOk, "pinned image: %s/%s", q15t::StatusName(good.status), good.reason);
    for (int s = 0; s < 3; ++s)
      for (int k = 0; k < 2; ++k) CHECK(Swapped(img, s, k), "pinned image: slot %d word %d not swapped", s, k);
    CHECK(Count("equiprefire: on", 1) == 1, "no summary line");
    // the kill file: nothing changed, one INFO line, reason off_file (plugin.cpp reports it as a choice, not a failure)
    pbhud::Image img3;
    World w3 = Make();
    Fresh(img3, w3);
    for (const auto& p : q15t::kEquipRefireWordPins) img3.Word32(p.va) = p.want;
    char dir[] = "/tmp/q15t_equiprefire_XXXXXX";
    if (mkdtemp(dir) == nullptr) std::abort();
    const std::string off = std::string(dir) + "/equiprefire.off";
    if (FILE* f = std::fopen(off.c_str(), "w")) std::fclose(f);
    q15t::SetModuleSwitchDirForTest(dir);
    g_lines.clear();
    const q15t::InstallResult offr = q15t::InstallEquipRefire(img3.base);
    CHECK(std::strcmp(offr.reason, "off_file") == 0, "kill file reason %s", offr.reason);
    for (int s = 0; s < 3; ++s)
      for (int k = 0; k < 2; ++k) CHECK(!Swapped(img3, s, k), "kill file but slot %d word %d was swapped", s, k);
    CHECK(Count("equiprefire off", 1) == 1, "no INFO line for the kill file");
    std::remove(off.c_str());
    rmdir(dir);
    q15t::SetModuleSwitchDirForTest("/nonexistent-equiprefire-test-dir");
    // no base
    g_lines.clear();
    const q15t::InstallResult nb = q15t::InstallEquipRefire(0);
    CHECK(std::strcmp(nb.reason, "no_base") == 0 && Count("no game base", 3) == 1, "no_base");
    Free(&w);
    Free(&w3);
  }

  // (g) a stale component or record is skipped with the logged reason, and the handler is not called
  {
    struct Case {
      const char* reason;
      int kind;
    };
    const Case cases[] = {{"component_replaced", 0}, {"record_gone", 1}, {"record_changed_elsewhere", 2}, {"not_local", 3}, {"component_unreadable", 4}};
    for (const Case& c : cases) {
      pbhud::Image img;
      World w = Make();
      Fresh(img, w);
      q15t::InstallEquipRefireAt(img.base, A, HostFns());
      g_now = 30000;
      Word(img, 1, 0)(w.comp(), Wide(kLocalHandle), 0x99);
      switch (c.kind) {
        case 0: w.Put<std::uintptr_t>(kCompOff, kVtable + 0x100); break;                // the vtable is not the one tracked
        case 1: w.Put<std::uint16_t>(kIndexOff + 2, 6); break;                         // the generation moved on
        case 2: w.Value(0, 1) = 0x98; break;                                           // the choice changed without the setter
        case 3: w.Put<u64>(kLinkOff, kRemoteLink); break;                              // the record is another player's now
        case 4: munmap(w.comp(), kPage); break;                                        // the component is gone
      }
      g_now = 30000 + rules::kSettleMs;
      q15t::EquipRefireTick(0);
      CHECK(Handlers() == 0, "%s: the handler ran on a stale record", c.reason);
      const std::string l = Find("equiprefire: equipment re-fire skipped");
      CHECK(l.find(std::string("reason=") + c.reason) != std::string::npos && l.find("slot=tac_mod") != std::string::npos, "%s: skip line: %s", c.reason,
            l.c_str());
      CHECK(Count("equiprefire: equipment re-fire skipped", 1) == 1, "%s: the skip line is not one INFO line", c.reason);
      const auto st = q15t::GetEquipRefireStats();
      CHECK(st.skipped == 1 && st.refired == 0, "%s: skipped=%u refired=%u", c.reason, st.skipped, st.refired);
      if (c.kind == 4) {
        // map the page back so Free() unmaps a whole arena
        mmap(w.comp(), kPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
      }
      Free(&w);
    }
  }

  // (h) the three slots call their own handler, with their own record value; one tick drains all three
  {
    pbhud::Image img;
    World w = Make();
    Fresh(img, w);
    q15t::InstallEquipRefireAt(img.base, A, HostFns());
    g_now = 40000;
    for (int s = 0; s < 3; ++s) Word(img, s, s % 2)(w.comp(), Wide(kLocalHandle), 0x300 + s);
    g_now = 40000 + rules::kSettleMs;
    q15t::EquipRefireTick(0);
    for (int s = 0; s < 3; ++s) {
      CHECK(g_handler_calls[s].size() == 1, "slot %d: %zu own-handler calls, want 1", s, g_handler_calls[s].size());
    }
    CHECK(Find("slot=weapon value=0x0000000000000300").empty() == false && Find("slot=tac_mod value=0x0000000000000301").empty() == false &&
              Find("slot=ordnance value=0x0000000000000302").empty() == false,
          "slot names / values in the re-fire lines");
    // a later change of one slot re-arms only that slot
    g_now = 50000;
    Word(img, 2, 0)(w.comp(), Wide(kLocalHandle), 0x400);
    g_now = 50000 + rules::kSettleMs;
    q15t::EquipRefireTick(0);
    CHECK(g_handler_calls[0].size() == 1 && g_handler_calls[1].size() == 1 && g_handler_calls[2].size() == 2, "re-arm: %zu/%zu/%zu", g_handler_calls[0].size(),
          g_handler_calls[1].size(), g_handler_calls[2].size());
    Free(&w);
  }

  // (i) an argument the game would read wildly: the net game getter is not called, the change is not local. Then the exact probe: a
  //     readable window of 8 bytes at the getter's displacement (0x8510 in the pinned NetGameMT) lets the getter run, one byte either way does not.
  {
    pbhud::Image img;
    World w = Make();
    Fresh(img, w);
    q15t::InstallEquipRefireAt(img.base, A, HostFns());
    w.Put<std::uintptr_t>(kCtxOff + rules::kCtxNetGameArgOffset, 0x20000);  // unmapped
    Word(img, 0, 0)(w.comp(), Wide(kLocalHandle), 0x66);
    CHECK(g_netgame_calls == 0 && g_userid_calls == 0, "the game's getters ran with an unreadable argument (%d/%d)", g_netgame_calls, g_userid_calls);
    g_now += 100000;
    q15t::EquipRefireTick(0);
    CHECK(Handlers() == 0 && q15t::GetEquipRefireStats().not_local == 1, "unreadable argument: handlers %zu", Handlers());
    void* win = mmap(nullptr, 3 * kPage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (win == MAP_FAILED) std::abort();
    auto* wb = static_cast<std::uint8_t*>(win);
    mprotect(wb, kPage, PROT_NONE);
    mprotect(wb + 2 * kPage, kPage, PROT_NONE);
    // the window ends exactly at the field, then starts exactly at it
    const std::uintptr_t starts[2] = {reinterpret_cast<std::uintptr_t>(wb) + 2 * kPage - 8, reinterpret_cast<std::uintptr_t>(wb) + kPage};
    for (int k = 0; k < 2; ++k) {
      Word(img, 0, 0)(w.comp(), Wide(kLocalHandle), 0x70 + k);  // a change
      g_netgame_calls = 0;
      w.Put<std::uintptr_t>(kCtxOff + rules::kCtxNetGameArgOffset, starts[k] - 0x8510);
      Word(img, 0, 0)(w.comp(), Wide(kLocalHandle), 0x80 + k);
      CHECK(g_netgame_calls == 1, "window %d: the getter ran %d times with its field readable, want 1", k, g_netgame_calls);
      for (int shift : {-1, 1}) {
        // a window one byte off the field's own: the argument moves, the field's 8 bytes now cross the window's edge
        if ((k == 0 && shift == -1) || (k == 1 && shift == 1)) continue;  // that edge is still inside the window
        g_netgame_calls = 0;
        w.Put<std::uintptr_t>(kCtxOff + rules::kCtxNetGameArgOffset, starts[k] - 0x8510 + shift);
        Word(img, 0, 0)(w.comp(), Wide(kLocalHandle), 0x90 + k);
        CHECK(g_netgame_calls == 0, "window %d shift %d: the getter ran with a field that is not readable", k, shift);
      }
    }
    munmap(win, 3 * kPage);
    Free(&w);
  }

  // (k) the per-call Debug record is capped, the cap is one WARNING, the counters go on
  {
    pbhud::Image img;
    World w = Make();
    Fresh(img, w);
    q15t::InstallEquipRefireAt(img.base, A, HostFns());
    for (int i = 0; i < 300; ++i) Word(img, 2, 0)(w.comp(), Wide(kRemoteHandle), 0x1000 + i);
    CHECK(Count("equiprefire: equipment change", 0) == 256, "want 256 Debug records, got %d", Count("equiprefire: equipment change", 0));
    CHECK(Count("per-call records capped", 2) == 1 && Find("per-call records capped").find("cap=256") != std::string::npos, "the cap WARNING");
    CHECK(q15t::GetEquipRefireStats().not_local == 300, "the counter stopped at the cap");
    Free(&w);
  }

  // (j) a tick before any install, and after a failed one, does nothing
  {
    q15t::ResetEquipRefireForTest();
    q15t::EquipRefireTick(0);
    CHECK(Handlers() == 0, "tick before install");
  }

  if (g_fail == 0) std::printf("equiprefire_test: all checks pass\n");
  return g_fail == 0 ? 0 : 1;
}
