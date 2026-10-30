// Host test of the plugin's hook install and wrapper against a fake "game library" slot (an mmap'd page).
// Built per arm (Hz match90, T-26) by src/quest/tests/q15t/run_host_tests.sh; the clock raise is a runtime switch (args {clock: raise}), tested in every build. Real code, real mprotect, real /proc/self/maps.
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "clock_levels.h"
#include "refresh_rate.h"
#include "nevr/plugin_interface.h"

extern "C" {
NEVR_PLUGIN_API int NvrPluginInitEx(const NvrGameContext*, const char*);
NEVR_PLUGIN_API uint32_t NvrPluginGetCapabilities(void);
}

static int g_fail = 0;
#define CHECK(cond, ...)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      ++g_fail;                                           \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);   \
      std::printf(__VA_ARGS__);                           \
      std::printf("\n");                                  \
    }                                                     \
  } while (0)

static int g_real_calls = 0;
static int32_t g_real_cpu = -1, g_real_gpu = -1;
extern "C" __attribute__((visibility("default"), noinline)) int32_t vrapi_SetClockLevels(void*, int32_t cpu, int32_t gpu) {
  ++g_real_calls;
  g_real_cpu = cpu;
  g_real_gpu = gpu;
  return 0;
}

static int g_hz_calls = 0;
static float g_hz_seen = -1.f;
extern "C" __attribute__((visibility("default"), noinline)) int32_t vrapi_SetDisplayRefreshRate(void*, float hz) {
  ++g_hz_calls;
  g_hz_seen = hz;
  return 0;
}

static int g_update_calls = 0;
extern "C" __attribute__((noinline, visibility("default"))) void FakeExclusiveUpdateGame(void*) { ++g_update_calls; }

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
static bool HasLine(const char* needle, int level = -1) {
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || (int)l.level == level)) return true;
  return false;
}

static const char* kId = Q15T_BUILD_ID_FOR_TEST;

static NvrGameContext MakeCtx(uintptr_t base, const char* build_id) {
  NvrGameContext c{};
  c.base_addr = base;
  c.flags = NEVR_HOST_IS_CLIENT | NEVR_HOST_PLATFORM_QUEST;
  c.ctx_size = sizeof(NvrGameContext);
  c.plugin_id = "q15t";
  c.log = &FakeLog;
  c.game_build_id = build_id;
  return c;
}

static uintptr_t* NewSlotPage(int prot_after) {
  void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) std::abort();
  auto* w = static_cast<uintptr_t*>(p);
  w[2] = reinterpret_cast<uintptr_t>(&vrapi_SetClockLevels);  // slot at byte offset 0x10
  w[3] = reinterpret_cast<uintptr_t>(&vrapi_SetDisplayRefreshRate);  // slot at byte offset 0x18
  w[4] = reinterpret_cast<uintptr_t>(&FakeExclusiveUpdateGame);  // slot at byte offset 0x20
  mprotect(p, 4096, prot_after);
  return w;
}

