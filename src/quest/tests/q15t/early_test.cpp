// Host test of the early plugin (src/quest/plugins/q15t/src/plugin_early.cpp) against the same fake game library the heaps test uses: one anonymous mapping
// with the four rodata sizes, the four GLOB_DAT words (read-only, like RELRO) and the four reader instructions at their pinned vaddrs.
// The entry points are the real ones; the context is what the loader hands an early plugin.
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/plugin_interface.h"
#include "heaps.h"
#include "memsys.h"
#include "module_switch.h"
#include "plugin_log.h"

extern "C" {
NEVR_PLUGIN_API NvrPluginInfo NvrPluginGetInfo(void);
NEVR_PLUGIN_API uint32_t NvrPluginGetCapabilities(void);
NEVR_PLUGIN_API int NvrPluginInitEx(const NvrGameContext*, const char*);
}

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
static int Count(const char* needle, int level = -1) {
  int c = 0;
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || (int)l.level == level)) ++c;
  return c;
}

static const uint64_t kMiB = 1ull << 20;
struct Expect {
  uintptr_t got, ro, reader;
  uint32_t reader_word;
  uint64_t stock, target;
};
static const Expect kExpect[4] = {
    {0x370b720, 0x2b9caa0, 0xf78590, 0xf9400101, 206 * kMiB, 320 * kMiB},
    {0x371d020, 0x2b9ca90, 0xf78524, 0xf9400101, 400 * kMiB, 640 * kMiB},
    {0x3710188, 0x2b9ca98, 0xf78614, 0xf9400101, 160 * kMiB, 512 * kMiB},
    {0x3715198, 0x2b9e1c0, 0x11ffdec, 0xf9400100, 40 * kMiB, 128 * kMiB},
};
static const size_t kLibSize = 0x3800000;

static uintptr_t NewLib() {
  void* p = mmap(nullptr, kLibSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) std::abort();
  const uintptr_t base = reinterpret_cast<uintptr_t>(p);
  for (const Expect& e : kExpect) {
    *reinterpret_cast<uint64_t*>(base + e.ro) = e.stock;
    *reinterpret_cast<uintptr_t*>(base + e.got) = base + e.ro;
    *reinterpret_cast<uint32_t*>(base + e.reader) = e.reader_word;
  }
  for (const Expect& e : kExpect)
    for (uintptr_t a : {e.got, e.ro, e.reader}) mprotect(reinterpret_cast<void*>((base + a) & ~uintptr_t(4095)), 4096, PROT_READ);
  return base;
}
static void Drop(uintptr_t base) { munmap(reinterpret_cast<void*>(base), kLibSize); }
static uint64_t GameRead(uintptr_t base, int i) {  // what the game does: ldr x8,[GOT]; ldr x1,[x8]
  return *reinterpret_cast<uint64_t*>(*reinterpret_cast<uintptr_t*>(base + kExpect[i].got));
}
static bool Untouched(uintptr_t base) {
  for (int i = 0; i < 4; ++i)
    if (*reinterpret_cast<uintptr_t*>(base + kExpect[i].got) != base + kExpect[i].ro) return false;
  return true;
}

static const char* kId = "b243509c08ce677aeb95fa348016949b3fc45230";
static NvrGameContext MakeCtx(uintptr_t base, const char* id, bool quest = true) {
  NvrGameContext c{};
  c.base_addr = base;
  c.flags = NEVR_HOST_IS_CLIENT | (quest ? static_cast<uint32_t>(NEVR_HOST_PLATFORM_QUEST) : 0u);
  c.ctx_size = sizeof(NvrGameContext);
  c.plugin_id = "q15t_early";
  c.log = &FakeLog;
  c.game_build_id = id;
  return c;
}

