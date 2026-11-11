// Host test of plugin.cpp's wiring against a fake game image that holds the pinned values of the words levelname and boosterpool hook. The plugin
// has no module left (levelname and boosterpool are the runtime's, src/quest/combat/, #2181): InitEx must name none, install none, leave those
// words as it found them (one owner per slot) and still return 0. Real code, real mprotect, real /proc/self/maps.
// Also the capability matrix: built with -DWIRING_CAPS_ONLY it only checks NevrPluginGetCapabilities against -DEXPECT_ALTERS / -DEXPECT_NETWORK.
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/plugin_interface.h"

extern "C" {
NEVR_PLUGIN_API int NevrPluginInitEx(const NevrGameContext*, const char*);
NEVR_PLUGIN_API uint32_t NevrPluginGetCapabilities(void);
}

static int g_fail = 0;
#define CHECK(cond, ...)                                 \
  do {                                                   \
    if (!(cond)) {                                       \
      ++g_fail;                                          \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__);  \
      std::printf(__VA_ARGS__);                          \
      std::printf("\n");                                 \
    }                                                    \
  } while (0)

#if defined(WIRING_NO_TICK)
// no module on: nothing registers a game tick, so InitEx installs none and leaves the tick slot as it found it; it registers the script libraries once
extern "C" __attribute__((noinline, visibility("default"))) void FakeExclusiveUpdateGame(void*) {}
extern "C" __attribute__((visibility("default"), noinline)) int32_t vrapi_SetClockLevels(void*, int32_t, int32_t) { return 0; }
extern "C" __attribute__((visibility("default"), noinline)) int32_t vrapi_SetDisplayRefreshRate(void*, float) { return 0; }
static std::vector<std::string> g_blines;
static void BLog(const char*, uint32_t, const char* msg, const NevrLogField* f, uint32_t n) {
  std::string t = msg;
  for (uint32_t i = 0; i < n; ++i) t += std::string(" ") + f[i].key + "=" + f[i].value;
  g_blines.push_back(t);
}
// The host's script redirect registration (#1737): InitEx asks for it once, on the pinned build only.
static int g_redirectCalls = 0;
static std::string g_redirectId;
static uint32_t g_redirectCount = 0;
static int32_t FakeRegisterScriptRedirect(const char* id, const uint64_t*, uint32_t count) {
  ++g_redirectCalls;
  g_redirectId = id != nullptr ? id : "(null)";
  g_redirectCount = count;
  return NEVR_SCRIPT_REDIRECT_OK;
}
int main() {
  const size_t kImage = 0x3a00000;
  void* p = mmap(nullptr, kImage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) std::abort();
  const uintptr_t base = reinterpret_cast<uintptr_t>(p);
  auto* w = static_cast<uintptr_t*>(p);
  w[2] = reinterpret_cast<uintptr_t>(&vrapi_SetClockLevels);
  w[3] = reinterpret_cast<uintptr_t>(&vrapi_SetDisplayRefreshRate);
  w[4] = reinterpret_cast<uintptr_t>(&FakeExclusiveUpdateGame);
  mprotect(p, kImage, PROT_READ);
  NevrGameContext ctx{};
  ctx.base_addr = base;
  ctx.flags = NEVR_HOST_IS_CLIENT | NEVR_HOST_PLATFORM_QUEST;
  ctx.ctx_size = sizeof(NevrGameContext);
  ctx.plugin_id = "q15t";
  ctx.log = &BLog;
  const NevrQuestHostApi quest{sizeof(NevrQuestHostApi), nullptr, &FakeRegisterScriptRedirect, nullptr, nullptr};
  ctx.quest = &quest;
  // Not the pinned build: InitEx refuses before it installs anything, so it registers no redirect either.
  ctx.game_build_id = "0000000000000000000000000000000000000000";
  CHECK(NevrPluginInitEx(&ctx, "") == 0, "InitEx must return 0 on a build it refuses");
  CHECK(g_redirectCalls == 0, "a refused build must not register the script redirect (calls=%d)", g_redirectCalls);
  ctx.game_build_id = Q15T_BUILD_ID_FOR_TEST;
  CHECK(NevrPluginInitEx(&ctx, "") == 0, "InitEx must return 0");
  CHECK(g_redirectCalls == 1 && g_redirectId == "q15t" && g_redirectCount == 9,
        "InitEx must register the nine script libraries once (calls=%d id=%s count=%u)", g_redirectCalls, g_redirectId.c_str(),
        g_redirectCount);
  for (const auto& l : g_blines) {
    CHECK(l.find("game tick callbacks") == std::string::npos && l.find("hook=CncaGame::ExclusiveUpdateGame") == std::string::npos, "a game tick record with no module on: %s", l.c_str());
  }
  CHECK(w[4] == reinterpret_cast<uintptr_t>(&FakeExclusiveUpdateGame), "the tick slot was written with no module on");
  if (g_fail == 0) std::printf("wiring_test (no tick): all checks pass (script libraries registered once, no game tick)\n");
  return g_fail == 0 ? 0 : 1;
}
#elif defined(WIRING_CAPS_ONLY)
extern "C" __attribute__((noinline, visibility("default"))) void FakeExclusiveUpdateGame(void*) {}  // host_overrides.h names it
int main() {
  const uint32_t caps = NevrPluginGetCapabilities();
  const bool gameplay = (caps & NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY) != 0, network = (caps & NEVR_PLUGIN_CAP_NETWORK) != 0;
  CHECK((caps & NEVR_PLUGIN_CAP_HOOKS_ENGINE) != 0, "caps 0x%x lack HOOKS_ENGINE", caps);
  CHECK(gameplay == (EXPECT_ALTERS != 0), "caps 0x%x: ALTERS_GAMEPLAY is %d, want %d", caps, gameplay, EXPECT_ALTERS);
  CHECK(network == (EXPECT_NETWORK != 0), "caps 0x%x: NETWORK is %d, want %d", caps, network, EXPECT_NETWORK);
  std::printf("wiring caps: 0x%x gameplay=%d network=%d (%s)\n", caps, gameplay, network, WIRING_LABEL);
  return g_fail == 0 ? 0 : 1;
}
#else

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
static int CountLines(const std::string& needle, int level = -1) {
  int n = 0;
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || (int)l.level == level)) ++n;
  return n;
}
static std::string FindLine(const std::string& needle) {
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos) return l.text;
  return "";
}

