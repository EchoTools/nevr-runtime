#include "aibots.h"

#include <dlfcn.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>

#include "plugin_log.h"
#include "refresh_rate.h"
#include "safe_read.h"

namespace q15t {
namespace {

// Source constants: <source build>/the source build's aibots:42-52. the disassembly (VA = ELF + 0x100000):
// NetGameMT 0x13027d4 `mov w8,#0x8510; ldr x0,[x0,x8]`; Scenarios() 0x138fd0c `ldr x0,[x0,#0x2ab0]`; ConnectBot 0x133bf78
// `ldr w8,[x19,#0x500]; add; str` (a u32, 0x50-byte rows); EnableScenario 0x133c2bc reads/writes scenarios+0x560 and flags
// [[[scenarios+0x558]+0x2da0]]; UpdateGameExpressionData 0x13809f0 stores (state == 9) at net+0x6340c+8 and a byte at +0xf.
constexpr std::uintptr_t kNetGameOffset = 0x8510, kNetGameGame = 8, kNetScenarios = 0x2ab0, kNetMatchIsCombat = 0x6341b;
constexpr std::uintptr_t kNetFlagsPtr = 0x2da0, kScenBotCount = 0x500, kScenCurrent = 0x560;
constexpr std::uintptr_t kFnEnableScenario = 0x123c2bc;
constexpr std::uint32_t kEnableFirstWord = 0xd100c3ff;  // sub sp, sp, #0x30
constexpr int kInGame = 9;
constexpr long long kMaxAgents = 16;  // 0x500 / 0x50: ConnectBot does not bound-check
constexpr double kLookEveryS = 0.25, kReqEveryS = 1.0, kIdleLogS = 10.0;
constexpr int kDefaultScenario = 1;  // RandomAttack
constexpr int kDefaultDelayMs = 3000;

const char kSymEnable[] = "_ZN10NRadEngine8NRadGame18CR15NetAIScenarios14EnableScenarioENS1_14EScenarioStateE";
const char* const kNames[] = {"patrol", "randomattack", "weaponattack", "spike", "aithrowtest", "aionevone", "aibdebugarena"};

AiBotsFns g_f{};
bool g_have_fns = false;
int g_phase = 0;
std::uintptr_t g_base = 0;
int g_scenario = kDefaultScenario;  // -1: off
int g_delay_ms = kDefaultDelayMs;
bool g_called = false;
long long g_last_count = -1, g_called_count = 0;
double g_count_since = 0, g_next_look = 0, g_next_req = 0, g_in_since = -1;
bool g_idle_logged = false;
long long g_req_sec = 0, g_req_nsec = 0;
char g_dir_override[256] = "";
double (*g_clock)() = nullptr;
void* g_lib = nullptr;

double Now() {
  if (g_clock != nullptr) return g_clock();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

void* LibLookup(const char* symbol) { return g_lib != nullptr ? dlsym(g_lib, symbol) : nullptr; }

// The plugin's data directory (as the T-26 module's; not created here).
bool DataDirPath(char* out, std::size_t n) {
  if (g_dir_override[0] != 0) {
    std::snprintf(out, n, "%s", g_dir_override);
    return true;
  }
  char cmd[256] = "";
  FILE* f = std::fopen("/proc/self/cmdline", "re");
  if (f == nullptr) return false;
  const std::size_t r = std::fread(cmd, 1, sizeof cmd - 1, f);
  std::fclose(f);
  cmd[r] = 0;
  if (char* colon = std::strchr(cmd, ':')) *colon = 0;
  if (std::strchr(cmd, '.') == nullptr || std::strchr(cmd, '/') != nullptr || std::strlen(cmd) > 160) return false;
  std::snprintf(out, n, "/sdcard/Android/data/%.160s/files/q15t", cmd);
  return true;
}

void LogAi(std::uint32_t level, const char* message, const char* why, long long scenario, long long bots, const char* detail) {
  char s[24], b[24];
  std::snprintf(s, sizeof s, "%lld", scenario);
  std::snprintf(b, sizeof b, "%lld", bots);
  const NevrLogField f[] = {{"why", why}, {"scenario", s}, {"bots", b}, {"detail", detail}};
  Log(level, message, f, 4);
}

int ParseScenario(const char* v) {  // -1 off, 0-6, -2 unknown
  if (std::strcmp(v, "off") == 0) return -1;
  for (int i = 0; i < 7; ++i)
    if (strcasecmp(v, kNames[i]) == 0) return i;
  char* end = nullptr;
  const long n = std::strtol(v, &end, 0);
  return (end != v && *end == 0 && n >= 0 && n <= 6) ? static_cast<int>(n) : -2;
}

void ReadRequest(const char* path) {
  char buf[256] = {};
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  const ssize_t n = read(fd, buf, sizeof buf - 1);
  close(fd);
  if (n < 0) return;
  char* save = nullptr;
  for (char* w = strtok_r(buf, " \t\r\n", &save); w != nullptr; w = strtok_r(nullptr, " \t\r\n", &save)) {
    if (std::strncmp(w, "scenario=", 9) == 0) {
      const int s = ParseScenario(w + 9);
      if (s == -2)
        LogAi(NEVR_LOG_WARNING, "aibots req", "unknown scenario in aibots.req, kept", g_scenario, -1, w);
      else
        g_scenario = s;
    } else if (std::strncmp(w, "delay=", 6) == 0) {
      g_delay_ms = static_cast<int>(std::atof(w + 6) * 1000);
    }
  }
  char detail[64];
  std::snprintf(detail, sizeof detail, "delay_ms %d", g_delay_ms);
  LogAi(NEVR_LOG_INFO, "aibots req", "aibots.req read", g_scenario, -1, detail);
}

void PollRequest() {  // (re)read aibots.req whenever it appears or changes
  char dir[256], path[300];
  if (!DataDirPath(dir, sizeof dir)) return;
  std::snprintf(path, sizeof path, "%s/aibots.req", dir);
  struct stat sb;
  if (stat(path, &sb) != 0) return;
  if (sb.st_mtim.tv_sec == g_req_sec && sb.st_mtim.tv_nsec == g_req_nsec) return;
  g_req_sec = sb.st_mtim.tv_sec;
  g_req_nsec = sb.st_mtim.tv_nsec;
  ReadRequest(path);
}

bool Start() {
  g_phase = 2;
  if (!g_have_fns) {
    g_lib = dlopen("libr15.so", RTLD_NOW | RTLD_NOLOAD);
    const char* why = "";
    if (g_lib == nullptr) {
      LogAi(NEVR_LOG_ERROR, "aibots off", "libr15.so is not loaded", -1, -1, "");
      return false;
    }
    if (!ResolveAiBotsFns(g_base, &LibLookup, &g_f, &why)) {
      LogAi(NEVR_LOG_ERROR, "aibots off", "EnableScenario is not at the pinned address (not the pinned build?)", -1, -1, why);
      return false;
    }
    g_have_fns = true;
  }
  PollRequest();
  char detail[64];
  std::snprintf(detail, sizeof detail, "delay_ms %d", g_delay_ms);
  LogAi(NEVR_LOG_INFO, "aibots on", "EnableScenario once the bots of a combat match are in game", g_scenario, -1, detail);
  g_phase = 1;
  return true;
}

void Look(std::uintptr_t game, double now) {
  bool ok = true;
  const auto net = Rd<std::uintptr_t>(game + kNetGameOffset, &ok);
  if (!ok) return;
  bool in_game = false;
  if (net != 0) {
    const auto back = Rd<std::uintptr_t>(net + kNetGameGame, &ok);
    const auto state = Rd<std::int32_t>(net, &ok);
    if (!ok) return;
    in_game = back == game && state == kInGame;
  }
  if (!in_game) {
    if (g_called) LogAi(NEVR_LOG_INFO, "aibots re-armed", "left the match", -1, -1, "");
    g_called = false;
    g_last_count = -1;
    g_in_since = -1;
    g_idle_logged = false;
    return;
  }
  if (g_in_since < 0) g_in_since = now;
  if (!g_called && !g_idle_logged && now - g_in_since >= kIdleLogS) {  // in a match this long and still nothing to do: say why, once
    g_idle_logged = true;
    bool ok3 = true;
    const auto c = Rd<std::uint8_t>(net + kNetMatchIsCombat, &ok3);
    const auto sc = Rd<std::uintptr_t>(net + kNetScenarios, &ok3);
    const auto n = sc != 0 ? Rd<std::uint32_t>(sc + kScenBotCount, &ok3) : 0u;
    char detail[96];
    std::snprintf(detail, sizeof detail, "in game %.0f s: matchiscombat %u, scenarios object %s, scenario %d%s", kIdleLogS, c, sc != 0 ? "yes" : "no", g_scenario,
                  ok3 ? "" : " (read failed)");
    LogAi(NEVR_LOG_INFO, "aibots idle", "no EnableScenario in this match yet", g_scenario, n, detail);
  }
  const auto combat = Rd<std::uint8_t>(net + kNetMatchIsCombat, &ok);
  if (!ok || combat == 0 || g_scenario < 0) return;
  const auto scen = Rd<std::uintptr_t>(net + kNetScenarios, &ok);
  if (!ok || scen == 0) return;
  const long long count = Rd<std::uint32_t>(scen + kScenBotCount, &ok);
  if (!ok) return;
  if (count == g_last_count && (count == 0 || (g_called && count == g_called_count))) return;  // the usual pass
  if (count != g_last_count) {
    if (count > kMaxAgents - 2) LogAi(NEVR_LOG_WARNING, "aibots agents", "bot agents near the table size (16)", g_scenario, count, "");
    if (count != 0) LogAi(NEVR_LOG_INFO, "aibots bots", "bot agent count changed; waiting for it to hold", g_scenario, count, "");
    g_last_count = count;
    g_count_since = now;
    return;
  }
  if ((now - g_count_since) * 1000 < g_delay_ms) return;
  const int scenario = g_scenario;
  const bool had = Rd<std::uintptr_t>(scen + kScenCurrent, &ok) != 0;
  g_f.enable(reinterpret_cast<void*>(scen), scenario);
  const bool has = Rd<std::uintptr_t>(scen + kScenCurrent, &ok) != 0;
  // context for the headset run: the engine's own server test (EnableScenario 0x133c2d4-0x133c2ec: flags & 2, or
  // (flags & 0x44) == 0x40, takes the host path) and whether a combat map is loaded (refresh_rate.cpp's CombatMapLoaded)
  bool ok2 = true;
  const auto flags = Rd<std::uint64_t>(Rd<std::uintptr_t>(net + kNetFlagsPtr, &ok2), &ok2);
  std::uint64_t level = 0;
  const bool map = CombatMapLoaded(game, &level);
  char detail[160];
  std::snprintf(detail, sizeof detail, "%s; scenario object %s; host %s; combat_map %d; delay_ms %d", g_called ? "bot count changed" : had ? "replacing one already set" : "first call",
                has ? "set" : "NOT set (not the host?)", !ok2 ? "unknown" : ((flags & 2) != 0 || (flags & 0x44) == 0x40) ? "yes" : "no", map ? 1 : 0, g_delay_ms);
  LogAi(NEVR_LOG_INFO, "aibots enable", kNames[scenario], scenario, count, detail);
  g_called = true;
  g_called_count = count;
}

}  // namespace

bool ResolveAiBotsFns(std::uintptr_t base, void* (*lookup)(const char*), AiBotsFns* out, const char** why) {
  AiBotsFns f{};
  const std::uintptr_t want = base + kFnEnableScenario;
  void* p = lookup != nullptr ? lookup(kSymEnable) : nullptr;
  if (p != nullptr && reinterpret_cast<std::uintptr_t>(p) != want) {
    *why = "EnableScenario name resolves elsewhere";
    return false;
  }
  if (p != nullptr) f.named = 1;
  bool ok = true;
  const auto first = Rd<std::uint32_t>(want, &ok);
  if (!ok || first != kEnableFirstWord) {
    *why = ok ? "first word is not sub sp,sp,#0x30" : "first word unreadable";
    return false;
  }
  f.enable = reinterpret_cast<void (*)(void*, int)>(want);
  *out = f;
  return true;
}

void InitAiBots(std::uintptr_t base, const AiBotsFns& fns) {
  g_base = base;
  g_f = fns;
  g_have_fns = fns.enable != nullptr;
}

void AiBotsTick(std::uintptr_t game) {
  if (g_phase == 2) return;
  if (g_phase == 0 && !Start()) return;
  const double now = Now();
  if (now >= g_next_req) {
    g_next_req = now + kReqEveryS;
    PollRequest();
  }
  if (now < g_next_look) return;
  g_next_look = now + kLookEveryS;
  Look(game, now);
}

int AiBotsPhase() { return g_phase; }

void SetAiBotsClockForTest(double (*clock)()) { g_clock = clock; }
void SetAiBotsDirForTest(const char* dir) { std::snprintf(g_dir_override, sizeof g_dir_override, "%s", dir != nullptr ? dir : ""); }
void ResetAiBotsForTest() {
  g_f = AiBotsFns{};
  g_have_fns = false;
  g_phase = 0;
  g_base = 0;
  g_scenario = kDefaultScenario;
  g_delay_ms = kDefaultDelayMs;
  g_called = false;
  g_last_count = -1;
  g_called_count = 0;
  g_count_since = g_next_look = g_next_req = 0;
  g_in_since = -1;
  g_idle_logged = false;
  g_req_sec = g_req_nsec = 0;
  g_lib = nullptr;
}

}  // namespace q15t
