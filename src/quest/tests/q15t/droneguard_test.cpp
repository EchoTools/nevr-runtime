// Host test of droneguard (src/quest/plugins/q15t/src/droneguard.cpp): the wrapper installed by the real installer on a fake JUMP_SLOT page and called
// through the slot, against a fake CR15NetFollowPlayerCS (target at +0x140, the player handle's cache at +0x148 / +0x16c / +0x188 /
// +0x190) and fake RefreshCache / IsComponentValid. Real SafeRead, real mprotect.
#include <sys/mman.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "droneguard.h"
#include "nevr/plugin_interface.h"
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

alignas(16) static uint8_t g_cs[0x200];
static void Put64(size_t off, uint64_t v) { std::memcpy(g_cs + off, &v, 8); }
static void Put32(size_t off, uint32_t v) { std::memcpy(g_cs + off, &v, 4); }

static const uintptr_t kSys = 0x5000, kTarget = 0x6000;
static int g_real_calls = 0;
static void* g_real_cs = nullptr;
static uint64_t g_real_h = 0;
extern "C" __attribute__((noinline)) void RealFollow(void* cs, uint64_t h) {
  ++g_real_calls;
  g_real_cs = cs;
  g_real_h = h;
}
extern "C" __attribute__((noinline)) void OtherFollow(void*, uint64_t) {}

// RefreshCache fake: records the handle it got and, like the engine, rewrites the cache (sys / idx) from the test's wish
static int g_refresh_calls = 0;
static void* g_refresh_arg = nullptr;
static uintptr_t g_after_sys = kSys;
static uint32_t g_after_idx = 0x00070003;
static int g_valid_calls = 0;
static const void* g_valid_sys = nullptr;
static uint64_t g_valid_idx = 0;
static bool g_valid = true;
static void* g_poison = nullptr;  // when set, the refresh makes this page unreadable instead of rewriting the cache
static void FRefresh(void* handle) {
  ++g_refresh_calls;
  g_refresh_arg = handle;
  if (g_poison != nullptr) {
    mprotect(g_poison, 4096, PROT_NONE);
    return;
  }
  Put64(0x188, g_after_sys);
  Put32(0x190, g_after_idx);
}
static bool FValid(const void* sys, uint64_t idx) {
  ++g_valid_calls;
  g_valid_sys = sys;
  g_valid_idx = idx;
  return g_valid;
}
static q15t::DroneGuardFns Fns() {
  q15t::DroneGuardFns f{};
  f.refresh = &FRefresh;
  f.is_valid = &FValid;
  return f;
}

using Fn = void (*)(void*, uint64_t);
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
static void World(uintptr_t target) {
  std::memset(g_cs, 0, sizeof g_cs);
  Put64(0x140, target);
  Put64(0x188, kSys);
  Put32(0x190, 0x00070003);
  g_cs[0x16c] = 0;
  g_real_calls = g_refresh_calls = g_valid_calls = 0;
  g_after_sys = kSys;
  g_after_idx = 0x00070003;
  g_valid = true;
}
static Fn Install(bool idx) {
  q15t::ResetDroneGuardForTest();
  Slot s = MakePage(reinterpret_cast<uintptr_t>(&RealFollow));
  g_lines.clear();
  const q15t::DroneGuardFns f = Fns();
  const auto r = q15t::InstallDroneGuardAt(s.base, 0x20, reinterpret_cast<uintptr_t>(&RealFollow) - s.base, idx, &f);
  CHECK(r.status == q15t::SwapStatus::kOk, "install: %s", q15t::StatusName(r.status));
  return reinterpret_cast<Fn>(s.w[4]);
}

