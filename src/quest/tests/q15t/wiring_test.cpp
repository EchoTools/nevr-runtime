// Host test of plugin.cpp's module wiring (every module option ON, Q15T_POWERBOOST=3, Q15T_SHIELDGAMMA=2) against a fake game image in
// which only the clock/refresh/tick slots and levelname's four vtable words are the pinned ones: every other module must refuse, loudly
// and by name, and InitEx must still return 0 with hooks swapped. Real code, real mprotect, real /proc/self/maps.
// Also the capability matrix: built with -DWIRING_CAPS_ONLY it only checks NvrPluginGetCapabilities against -DEXPECT_ALTERS / -DEXPECT_NETWORK.
#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bvh.h"
#include "nevr/plugin_interface.h"
#include "fxtrail.h"
#include "wallfire.h"

extern "C" {
NEVR_PLUGIN_API int NvrPluginInitEx(const NvrGameContext*, const char*);
NEVR_PLUGIN_API uint32_t NvrPluginGetCapabilities(void);
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

#if defined(WIRING_BVH_ONLY)
// only Q15T_BVH on (no refresh tick, no T-26): the bvh tick alone must register and make InitEx install the game tick
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
  const q15t::BvhPins& bp = q15t::kBvhPinned;
  for (const q15t::BvhHookSlot* h : {&bp.init, &bp.shutdown, &bp.intersect, &bp.occluded, &bp.touch, &bp.gate})
    *reinterpret_cast<uintptr_t*>(base + h->slot) = base + h->fn;
  mprotect(p, kImage, PROT_READ);
  NvrGameContext ctx{};
  ctx.base_addr = base;
  ctx.flags = NEVR_HOST_IS_CLIENT | NEVR_HOST_PLATFORM_QUEST;
  ctx.ctx_size = sizeof(NvrGameContext);
  ctx.plugin_id = "q15t";
  ctx.log = &BLog;
  const NevrQuestHostApi quest{sizeof(NevrQuestHostApi), nullptr, &FakeRegisterScriptRedirect};
  ctx.quest = &quest;
  // Not the pinned build: InitEx refuses before it installs anything, so it registers no redirect either.
  ctx.game_build_id = "0000000000000000000000000000000000000000";
  CHECK(NvrPluginInitEx(&ctx, "") == 0, "InitEx must return 0 on a build it refuses");
  CHECK(g_redirectCalls == 0, "a refused build must not register the script redirect (calls=%d)", g_redirectCalls);
  ctx.game_build_id = Q15T_BUILD_ID_FOR_TEST;
  CHECK(NvrPluginInitEx(&ctx, "") == 0, "InitEx must return 0");
  CHECK(g_redirectCalls == 1 && g_redirectId == "q15t" && g_redirectCount == 9,
        "InitEx must register the nine script libraries once (calls=%d id=%s count=%u)", g_redirectCalls, g_redirectId.c_str(),
        g_redirectCount);
  bool ticks = false, tick_hook = false;
  for (const auto& l : g_blines) {
    if (l.find("game tick callbacks registered=1 max=16") != std::string::npos) ticks = true;
    if (l.find("hook=CncaGame::ExclusiveUpdateGame (game-thread tick) status=ok") != std::string::npos) tick_hook = true;
  }
  CHECK(ticks && tick_hook, "bvh alone must register one tick and install the game tick (ticks=%d hook=%d)", ticks, tick_hook);
  CHECK(w[4] != reinterpret_cast<uintptr_t>(&FakeExclusiveUpdateGame), "the tick slot was not swapped");
  if (g_fail == 0) std::printf("wiring_test (bvh only): all checks pass (one tick registered, game tick installed)\n");
  return g_fail == 0 ? 0 : 1;
}
#elif defined(WIRING_GATE_LINK_ONLY)
// only the headset caller-range gates on (Q15T_GATE_LINK, and Q15T_GATE_BOTS when EXPECT_BOTS): InitEx must install the one
// CVR::HeadsetType wrapper with the pinned ranges and the link answer (kind 4); a gate that is off gets an empty range.
#include "headset_gates.h"
#include "module_switch.h"
extern "C" __attribute__((noinline, visibility("default"))) void FakeExclusiveUpdateGame(void*) {}
extern "C" __attribute__((visibility("default"), noinline)) int32_t vrapi_SetClockLevels(void*, int32_t, int32_t) { return 0; }
extern "C" __attribute__((visibility("default"), noinline)) int32_t vrapi_SetDisplayRefreshRate(void*, float) { return 0; }
static void GLog(const char*, uint32_t, const char*, const NevrLogField*, uint32_t) {}
int main() {
  const size_t kImage = 0x3a00000;
  void* p = mmap(nullptr, kImage, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (p == MAP_FAILED) std::abort();
  const uintptr_t base = reinterpret_cast<uintptr_t>(p);
  auto* w = static_cast<uintptr_t*>(p);
  w[2] = reinterpret_cast<uintptr_t>(&vrapi_SetClockLevels);
  w[3] = reinterpret_cast<uintptr_t>(&vrapi_SetDisplayRefreshRate);
  w[4] = reinterpret_cast<uintptr_t>(&FakeExclusiveUpdateGame);
  *reinterpret_cast<uintptr_t*>(base + 0x36f9fd8) = base + 0x1a6f164;  // the CVR::HeadsetType slot holds the pinned function
  mprotect(p, kImage, PROT_READ);
  NvrGameContext ctx{};
  ctx.base_addr = base;
  ctx.flags = NEVR_HOST_IS_CLIENT | NEVR_HOST_PLATFORM_QUEST;
  ctx.ctx_size = sizeof(NvrGameContext);
  ctx.plugin_id = "q15t";
  ctx.log = &GLog;
  ctx.game_build_id = Q15T_BUILD_ID_FOR_TEST;
#ifdef WIRING_LEVELNAME_OFF
  // levelname.off in the data directory: the bot-body gate must follow levelname off (combat_gates.h, needs_levelname)
  char dir[] = "/tmp/q15t_wiring_ln_XXXXXX";
  if (mkdtemp(dir) == nullptr) std::abort();
  if (FILE* f = std::fopen((std::string(dir) + "/levelname.off").c_str(), "w")) std::fclose(f);
  q15t::SetModuleSwitchDirForTest(dir);
#endif
  CHECK(NvrPluginInitEx(&ctx, "") == 0, "InitEx must return 0");
  CHECK(*reinterpret_cast<uintptr_t*>(base + 0x36f9fd8) != base + 0x1a6f164, "the CVR::HeadsetType slot was not swapped");
  const q15t::HeadsetGateRanges r = q15t::GetHeadsetGateRangesForTest();
  CHECK(r.link_lo == base + 0x202ba74 && r.link_hi == base + 0x202bac8, "link range %llx..%llx, want the Bind (ELF 0x202ba74..0x202bac8)",
        (unsigned long long)(r.link_lo - base), (unsigned long long)(r.link_hi - base));
  CHECK(r.link_answer == 4, "link answer %d: want the link kind (4), a kind the Bind has a case for and not the Quest kind (3)", r.link_answer);
#if defined(EXPECT_BOTS) && !defined(WIRING_LEVELNAME_OFF)
  CHECK(r.bots_lo == base + 0x124dd38 && r.bots_hi == base + 0x124e004, "bots range %llx..%llx", (unsigned long long)(r.bots_lo - base),
        (unsigned long long)(r.bots_hi - base));
#else
  CHECK(r.bots_lo == r.bots_hi, "bots gate is off: its range must be empty, got %llx..%llx", (unsigned long long)(r.bots_lo - base),
        (unsigned long long)(r.bots_hi - base));
#endif
  if (g_fail == 0) std::printf("wiring_test (gate link%s): all checks pass\n",
#ifdef EXPECT_BOTS
                               " + bots"
#else
                               " only"
#endif
  );
  return g_fail == 0 ? 0 : 1;
}
#elif defined(WIRING_CAPS_ONLY)
extern "C" __attribute__((noinline, visibility("default"))) void FakeExclusiveUpdateGame(void*) {}  // host_overrides.h names it
int main() {
  const uint32_t caps = NvrPluginGetCapabilities();
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
  // fxtrail: its eleven code words are the pinned ones, so its off switch (set below) is what stops it
  for (const auto& pin : q15t::kFxTrailWordPins) *reinterpret_cast<uint32_t*>(base + pin.va) = pin.want;
  // bvh: its six slots hold their pinned functions. wallfire (installed after it): the query slot, the occluded slot, the site word.
  const q15t::BvhPins& bp = q15t::kBvhPinned;
  for (const q15t::BvhHookSlot* h : {&bp.init, &bp.shutdown, &bp.intersect, &bp.occluded, &bp.touch, &bp.gate})
    *reinterpret_cast<uintptr_t*>(base + h->slot) = base + h->fn;
  const q15t::WallfirePins& wp = q15t::kWallfirePinned;
  *reinterpret_cast<uintptr_t*>(base + wp.query_slot) = base + wp.query_fn;
  *reinterpret_cast<uintptr_t*>(base + wp.occluded_slot) = base + wp.occluded_fn;
  *reinterpret_cast<uint32_t*>(base + wp.site) = wp.site_word;
  mprotect(p, kImage, PROT_READ);

  // fxtrail's own off switch (fxtrail.off in the data directory): a choice, so INFO status=off, not an ERROR
  char dir[] = "/tmp/q15t_wiring_XXXXXX";
  if (mkdtemp(dir) == nullptr) std::abort();
  const std::string off_file = std::string(dir) + "/fxtrail.off";
  if (FILE* f = std::fopen(off_file.c_str(), "w")) std::fclose(f);
  q15t::SetFxTrailDirForTest(dir);

  NvrGameContext ctx{};
  ctx.base_addr = base;
  ctx.flags = NEVR_HOST_IS_CLIENT | NEVR_HOST_PLATFORM_QUEST;
  ctx.ctx_size = sizeof(NvrGameContext);
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  ctx.game_build_id = Q15T_BUILD_ID_FOR_TEST;

  const uint32_t caps = NvrPluginGetCapabilities();
  CHECK((caps & NEVR_PLUGIN_CAP_ALTERS_GAMEPLAY) != 0, "caps 0x%x: ALTERS_GAMEPLAY missing with every module on", caps);
  CHECK((caps & NEVR_PLUGIN_CAP_NETWORK) != 0, "caps 0x%x: NETWORK missing with ec on", caps);

  const int rc = NvrPluginInitEx(&ctx, "");
  CHECK(rc == 0, "InitEx returned %d with hooks swapped and modules refusing; the loader would dlclose a live plugin", rc);
  const uintptr_t clock_now = w[2];
  CHECK(clock_now != reinterpret_cast<uintptr_t>(&vrapi_SetClockLevels), "the clock slot was not swapped: the run proves nothing");

  // every compiled module is named in the init record
  const char* kAll[] = {"fxtrail",  "voipdrop",   "custctx",  "droneguard", "peerclamp",  "gunconnect", "aibots",
                        "bvh",      "wallfire",   "impactsnd", "ec",         "ecfx",       "dronekill",  "dronemuzzle",
                        "powerboost", "hudmeter", "shieldgamma", "levelname", "boosterpool", "music", "sensorwd"};
  const std::string init = FindLine("q15t init");
  for (const char* m : kAll) {
    CHECK(init.find(std::string(m) + " ") != std::string::npos, "q15t init line does not list module %s: %s", m, init.c_str());
    const int ok = CountLines(std::string("module install module=") + m + " ");
    const int bad = CountLines(std::string("module install FAILED module=") + m + " ");
    CHECK(ok + bad == 1, "module %s: %d install lines + %d FAILED lines, want exactly one record", m, ok, bad);
  }

  // what must have installed, been switched off, or refused, on this image
  const char* kInstalled[] = {"bvh", "wallfire", "levelname", "boosterpool"};
  const char* kArmed[] = {"gunconnect", "aibots"};
  const char* kOff[] = {"ec", "ecfx", "fxtrail"};
  struct Refusal {
    const char* module;
    const char* reason;  // the reason token the record must carry
  };
  const Refusal kRefused[] = {{"voipdrop", "word_pin_mismatch"}, {"custctx", "word_pin_mismatch"},  {"droneguard", "word_pin_mismatch"},
                              {"peerclamp", "word_pin_mismatch"}, {"music", "word_pin_mismatch"}, {"sensorwd", "word_pin_mismatch"}, {"impactsnd", "hit_code_word_mismatch"}, {"dronekill", "engine_export_missing"},
                              {"dronemuzzle", "needs_dronekill_shot"}, {"powerboost", "hook_failed"}, {"hudmeter", "unexpected_original"},
                              {"shieldgamma", "unexpected_original"}};
  for (const char* m : kInstalled)
    CHECK(CountLines(std::string("module install module=") + m + " status=ok", 1) == 1, "module %s did not report status=ok at INFO", m);
  for (const char* m : kArmed)
    CHECK(CountLines(std::string("module install module=") + m + " status=tick_registered", 1) == 1, "module %s: no tick_registered INFO", m);
  for (const char* m : kOff)
    CHECK(CountLines(std::string("module install module=") + m + " status=off", 1) == 1, "module %s: a deliberate off must be INFO status=off", m);
  for (const Refusal& r : kRefused) {
    const std::string line = FindLine(std::string("module install FAILED module=") + r.module + " ");
    CHECK(CountLines(std::string("module install FAILED module=") + r.module + " ", 3) == 1, "module %s: its refusal is not an ERROR line", r.module);
    CHECK(line.find(std::string(" reason=") + r.reason + " real_resolved=") != std::string::npos, "module %s: want reason %s: %s", r.module, r.reason,
          line.c_str());
    CHECK(line.find(" status=") != std::string::npos, "module %s: ERROR line has no status: %s", r.module, line.c_str());
  }
  // shieldgamma is built with the bitmask 2: the tube mirror on, the gamma off
  CHECK(FindLine("shieldgamma: OFF").find("gamma=0 tube_mirror=1") != std::string::npos, "shieldgamma bitmask 2 must mean mirror only: %s",
        FindLine("shieldgamma: OFF").c_str());
  // what the installs did to the image
  CHECK(w_slot(base, q15t::kFxTrailSlot) == 0, "fxtrail was off: its slot must be untouched");
  CHECK(w_slot(base, bp.init.slot) != base + bp.init.fn && w_slot(base, bp.gate.slot) != base + bp.gate.fn, "bvh: its slots were not swapped");
  CHECK(w_slot(base, wp.query_slot) != base + wp.query_fn, "wallfire (after bvh) did not swap its slot");
  CHECK(w_slot(base, 0x362ef90) != base + 0x1202ecc, "levelname: its slot was not swapped");
  CHECK(w_slot(base, 0x36cd1f8) != base + 0x18037c4, "boosterpool: its GOT slot was not swapped");
  CHECK(CountLines("wallfire on:") == 1 && CountLines("bvh on:") == 1, "bvh and wallfire did not both log 'on'");
  // order: wallfire is installed after bvh (it refuses as bvh_not_installed otherwise), and the tick comes after every module
  size_t at_bvh = 0, at_wall = 0, at_tick = 0, at_ec = 0, at_mod_last = 0;
  for (size_t i = 0; i < g_lines.size(); ++i) {
    const std::string& t = g_lines[i].text;
    if (t.find("module install module=bvh ") != std::string::npos) at_bvh = i;
    if (t.find("module install module=wallfire ") != std::string::npos) at_wall = i;
    if (t.find("module install module=ec ") != std::string::npos) at_ec = i;
    if (t.find("hook=CncaGame::ExclusiveUpdateGame") != std::string::npos) at_tick = i;
    if (t.find("module install") != std::string::npos) at_mod_last = i;
  }
  CHECK(at_bvh != 0 && at_wall > at_bvh, "wallfire (line %zu) must be installed after bvh (line %zu)", at_wall, at_bvh);
  CHECK(at_ec != 0 && CountLines("module install module=ecfx ") == 1 && FindLine("module=ecfx ") != "", "ecfx record missing");
  CHECK(at_tick > at_mod_last, "the game tick (line %zu) must be installed after the last module (line %zu)", at_tick, at_mod_last);
  size_t at_ecfx = 0;
  for (size_t i = 0; i < g_lines.size(); ++i)
    if (g_lines[i].text.find("module install module=ecfx ") != std::string::npos) at_ecfx = i;
  CHECK(at_ecfx > at_ec, "ecfx (line %zu) must be installed after ec (line %zu)", at_ecfx, at_ec);

  // the tick table: hz (the refresh tick), gunconnect, aibots, impactsnd, sensorwd and bvh register; dronekill, dronemuzzle, powerboost and
  // weapon_arm did not install here, so they register nothing. The tick is installed once, after every registration.
  CHECK(CountLines("game tick callbacks registered=6 max=16", 1) == 1, "tick count line: %s", FindLine("game tick callbacks").c_str());
  CHECK(CountLines("hook install hook=CncaGame::ExclusiveUpdateGame (game-thread tick) status=ok", 1) == 1, "the game tick was not installed ok");
  CHECK(w[4] != reinterpret_cast<uintptr_t>(&FakeExclusiveUpdateGame), "the tick slot was not swapped");
  CHECK(CountLines("game tick callbacks") == 1, "more than one tick install record");
  CHECK(CountLines("module install FAILED") == 12, "%d FAILED lines, want 12", CountLines("module install FAILED"));

  // one frame through the swapped tick slot, as CncaGame::Render calls it: the registered callbacks of gunconnect and aibots run
  // (each starts, finds no game library on the host and says so once, at ERROR)
  static uintptr_t game[0x2000 / 8] = {};
  game[0] = base + Q15T_GAME_VPTR;
  reinterpret_cast<void (*)(void*)>(w[4])(game);
  CHECK(CountLines("gunconnect off", 3) == 1 && CountLines("aibots off", 3) == 1, "gunconnect / aibots ticks did not run: gunconnect %d, aibots %d",
        CountLines("gunconnect off", 3), CountLines("aibots off", 3));

  std::remove(off_file.c_str());
  rmdir(dir);
  if (g_fail == 0) std::printf("wiring_test: all checks pass (21 modules named, 4 installed, 3 off, 12 refusals at ERROR, InitEx returned 0)\n");
  else {
    for (const auto& l : g_lines) std::printf("  [%u] %s\n", l.level, l.text.c_str());
    std::printf("wiring_test: %d failure(s)\n", g_fail);
  }
  return g_fail == 0 ? 0 : 1;
}
#endif
