// Host test of T-26 (weapon_arm.cpp) against a fake CR15Game laid out as the game's gamespace tables and a fake
// equipment component system. Real SafeRead, real files, a fake clock; the nine engine calls are fakes.
#include <dirent.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/plugin_interface.h"
#include "plugin_log.h"
#include "weapon_arm.h"

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
static bool HasLine(const char* needle, int level = -1) {
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || (int)l.level == level)) return true;
  return false;
}

static const uintptr_t kBase = 0x7000000000ull;
static const uintptr_t kVptrVaddr = 0x3698200;

// ---- the fake world ----
alignas(16) static uintptr_t g_game[0x2000 / 8];
alignas(16) static uintptr_t g_gs[0x500 / 8];
alignas(16) static uintptr_t g_table[2];
alignas(16) static uintptr_t g_sys[4];
alignas(16) static uintptr_t g_cs[0x140 / 8];
alignas(16) static uint16_t g_gens[8];
alignas(16) static char g_queue_buf[0x40];

static const char* g_xplat = "OVR-ORG-1234567890";
static uint32_t g_arm = 0;
static long long g_prof_arm = -1;
static bool g_valid = true, g_has_net = true;
static int g_set_calls = 0, g_queue_calls = 0;
static uint64_t g_set_handle = 0, g_set_value = 0;
static void* g_set_cs = nullptr;

static void* FNet(void*) { return g_has_net ? reinterpret_cast<void*>(0x1000) : nullptr; }
static void* FProfile(void*) { return reinterpret_cast<void*>(0x2000); }
static const char* FTString(void*, const char*, const char*, unsigned) { return g_xplat; }
static long long FJsonInt(void*, const char*, long long, unsigned) { return g_prof_arm; }
static bool FValid(void*, uint64_t) { return g_valid; }
static uint32_t FGetArm(void*, uint64_t) { return g_arm; }
static void FSetArm(void* cs, uint64_t h, uint64_t v) {
  ++g_set_calls;
  g_set_cs = cs;
  g_set_handle = h;
  g_set_value = v;
}
static void FInvoke(void*, char*) {}
static void* FQueue(void*, void (*)(void*, char*)) {
  ++g_queue_calls;
  std::memset(g_queue_buf, 0, sizeof g_queue_buf);
  return g_queue_buf;
}

static double g_now = 100.0;
static double Clock() { return g_now; }

static void BuildWorld(int direct) {
  std::memset(g_game, 0, sizeof g_game);
  std::memset(g_gs, 0, sizeof g_gs);
  std::memset(g_cs, 0, sizeof g_cs);
  std::memset(g_gens, 0, sizeof g_gens);
  const uintptr_t game = reinterpret_cast<uintptr_t>(g_game), gs = reinterpret_cast<uintptr_t>(g_gs),
                  cs = reinterpret_cast<uintptr_t>(g_cs);
  g_table[0] = 0x5555;
  g_table[1] = gs;
  g_game[0x1f48 / 8] = reinterpret_cast<uintptr_t>(g_table);
  g_game[0x1f78 / 8] = 1;
  g_gs[0x8 / 8] = game;
  g_gs[0x420 / 8] = reinterpret_cast<uintptr_t>(g_sys);
  g_gs[0x450 / 8] = 1;
  g_gs[0x368 / 8] = static_cast<uintptr_t>(direct);
  g_sys[0] = 0xca14940c84b03f6eull;
  g_sys[1] = cs;
  g_cs[0] = kBase + kVptrVaddr;
  g_cs[0x80 / 8] = gs;
  reinterpret_cast<int16_t*>(g_cs)[0x130 / 2] = 3;  // slot
  g_cs[0xc8 / 8] = reinterpret_cast<uintptr_t>(g_gens);
  g_gens[3 * 2 + 1] = 7;  // generation of slot 3: tab + slot*4 + 2
}

static std::string g_dir;
static void ClearDir() {
  DIR* d = opendir(g_dir.c_str());
  if (d == nullptr) return;
  while (dirent* e = readdir(d))
    if (e->d_name[0] != '.') unlink((g_dir + "/" + e->d_name).c_str());
  closedir(d);
}
static bool FileExists(const char* name) { return access((g_dir + "/" + name).c_str(), F_OK) == 0; }
static std::string ReadFile(const char* name) {
  std::string s;
  FILE* f = std::fopen((g_dir + "/" + name).c_str(), "r");
  if (f == nullptr) return s;
  char b[256];
  size_t n;
  while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
  std::fclose(f);
  return s;
}

