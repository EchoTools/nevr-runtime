// Host test of aibots.cpp against a fake CR15Game / CR15NetGame / CR15NetAIScenarios. Real SafeRead, real request file,
// a fake clock; EnableScenario is a fake that records its calls.
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "aibots.h"
#include "nevr/plugin_interface.h"
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
static int CountLines(const char* a, const char* b = "", int level = -1) {
  int n = 0;
  for (const auto& l : g_lines)
    if (l.text.find(a) != std::string::npos && l.text.find(b) != std::string::npos && (level < 0 || (int)l.level == level)) ++n;
  return n;
}
static bool HasLine(const char* needle, int level = -1) { return CountLines(needle, "", level) > 0; }

static const uintptr_t kBase = 0x7000000000ull;
template <typename T>
static void Put(void* base, size_t off, T v) {
  std::memcpy(static_cast<char*>(base) + off, &v, sizeof v);
}

// ---- the fake world ----
alignas(16) static uintptr_t g_game[0x9000 / 8];
alignas(16) static uintptr_t g_net[0x64000 / 8];
alignas(16) static uintptr_t g_scen[0x600 / 8];
alignas(16) static uintptr_t g_flags_ptr[1];
alignas(16) static uintptr_t g_table[2];
alignas(16) static uintptr_t g_gs[0x500 / 8];

struct EnableCall {
  void* scen;
  int state;
  double at;
};
static std::vector<EnableCall> g_calls;
static double g_now = 100.0;
static double Clock() { return g_now; }
static bool g_enable_sets = true;
static void FEnable(void* scen, int state) {
  g_calls.push_back({scen, state, g_now});
  if (g_enable_sets) Put<uintptr_t>(scen, 0x560, 0x1234);
}

static void SetCount(uint32_t n) { Put<uint32_t>(g_scen, 0x500, n); }
static void SetState(int32_t s) { Put<int32_t>(g_net, 0, s); }
static void BuildWorld() {
  std::memset(g_game, 0, sizeof g_game);
  std::memset(g_net, 0, sizeof g_net);
  std::memset(g_scen, 0, sizeof g_scen);
  std::memset(g_gs, 0, sizeof g_gs);
  g_flags_ptr[0] = 0x40;  // a host
  g_game[0x8510 / 8] = reinterpret_cast<uintptr_t>(g_net);
  Put<uintptr_t>(g_net, 8, reinterpret_cast<uintptr_t>(g_game));
  SetState(9);
  Put<uint8_t>(g_net, 0x6341b, 1);
  Put<uintptr_t>(g_net, 0x2ab0, reinterpret_cast<uintptr_t>(g_scen));
  Put<uintptr_t>(g_net, 0x2da0, reinterpret_cast<uintptr_t>(g_flags_ptr));
  SetCount(3);
  // one gamespace holding no combat level
  g_table[0] = 0x5555;
  g_table[1] = reinterpret_cast<uintptr_t>(g_gs);
  g_game[0x1f48 / 8] = reinterpret_cast<uintptr_t>(g_table);
  g_game[0x1f78 / 8] = 1;
  g_gs[0x8 / 8] = reinterpret_cast<uintptr_t>(g_game);
  g_gs[0x58 / 8] = 0x1111;
}

static std::string g_dir;
static time_t g_mt = 1000000;
static void ClearDir() {
  DIR* d = opendir(g_dir.c_str());
  if (d == nullptr) return;
  while (dirent* e = readdir(d))
    if (e->d_name[0] != '.') unlink((g_dir + "/" + e->d_name).c_str());
  closedir(d);
}
static void WriteReq(const char* text) {
  const std::string p = g_dir + "/aibots.req";
  FILE* f = std::fopen(p.c_str(), "w");
  std::fputs(text, f);
  std::fclose(f);
  g_mt += 10;  // an mtime that differs from the last one however fast the test runs
  timespec ts[2] = {{g_mt, 0}, {g_mt, 0}};
  utimensat(AT_FDCWD, p.c_str(), ts, 0);
}