int main() {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);

  // the pinned addresses (R0_readelf_r.txt line 131799; disassembly 0x2249764 - 0x100000; RefreshCache / IsComponentValid from the source)
  CHECK(q15t::kDroneGuardSlot == 0x36f1218 && q15t::kDroneGuardReal == 0x2149764 && q15t::kDroneRefreshCache == 0x1210848 &&
            q15t::kDroneIsValid == 0x1cdb3e0,
        "pinned addresses changed");

  // ---- stage 0 ----
  Fn hook = Install(false);
  CHECK(CountLines("droneguard on (TEST ONLY)", 1) == 1 && CountLines("idx guard on") == 0, "install line");

  // 1. target set: the real function with the same arguments, nothing skipped, the idx stage never touched
  World(kTarget);
  hook(g_cs, 0xabcd1234ull);
  CHECK(g_real_calls == 1 && g_real_cs == g_cs && g_real_h == 0xabcd1234ull, "target set: real %d", g_real_calls);
  CHECK(g_refresh_calls == 0 && g_valid_calls == 0, "stage 1 ran with the idx stage off");
  CHECK(q15t::GetDroneGuardStats().skips == 0 && CountLines("first skip") == 0, "a stock call skipped");

  // 2. target null: Follow is not called; one 'first skip' line; a total at every 600th skip, at most 8
  g_lines.clear();
  World(0);
  hook(g_cs, 1);
  CHECK(g_real_calls == 0 && q15t::GetDroneGuardStats().skips == 1, "null target: real %d skips %u", g_real_calls, q15t::GetDroneGuardStats().skips);
  CHECK(CountLines("droneguard first skip: follow target null", 1) == 1, "no first-skip line");
  for (int i = 0; i < 598; ++i) hook(g_cs, 1);
  CHECK(CountLines("skips so far") == 0 && CountLines("first skip") == 1, "early total or repeated first-skip line");
  hook(g_cs, 1);
  CHECK(CountLines("skips so far") == 1 && CountLines("skips=600") == 1, "no total at the 600th skip");
  for (int i = 0; i < 600 * 9; ++i) hook(g_cs, 1);
  CHECK(CountLines("skips so far") == 8, "totals %d (cap 8)", CountLines("skips so far"));
  CHECK(g_real_calls == 0, "Follow called with a null target");

  // 3. the target arrives: one 'target set' line with the skip count, Follow runs
  g_lines.clear();
  const unsigned skips = q15t::GetDroneGuardStats().skips;
  Put64(0x140, kTarget);
  hook(g_cs, 5);
  hook(g_cs, 6);
  CHECK(g_real_calls == 2 && g_real_h == 6, "Follow not called once the target is set");
  CHECK(CountLines("droneguard target set after skips", 1) == 1 && CountLines(("skips=" + std::to_string(skips)).c_str()) == 1,
        "target-set record (skips %u)", skips);
  // and a later null again starts a new first-skip line
  Put64(0x140, 0);
  hook(g_cs, 7);
  CHECK(CountLines("droneguard first skip") == 1, "no new first-skip line after the target went away");

  // 4. state lines: first call and every change of (idx, flags, target null, sys null), at most 16; read-only
  hook = Install(false);
  World(kTarget);
  hook(g_cs, 1);
  hook(g_cs, 1);
  CHECK(CountLines("droneguard state") == 1, "state lines %d after two equal calls", CountLines("droneguard state"));
  CHECK(CountLines("target=set handle_sys=set idx_unresolved=no raw190=00070003 flags16c=00") == 1, "state record");
  Put32(0x190, 0x0007ffff);
  hook(g_cs, 1);
  CHECK(CountLines("droneguard state") == 2 && CountLines("idx_unresolved=yes raw190=0007ffff") == 1, "idx change not reported");
  g_cs[0x16c] = 2;
  hook(g_cs, 1);
  CHECK(CountLines("flags16c=02") == 1, "flags change not reported");
  Put64(0x188, 0);
  hook(g_cs, 1);
  CHECK(CountLines("handle_sys=0") == 1, "handle-sys change not reported");
  Put64(0x140, 0);
  hook(g_cs, 1);
  CHECK(CountLines("target=0 handle_sys=0") == 1, "target change not reported");
  for (uint32_t i = 0; i < 40; ++i) {
    Put32(0x190, 0x00100000 + i);
    hook(g_cs, 1);
  }
  CHECK(CountLines("droneguard state") == 16, "state lines %d (cap 16)", CountLines("droneguard state"));

  // 5. doubt: an unreadable system goes to Follow (and the wrapper reads nothing else)
  hook = Install(true);
  World(kTarget);
  g_real_calls = 0;
  hook(reinterpret_cast<void*>(0x20), 9);
  CHECK(g_real_calls == 1 && g_refresh_calls == 0, "unreadable cs: real %d refresh %d", g_real_calls, g_refresh_calls);
  // a system that ends before +0x194 is unreadable as a whole
  {
    auto* two = static_cast<uint8_t*>(mmap(nullptr, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    mprotect(two + 4096, 4096, PROT_NONE);
    g_real_calls = 0;
    hook(two + 4096 - 0x150, 9);
    CHECK(g_real_calls == 1, "a partly unreadable system is not skipped");
    munmap(two, 8192);
  }

  // ---- stage 1 ----
  // 6. a healthy player: RefreshCache on cs+0x148 first, IsComponentValid(target, idx), then Follow
  hook = Install(true);
  CHECK(CountLines("idx guard on (TEST ONLY)", 1) == 1, "no idx install line");
  World(kTarget);
  hook(g_cs, 0x77);
  CHECK(g_refresh_calls == 1 && g_refresh_arg == g_cs + 0x148, "refresh arg %p (cs+0x148 = %p)", g_refresh_arg, (void*)(g_cs + 0x148));
  CHECK(g_valid_calls == 1 && g_valid_sys == reinterpret_cast<const void*>(kTarget) && g_valid_idx == 0x00070003, "IsComponentValid args %p %llx",
        g_valid_sys, (unsigned long long)g_valid_idx);
  CHECK(g_real_calls == 1 && g_real_h == 0x77 && q15t::GetDroneGuardStats().idx_skips == 0, "healthy player not followed");

  // 7. the null half still comes first: no refresh when the target is null
  World(0);
  hook(g_cs, 1);
  CHECK(g_refresh_calls == 0 && g_real_calls == 0, "refresh called for a null target");

  // 8. the index word after the refresh decides (the cache the refresh just wrote, not the one before)
  World(kTarget);
  g_after_idx = 0x0007ffff;  // refresh leaves the index unresolved
  hook(g_cs, 1);
  CHECK(g_real_calls == 0 && g_valid_calls == 0 && q15t::GetDroneGuardStats().idx_skips == 1, "idx ffff not skipped (real %d, valid %d)", g_real_calls, g_valid_calls);
  CHECK(CountLines("droneguard first idx skip", 1) == 1 && CountLines("why=idx ffff") == 1, "no first idx-skip line");
  World(kTarget);
  g_after_idx = 0xffff;  // only the low 16 bits count: generation 0 and index ffff
  hook(g_cs, 1);
  CHECK(g_real_calls == 0, "idx 0000ffff not skipped");
  World(kTarget);
  g_after_idx = 0x0001fffe;  // not unresolved
  hook(g_cs, 1);
  CHECK(g_real_calls == 1, "idx fffe skipped");
  World(kTarget);
  g_after_sys = 0;  // the cached system is 0
  hook(g_cs, 1);
  CHECK(g_real_calls == 0 && g_valid_calls == 0 && CountLines("why=handle-sys 0") == 1, "handle-sys 0 not skipped");

  // 9. IsComponentValid refuses the cached index: skipped; resolved again: one 'resolved' line
  World(kTarget);
  hook(g_cs, 1);  // a healthy call ends the previous idx-skip run, so the next skip logs its reason
  World(kTarget);
  g_valid = false;
  hook(g_cs, 1);
  CHECK(g_real_calls == 0 && g_valid_calls == 1 && CountLines("why=refused but cached") == 1, "refused index not skipped");
  g_lines.clear();
  World(kTarget);
  hook(g_cs, 1);
  CHECK(g_real_calls == 1 && CountLines("droneguard player resolved after idx skips", 1) == 1, "no 'resolved' line");
  hook(g_cs, 1);
  CHECK(CountLines("droneguard player resolved") == 1, "'resolved' line repeated");

  // 9b. the cache becomes unreadable right after the refresh: doubt, Follow runs
  {
    auto* pg = static_cast<uint8_t*>(mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    hook = Install(true);
    World(kTarget);
    std::memcpy(pg + 0x140, g_cs + 0x140, 0x60);
    g_poison = pg;
    hook(pg, 1);
    g_poison = nullptr;
    CHECK(g_real_calls == 1 && g_refresh_calls == 1 && q15t::GetDroneGuardStats().idx_skips == 0, "unreadable after the refresh: real %d", g_real_calls);
    munmap(pg, 4096);
  }

  // 10. idx skip totals: every 600th, at most 8
  hook = Install(true);
  World(kTarget);
  g_after_idx = 0xffff;
  for (int i = 0; i < 600; ++i) hook(g_cs, 1);
  CHECK(CountLines("idx skips so far") == 1 && CountLines("idx_skips=600") == 1, "no idx total at the 600th skip");
  for (int i = 0; i < 600 * 9; ++i) hook(g_cs, 1);
  CHECK(CountLines("idx skips so far") == 8, "idx totals %d (cap 8)", CountLines("idx skips so far"));

  // 11. stage 1 requested but the engine functions are missing: one ERROR line, the null half only
  q15t::ResetDroneGuardForTest();
  {
    Slot s = MakePage(reinterpret_cast<uintptr_t>(&RealFollow));
    g_lines.clear();
    const auto r = q15t::InstallDroneGuardAt(s.base, 0x20, reinterpret_cast<uintptr_t>(&RealFollow) - s.base, true, nullptr);
    CHECK(r.status == q15t::SwapStatus::kOk, "null half not installed without stage 1's functions");
    CHECK(CountLines("droneguard idx guard off", 3) == 1, "missing stage-1 export not loud");
    World(kTarget);
    reinterpret_cast<Fn>(s.w[4])(g_cs, 1);
    CHECK(g_real_calls == 1 && g_refresh_calls == 0, "stage 1 ran without its functions");
    q15t::DroneGuardFns half{};
    half.refresh = &FRefresh;
    q15t::ResetDroneGuardForTest();
    Slot s2 = MakePage(reinterpret_cast<uintptr_t>(&RealFollow));
    q15t::InstallDroneGuardAt(s2.base, 0x20, reinterpret_cast<uintptr_t>(&RealFollow) - s2.base, true, &half);
    World(kTarget);
    reinterpret_cast<Fn>(s2.w[4])(g_cs, 1);
    CHECK(g_real_calls == 1 && g_refresh_calls == 0, "stage 1 ran with one function missing");
    // the resolving installer on a host without the game library: null half only (Q15T_DRONEGUARD_IDX is 0 by default)
    CHECK(!q15t::ResolveDroneGuardFns(s2.base, &half), "resolved without a game library");
    // the address check on what dlsym would return
    q15t::DroneGuardFns got{};
    const uintptr_t b = 0x7000000000ull;
    CHECK(q15t::DroneGuardExportsMatch(b, reinterpret_cast<void*>(b + q15t::kDroneRefreshCache), reinterpret_cast<void*>(b + q15t::kDroneIsValid), &got) &&
              got.refresh == reinterpret_cast<void (*)(void*)>(b + q15t::kDroneRefreshCache) &&
              got.is_valid == reinterpret_cast<bool (*)(const void*, uint64_t)>(b + q15t::kDroneIsValid),
          "exports at the pinned addresses refused");
    CHECK(!q15t::DroneGuardExportsMatch(b, reinterpret_cast<void*>(b + 0x1210840), reinterpret_cast<void*>(b + q15t::kDroneIsValid), &got), "wrong RefreshCache accepted");
    CHECK(!q15t::DroneGuardExportsMatch(b, reinterpret_cast<void*>(b + q15t::kDroneRefreshCache), reinterpret_cast<void*>(b + 0x1cdb3e8), &got), "wrong IsComponentValid accepted");
    CHECK(!q15t::DroneGuardExportsMatch(b, nullptr, nullptr, &got), "null exports accepted");
  }

  // 12. a slot that does not hold the pinned function is refused and left alone, with an ERROR line
  q15t::ResetDroneGuardForTest();
  g_lines.clear();
  Slot bad = MakePage(0x4242420);
  const auto rb = q15t::InstallDroneGuardAt(bad.base, 0x20, reinterpret_cast<uintptr_t>(&RealFollow) - bad.base, true, nullptr);
  CHECK(rb.status == q15t::SwapStatus::kUnexpectedOriginal && bad.w[4] == 0x4242420, "foreign word: %s", q15t::StatusName(rb.status));
  {
    char named[128];
    std::snprintf(named, sizeof named, "pin=0x20 found=0x4242420 expected=0x%llx", static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(&RealFollow)));
    CHECK(CountLines("droneguard refused", 3) == 1 && CountLines(named) == 1, "refusal not loud / does not name pin, found, expected");
  }
  Slot other = MakePage(reinterpret_cast<uintptr_t>(&OtherFollow));
  const auto rc = q15t::InstallDroneGuardAt(other.base, 0x20, reinterpret_cast<uintptr_t>(&RealFollow) - other.base, false, nullptr);
  CHECK(rc.status == q15t::SwapStatus::kUnexpectedOriginal && other.w[4] == reinterpret_cast<uintptr_t>(&OtherFollow),
        "other function's word accepted: %s", q15t::StatusName(rc.status));

  // ---- code-word pins: every word read, a changed or unreadable one refused loudly, no file hash anywhere ----
  {
    auto count = [](const char* needle, int level) { return CountLines(needle, level); };
    auto clear = []() { g_lines.clear(); };
    g_fail += pincheck::Run("droneguard", q15t::kDroneGuardWordPins, q15t::kDroneGuardWordPinCount, 4, 0x5dbca934e8f10880ull,
                            [](std::uintptr_t b) { return q15t::DroneGuardWordPinsMatch(b); }, count, clear);
    g_fail += pincheck::Run("droneguard idx", q15t::kDroneGuardIdxWordPins, q15t::kDroneGuardIdxWordPinCount, 3, 0x940bca689c7de833ull,
                            [](std::uintptr_t b) { return q15t::DroneGuardIdxWordPinsMatch(b); }, count, clear);
    // stage 1's resolver checks its own pins first: a changed word is one ERROR line, good words are quiet (dlsym then fails on a host)
    {
      pincheck::Text it = pincheck::Make(q15t::kDroneGuardIdxWordPins, q15t::kDroneGuardIdxWordPinCount);
      q15t::DroneGuardFns rf{};
      g_lines.clear();
      CHECK(!q15t::ResolveDroneGuardFns(it.base(), &rf) && CountLines("code word pin mismatch") == 0, "good idx pins logged a mismatch");
      *it.word(q15t::kDroneGuardIdxWordPins[1].va) ^= 1u;
      g_lines.clear();
      CHECK(!q15t::ResolveDroneGuardFns(it.base(), &rf) && CountLines("droneguard idx guard refused: code word pin mismatch", 3) == 1 &&
                CountLines("null half only") == 1,
            "idx resolver ignored a changed pin");
      pincheck::Free(&it);
    }
    // the real installer refuses on a changed word, before anything else (reason word_pin_mismatch, one ERROR line, nothing swapped)
    pincheck::Text t = pincheck::Make(q15t::kDroneGuardWordPins, q15t::kDroneGuardWordPinCount);
    *t.word(q15t::kDroneGuardWordPins[0].va) ^= 1u;
    g_lines.clear();
    const auto pr = q15t::InstallDroneGuard(t.base());
    CHECK(std::strcmp(pr.reason, "word_pin_mismatch") == 0 && CountLines("code word pin mismatch", 3) == 1, "installer ignored a changed word");
    CHECK(CountLines("droneguard on", -1) == 0, "installer went on after a changed word");
    pincheck::Free(&t);
  }

  if (g_fail == 0) std::printf("droneguard_test: all checks passed\n");
  return g_fail == 0 ? 0 : 1;
}
