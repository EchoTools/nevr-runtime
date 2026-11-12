#include "quest/combat/aibots.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "quest/combat/aibots_targets.h"
#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/netgame_targets.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::aibots {

namespace {

namespace at = targets;
constexpr double kLookEveryS = 0.25, kReqEveryS = 1.0, kIdleLogS = 10.0;
constexpr int kDefaultScenario = 1;  // RandomAttack
constexpr int kDefaultDelayMs = 3000;
const char* const kNames[] = {"patrol", "randomattack", "weaponattack", "spike", "aithrowtest", "aionevone", "aibdebugarena"};

Fns g_f{};
std::atomic<int> g_phase{0};
bool g_poll = false;
int g_scenario = kDefaultScenario;  // -1: off
int g_delayMs = kDefaultDelayMs;
bool g_called = false, g_scopedSaid = false;
long long g_lastCount = -1, g_calledCount = 0;
double g_countSince = 0, g_nextLook = 0, g_nextReq = 0, g_inSince = -1;
bool g_idleLogged = false;
long long g_reqSec = 0, g_reqNsec = 0;
double (*g_clock)() = nullptr;
std::atomic<std::uint32_t> g_looks{0}, g_enables{0}, g_scopedOut{0};

double Now() noexcept {
  if (g_clock != nullptr) return g_clock();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1e-9;
}

void* DlLookup(const char* symbol) noexcept {
  void* const lib = dlopen("libr15.so", RTLD_NOW | RTLD_NOLOAD);
  return lib != nullptr ? dlsym(lib, symbol) : nullptr;
}

// A development build's request file (afk_aibots.cpp:167): `scenario=<..> delay=<seconds>`.
void ReadRequest(const char* path) noexcept {
  char buf[256] = {};
  const int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return;
  const ssize_t n = read(fd, buf, sizeof(buf) - 1);
  close(fd);
  if (n < 0) return;
  char* save = nullptr;
  for (char* w = strtok_r(buf, " \t\r\n", &save); w != nullptr; w = strtok_r(nullptr, " \t\r\n", &save)) {
    if (std::strncmp(w, "scenario=", 9) == 0) {
      const int s = ParseScenario(w + 9);
      if (s == -2) {
        sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_aibots_scenario",
                            {{"reason", "unknown_word_in_request"}, {"scenario", g_scenario},
                             {"msg", "the request file names no scenario; the one before it is kept"}});
      } else {
        g_scenario = s;
      }
    } else if (std::strncmp(w, "delay=", 6) == 0) {
      g_delayMs = static_cast<int>(std::atof(w + 6) * 1000);
    }
  }
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_aibots_req", {{"scenario", g_scenario}, {"delay_ms", g_delayMs}});
}

void PollRequest() noexcept {  // (re)read aibots.req whenever it appears or changes
  char dir[300], path[400];
  if (!legacy::Directory(dir, sizeof(dir))) return;
  std::snprintf(path, sizeof(path), "%s/aibots.req", dir);
  struct stat sb;
  if (stat(path, &sb) != 0) return;
  if (sb.st_mtim.tv_sec == g_reqSec && sb.st_mtim.tv_nsec == g_reqNsec) return;
  g_reqSec = sb.st_mtim.tv_sec;
  g_reqNsec = sb.st_mtim.tv_nsec;
  ReadRequest(path);
}