int main() {
  char tmpl[] = "/tmp/q15t-early-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  q15t::SetModuleSwitchDirForTest(tmpl);

  // 0. what the loader reads before it calls the init
  const NvrPluginInfo info = NvrPluginGetInfo();
  CHECK(info.name != nullptr && std::strcmp(info.name, "q15t_early") == 0, "plugin name %s: it must differ from the main plugin's", info.name);
  CHECK(NvrPluginGetCapabilities() == NEVR_PLUGIN_CAP_HOOKS_ENGINE, "caps 0x%x: only HOOKS_ENGINE (sizes are not gameplay)", NvrPluginGetCapabilities());
  CHECK(NvrPluginInitEx(nullptr, "{}") != 0, "a null context must be refused");

  // 1. the pinned build on a Quest, the library base known: heaps swapped all four rows, memsys refused on this image (its code words
  // are not there) with its own ERROR record, and the init returns 0 either way
  {
    g_lines.clear();
    q15t::ResetHeapsForTest();
    q15t::SetHeapsDeviceForTest("eureka");
    q15t::ResetMemsysForTest();
    const uintptr_t base = NewLib();
    NvrGameContext ctx = MakeCtx(base, kId);
    CHECK(NvrPluginInitEx(&ctx, "{}") == 0, "InitEx must return 0 with hooks swapped");
    for (int i = 0; i < 4; ++i)
      CHECK(GameRead(base, i) == kExpect[i].target, "row %d: the game reads %llu MiB, want %llu", i,
            (unsigned long long)(GameRead(base, i) >> 20), (unsigned long long)(kExpect[i].target >> 20));
    CHECK(Count("q15t_early init", 1) == 1, "no init record at INFO");
    CHECK(Count("module install module=heaps status=ok", 1) == 1, "no heaps record at INFO");
    CHECK(Count("module install FAILED module=memsys ", 3) == 1, "memsys: one ERROR record expected");
    CHECK(Count("module install module=memsys") == 0, "memsys must not also report success");
    Drop(base);
  }
  // 2. another game build: nothing touched, one WARNING, the plugin stays loaded
  {
    g_lines.clear();
    q15t::ResetHeapsForTest();
    const uintptr_t base = NewLib();
    NvrGameContext ctx = MakeCtx(base, "0000000000000000000000000000000000000000");
    CHECK(NvrPluginInitEx(&ctx, "{}") == 0, "a refused build must still return 0");
    CHECK(Untouched(base), "a slot was swapped on the wrong build");
    CHECK(Count("refused: not the pinned game build", 2) == 1, "no WARNING for the refusal");
    CHECK(Count("module install") == 0, "a module ran on the wrong build");
    Drop(base);
  }
  // 3. no game build id yet / not a Quest / base not known: the same refusal
  {
    g_lines.clear();
    q15t::ResetHeapsForTest();
    const uintptr_t base = NewLib();
    NvrGameContext none = MakeCtx(base, nullptr);
    CHECK(NvrPluginInitEx(&none, "{}") == 0 && Untouched(base), "no build id: must refuse and touch nothing");
    NvrGameContext pc = MakeCtx(base, kId, /*quest=*/false);
    CHECK(NvrPluginInitEx(&pc, "{}") == 0 && Untouched(base), "not a Quest: must refuse and touch nothing");
    NvrGameContext nobase = MakeCtx(0, kId);
    CHECK(NvrPluginInitEx(&nobase, "{}") == 0, "base 0: must return 0");
    CHECK(Untouched(base), "base 0: a slot of another library was touched");
    CHECK(Count("refused: not the pinned game build", 2) == 3, "three refusals expected, got %d", Count("refused: not the pinned game build", 2));
    CHECK(Count("module install") == 0, "a module ran although the context was refused");
    Drop(base);
  }
  // 4. a context too small to carry the log and the build id (an old host): refused, no crash
  {
    g_lines.clear();
    NvrGameContext small = MakeCtx(0x1000, kId);
    small.ctx_size = offsetof(NvrGameContext, flags) + sizeof(small.flags);
    CHECK(NvrPluginInitEx(&small, "{}") == 0, "a short context must return 0");
    CHECK(Count("module install") == 0, "a module ran on a short context");
  }
  if (g_fail == 0) std::printf("early_test: all checks passed\n");
  else std::printf("early_test: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
