// Host test of the plugin's hook install and wrapper against a fake "game library" slot (an mmap'd page).
// Built per arm (Hz level rule, T-26) by src/quest/tests/q15t/run_host_tests.sh; the clock raise is a runtime switch (args {clock: raise}), tested in every build. Real code, real mprotect, real /proc/self/maps.
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
#include "module_switch.h"
#include "nevr/symbol_hash.h"
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
      using EchoVR::CSymbol64Hash;
      CHECK(HasLine("q15t_refresh_rate_install") && HasLine("mode=lobby") && HasLine("level_hz=90"),
            "no install record for the default level rule (lobby, 90 Hz off a Quest 2)");
      gs[0x58 / 8] = CSymbol64Hash("mpl_lobby_hub_unknown");  // no counted level
      // through the game-tick slot, as CncaGame::Render calls it
      const uintptr_t tick_slot = page[Q15T_SLOT_EXCLUSIVEUPDATEGAME / sizeof(uintptr_t)];
      CHECK(tick_slot != reinterpret_cast<uintptr_t>(&FakeExclusiveUpdateGame), "tick slot not replaced");
      auto tick = reinterpret_cast<void (*)(void*)>(tick_slot);
      g_hz_calls = 0;
      tick(game);
      CHECK(g_update_calls >= 1, "the original ExclusiveUpdateGame was not called");
      CHECK(g_hz_calls == 0, "no switch expected without a combat or lobby level, got %d calls", g_hz_calls);
      gs[0x58 / 8] = CSymbol64Hash("mpl_combat_fission");
      q15t::ResetHzScheduleForTest();
      tick(game);
      CHECK(g_hz_calls == 1 && g_hz_seen == 90.f, "entering a combat map must ask 90: calls %d, %.1f", g_hz_calls, (double)g_hz_seen);
      CHECK(HasLine("refresh switch") && HasLine("why=combat or lobby level loaded"), "no switch record");
      hz(reinterpret_cast<void*>(0x1234), 120.f);
      CHECK(g_hz_seen == 90.f, "a game request during a match must stay 90: %.1f", (double)g_hz_seen);
      CHECK(HasLine("asked_hz=120.0 sent_hz=90.0"), "the per-call record must carry asked_hz and sent_hz");
      q15t::ResetHzScheduleForTest();
      tick(game);
      CHECK(g_hz_calls == 2, "no change, no re-ask: calls %d", g_hz_calls);
      gs[0x58 / 8] = CSymbol64Hash("mpl_lobby_b_social");  // combat map -> lobby: still the level rate, no re-ask
      q15t::ResetHzScheduleForTest();
      tick(game);
      CHECK(g_hz_calls == 2, "a lobby level after a combat map keeps 90 without a re-ask: calls %d", g_hz_calls);
      // an Echo Arena level loaded in the second table wins over the lobby level: the game's 120 again
      static uintptr_t gs2[0x60 / 8 + 1] = {};
      static uintptr_t table2[2] = {};
      gs2[0x8 / 8] = g;
      gs2[0x58 / 8] = CSymbol64Hash("mpl_arena_a");
      table2[1] = reinterpret_cast<uintptr_t>(gs2);
      game[0x1f88 / 8] = reinterpret_cast<uintptr_t>(table2);
      game[0x1fb8 / 8] = 1;
      q15t::ResetHzScheduleForTest();
      tick(game);
      CHECK(g_hz_calls == 3 && g_hz_seen == 120.f, "an Echo Arena level must ask the game's 120: %.1f", (double)g_hz_seen);
      CHECK(HasLine("why=echo arena level loaded: session rate"), "no Echo Arena switch record");
      game[0x1fb8 / 8] = 0;  // Echo Arena unloaded, the lobby left loaded: 90 again
      q15t::ResetHzScheduleForTest();
      tick(game);
      CHECK(g_hz_calls == 4 && g_hz_seen == 90.f, "back in the lobby must ask 90: %.1f", (double)g_hz_seen);
      gs[0x58 / 8] = CSymbol64Hash("mpl_lobby_hub_unknown");
      q15t::ResetHzScheduleForTest();
      tick(game);
      CHECK(g_hz_calls == 5 && g_hz_seen == 120.f, "leaving every counted level must ask the game's 120: %.1f", (double)g_hz_seen);
      CHECK(HasLine("why=no combat or lobby level: session rate"), "no leave record");
      // every level id is the CSymbol64 of its name
      {
        const char* lobbies[] = {"mpl_lobby_b", "mpl_lobby_b2", "mpl_lobby_b2_lowspec", "mpl_lobby_b_combat", "mpl_lobby_b_arena", "mpl_lobby_b_social"};
        const char* combats[] = {"mpl_combat_dyson", "mpl_combat_combustion", "mpl_combat_fission", "mpl_combat_gauss"};
        const char* arenas[] = {"mpl_arena_a", "mpl_arena_a_lowspec", "mpl_arena_b", "mpl_tutorial_a"};
        uint64_t lv = 0;
        for (const char* name : lobbies) {
          gs[0x58 / 8] = CSymbol64Hash(name);
          CHECK(q15t::ClassifyLoadedLevels(g, true, &lv) == q15t::HzLevelKind::kCombatOrLobby && lv == CSymbol64Hash(name), "%s is a lobby level", name);
          CHECK(q15t::ClassifyLoadedLevels(g, false, &lv) == q15t::HzLevelKind::kNone, "%s counts without the lobby rule", name);
          CHECK(!q15t::CombatMapLoaded(g, &lv), "CombatMapLoaded must not see the lobby level %s", name);
        }
        for (const char* name : combats) {
          gs[0x58 / 8] = CSymbol64Hash(name);
          CHECK(q15t::ClassifyLoadedLevels(g, false, &lv) == q15t::HzLevelKind::kCombatOrLobby, "%s is a combat map", name);
          CHECK(q15t::CombatMapLoaded(g, &lv), "CombatMapLoaded must still see %s", name);
        }
        for (const char* name : arenas) {
          gs[0x58 / 8] = CSymbol64Hash(name);
          CHECK(q15t::ClassifyLoadedLevels(g, true, &lv) == q15t::HzLevelKind::kEchoArena, "%s is an Echo Arena level", name);
          CHECK(q15t::ClassifyLoadedLevels(g, false, &lv) == q15t::HzLevelKind::kNone, "%s decides without the lobby rule", name);
        }
      }
      // the Quest 2 rule: a "hollywood" device asks 72; hz72q2.off keeps 90; hzlobby90.off drops the lobby; hzlevel.off is witness only
      {
        char dir[] = "/tmp/q15t_hz_XXXXXX";
        CHECK(mkdtemp(dir) != nullptr, "mkdtemp");
        q15t::SetModuleSwitchDirForTest(dir);
        q15t::SetHzDeviceForTest("hollywood");
        q15t::HzRule r = q15t::ConfigureHzRule();
        CHECK(r.on && r.lobby && r.quest2 && r.low_hz == 72.f, "Quest 2: level rate %.1f", (double)r.low_hz);
        CHECK(HasLine("device=hollywood") && HasLine("level_hz=72"), "no Quest 2 install record");
        gs[0x58 / 8] = CSymbol64Hash("mpl_lobby_b");
        q15t::ResetHzScheduleForTest();
        tick(game);
        CHECK(g_hz_seen == 72.f, "a Quest 2 lobby must ask 72: %.1f", (double)g_hz_seen);
        const std::string q2off = std::string(dir) + "/hz72q2.off";
        FILE* f = std::fopen(q2off.c_str(), "w");
        if (f) std::fclose(f);
        r = q15t::ConfigureHzRule();
        CHECK(r.on && r.quest2 && r.low_hz == 90.f, "hz72q2.off must keep 90: %.1f", (double)r.low_hz);
        const std::string lobbyoff = std::string(dir) + "/hzlobby90.off";
        f = std::fopen(lobbyoff.c_str(), "w");
        if (f) std::fclose(f);
        r = q15t::ConfigureHzRule();
        CHECK(r.on && !r.lobby && HasLine("mode=match"), "hzlobby90.off must leave combat maps only");
        const std::string leveloff = std::string(dir) + "/hzlevel.off";
        f = std::fopen(leveloff.c_str(), "w");
        if (f) std::fclose(f);
        r = q15t::ConfigureHzRule();
        CHECK(!r.on && HasLine("mode=witness"), "hzlevel.off must leave the witness only");
        const int before = g_hz_calls;
        gs[0x58 / 8] = CSymbol64Hash("mpl_combat_gauss");
        q15t::ResetHzScheduleForTest();
        tick(game);
        CHECK(g_hz_calls == before, "the witness must not switch: calls %d -> %d", before, g_hz_calls);
        std::remove(leveloff.c_str());
        std::remove(lobbyoff.c_str());
        std::remove(q2off.c_str());
        rmdir(dir);
        q15t::SetModuleSwitchDirForTest("");
        q15t::SetHzDeviceForTest(nullptr);
        q15t::ConfigureHzRule();
      }
      // an object that is not the CR15Game: no callback, one ERROR line
      static uintptr_t other[0x2000 / 8] = {};
      other[0] = 0x777;
      q15t::ResetHzScheduleForTest();
      const int before_other = g_hz_calls;
      tick(other);
      CHECK(g_hz_calls == before_other, "a non-CR15Game object ran the tick");
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