void Look(std::uintptr_t game, double now) noexcept {
  bool ok = true;
  const auto net = SafeRead<std::uintptr_t>(game + netgame::kNetGameOffset, &ok);
  if (!ok) return;
  bool inGame = false;
  if (net != 0) {
    const auto back = SafeRead<std::uintptr_t>(net + netgame::kNetGameGame, &ok);
    const auto state = SafeRead<std::int32_t>(net, &ok);
    if (!ok) return;
    inGame = back == game && state == at::kInGame;
  }
  if (!inGame) {
    if (g_called) {
      sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_aibots_rearmed",
                          {{"msg", "the net game left the match; the scenario is enabled again for the next one"}});
    }
    g_called = false;
    g_scopedSaid = false;
    g_lastCount = -1;
    g_inSince = -1;
    g_idleLogged = false;
    return;
  }
  if (g_inSince < 0) g_inSince = now;
  // The game's own combat flag first: in an arena match it is 0 and the look ends here, with four reads and no line, however long the match lasts.
  const auto combat = SafeRead<std::uint8_t>(net + at::kNetMatchIsCombat, &ok);
  if (!ok || combat == 0) return;
  if (!g_called && !g_idleLogged && now - g_inSince >= kIdleLogS) {  // in a combat match this long and still nothing to do: say why, once
    g_idleLogged = true;
    bool ok3 = true;
    const auto sc = SafeRead<std::uintptr_t>(net + at::kNetScenarios, &ok3);
    const auto n = sc != 0 ? SafeRead<std::uint32_t>(sc + at::kScenBotCount, &ok3) : 0u;
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_aibots_idle",
                        {{"matchiscombat", combat}, {"scenarios_object", sc != 0 ? "yes" : "no"}, {"scenario", g_scenario}, {"bots", n},
                         {"read_failed", ok3 ? 0 : 1}, {"msg", "ten seconds in a combat match and EnableScenario has not been called"}});
  }
  if (g_scenario < 0) return;
  const auto scen = SafeRead<std::uintptr_t>(net + at::kNetScenarios, &ok);
  if (!ok || scen == 0) return;
  const long long count = SafeRead<std::uint32_t>(scen + at::kScenBotCount, &ok);
  if (!ok) return;
  if (count == g_lastCount && (count == 0 || (g_called && count == g_calledCount))) return;  // the usual pass
  if (count != g_lastCount) {
    if (count > at::kMaxAgents - 2) {
      sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_aibots_agents",
                          {{"bots", count}, {"msg", "bot agents near the game's table size (16); ConnectBot does not check it"}});
    }
    if (count != 0) {
      sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_aibots_bots",
                          {{"scenario", g_scenario}, {"bots", count}, {"msg", "the bot agent count changed; EnableScenario waits for it to hold"}});
    }
    g_lastCount = count;
    g_countSince = now;
    return;
  }
  if ((now - g_countSince) * 1000 < g_delayMs) return;
  // An Echo Arena level that may be loaded keeps the game's own scenario handling, whatever the net game's flags say (same APK as arena): the combat
  // level answer (combat_scope.h `SeenEchoArena`), which also refuses when its last look is older than kStaleAfterMs or there is none (fails closed).
  // One record of the last look decides, explains the refusal and gives the line's combat-map flag, so the three agree.
  const scope::Look look = scope::LastLook();
  if (scope::SeenEchoArena(look)) {
    g_scopedOut.fetch_add(1, std::memory_order_relaxed);
    if (!g_scopedSaid) {
      g_scopedSaid = true;
      const bool sawFreshLook = look.fresh;  // fresh, so the refusal is an Echo Arena level the look saw
      sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_aibots_scoped_out",
                          {{"bots", count}, {"reason", sawFreshLook ? "echo_arena" : "no_fresh_look"},
                           {"msg", sawFreshLook ? "an Echo Arena level is loaded, so EnableScenario was not called; the game keeps its own bots"
                                         : "the combat level answer has no look younger than a second, so an Echo Arena level is not ruled out and "
                                           "EnableScenario was not called; the game keeps its own bots"}});
    }
    return;
  }
  const int scenario = g_scenario;
  const bool had = SafeRead<std::uintptr_t>(scen + at::kScenCurrent, &ok) != 0;
  g_f.enable(reinterpret_cast<void*>(scen), scenario);
  g_enables.fetch_add(1, std::memory_order_relaxed);
  const bool has = SafeRead<std::uintptr_t>(scen + at::kScenCurrent, &ok) != 0;
  // Context for the headset run: the engine's own server test (EnableScenario 0x123c2d4-0x123c2ec: flags & 2, or (flags & 0x44) == 0x40, takes
  // the host path) and whether a combat map is loaded.
  bool ok2 = true;
  const auto flags = SafeRead<std::uint64_t>(SafeRead<std::uintptr_t>(net + at::kNetFlagsPtr, &ok2), &ok2);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_aibots_enable",
                      {{"scenario_name", kNames[scenario]}, {"scenario", scenario}, {"bots", count},
                       {"case", g_called ? "bot count changed" : had ? "replacing one already set" : "first call"},
                       {"scenario_object", has ? "set" : "not_set"},
                       {"host", !ok2 ? "unknown" : ((flags & 2) != 0 || (flags & 0x44) == 0x40) ? "yes" : "no"},
                       {"combat_map", (look.bits & scope::kBitCombatMap) != 0 ? 1 : 0}, {"delay_ms", g_delayMs},
                       {"msg", "EnableScenario was called: the bots of this practice match get their behaviours"}});
  g_called = true;
  g_calledCount = count;
}

