// Host test of the plugin's entry (NevrPluginInitEx) against a fake "game library" page (an mmap'd page whose words stand for the JUMP_SLOTs).
// Built by src/quest/tests/q15t/CMakeLists.txt. Real code, real mprotect, real /proc/self/maps.
//
// What it pins (#2172, #2000): the plugin installs nothing on `vrapi_SetClockLevels` and `vrapi_SetDisplayRefreshRate`. The runtime owns both
// behaviours (the refresh-rate rule in src/quest/combat/refresh_rate.h, the clock levels in src/quest/sentinel/clock_levels_hook.h), and the
// slots hold the runtime's thunks by the time the plugin runs, so a plugin install there would be refused (`unexpected_original`); the plugin
// does not try. The test fills both words and shows them untouched after a full init, in the pinned build and in a refused one, and that
// an `args: {clock: raise}` the plugin no longer reads is not read.
#include <dlfcn.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "got_swap.h"
#include "nevr/plugin_interface.h"

extern "C" {
NEVR_PLUGIN_API int NevrPluginInitEx(const NevrGameContext*, const char*);
NEVR_PLUGIN_API uint32_t NevrPluginGetCapabilities(void);
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

// The two words the plugin used to wrap, at the byte offsets the old host test used (0x10, 0x18), and the tick word (0x20).
constexpr uintptr_t kClockSlot = 0x10;
constexpr uintptr_t kRefreshSlot = 0x18;
constexpr uintptr_t kTickSlot = 0x20;

extern "C" __attribute__((visibility("default"), noinline)) int32_t vrapi_SetClockLevels(void*, int32_t, int32_t) { return 0; }
extern "C" __attribute__((visibility("default"), noinline)) int32_t vrapi_SetDisplayRefreshRate(void*, float) { return 0; }
extern "C" __attribute__((noinline, visibility("default"))) void FakeExclusiveUpdateGame(void*) {}

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

static NevrGameContext MakeCtx(uintptr_t base, const char* build_id) {
  NevrGameContext c{};
  c.base_addr = base;
  c.flags = NEVR_HOST_IS_CLIENT | NEVR_HOST_PLATFORM_QUEST;
  c.ctx_size = sizeof(NevrGameContext);
  c.plugin_id = "q15t";
  c.log = &FakeLog;
  c.game_build_id = build_id;
  return c;
}

static uintptr_t* NewSlotPage(int prot_after) {
  void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) std::abort();
  auto* w = static_cast<uintptr_t*>(p);
  w[kClockSlot / sizeof(uintptr_t)] = reinterpret_cast<uintptr_t>(&vrapi_SetClockLevels);
  w[kRefreshSlot / sizeof(uintptr_t)] = reinterpret_cast<uintptr_t>(&vrapi_SetDisplayRefreshRate);
  w[kTickSlot / sizeof(uintptr_t)] = reinterpret_cast<uintptr_t>(&FakeExclusiveUpdateGame);
  mprotect(p, 4096, prot_after);
  return w;
}

static bool VrapiSlotsUntouched(const uintptr_t* page) {
  return page[kClockSlot / sizeof(uintptr_t)] == reinterpret_cast<uintptr_t>(&vrapi_SetClockLevels) &&
         page[kRefreshSlot / sizeof(uintptr_t)] == reinterpret_cast<uintptr_t>(&vrapi_SetDisplayRefreshRate);
}

int main() {
  // 1. the pinned build: a full init installs nothing on the two vrapi words and says nothing about them.
  {
    g_lines.clear();
    uintptr_t* page = NewSlotPage(PROT_READ);
    NevrGameContext ctx = MakeCtx(reinterpret_cast<uintptr_t>(page), kId);
    CHECK(NevrPluginInitEx(&ctx, "") == 0, "InitEx must return 0");
    CHECK(VrapiSlotsUntouched(page), "the plugin wrote vrapi_SetClockLevels or vrapi_SetDisplayRefreshRate: the runtime owns both");
    CHECK(!HasLine("vrapi_SetClockLevels") && !HasLine("vrapi_SetDisplayRefreshRate"), "the plugin names a vrapi slot it does not install");
    CHECK(!HasLine("clock_candidate") && !HasLine("hz_mode") && !HasLine("unexpected_original"), "a clock or refresh-rate field, or a refusal, on the init line");
    CHECK(HasLine("q15t init", 1) && HasLine("game_build_id="), "no init line");
    q15t::Mapping m;
    CHECK(q15t::FindMapping(reinterpret_cast<uintptr_t>(page), &m) && std::strncmp(m.perms, "r--", 3) == 0,
          "page protection changed (perms %s)", m.perms);
    CHECK(!HasLine("CncaGame::ExclusiveUpdateGame"), "no module ticks here: the game tick must not be installed");
    CHECK(page[kTickSlot / sizeof(uintptr_t)] == reinterpret_cast<uintptr_t>(&FakeExclusiveUpdateGame), "tick slot swapped with no module to tick");
  }
  // 2. an `args: {clock: raise}` that older builds read: not read, not warned about, nothing installed.
  for (const char* args : {"{\"clock\":\"raise\"}", "{\"clock\":\"bogus\"}", "{}", "not json"}) {
    g_lines.clear();
    uintptr_t* page = NewSlotPage(PROT_READ);
    NevrGameContext ctx = MakeCtx(reinterpret_cast<uintptr_t>(page), kId);
    CHECK(NevrPluginInitEx(&ctx, args) == 0, "InitEx failed for args %s", args);
    CHECK(VrapiSlotsUntouched(page), "args %s made the plugin write a vrapi slot", args);
    CHECK(!HasLine("unknown clock arg") && !HasLine("clock"), "args %s: a clock line", args);
  }
  // 3. wrong build id: nothing installed.
  {
    g_lines.clear();
    uintptr_t* page = NewSlotPage(PROT_READ);
    NevrGameContext ctx = MakeCtx(reinterpret_cast<uintptr_t>(page), "0000000000000000000000000000000000000000");
    CHECK(NevrPluginInitEx(&ctx, "") == 0, "InitEx must return 0 on a refused build id (loaded, nothing installed)");
    CHECK(VrapiSlotsUntouched(page), "slot touched on wrong build");
    CHECK(HasLine("refused", 2), "no refusal line");
  }
  // 4. no context: refused before anything is touched.
  CHECK(NevrPluginInitEx(nullptr, "") != 0, "a null context must be refused");
  // 5. the capability flags (no module option is on here): no ALTERS_GAMEPLAY.
  {
    const uint32_t caps = NevrPluginGetCapabilities();
    const bool gameplay = (caps & NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY) != 0;
    CHECK(!gameplay, "caps 0x%x: ALTERS_GAMEPLAY is %d with no module on", caps, gameplay);
    CHECK((caps & NEVR_PLUGIN_CAP_HOOKS_ENGINE) != 0 && (caps & NEVR_PLUGIN_CAP_NETWORK) == 0, "caps 0x%x", caps);
  }
  if (g_fail == 0) {
    std::printf("host_test: all checks pass\n");
  } else {
    std::printf("host_test: %d failure(s)\n", g_fail);
  }
  return g_fail == 0 ? 0 : 1;
}