int main() {
  const uintptr_t slot_off = Q15T_SLOT_SETCLOCKLEVELS;
  // 1. install on a read-only (RELRO-like) page, call through the slot.
  {
    g_lines.clear();
    uintptr_t* page = NewSlotPage(PROT_READ);
    NvrGameContext ctx = MakeCtx(reinterpret_cast<uintptr_t>(page), kId);
    const int init_rc = NvrPluginInitEx(&ctx, "");
    CHECK(init_rc == 0, "InitEx returned %d with hooks swapped: the loader would dlclose a live plugin", init_rc);
    const uintptr_t now = page[slot_off / sizeof(uintptr_t)];
    CHECK(now != reinterpret_cast<uintptr_t>(&vrapi_SetClockLevels), "slot not replaced");
    auto fn = reinterpret_cast<int32_t (*)(void*, int32_t, int32_t)>(now);
    g_real_calls = 0;
    const int32_t rc = fn(nullptr, 2, 2);
    CHECK(rc == 0 && g_real_calls == 1, "real not called once, rc=%d calls=%d", rc, g_real_calls);
    CHECK(g_real_cpu == 2 && g_real_gpu == 2, "default args changed the levels: %d/%d", g_real_cpu, g_real_gpu);
    CHECK(HasLine("clock_candidate=0") && HasLine("clock_arg= "), "init line does not show the log-only default");
    CHECK(!q15t::ClockRaise(), "default args left the raise on");
    {
      const uintptr_t hz_slot = page[Q15T_SLOT_SETDISPLAYREFRESHRATE / sizeof(uintptr_t)];
      CHECK(hz_slot != reinterpret_cast<uintptr_t>(&vrapi_SetDisplayRefreshRate), "refresh slot not replaced");
      auto hz = reinterpret_cast<int32_t (*)(void*, float)>(hz_slot);
      g_hz_calls = 0;
      hz(reinterpret_cast<void*>(0x1234), 120.f);
      CHECK(g_hz_calls == 1 && g_hz_seen == 120.f, "outside a match the game's 120 Hz must pass: %.1f", (double)g_hz_seen);
      CHECK(HasLine("vrapi_SetDisplayRefreshRate") && HasLine("asked_hz=120.0"), "no per-call refresh record");
#if Q15T_HZ_MATCH90
      // a fake CR15Game with one gamespace table holding one gamespace
      static uintptr_t game[0x2000 / 8] = {};
      static uintptr_t gs[0x60 / 8 + 1] = {};
      static uintptr_t table[2] = {};
      const uintptr_t g = reinterpret_cast<uintptr_t>(game);
      game[0] = reinterpret_cast<uintptr_t>(page) + Q15T_GAME_VPTR;
      table[0] = 0x5555;
      table[1] = reinterpret_cast<uintptr_t>(gs);
      game[0x1f48 / 8] = reinterpret_cast<uintptr_t>(table);
      game[0x1f78 / 8] = 1;
      gs[0x8 / 8] = g;
      gs[0x58 / 8] = 0x5763ceff4e96bc48ull;  // a lobby level: not a combat map
      // through the game-tick slot, as CncaGame::Render calls it
      const uintptr_t tick_slot = page[Q15T_SLOT_EXCLUSIVEUPDATEGAME / sizeof(uintptr_t)];
      CHECK(tick_slot != reinterpret_cast<uintptr_t>(&FakeExclusiveUpdateGame), "tick slot not replaced");
      auto tick = reinterpret_cast<void (*)(void*)>(tick_slot);
      g_hz_calls = 0;
      tick(game);
      CHECK(g_update_calls >= 1, "the original ExclusiveUpdateGame was not called");
      CHECK(g_hz_calls == 0, "no switch expected outside a combat map, got %d calls", g_hz_calls);
      gs[0x58 / 8] = 0xdf5ca7b7dfa383d4ull;  // a combat map
      q15t::ResetHzScheduleForTest();
      tick(game);
      CHECK(g_hz_calls == 1 && g_hz_seen == 90.f, "entering a combat map must ask 90: calls %d, %.1f", g_hz_calls, (double)g_hz_seen);
      CHECK(HasLine("refresh switch") && HasLine("why=combat map loaded"), "no switch record");
      hz(reinterpret_cast<void*>(0x1234), 120.f);
      CHECK(g_hz_seen == 90.f, "a game request during a match must stay 90: %.1f", (double)g_hz_seen);
      q15t::ResetHzScheduleForTest();
      tick(game);
      CHECK(g_hz_calls == 2, "no change, no re-ask: calls %d", g_hz_calls);
      gs[0x58 / 8] = 0x5763ceff4e96bc48ull;
      q15t::ResetHzScheduleForTest();
      tick(game);
      CHECK(g_hz_calls == 3 && g_hz_seen == 120.f, "leaving the combat map must ask the game's 120 again: %.1f", (double)g_hz_seen);
      // an object that is not the CR15Game: no callback, one ERROR line
      static uintptr_t other[0x2000 / 8] = {};
      other[0] = 0x777;
      q15t::ResetHzScheduleForTest();
      tick(other);
      CHECK(g_hz_calls == 3, "a non-CR15Game object ran the tick");
      CHECK(HasLine("game tick: object is not the CR15Game", 3), "no ERROR line for a wrong vptr");
#else
      CHECK(!HasLine("CncaGame::ExclusiveUpdateGame"), "the witness arm must not install the game tick");
#endif
#if Q15T_T26 && !Q15T_HZ_MATCH90
      // no game library is loaded on the host: the arm refuses loudly and installs no tick
      CHECK(HasLine("t26 not installed", 3), "T-26 arm without engine exports must log an ERROR");
      CHECK(page[Q15T_SLOT_EXCLUSIVEUPDATEGAME / sizeof(uintptr_t)] == reinterpret_cast<uintptr_t>(&FakeExclusiveUpdateGame),
            "tick slot swapped although T-26 could not resolve its exports");
#endif
    }
    CHECK(HasLine("hook install"), "no install record");
    CHECK(HasLine("status=ok", 1), "install not logged ok at INFO");
    CHECK(HasLine("vrapi_SetClockLevels") && HasLine("asked_cpu=2"), "no per-call record");
    q15t::Mapping m;
    CHECK(q15t::FindMapping(reinterpret_cast<uintptr_t>(page), &m) && std::strncmp(m.perms, "r--", 3) == 0,
          "page protection not restored (perms %s)", m.perms);
    const q15t::ClockStats s = q15t::GetClockStats();
    CHECK(s.calls >= 1, "stats empty");
  }
  // 2. wrong build id: nothing installed.
  {
    g_lines.clear();
    uintptr_t* page = NewSlotPage(PROT_READ);
    NvrGameContext ctx = MakeCtx(reinterpret_cast<uintptr_t>(page), "0000000000000000000000000000000000000000");
    CHECK(NvrPluginInitEx(&ctx, "") == 0, "InitEx must return 0 on a refused build id (loaded, nothing installed)");
    CHECK(page[slot_off / sizeof(uintptr_t)] == reinterpret_cast<uintptr_t>(&vrapi_SetClockLevels), "slot touched on wrong build");
    CHECK(HasLine("refused", 2), "no refusal line");
  }
  // 3. the slot holds some other word: refused, slot unchanged, ERROR line.
  {
    g_lines.clear();
    uintptr_t* page = NewSlotPage(PROT_READ);
    mprotect(page, 4096, PROT_READ | PROT_WRITE);
    page[slot_off / sizeof(uintptr_t)] = 0x1234560;
    mprotect(page, 4096, PROT_READ);
    NvrGameContext ctx = MakeCtx(reinterpret_cast<uintptr_t>(page), kId);
    CHECK(NvrPluginInitEx(&ctx, "") == 0, "InitEx must return 0 when a slot is refused");
    CHECK(page[slot_off / sizeof(uintptr_t)] == 0x1234560, "foreign word overwritten");
    CHECK(HasLine("status=unexpected_original", 3), "no ERROR refusal for foreign word");
  }
  // 4. executable page: never written.
  {
    g_lines.clear();
    uintptr_t* page = NewSlotPage(PROT_READ | PROT_EXEC);
    NvrGameContext ctx = MakeCtx(reinterpret_cast<uintptr_t>(page), kId);
    CHECK(NvrPluginInitEx(&ctx, "") == 0, "InitEx must return 0 when a page is executable");
    CHECK(page[slot_off / sizeof(uintptr_t)] == reinterpret_cast<uintptr_t>(&vrapi_SetClockLevels), "executable page was written");
    CHECK(HasLine("status=executable_page", 3), "no ERROR for executable page");
  }
  // 5. no context: refused before anything is touched. 6. the capability flags of the perf and T-26 arms (no module option is on here).
  CHECK(NvrPluginInitEx(nullptr, "") != 0, "a null context must be refused");
  {
    const uint32_t caps = NvrPluginGetCapabilities();
    const bool gameplay = (caps & NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY) != 0;
    CHECK(gameplay == (Q15T_HZ_MATCH90 || Q15T_T26), "caps 0x%x: ALTERS_GAMEPLAY is %d for this arm (the clock raise is not gameplay)", caps, gameplay);
    CHECK((caps & NEVR_PLUGIN_CAP_HOOKS_ENGINE) != 0 && (caps & NEVR_PLUGIN_CAP_NETWORK) == 0, "caps 0x%x", caps);
  }
  // 7. the clock switch, read from args_json: raise only on {"clock":"raise"}; everything else is log-only; each InitEx sets it afresh.
  {
    struct Case {
      const char* args;
      bool raise;
      bool warn;
      const char* what;
      const char* want_line = nullptr;  // a substring the log must hold (the value as decoded)
    };
    const Case cases[] = {
        {"{\"clock\":\"raise\"}", true, false, "raise"},
        {nullptr, false, false, "null args"},
        {"{}", false, false, "empty object"},
        {"{\"clock\":\"log\"}", false, false, "explicit log"},
        {"{\"clock\":\"Raise\"}", false, true, "wrong case"},
        {"{\"clock\":\"raise \"}", false, true, "trailing space"},
        {"{\"clock\":\"raises\"}", false, true, "longer word"},
        {"{\"clock\":\"\"}", false, true, "empty value"},
        {"{\"clock\":\"0123456789012345678901234567890123456789\"}", false, false, "value longer than the buffer"},
        {"{\"other\":\"raise\"}", false, false, "raise under another key"},
        {"{\"x\":\"clock\",\"y\":\"raise\"}", false, false, "the words as values"},
        {"{\"a\":\"b\",\"clock\":\"raise\"}", true, false, "raise after another key"},
        {"{ \"clock\" : \"raise\" }", true, false, "spaces"},
        {"{\"clock\":raise}", false, false, "bare word"},
        {"{\"clock\":\"raise\"", true, false, "no closing brace (value already read)"},
        {"{\"clock\":\"rai", false, false, "cut off inside the value"},
        {"not json", false, false, "not json"},
        {"", false, false, "empty text"},
        {"{\"clock\":\"rai\\u0073e\"}", false, true, "unicode escape is never decoded"},
        {"{\"clock\":\"\\raise\"}", false, true, "escape in the value"},
        {"{\"cloc\":\"x\",\"clock\":\"raise\"}", true, false, "a key that is a prefix of ours comes first"},
        {"{\"clockx\":\"raise\"}", false, false, "a key that extends ours"},
        {"{\"a\":\"b\" \"clock\":\"raise\"}", false, false, "no comma between members"},
        {"{\"clock\" \"raise\"}", false, false, "no colon"},
        {"{\"a\":\"b\"x\"clock\":\"raise\"}", false, false, "garbage between members"},
        {"{\"clock\"x\"raise\"}", false, false, "garbage instead of the colon"},
        {"{\"clock\":\"a\\\"b\\\\c\\/d\"}", false, true, "escapes are decoded", "value=a\"b\\c/d "},
        {"{\"a\":\"x\\\"},{\",\"clock\":\"raise\"}", true, false, "an escaped quote inside an earlier value"},
        {"{\"a\\\\\":\"x\",\"clock\":\"raise\"}", true, false, "an escaped backslash ends an earlier key"},
        {"{\"clock\":\"raise\"}", true, false, "raise again after the others"},
    };
    for (const Case& c : cases) {
      g_lines.clear();
      uintptr_t* page = NewSlotPage(PROT_READ);
      NvrGameContext ctx = MakeCtx(reinterpret_cast<uintptr_t>(page), kId);
      CHECK(NvrPluginInitEx(&ctx, c.args) == 0, "InitEx failed for %s", c.what);
      CHECK(q15t::ClockRaise() == c.raise, "%s: ClockRaise()=%d want %d", c.what, q15t::ClockRaise(), c.raise);
      auto fn = reinterpret_cast<int32_t (*)(void*, int32_t, int32_t)>(page[slot_off / sizeof(uintptr_t)]);
      fn(nullptr, 1, 1);  // the wrapper logs the first calls and every change of the asked levels: make this one a change
      fn(nullptr, 2, 2);
      CHECK(g_real_cpu == (c.raise ? 4 : 2) && g_real_gpu == (c.raise ? 4 : 2), "%s: forwarded %d/%d", c.what, g_real_cpu, g_real_gpu);
      if (c.raise) {
        fn(nullptr, 5, 1);
        CHECK(g_real_cpu == 5 && g_real_gpu == 4, "%s: the raise must only raise: forwarded %d/%d, want 5/4", c.what, g_real_cpu, g_real_gpu);
      }
      CHECK(HasLine(c.raise ? "clock_candidate=1" : "clock_candidate=0"), "%s: init line has the wrong clock_candidate", c.what);
      CHECK(HasLine(c.raise ? " candidate=1" : " candidate=0"), "%s: per-call record has the wrong candidate", c.what);
      CHECK(!c.raise || HasLine("clock_arg=raise "), "%s: init line does not echo the arg", c.what);
      CHECK(c.want_line == nullptr || HasLine(c.want_line), "%s: log lacks [%s]", c.what, c.want_line);
      CHECK(HasLine("unknown clock arg", 2) == c.warn, "%s: unknown-arg WARNING present=%d want %d", c.what, HasLine("unknown clock arg", 2), c.warn);
    }
  }
  if (g_fail == 0) {
    std::printf("host_test: all checks pass (hz match90=%d, t26=%d)\n", Q15T_HZ_MATCH90, Q15T_T26);
  } else {
    std::printf("host_test: %d failure(s) (hz match90=%d, t26=%d)\n", g_fail, Q15T_HZ_MATCH90, Q15T_T26);
  }
  return g_fail == 0 ? 0 : 1;
}