ModuleResult Refused(const char* reason, const char* detail) noexcept {
  g_phase.store(2, std::memory_order_release);
  sentinel::LogFields(sentinel::LogLevel::kError, "combat_aibots_install",
                      {{"result", "refused"}, {"reason", reason}, {"detail", detail},
                       {"msg", "practice AI bots are not started: they stand still as the game has them"}});
  return {false, reason};
}

void WarnUnknownScenario() noexcept {
  sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_aibots_scenario",
                      {{"reason", "unknown_word"}, {"msg", "combat.aibots_scenario names no scenario; RandomAttack stays"}});
}

void WarnLegacyRequest(bool devBuild) noexcept {
  if (devBuild || !legacy::Present("aibots.req")) return;
  sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_aibots_legacy_switch",
                      {{"file", "aibots.req"}, {"replaced_by", "combat.aibots_scenario"},
                       {"msg", "the plugin's request file is read in development builds only and changes nothing here; set the combat keys in config.yaml instead"}});
}

}  // namespace

int ParseScenario(const char* text) noexcept {
  if (text == nullptr) return -2;
  if (std::strcmp(text, "off") == 0) return -1;
  for (int i = 0; i < 7; ++i) {
    if (strcasecmp(text, kNames[i]) == 0) return i;
  }
  char* end = nullptr;
  const long n = std::strtol(text, &end, 0);
  return (end != text && *end == 0 && n >= 0 && n <= 6) ? static_cast<int>(n) : -2;
}

bool Resolve(std::uintptr_t base, SymbolLookup lookup, Fns* out, const char** why) noexcept {
  Fns f{};
  const std::uintptr_t want = base + at::kEnableVaddr;
  void* const p = lookup != nullptr ? lookup(at::kEnableSymbol) : nullptr;
  if (p != nullptr && reinterpret_cast<std::uintptr_t>(p) != want) {
    *why = "EnableScenario name resolves elsewhere";
    return false;
  }
  if (p != nullptr) f.named = 1;
  bool ok = true;
  const auto first = SafeRead<std::uint32_t>(want, &ok);
  if (!ok || first != at::kEnableFirstWord) {
    *why = ok ? "first word is not sub sp,sp,#0x30" : "first word unreadable";
    return false;
  }
  f.enable = reinterpret_cast<void (*)(void*, int)>(want);
  *out = f;
  return true;
}

ModuleResult InstallResolved(std::uintptr_t base, const Fns& fns, const Switches& switches) noexcept {
  (void)base;
  g_f = fns;
  g_poll = switches.devBuild;
  g_scenario = kDefaultScenario;
  g_delayMs = kDefaultDelayMs;
  const char* reason = "ok";
  if (switches.scenario != nullptr) {
    const int s = ParseScenario(switches.scenario);
    if (s == -2) {
      if (!switches.scenarioChecked) WarnUnknownScenario();
    } else {
      g_scenario = s;
    }
  }
  if (g_poll) PollRequest();
  if (g_scenario < 0 && !g_poll) {
    g_phase.store(2, std::memory_order_release);
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_aibots_install",
                        {{"result", "off"}, {"reason", "scenario_off"}, {"scenario", g_scenario}, {"poll", 0},
                         {"msg", "combat.aibots_scenario is off: practice AI bots stand still as the game has them"}});
    return {false, "off_config", true};
  }
  if (!tick::Add(&Tick)) return Refused("tick_table_full", "");
  g_phase.store(1, std::memory_order_release);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_aibots_install",
                      {{"result", "installed"}, {"reason", reason}, {"scenario", g_scenario}, {"delay_ms", g_delayMs}, {"poll", g_poll ? 1 : 0},
                       {"msg", "EnableScenario is called once the bots of a combat match are in game; it installs no hook"}});
  return {true, "installed"};
}