static uintptr_t w_slot(uintptr_t base, uintptr_t vaddr) { return *reinterpret_cast<const uintptr_t*>(base + vaddr); }

static int g_real_calls = 0;
extern "C" __attribute__((visibility("default"), noinline)) int32_t vrapi_SetClockLevels(void*, int32_t, int32_t) {
  ++g_real_calls;
  return 0;
}
extern "C" __attribute__((visibility("default"), noinline)) int32_t vrapi_SetDisplayRefreshRate(void*, float) { return 0; }
extern "C" __attribute__((noinline, visibility("default"))) void FakeExclusiveUpdateGame(void*) {}

int main() {
  // The image: a sparse anonymous mapping whose offsets are the pinned build's ELF vaddrs (levelname's four vtable words live at
  // 0x362ef90.. and hold base + 0x1202ecc), so the production addresses are used as they are. Everything else reads zero.
  const size_t kImage = 0x3a00000;
  void* p = mmap(nullptr, kImage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) std::abort();
  const uintptr_t base = reinterpret_cast<uintptr_t>(p);
  auto* w = static_cast<uintptr_t*>(p);
  w[2] = reinterpret_cast<uintptr_t>(&vrapi_SetClockLevels);       // 0x10
  w[3] = reinterpret_cast<uintptr_t>(&vrapi_SetDisplayRefreshRate);  // 0x18
  w[4] = reinterpret_cast<uintptr_t>(&FakeExclusiveUpdateGame);    // 0x20
  const uintptr_t kLevelNameSlots[4] = {0x362ef90, 0x3630e58, 0x3631278, 0x36316f8};
  for (uintptr_t s : kLevelNameSlots) *reinterpret_cast<uintptr_t*>(base + s) = base + 0x1202ecc;
  // boosterpool: the GOT word of the AllocatePooledActor import holds its pinned function (ELF 0x18037c4)
  *reinterpret_cast<uintptr_t*>(base + 0x36cd1f8) = base + 0x18037c4;
  mprotect(p, kImage, PROT_READ);

  NevrGameContext ctx{};
  ctx.base_addr = base;
  ctx.flags = NEVR_HOST_IS_CLIENT | NEVR_HOST_PLATFORM_QUEST;
  ctx.ctx_size = sizeof(NevrGameContext);
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  ctx.game_build_id = Q15T_BUILD_ID_FOR_TEST;

  const uint32_t caps = NevrPluginGetCapabilities();
  CHECK((caps & NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY) == 0, "caps 0x%x: ALTERS_GAMEPLAY set, and no module of the plugin changes the game", caps);
  CHECK((caps & NEVR_PLUGIN_CAP_NETWORK) == 0, "caps 0x%x: NETWORK set, and no module of the plugin sends (the EC client is the runtime's)", caps);

  const int rc = NevrPluginInitEx(&ctx, "");
  CHECK(rc == 0, "InitEx returned %d; the loader would dlclose the plugin", rc);
  // the runtime owns the clock levels and the refresh rate (#2000): the plugin leaves both slots alone
  CHECK(w[2] == reinterpret_cast<uintptr_t>(&vrapi_SetClockLevels), "the plugin wrote the clock slot");
  CHECK(w[3] == reinterpret_cast<uintptr_t>(&vrapi_SetDisplayRefreshRate), "the plugin wrote the refresh-rate slot");
  // POWER BOOST is the runtime's (src/quest/combat/powerboost.h): the plugin leaves its five words alone and names no powerboost module
  for (uintptr_t s : {uintptr_t{0x36e3868}, uintptr_t{0x36f7820}, uintptr_t{0x3725b80}, uintptr_t{0x36d7d08}, uintptr_t{0x36ca098}})
    CHECK(w_slot(base, s) == 0, "the plugin wrote POWER BOOST's word at %#lx", static_cast<unsigned long>(s));
  CHECK(CountLines("powerboost") == 0, "the plugin logs a powerboost line: %s", FindLine("powerboost").c_str());
  CHECK(CountLines("vrapi_SetClockLevels") == 0 && CountLines("vrapi_SetDisplayRefreshRate") == 0, "the plugin names a vrapi slot it does not install");

  // levelname and boosterpool are the runtime's (#2181): the init record names no module, the plugin writes no module record, and the five
  // words both hook keep the game's values, so on a q15t-variant APK the runtime's combat rows are their one owner.
  const std::string init = FindLine("q15t init");
  CHECK(!init.empty() && init.find("modules= ") == std::string::npos && init.substr(init.size() - 8) == "modules=", "q15t init names a module: %s", init.c_str());
  CHECK(CountLines("module install") == 0, "the plugin wrote a module record: %s", FindLine("module install").c_str());
  CHECK(CountLines("levelname") == 0 && CountLines("boosterpool") == 0, "the plugin names levelname or boosterpool: %s%s",
        FindLine("levelname").c_str(), FindLine("boosterpool").c_str());
  for (uintptr_t s : kLevelNameSlots)
    CHECK(w_slot(base, s) == base + 0x1202ecc, "the plugin wrote levelname's word at %#lx", static_cast<unsigned long>(s));
  CHECK(w_slot(base, 0x36cd1f8) == base + 0x18037c4, "the plugin wrote boosterpool's GOT slot");
  // no module of the plugin registers a game tick: the plugin installs none and writes no tick record, and the tick slot is the one the host passed.
  CHECK(CountLines("game tick callbacks") == 0, "the plugin logged a tick table: %s", FindLine("game tick callbacks").c_str());
  CHECK(CountLines("hook=CncaGame::ExclusiveUpdateGame") == 0, "the plugin installed the game tick: %s", FindLine("hook=CncaGame::ExclusiveUpdateGame").c_str());
  CHECK(w[4] == reinterpret_cast<uintptr_t>(&FakeExclusiveUpdateGame), "the tick slot was written");

  if (g_fail == 0) std::printf("wiring_test: all checks pass (no module named or installed, levelname's and boosterpool's words untouched, InitEx returned 0)\n");
  else {
    for (const auto& l : g_lines) std::printf("  [%u] %s\n", l.level, l.text.c_str());
    std::printf("wiring_test: %d failure(s)\n", g_fail);
  }
  return g_fail == 0 ? 0 : 1;
}
#endif