static q15t::WeaponArmFns Fns() {
  q15t::WeaponArmFns f{};
  f.net_game = &FNet;
  f.client_profile = &FProfile;
  f.t_string = &FTString;
  f.json_int = &FJsonInt;
  f.is_valid = &FValid;
  f.get_arm = &FGetArm;
  f.set_arm = &FSetArm;
  f.invoke = &FInvoke;
  f.queue = &FQueue;
  return f;
}

static void Reset(int direct, uint32_t arm, long long prof_arm) {
  g_lines.clear();
  ClearDir();
  BuildWorld(direct);
  g_arm = arm;
  g_prof_arm = prof_arm;
  g_valid = true;
  g_has_net = true;
  g_set_calls = g_queue_calls = 0;
  g_now = 100.0;
  q15t::ResetWeaponArmForTest();
  q15t::InitWeaponArm(kBase, Fns());
}
// one pass per second of fake time
static void Pass() {
  q15t::WeaponArmTick(reinterpret_cast<uintptr_t>(g_game));
  g_now += 1.0;
}

int main() {
  char tmpl[] = "/tmp/q15t-t26-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  g_dir = tmpl;
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  q15t::SetLogContext(&ctx);
  q15t::SetWeaponArmClockForTest(&Clock);
  q15t::SetWeaponArmDirForTest(g_dir.c_str());

  // 1. a new profile with the gun on the left, direct call: held for 3 passes, set once, marker once the arm reads 1
  Reset(1, 0, -1);
  Pass();
  Pass();
  CHECK(g_set_calls == 0, "set before the 3rd pass: %d", g_set_calls);
  Pass();
  CHECK(g_set_calls == 1 && g_set_value == 1 && g_set_cs == reinterpret_cast<void*>(g_cs), "no direct set: calls %d value %llu", g_set_calls,
        (unsigned long long)g_set_value);
  CHECK(g_set_handle == (3ull | (7ull << 16)), "handle %llx", (unsigned long long)g_set_handle);
  CHECK(q15t::WeaponArmPhase() == 1 && !FileExists("t26arm.OVR-ORG-1234567890"), "marker before the arm read 1");
  CHECK(HasLine("t26 set") && HasLine("detail=weaponarm -> 1"), "no set record");
  g_arm = 1;
  Pass();
  CHECK(FileExists("t26arm.OVR-ORG-1234567890"), "no marker after the arm read 1");
  CHECK(ReadFile("t26arm.OVR-ORG-1234567890").find("weaponarm now 1") != std::string::npos, "marker text");
  CHECK(q15t::WeaponArmPhase() == 2, "not done");
  for (int i = 0; i < 5; ++i) Pass();
  CHECK(g_set_calls == 1, "one-shot broken: %d sets", g_set_calls);
  CHECK(HasLine("t26 done", 1), "no INFO done line");

  // 2. deferred route: the functor goes into the gamespace's queue with the exact layout, no direct call
  Reset(0, 0, -1);
  Pass();
  Pass();
  Pass();
  CHECK(g_set_calls == 0 && g_queue_calls == 1, "deferred: direct %d queue %d", g_set_calls, g_queue_calls);
  CHECK(*reinterpret_cast<void**>(g_queue_buf) == reinterpret_cast<void*>(g_cs), "functor cs");
  CHECK(*reinterpret_cast<uint32_t*>(g_queue_buf + 0x18) == (3u | 7u << 16), "functor handle");
  CHECK(*reinterpret_cast<uint64_t*>(g_queue_buf + 0x20) == 1, "functor arg");
  CHECK(*reinterpret_cast<void**>(g_queue_buf + 0x08) == reinterpret_cast<void*>(&FSetArm), "functor method");
  CHECK(*reinterpret_cast<int64_t*>(g_queue_buf + 0x10) == 0, "functor adjust");
  CHECK(HasLine("t26 set") && HasLine("why=deferred"), "no deferred record");

  // 3. the gun is already on the right: marker only, nothing set
  Reset(1, 1, -1);
  Pass();
  Pass();
  Pass();
  CHECK(g_set_calls == 0 && g_queue_calls == 0, "set although already right");
  CHECK(FileExists("t26arm.OVR-ORG-1234567890") && ReadFile("t26arm.OVR-ORG-1234567890").find("already") != std::string::npos,
        "no marker for an already-right profile");

  // 4. the profile saved weaponarm 1 (arm reads 0): the player's own choice stands, nothing set
  Reset(1, 0, 1);
  Pass();
  Pass();
  Pass();
  CHECK(g_set_calls == 0 && FileExists("t26arm.OVR-ORG-1234567890"), "profile weaponarm 1 was overridden");

  // 5. marker from an earlier launch: the profile is never touched
  Reset(1, 0, -1);
  FILE* m = std::fopen((g_dir + "/t26arm.OVR-ORG-1234567890").c_str(), "w");
  std::fputs("old\n", m);
  std::fclose(m);
  for (int i = 0; i < 6; ++i) Pass();
  CHECK(g_set_calls == 0, "set although the marker exists");
  CHECK(HasLine("marker present"), "no marker-present record");

  // 6. the arm never reads 1 after the set: gives up after 10 s, no marker
  Reset(1, 0, -1);
  Pass();
  Pass();
  Pass();
  CHECK(g_set_calls == 1, "no set");
  for (int i = 0; i < 12; ++i) Pass();
  CHECK(!FileExists("t26arm.OVR-ORG-1234567890"), "marker written without the arm reading 1");
  CHECK(HasLine("t26 gave up", 2), "no WARNING when the set did not take");
  CHECK(q15t::WeaponArmPhase() == 2 && g_set_calls == 1, "retried within the launch");

  // 7. the local equipment component is not valid yet: waits, sets nothing; becomes valid: the hold restarts
  Reset(1, 0, -1);
  g_valid = false;
  for (int i = 0; i < 5; ++i) Pass();
  CHECK(g_set_calls == 0 && HasLine("no local equipment component yet"), "set on an invalid component");
  g_valid = true;
  Pass();
  Pass();
  CHECK(g_set_calls == 0, "no hold after the component became valid");
  Pass();
  CHECK(g_set_calls == 1, "never set once valid");

  // 8. a component system with the wrong vptr, or a key that is not the equipment key, is not found
  Reset(1, 0, -1);
  g_cs[0] = kBase + 0x1111;
  for (int i = 0; i < 4; ++i) Pass();
  CHECK(g_set_calls == 0 && HasLine("no equipment CS yet"), "wrong vptr accepted");
  Reset(1, 0, -1);
  g_sys[0] = 0x1234;
  for (int i = 0; i < 4; ++i) Pass();
  CHECK(g_set_calls == 0, "wrong key accepted");
  Reset(1, 0, -1);
  g_cs[0x80 / 8] = 0x9999;  // cs+0x80 is not its gamespace
  for (int i = 0; i < 4; ++i) Pass();
  CHECK(g_set_calls == 0 && HasLine("no equipment CS yet"), "cs whose gamespace link differs accepted");

  // 9. no netgame yet: waits, no crash, no set
  Reset(1, 0, -1);
  g_has_net = false;
  for (int i = 0; i < 4; ++i) Pass();
  CHECK(g_set_calls == 0 && HasLine("why=no netgame"), "netgame absent not reported");

  // 10. t26arm.off in the data directory: nothing set, one line
  Reset(1, 0, -1);
  m = std::fopen((g_dir + "/t26arm.off").c_str(), "w");
  std::fclose(m);
  for (int i = 0; i < 5; ++i) Pass();
  CHECK(g_set_calls == 0 && HasLine("t26 off") && HasLine("t26arm.off present"), "t26arm.off not honoured");

  // 11. an export is missing (no game library loaded on the host): one ERROR, nothing runs
  g_lines.clear();
  ClearDir();
  q15t::ResetWeaponArmForTest();
  q15t::InitWeaponArm(kBase, q15t::WeaponArmFns{});
  for (int i = 0; i < 3; ++i) Pass();
  CHECK(HasLine("t26 off", 3) && g_set_calls == 0, "missing export not loud");

  // 12. a data directory that cannot be written: waits and reports, sets nothing (no marker could be kept)
  Reset(1, 0, -1);
  q15t::SetWeaponArmDirForTest("/nonexistent/q15t");
  for (int i = 0; i < 5; ++i) Pass();
  CHECK(g_set_calls == 0 && HasLine("no writable data directory"), "set without a place for the marker");
  q15t::SetWeaponArmDirForTest(g_dir.c_str());

  // 13. a second login (another profile id) in the same process starts over
  Reset(1, 0, -1);
  for (int i = 0; i < 4; ++i) Pass();
  g_arm = 1;
  Pass();
  CHECK(q15t::WeaponArmPhase() == 2, "first profile not finished");
  // the tick returns at once while done: a later login in the same process is not retried (q15t: once per launch)
  g_xplat = "OVR-ORG-9999";
  g_arm = 0;
  for (int i = 0; i < 6; ++i) Pass();
  CHECK(g_set_calls == 1, "a finished tick must stay finished within the launch: %d", g_set_calls);

  ClearDir();
  rmdir(g_dir.c_str());
  if (g_fail == 0) std::printf("t26_test: all checks passed\n");
  return g_fail == 0 ? 0 : 1;
}