ModuleResult InstallAt(sentinel::ImageLookup lookup, SymbolLookup symbols, const Switches& asked) noexcept {
  Switches switches = asked;
  WarnLegacyRequest(switches.devBuild);
  if (switches.on && switches.scenario != nullptr && ParseScenario(switches.scenario) == -2) {  // said once, before the library is looked at
    WarnUnknownScenario();
    switches.scenarioChecked = true;
  }
  if (!switches.on) {
    g_phase.store(2, std::memory_order_release);
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_aibots_install",
                        {{"result", "off"}, {"reason", "off_config"}, {"switch", "combat.aibots"},
                         {"msg", "combat.aibots is false: practice AI bots stand still as the game has them"}});
    return {false, "off_config", true};
  }
  // The call is made only when the combat level answer rules out an Echo Arena level: when that answer cannot change (the tick or the scope row's refresh
  // is not installed) every call would be refused, so the module is not installed (combat_scope.h `Live`). A scenario that is off in a release build is
  // the module off, decided in InstallResolved, and wins.
  const bool scenarioOff = !switches.devBuild && switches.scenario != nullptr && ParseScenario(switches.scenario) == -1;
  if (!scenarioOff && !scope::Live()) return Refused("scope_not_live", "");
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) return Refused("module_not_loaded", "");
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused("no_build_id", "");
  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return Refused("build_id_mismatch", "");
  const pinned::Pins lists[] = {at::kPins, netgame::kPins};
  pinned::Failure failure;
  if (pinned::FirstFailure(image.base, lists, sizeof(lists) / sizeof(lists[0]), &failure)) {
    char atText[19], haveText[19], wantText[19];
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_aibots_pinned_word",
                        {{"vaddr", sentinel::HexString(atText, failure.vaddr)}, {"have", failure.readable ? sentinel::HexString(haveText, failure.have) : "unreadable"},
                         {"want", sentinel::HexString(wantText, failure.want)}, {"what", failure.what}});
    return Refused("pinned_word_mismatch", failure.what);
  }
  Fns fns{};
  const char* why = "";
  if (!Resolve(image.base, symbols, &fns, &why)) return Refused("export_elsewhere", why);
  return InstallResolved(image.base, fns, switches);
}

ModuleResult Install(const Switches& switches) noexcept { return InstallAt(&sentinel::FindLoadedImage, &DlLookup, switches); }

void Tick(std::uintptr_t game) noexcept {
  if (g_phase.load(std::memory_order_acquire) != 1) return;
  const double now = Now();
  if (g_poll && now >= g_nextReq) {
    g_nextReq = now + kReqEveryS;
    PollRequest();
  }
  if (now < g_nextLook) return;
  g_nextLook = now + kLookEveryS;
  g_looks.fetch_add(1, std::memory_order_relaxed);
  Look(game, now);
}

int Phase() noexcept { return g_phase.load(std::memory_order_acquire); }

Stats CurrentStats() noexcept { return {g_looks.load(), g_enables.load(), g_scopedOut.load()}; }

void SetClockForTest(double (*clock)()) noexcept { g_clock = clock; }
void SetLegacyDirForTest(const char* dir) noexcept { legacy::SetDirectoryForTest(dir); }

void ResetForTest() noexcept {
  g_f = Fns{};
  g_phase.store(0);
  g_poll = false;
  g_scenario = kDefaultScenario;
  g_delayMs = kDefaultDelayMs;
  g_called = false;
  g_scopedSaid = false;
  g_lastCount = -1;
  g_calledCount = 0;
  g_countSince = g_nextLook = g_nextReq = 0;
  g_inSince = -1;
  g_idleLogged = false;
  g_reqSec = g_reqNsec = 0;
  g_looks.store(0);
  g_enables.store(0);
  g_scopedOut.store(0);
}

}  // namespace nevr_quest::combat::aibots