static void Reset() {
  g_lines.clear();
  ClearDir();
  BuildWorld();
  g_calls.clear();
  g_enable_sets = true;
  g_now = 100.0;
  q15t::ResetAiBotsForTest();
  q15t::AiBotsFns f{};
  f.enable = &FEnable;
  q15t::InitAiBots(kBase, f);
}
static void Tick() {
  q15t::AiBotsTick(reinterpret_cast<uintptr_t>(g_game));
  g_now += 0.0625;
}
static void Run(double seconds) {
  const double end = g_now + seconds;
  while (g_now < end) Tick();
}

static uint32_t g_word[4];
static void* g_lookup_ret = nullptr;
static void* Lookup(const char*) { return g_lookup_ret; }

int main() {
  char tmpl[] = "/tmp/q15t-ai-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  g_dir = tmpl;
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);
  q15t::SetAiBotsClockForTest(&Clock);
  q15t::SetAiBotsDirForTest(g_dir.c_str());

  // 0. the compiled-in defaults, before any test reset: scenario RandomAttack (1), delay 3 s
  g_lines.clear();
  BuildWorld();
  g_calls.clear();
  {
    q15t::AiBotsFns f{};
    f.enable = &FEnable;
    q15t::InitAiBots(kBase, f);
  }
  Run(4);
  CHECK(g_calls.size() == 1 && g_calls[0].state == 1 && g_calls[0].at == 103.0, "defaults: %zu calls, state %d at %.4f", g_calls.size(), g_calls.empty() ? -9 : g_calls[0].state,
        g_calls.empty() ? 0.0 : g_calls[0].at);

  // 1. defaults: three bots, held for 3 s, EnableScenario(scenarios, 1 = RandomAttack) once
  Reset();
  Run(2.9);
  CHECK(g_calls.empty(), "EnableScenario before the 3 s hold: %zu", g_calls.size());
  CHECK(HasLine("aibots on", 1) && CountLines("aibots bots", "bots=3") > 0, "no start / bots records");
  Run(0.3);
  CHECK(g_calls.size() == 1 && g_calls[0].scen == static_cast<void*>(g_scen) && g_calls[0].state == 1 && g_calls[0].at == 103.0, "enable: %zu calls, state %d at %.4f",
        g_calls.size(), g_calls.empty() ? -9 : g_calls[0].state, g_calls.empty() ? 0.0 : g_calls[0].at);
  CHECK(CountLines("aibots enable", "scenario=1 bots=3") == 1 && HasLine("why=randomattack") && HasLine("first call; scenario object set; host yes; combat_map 0"), "no enable record");
  Run(10);
  CHECK(g_calls.size() == 1, "repeated: %zu", g_calls.size());

  // 2. no bots (arena, lobby, online): nothing, however long
  Reset();
  SetCount(0);
  Run(20);
  CHECK(g_calls.empty() && !HasLine("aibots bots"), "enable for zero bots");

  // 3. not in game / not a combat match / no scenarios object: nothing
  Reset();
  SetState(8);
  Run(10);
  CHECK(g_calls.empty(), "enable while not in game");
  SetState(9);
  Put<uint8_t>(g_net, 0x6341b, 0);
  Run(10);
  CHECK(g_calls.empty(), "enable outside a combat match");
  Put<uint8_t>(g_net, 0x6341b, 1);
  Put<uintptr_t>(g_net, 0x2ab0, 0);
  Run(10);
  CHECK(g_calls.empty(), "enable without a scenarios object");
  Put<uintptr_t>(g_net, 0x2ab0, reinterpret_cast<uintptr_t>(g_scen));
  Put<uintptr_t>(g_net, 8, 0x1234);  // the net game does not point back at this game
  Run(10);
  CHECK(g_calls.empty(), "enable for a net game of another game");
  Put<uintptr_t>(g_net, 8, reinterpret_cast<uintptr_t>(g_game));
  Run(4);
  CHECK(g_calls.size() == 1, "not enabled once everything held: %zu", g_calls.size());

  // 3b. in a match for 10 s with nothing to do: one line says why, once per match; none when the call was made
  Reset();
  Put<uint8_t>(g_net, 0x6341b, 0);
  Run(9);
  CHECK(!HasLine("aibots idle"), "idle line before 10 s");
  Run(3);
  CHECK(CountLines("aibots idle", "matchiscombat 0, scenarios object yes") == 1 && CountLines("aibots idle", "", 1) == 1, "no idle line after 10 s");
  Run(10);
  CHECK(CountLines("aibots idle") == 1, "idle line repeated");
  SetState(10);
  Run(1);
  SetState(9);
  Run(9);
  CHECK(CountLines("aibots idle") == 1, "idle line for the next match before its 10 s");
  Run(3);
  CHECK(CountLines("aibots idle") == 2, "idle line not re-armed for the next match");
  Reset();
  Run(14);
  CHECK(!HasLine("aibots idle"), "idle line after a successful call");

  // 4. the count changing resets the hold
  Reset();
  Run(2);
  SetCount(4);
  Run(2.9);
  CHECK(g_calls.empty(), "enable although the count changed 2.9 s ago");
  Run(0.5);
  CHECK(g_calls.size() == 1 && g_calls[0].at == 105.0, "enable at %.4f, wanted 105.0", g_calls.empty() ? 0.0 : g_calls[0].at);

  // 4b. the game is looked at 4 Hz: a count that appears off the 0.25 s grid is seen on the next grid point
  Reset();
  SetCount(0);
  Run(1.0625);
  SetCount(2);
  Run(5);
  CHECK(g_calls.size() == 1 && g_calls[0].at >= 104.0 && g_calls[0].at <= 104.5, "looked at too slowly: enable at %.4f, wanted 104.25", g_calls.empty() ? 0.0 : g_calls[0].at);

  // 5. the count changing after the call: called again once it holds; a replaced scenario is said so
  Reset();
  Run(4);
  SetCount(5);
  Run(2.9);
  CHECK(g_calls.size() == 1, "re-enabled too early: %zu", g_calls.size());
  Run(0.5);
  CHECK(g_calls.size() == 2 && HasLine("bot count changed; scenario object set"), "no second call for the new count: %zu", g_calls.size());
  Reset();
  Put<uintptr_t>(g_scen, 0x560, 0x99);
  Run(4);
  CHECK(HasLine("replacing one already set"), "an existing scenario not reported");
  Reset();
  g_enable_sets = false;
  Run(4);
  CHECK(HasLine("NOT set (not the host?)"), "a scenario that was not created not reported");

  // 6. leaving the match re-arms
  Reset();
  Run(4);
  CHECK(g_calls.size() == 1, "setup");
  SetState(10);
  Run(1);
  CHECK(CountLines("aibots re-armed", "", 1) == 1, "no re-armed record");
  Run(2);
  CHECK(CountLines("aibots re-armed") == 1, "re-armed repeated");
  SetState(9);
  const double t_back = g_now;
  Run(4);
  CHECK(g_calls.size() == 2 && g_calls[1].at >= t_back + 3.0, "not enabled (or enabled without the hold) in the next match: %zu calls, at %.4f, back at %.4f", g_calls.size(),
        g_calls.size() < 2 ? 0.0 : g_calls[1].at, t_back);
  Reset();
  SetState(10);
  Run(2);
  CHECK(!HasLine("aibots re-armed"), "re-armed without a call");
  Reset();
  Run(4);
  g_game[0x8510 / 8] = 0;  // no net game at all (between matches)
  Run(1);
  CHECK(CountLines("aibots re-armed") == 1, "a vanished net game did not re-arm");

  // 7. aibots.req: scenario and delay, re-read when it changes; off; unknown names keep the old value
  Reset();
  WriteReq("scenario=off\n");
  Run(10);
  CHECK(g_calls.empty() && CountLines("aibots req", "why=aibots.req read") == 1, "scenario=off: the request must be read once per change, not per poll (%d)", CountLines("aibots req", "why=aibots.req read"));
  WriteReq("scenario=aionevone delay=1\n");
  Run(2);
  CHECK(g_calls.size() == 1 && g_calls[0].state == 5, "scenario=aionevone delay=1: %zu calls, state %d", g_calls.size(), g_calls.empty() ? -9 : g_calls[0].state);
  CHECK(g_calls.size() == 1 && g_calls[0].at - 110.0 < 2.0, "delay=1 not honoured (at %.4f)", g_calls.empty() ? 0.0 : g_calls[0].at);
  WriteReq("scenario=4\n");
  Run(5);
  CHECK(g_calls.size() == 1, "a changed request re-enabled by itself: %zu", g_calls.size());
  Reset();
  WriteReq("scenario=3 delay=0.5 debug=1\n");
  Run(2);
  CHECK(g_calls.size() == 1 && g_calls[0].state == 3 && g_calls[0].at == 100.5, "request present at start: state %d at %.4f", g_calls.empty() ? -9 : g_calls[0].state, g_calls.empty() ? 0.0 : g_calls[0].at);
  Reset();
  WriteReq("scenario=PATROL\n");
  Run(4);
  CHECK(g_calls.size() == 1 && g_calls[0].state == 0, "scenario=PATROL");
  Reset();
  WriteReq("scenario=bogus\n");
  Run(4);
  CHECK(g_calls.size() == 1 && g_calls[0].state == 1 && HasLine("unknown scenario", 2), "unknown scenario name");
  Reset();
  WriteReq("scenario=7\n");
  Run(4);
  CHECK(g_calls.size() == 1 && g_calls[0].state == 1 && HasLine("unknown scenario", 2), "scenario 7 out of range");

  // 8. too many agents: a WARNING
  Reset();
  SetCount(15);
  Run(4);
  CHECK(HasLine("aibots agents", 2), "no warning for 15 agents");
  Reset();
  SetCount(14);
  Run(4);
  CHECK(!HasLine("aibots agents"), "warning for 14 agents");

  // 9. context in the enable record: the engine's own host test and a loaded combat map
  const uint64_t flag_cases[][2] = {{2, 1}, {0x40, 1}, {0x44, 0}, {0, 0}, {0x4, 0}};
  for (const auto& fc : flag_cases) {
    Reset();
    g_flags_ptr[0] = fc[0];
    Run(4);
    CHECK(HasLine(fc[1] ? "host yes" : "host no"), "flags %llx: host not %s", (unsigned long long)fc[0], fc[1] ? "yes" : "no");
  }
  Reset();
  g_gs[0x58 / 8] = 0x43e2da7914642604ull;
  Run(4);
  CHECK(HasLine("combat_map 1"), "combat map not reported");

  // 10. the pinned address: name and first word are checked
  {
    q15t::AiBotsFns f{};
    const char* why = "";
    g_word[0] = 0xd100c3ff;
    const uintptr_t base = reinterpret_cast<uintptr_t>(&g_word[0]) - 0x123c2bc;
    g_lookup_ret = nullptr;
    CHECK(q15t::ResolveAiBotsFns(base, nullptr, &f, &why) && f.named == 0 && reinterpret_cast<uintptr_t>(f.enable) == base + 0x123c2bc, "fixed-address resolve");
    g_lookup_ret = &g_word[0];
    CHECK(q15t::ResolveAiBotsFns(base, &Lookup, &f, &why) && f.named == 1, "name at the pinned address");
    g_lookup_ret = &g_word[1];
    q15t::AiBotsFns g{};
    CHECK(!q15t::ResolveAiBotsFns(base, &Lookup, &g, &why) && g.enable == nullptr, "a name at another address accepted");
    g_word[0] = 0xd100c3fe;
    CHECK(!q15t::ResolveAiBotsFns(base, nullptr, &g, &why) && g.enable == nullptr && std::strstr(why, "first word") != nullptr, "another first word accepted");
    CHECK(!q15t::ResolveAiBotsFns(kBase, nullptr, &g, &why) && std::strstr(why, "unreadable") != nullptr, "an unreadable address accepted");
  }

  // 11. no libr15 to resolve from (host): one ERROR, nothing runs
  Reset();
  q15t::InitAiBots(kBase, q15t::AiBotsFns{});
  Run(5);
  CHECK(g_calls.empty() && q15t::AiBotsPhase() == 2 && HasLine("aibots off", 3), "missing library not loud");

  ClearDir();
  rmdir(g_dir.c_str());
  if (g_fail == 0) std::printf("aibots_test: all checks passed\n");
  return g_fail == 0 ? 0 : 1;
}
