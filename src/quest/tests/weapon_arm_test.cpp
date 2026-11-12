// Host test of the gun hand (src/quest/combat/weapon_arm.cpp, #2185, T-26; moved from the q15t plugin's weapon_arm): a profile whose weaponarm is not 1 is set to
// the right hand once per marker. It runs against a fake CR15Game laid out as the game's gamespace tables and a fake equipment component system. Real SafeRead, real files, a fake clock; the nine engine calls are fakes.
//
// Moved: sections 1 to 13 are the plugin test's sections with the same assertions (the hold of 3 passes, the direct and the deferred route with the functor's
// bytes, an already-right gun and a profile that holds the right hand, a marker from an earlier launch, the give-up after 10 s, an invalid component, the
// wrong vptr / key / gamespace link, no netgame, a data directory that cannot be written, a second login in one process). What changed is the fixture (the
// game has a level, the module goes on only in a combat level, InstallResolved with the fakes) and the lines (`combat_weapon_arm_*` records; a profile id is
// logged by its last four characters only). New with the move: 14 a marker the plugin wrote stops the module, whatever else is true, and the module writes
// the marker under the same name in the same directory (player state, ADR 0046 rule 4), 15 the switch, 16 the kill file is not read and warns, 17 the
// scope (an arena match, a lobby arena players use and a transition holding an arena level set nothing and write no marker; a profile with a marker never
// scans the levels), 18 the install on an image, 19 the host cost of the arena pass. The scope is the real combat level answer (combat_scope.h, #2382):
// each test pass runs its refresh against the fake game before the module's callback, as the tick does, a look older than 1 s is not believed, and a
// scope that cannot change refuses the install (`scope_not_live`).
//
// Built and run by `just test-quest-combat`.
#include "quest/combat/weapon_arm.h"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "nevr/symbol_hash.h"
#include "quest/combat/combat_level.h"
#include "quest/combat/combat_scope.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/gamespace_targets.h"
#include "quest/combat/netgame_targets.h"
#include "quest/combat/safe_read.h"
#include "quest/combat/weapon_arm_targets.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/combat_libr15_image.h"
#include "quest/tests/combat_pins.h"
#include "quest/tests/combat_scope_world.h"
#include "quest/tests/test_check.h"

namespace {
// Reads made through SafeRead: safe_read.h's test seam adds one to this counter for each (set in main).
std::atomic<std::uint64_t> g_safeReads{0};
std::uint64_t SafeReadCalls() { return g_safeReads.load(std::memory_order_relaxed); }

namespace wa = nevr_quest::combat::weapon_arm;
namespace scope = nevr_quest::combat::scope;
namespace world = combat_test::scope_world;
using EchoVR::CSymbol64Hash;

#define CHECK(cond, ...)                                                           \
  do {                                                                             \
    if (!(cond)) {                                                                 \
      std::fprintf(stderr, "%s:%d CHECK failed: %s  ", __FILE__, __LINE__, #cond); \
      std::fprintf(stderr, __VA_ARGS__);                                           \
      std::fprintf(stderr, "\n");                                                  \
      ++quest_test::Failures();                                                    \
    }                                                                              \
  } while (0)

struct Line {
  sentinel::LogLevel level;
  std::string text;
};
std::vector<Line> g_lines;
void Capture(sentinel::LogLevel level, const char* line) { g_lines.push_back({level, line}); }
bool Has(const std::string& text, const std::string& needle) { return text.find(needle) != std::string::npos; }
std::vector<Line> Lines(const char* event) {
  std::vector<Line> out;
  const std::string key = std::string("\"event\":\"") + event + "\"";
  for (const Line& l : g_lines) {
    if (Has(l.text, key)) out.push_back(l);
  }
  return out;
}
int Count(const char* event, const char* needle = "", int level = -1) {
  int n = 0;
  for (const Line& l : Lines(event)) n += Has(l.text, needle) && (level < 0 || static_cast<int>(l.level) == level) ? 1 : 0;
  return n;
}
bool HasLine(const char* event, const char* needle = "", int level = -1) { return Count(event, needle, level) > 0; }
constexpr int kInfo = static_cast<int>(sentinel::LogLevel::kInfo), kWarn = static_cast<int>(sentinel::LogLevel::kWarn), kError = static_cast<int>(sentinel::LogLevel::kError);

const std::uintptr_t kBase = 0x7000000000ull;
const std::uintptr_t kVptrVaddr = 0x3698200;

// ---- the fake world: the offsets are spelled here, not taken from the headers ----
alignas(16) std::uintptr_t g_game[0x2000 / 8];
alignas(16) std::uintptr_t g_gs[0x500 / 8];
alignas(16) std::uintptr_t g_gs2[0x100 / 8];
alignas(16) std::uintptr_t g_table[4];
alignas(16) std::uintptr_t g_sys[4];
alignas(16) std::uintptr_t g_cs[0x140 / 8];
alignas(16) std::uint16_t g_gens[8];
alignas(16) char g_queueBuf[0x40];

const char* g_xplat = "OVR-ORG-1234567890";
std::uint32_t g_arm = 0;
long long g_profArm = -1;
bool g_valid = true, g_hasNet = true;
int g_setCalls = 0, g_queueCalls = 0, g_profileCalls = 0;
std::uint64_t g_setHandle = 0, g_setValue = 0;
void* g_setCs = nullptr;

void* FNet(void*) { return g_hasNet ? reinterpret_cast<void*>(0x1000) : nullptr; }
void* FProfile(void*) {
  ++g_profileCalls;
  return reinterpret_cast<void*>(0x2000);
}
const char* FTString(void*, const char*, const char*, unsigned) { return g_xplat; }
long long FJsonInt(void*, const char*, long long, unsigned) { return g_profArm; }
bool FValid(void*, std::uint64_t) { return g_valid; }
std::uint32_t FGetArm(void*, std::uint64_t) { return g_arm; }
void FSetArm(void* cs, std::uint64_t h, std::uint64_t v) {
  ++g_setCalls;
  g_setCs = cs;
  g_setHandle = h;
  g_setValue = v;
}
void FInvoke(void*, char*) {}
void* g_queueObject = nullptr;
void* FQueue(void* queueObject, void (*)(void*, char*)) {
  ++g_queueCalls;
  g_queueObject = queueObject;
  std::memset(g_queueBuf, 0, sizeof g_queueBuf);
  return g_queueBuf;
}

double g_now = 100.0;
double Clock() { return g_now; }
// The combat level answer's clock (combat_scope.h) is the same fake clock, in ms.
std::int64_t ScopeClock() noexcept { return static_cast<std::int64_t>(g_now * 1000.0); }

// The game's levels: `level` in the first gamespace (the equipment system's), and optionally `second` in another.
void BuildWorld(int direct, const char* level = "mpl_combat_dyson", const char* second = nullptr) {
  std::memset(g_game, 0, sizeof g_game);
  std::memset(g_gs, 0, sizeof g_gs);
  std::memset(g_gs2, 0, sizeof g_gs2);
  std::memset(g_cs, 0, sizeof g_cs);
  std::memset(g_gens, 0, sizeof g_gens);
  const std::uintptr_t game = reinterpret_cast<std::uintptr_t>(g_game), gs = reinterpret_cast<std::uintptr_t>(g_gs), cs = reinterpret_cast<std::uintptr_t>(g_cs);
  g_table[0] = 0x5555;
  g_table[1] = gs;
  g_table[2] = 0x6666;
  g_table[3] = reinterpret_cast<std::uintptr_t>(g_gs2);
  g_game[0x1f48 / 8] = reinterpret_cast<std::uintptr_t>(g_table);
  g_game[0x1f78 / 8] = second != nullptr ? 2 : 1;
  g_gs[0x8 / 8] = game;
  g_gs[0x58 / 8] = static_cast<std::uintptr_t>(CSymbol64Hash(level));
  g_gs[0x420 / 8] = reinterpret_cast<std::uintptr_t>(g_sys);
  g_gs[0x450 / 8] = 1;
  g_gs[0x368 / 8] = static_cast<std::uintptr_t>(direct);
  g_gs2[0x8 / 8] = game;
  if (second != nullptr) g_gs2[0x58 / 8] = static_cast<std::uintptr_t>(CSymbol64Hash(second));
  g_sys[0] = 0xca14940c84b03f6eull;
  g_sys[1] = cs;
  g_cs[0] = kBase + kVptrVaddr;
  g_cs[0x80 / 8] = gs;
  reinterpret_cast<std::int16_t*>(g_cs)[0x130 / 2] = 3;  // slot
  g_cs[0xc8 / 8] = reinterpret_cast<std::uintptr_t>(g_gens);
  g_gens[3 * 2 + 1] = 7;  // generation of slot 3: tab + slot*4 + 2
}

std::string g_dir;
void ClearDir() {
  DIR* d = opendir(g_dir.c_str());
  if (d == nullptr) return;
  while (dirent* e = readdir(d)) {
    if (e->d_name[0] != '.') unlink((g_dir + "/" + e->d_name).c_str());
  }
  closedir(d);
}
bool FileExists(const char* name) { return access((g_dir + "/" + name).c_str(), F_OK) == 0; }
std::string ReadFile(const char* name) {
  std::string s;
  std::FILE* f = std::fopen((g_dir + "/" + name).c_str(), "r");
  if (f == nullptr) return s;
  char b[256];
  std::size_t n;
  while ((n = std::fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
  std::fclose(f);
  return s;
}

wa::Fns Fns() {
  wa::Fns f{};
  f.netGame = &FNet;
  f.clientProfile = &FProfile;
  f.tString = &FTString;
  f.jsonInt = &FJsonInt;
  f.isValid = &FValid;
  f.getArm = &FGetArm;
  f.setArm = &FSetArm;
  f.invoke = &FInvoke;
  f.queue = &FQueue;
  return f;
}

void Reset(int direct, std::uint32_t arm, long long profArm, const char* level = "mpl_combat_dyson", const char* second = nullptr) {
  g_lines.clear();
  ClearDir();
  BuildWorld(direct, level, second);
  g_arm = arm;
  g_profArm = profArm;
  g_valid = true;
  g_hasNet = true;
  g_xplat = "OVR-ORG-1234567890";
  g_setCalls = g_queueCalls = g_profileCalls = 0;
  g_now = 100.0;
  nevr_quest::combat::tick::ResetForTest();
  scope::ResetForTest();
  scope::SetClockForTest(&ScopeClock);
  wa::ResetForTest();
  wa::SetClockForTest(&Clock);
  wa::SetLegacyDirForTest(g_dir.c_str());
  QCHECK(wa::InstallResolved(kBase, Fns()).installed);
}
// The module's callback alone, then one second of fake time: what the module itself reads, with nothing refreshing the combat level answer.
void ModulePass() {
  wa::Tick(reinterpret_cast<std::uintptr_t>(g_game));
  g_now += 1.0;
}
// One pass per second of fake time, as the game-thread tick runs it: the combat level answer's refresh (the scope row registers it before this module's
// callback), then the module's.
void Pass() {
  scope::Refresh(reinterpret_cast<std::uintptr_t>(g_game));
  ModulePass();
}

void TheGunOnTheLeft() {  // 1. a profile with no weaponarm key and the gun on the left, direct call: held for 3 passes, set once, marker once the arm reads 1
  Reset(1, 0, -1);
  Pass();
  Pass();
  CHECK(g_setCalls == 0, "set before the 3rd pass: %d", g_setCalls);
  Pass();
  CHECK(g_setCalls == 1 && g_setValue == 1 && g_setCs == reinterpret_cast<void*>(g_cs), "no direct set: calls %d value %llu", g_setCalls, static_cast<unsigned long long>(g_setValue));
  CHECK(g_setHandle == (3ull | (7ull << 16)), "handle %llx", static_cast<unsigned long long>(g_setHandle));
  CHECK(wa::Phase() == 1 && !FileExists("t26arm.OVR-ORG-1234567890"), "marker before the arm read 1");
  CHECK(HasLine("combat_weapon_arm_set", "\"route\":\"direct\"") && HasLine("combat_weapon_arm_set", "weaponarm -> 1"), "no set record");
  g_arm = 1;
  Pass();
  CHECK(FileExists("t26arm.OVR-ORG-1234567890"), "no marker after the arm read 1");
  CHECK(ReadFile("t26arm.OVR-ORG-1234567890").find("weaponarm now 1") != std::string::npos, "marker text");
  CHECK(wa::Phase() == 2, "not done");
  for (int i = 0; i < 5; ++i) Pass();
  CHECK(g_setCalls == 1, "one-shot broken: %d sets", g_setCalls);
  CHECK(HasLine("combat_weapon_arm_done", "\"marker\":\"written\"", kInfo), "no INFO done line");
  for (const Line& l : g_lines) CHECK(!Has(l.text, "1234567890"), "the whole profile id was logged: %s", l.text.c_str());
}

void TheDeferredRoute() {  // 2. the functor goes into the gamespace's queue with the exact layout, no direct call
  Reset(0, 0, -1);
  Pass();
  Pass();
  Pass();
  CHECK(g_setCalls == 0 && g_queueCalls == 1, "deferred: direct %d queue %d", g_setCalls, g_queueCalls);
  CHECK(g_queueObject == reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(g_gs) + 0x170), "the functor was queued on another object than the gamespace's queue at gs+0x170");
  CHECK(*reinterpret_cast<void**>(g_queueBuf) == reinterpret_cast<void*>(g_cs), "functor cs");
  CHECK(*reinterpret_cast<std::uint32_t*>(g_queueBuf + 0x18) == (3u | 7u << 16), "functor handle");
  CHECK(*reinterpret_cast<std::uint64_t*>(g_queueBuf + 0x20) == 1, "functor arg");
  CHECK(*reinterpret_cast<void**>(g_queueBuf + 0x08) == reinterpret_cast<void*>(&FSetArm), "functor method");
  CHECK(*reinterpret_cast<std::int64_t*>(g_queueBuf + 0x10) == 0, "functor adjust");
  CHECK(HasLine("combat_weapon_arm_set", "\"route\":\"deferred\""), "no deferred record");
}

void AlreadyRight() {  // 3, 4
  Reset(1, 1, -1);
  Pass();
  Pass();
  Pass();
  CHECK(g_setCalls == 0 && g_queueCalls == 0, "set although already right");
  CHECK(FileExists("t26arm.OVR-ORG-1234567890") && ReadFile("t26arm.OVR-ORG-1234567890").find("already") != std::string::npos, "no marker for an already-right profile");
  Reset(1, 0, 1);  // the profile holds weaponarm 1 (arm reads 0): the marker only
  Pass();
  Pass();
  Pass();
  CHECK(g_setCalls == 0 && FileExists("t26arm.OVR-ORG-1234567890"), "profile weaponarm 1 was overridden");
}

void AMarkerThatCannotBeWrittenIsLoud() {  // the set is made, then the directory stops taking files
  Reset(1, 0, -1);
  for (int i = 0; i < 3; ++i) Pass();
  CHECK(g_setCalls == 1, "setup");
  chmod(g_dir.c_str(), 0555);
  g_arm = 1;
  Pass();
  chmod(g_dir.c_str(), 0755);
  CHECK(!FileExists("t26arm.OVR-ORG-1234567890"), "a marker in a directory that takes no files");
  CHECK(Count("combat_weapon_arm_marker_failed", "", kError) == 1 && HasLine("combat_weapon_arm_done", "\"marker\":\"write_failed\"", kError), "an unwritable marker not loud");
  CHECK(wa::Phase() == 2, "not done");
}

// The rule for which profile is set (B4): a profile whose `weaponarm` is 1 gets the marker only; every other profile, `weaponarm` 0 and no key alike, is set
// to the right hand. The community service sends `weaponarm` for every profile and a new account reads 0, so "never chose" and "chose left" are one value.
void AProfileThatIsNotRightIsSetAndOneThatIsRightIsNot() {
  // weaponarm 0 (the value a new account reads on the service), no marker: set, and the set is logged
  Reset(1, 0, 0);
  for (int i = 0; i < 3; ++i) Pass();
  CHECK(g_setCalls == 1 && g_setValue == 1, "a profile with weaponarm 0 was not set: calls %d", g_setCalls);
  CHECK(HasLine("combat_weapon_arm_set", "\"route\":\"direct\"") && Count("combat_weapon_arm_set", "") == 1, "no combat_weapon_arm_set for weaponarm 0");
  g_arm = 1;
  Pass();
  CHECK(FileExists("t26arm.OVR-ORG-1234567890"), "no marker after the set of a weaponarm 0 profile");
  // weaponarm 1 (the profile holds the right hand while the component reads left): the marker only
  Reset(1, 0, 1);
  for (int i = 0; i < 4; ++i) Pass();
  CHECK(g_setCalls == 0 && g_queueCalls == 0, "a profile with weaponarm 1 was set");
  CHECK(Count("combat_weapon_arm_set", "") == 0, "a set record for weaponarm 1");
  CHECK(FileExists("t26arm.OVR-ORG-1234567890") && HasLine("combat_weapon_arm_done", "\"outcome\":\"profile weaponarm is 1, nothing changed\"", kInfo), "no marker for weaponarm 1");
  // no key at all: set
  Reset(1, 0, -1);
  for (int i = 0; i < 3; ++i) Pass();
  CHECK(g_setCalls == 1, "a profile with no weaponarm key was not set");
  // weaponarm 0 with the deferred route: queued, not called
  Reset(0, 0, 0);
  for (int i = 0; i < 3; ++i) Pass();
  CHECK(g_setCalls == 0 && g_queueCalls == 1, "weaponarm 0 on the deferred route: direct %d queue %d", g_setCalls, g_queueCalls);
  // the marker is gone (a reinstall) for a profile of 1: the next launch writes the marker again and sets nothing
  Reset(1, 0, 1);
  for (int i = 0; i < 4; ++i) Pass();
  ClearDir();
  wa::ResetForTest();
  wa::SetClockForTest(&Clock);
  wa::SetLegacyDirForTest(g_dir.c_str());
  nevr_quest::combat::tick::ResetForTest();
  QCHECK(wa::InstallResolved(kBase, Fns()).installed);
  for (int i = 0; i < 6; ++i) Pass();
  CHECK(g_setCalls == 0 && g_queueCalls == 0 && FileExists("t26arm.OVR-ORG-1234567890"), "a missing marker set the hand of a profile that holds weaponarm 1");
}

void AMarkerFromAnEarlierLaunch() {  // 5
  Reset(1, 0, -1);
  std::FILE* m = std::fopen((g_dir + "/t26arm.OVR-ORG-1234567890").c_str(), "w");
  std::fputs("old\n", m);
  std::fclose(m);
  for (int i = 0; i < 6; ++i) Pass();
  CHECK(g_setCalls == 0, "set although the marker exists");
  CHECK(HasLine("combat_weapon_arm_done", "\"outcome\":\"marker present: nothing changed\",\"marker\":\"present\""), "no marker-present record");
}

void GivingUp() {  // 6. the arm never reads 1 after the set: gives up after 10 s, no marker
  Reset(1, 0, -1);
  Pass();
  Pass();
  Pass();
  CHECK(g_setCalls == 1, "no set");
  for (int i = 0; i < 12; ++i) Pass();
  CHECK(!FileExists("t26arm.OVR-ORG-1234567890"), "marker written without the arm reading 1");
  CHECK(HasLine("combat_weapon_arm_gave_up", "", kWarn), "no WARNING when the set did not take");
  // a failed set stops for the rest of the launch: more passes, with the arm still left, set nothing again and say nothing again
  CHECK(wa::Phase() == 2, "not done after the give-up: phase %d", wa::Phase());
  for (int i = 0; i < 30; ++i) Pass();
  CHECK(wa::Phase() == 2, "the give-up did not stay done: phase %d", wa::Phase());
  CHECK(g_setCalls == 1 && g_queueCalls == 0, "retried after the give-up: %d sets, %d queued", g_setCalls, g_queueCalls);
  CHECK(Count("combat_weapon_arm_gave_up", "") == 1, "%d give-up records, want exactly 1", Count("combat_weapon_arm_gave_up", ""));
  CHECK(!FileExists("t26arm.OVR-ORG-1234567890"), "a marker appeared after the give-up");
  // not before 10 s: an arm that reads 1 eight passes after the set still gets its marker
  Reset(1, 0, -1);
  for (int i = 0; i < 3; ++i) Pass();
  for (int i = 0; i < 8; ++i) Pass();
  CHECK(wa::Phase() == 1 && !HasLine("combat_weapon_arm_gave_up"), "gave up before the 10 s");
  g_arm = 1;
  Pass();
  CHECK(FileExists("t26arm.OVR-ORG-1234567890"), "no marker for an arm that took 8 s");
  CHECK(wa::Phase() == 2 && g_setCalls == 1, "retried within the launch");
}

void AnInvalidComponent() {  // 7. waits, sets nothing; becomes valid: the hold restarts
  Reset(1, 0, -1);
  g_valid = false;
  for (int i = 0; i < 5; ++i) Pass();
  CHECK(g_setCalls == 0 && HasLine("combat_weapon_arm_waiting", "no local equipment component yet"), "set on an invalid component");
  g_valid = true;
  Pass();
  Pass();
  CHECK(g_setCalls == 0, "no hold after the component became valid");
  Pass();
  CHECK(g_setCalls == 1, "never set once valid");
}

void NotFound() {  // 8
  Reset(1, 0, -1);
  g_cs[0] = kBase + 0x1111;
  for (int i = 0; i < 4; ++i) Pass();
  CHECK(g_setCalls == 0 && HasLine("combat_weapon_arm_waiting", "no equipment CS yet"), "wrong vptr accepted");
  Reset(1, 0, -1);
  g_sys[0] = 0x1234;
  for (int i = 0; i < 4; ++i) Pass();
  CHECK(g_setCalls == 0, "wrong key accepted");
  Reset(1, 0, -1);
  g_cs[0x80 / 8] = 0x9999;  // cs+0x80 is not its gamespace
  for (int i = 0; i < 4; ++i) Pass();
  CHECK(g_setCalls == 0 && HasLine("combat_weapon_arm_waiting", "no equipment CS yet"), "cs whose gamespace link differs accepted");
}

void NoNetGame() {  // 9
  Reset(1, 0, -1);
  g_hasNet = false;
  for (int i = 0; i < 4; ++i) Pass();
  CHECK(g_setCalls == 0 && HasLine("combat_weapon_arm_waiting", "\"why\":\"no netgame\""), "netgame absent not reported");
}

// A libr15 image with the tick's slot row, on which the tick is installed and the scope row registers its refresh: the combat level answer can
// change, as the shipped table leaves it before this module's row.
combat_test::Image g_tickImage;
bool TickImageLookup(const char* module, sentinel::ElfImage* out) {
  if (!g_tickImage.Present() || std::strcmp(module, sentinel::pinned::kLibR15) != 0) return false;
  *out = g_tickImage.Elf();
  return true;
}
void StartTheScope() {
  nevr_quest::combat::tick::ResetForTest();
  scope::ResetForTest();
  scope::SetClockForTest(&ScopeClock);
  g_tickImage.Build({world::TickRow()});
  world::PutTickSlot(g_tickImage);
  QCHECK(world::StartScope(&TickImageLookup));
}

void NoLibrary() {  // 11. no game library loaded on the host: one ERROR, nothing runs (the scope's refresh is the one tick callback)
  g_lines.clear();
  ClearDir();
  StartTheScope();
  wa::ResetForTest();
  wa::SetClockForTest(&Clock);
  const nevr_quest::combat::ModuleResult r = wa::InstallAt([](const char*, sentinel::ElfImage*) { return false; }, nullptr, true);
  BuildWorld(1);
  g_setCalls = 0;
  for (int i = 0; i < 3; ++i) Pass();
  CHECK(!r.installed && HasLine("combat_weapon_arm_install", "\"reason\":\"module_not_loaded\"", kError) && g_setCalls == 0 && nevr_quest::combat::tick::Count() == 1,
        "missing library not loud");
  g_tickImage.Drop();
}

// The combat level answer cannot change (the tick is not installed, or the scope row did not register its refresh): the pass would never act, so the
// module is not installed, says why at Error and registers nothing. Off by the key wins: nothing is asked of the scope.
void AScopeThatCannotChangeRefusesTheInstall() {
  g_lines.clear();
  nevr_quest::combat::tick::ResetForTest();
  scope::ResetForTest();
  wa::ResetForTest();
  wa::SetLegacyDirForTest(g_dir.c_str());
  nevr_quest::combat::ModuleResult r = wa::InstallAt([](const char*, sentinel::ElfImage*) { return true; }, nullptr, true);
  CHECK(!r.installed && !r.off && std::strcmp(r.status, "scope_not_live") == 0 && wa::Phase() == 2 && nevr_quest::combat::tick::Count() == 0,
        "a scope that cannot change: %s", r.status);
  CHECK(Lines("combat_weapon_arm_install").size() == 1 && Lines("combat_weapon_arm_install")[0].level == sentinel::LogLevel::kError &&
            HasLine("combat_weapon_arm_install", "\"reason\":\"scope_not_live\""),
        "scope_not_live not loud");
  // the tick is installed but no row registered the refresh
  g_lines.clear();
  g_tickImage.Build({world::TickRow()});
  world::PutTickSlot(g_tickImage);
  QCHECK(nevr_quest::combat::tick::InstallAt(&TickImageLookup));
  wa::ResetForTest();
  r = wa::InstallAt([](const char*, sentinel::ElfImage*) { return true; }, nullptr, true);
  CHECK(!r.installed && std::strcmp(r.status, "scope_not_live") == 0 && !scope::Live(), "an unregistered refresh: %s", r.status);
  // off by the key: off_config, whatever the scope
  wa::ResetForTest();
  r = wa::InstallAt([](const char*, sentinel::ElfImage*) { return true; }, nullptr, false);
  CHECK(!r.installed && r.off && std::strcmp(r.status, "off_config") == 0, "off with a dead scope: %s", r.status);
  nevr_quest::combat::tick::ResetForTest();
  g_tickImage.Drop();
}

void AnUnwritableDirectory() {  // 12. waits and reports, sets nothing (no marker could be kept)
  Reset(1, 0, -1);
  wa::SetLegacyDirForTest("/nonexistent/q15t");
  for (int i = 0; i < 5; ++i) Pass();
  CHECK(g_setCalls == 0 && HasLine("combat_weapon_arm_waiting", "no writable data directory"), "set without a place for the marker");
  wa::SetLegacyDirForTest(g_dir.c_str());
}

void ASecondLogin() {  // 13. a finished module stays finished within the launch
  Reset(1, 0, -1);
  for (int i = 0; i < 4; ++i) Pass();
  g_arm = 1;
  Pass();
  CHECK(wa::Phase() == 2, "first profile not finished");
  g_xplat = "OVR-ORG-9999";
  g_arm = 0;
  for (int i = 0; i < 6; ++i) Pass();
  CHECK(g_setCalls == 1, "a finished tick must stay finished within the launch: %d", g_setCalls);
}

void AMarkerThePluginWroteStopsTheModule() {  // 14
  // The plugin's marker: the same name in the same directory, with whatever text the plugin wrote.
  const std::uint64_t scans0 = nevr_quest::combat::level::ScanCalls(), reads0 = SafeReadCalls();
  for (const char* text : {"weaponarm 1 profile -1 weaponarm now 1\n", "weaponarm 1 profile 1 weaponarm right already, nothing changed\n", "old\n", ""}) {
    Reset(1, 0, -1);
    std::FILE* m = std::fopen((g_dir + "/t26arm.OVR-ORG-1234567890").c_str(), "w");
    std::fputs(text, m);
    std::fclose(m);
    for (int i = 0; i < 8; ++i) ModulePass();  // the module's callback alone: the combat level answer's own looks are counted in 17
    CHECK(g_setCalls == 0 && g_queueCalls == 0 && wa::Phase() == 2, "a plugin marker (%zu bytes) did not stop the module", std::strlen(text));
    CHECK(ReadFile("t26arm.OVR-ORG-1234567890") == text, "the module rewrote a plugin marker");
    CHECK(wa::CurrentStats().scopedOut == 0, "a profile with a marker scanned the levels");
    CHECK(nevr_quest::combat::level::ScanCalls() == scans0 && SafeReadCalls() == reads0, "a profile with a marker scanned or read the game");
  }
  // The module's own marker is the plugin's file name: the directory is files/q15t, the name t26arm.<id> with the id's odd characters made '_'.
  Reset(1, 1, -1);
  g_xplat = "OVR-ORG-12/34 56";
  for (int i = 0; i < 4; ++i) Pass();
  CHECK(FileExists("t26arm.OVR-ORG-12_34_56"), "the marker is not named as the plugin names it");
}

void TheNineAddresses() {  // the fixed addresses, written out here, not taken from the targets header
  wa::Fns f{};
  const char* why = "";
  CHECK(wa::Resolve(kBase, nullptr, &f, &why) && f.named == 0, "fixed-address resolve");
  const std::uintptr_t want[9] = {0x12027d4, 0x1254698, 0xfa2e7c, 0xfa40ec, 0x2238c04, 0x217d100, 0x217cc30, 0x24acf18, 0x24acce4};
  const std::uintptr_t have[9] = {reinterpret_cast<std::uintptr_t>(f.netGame),   reinterpret_cast<std::uintptr_t>(f.clientProfile), reinterpret_cast<std::uintptr_t>(f.tString),
                                  reinterpret_cast<std::uintptr_t>(f.jsonInt),   reinterpret_cast<std::uintptr_t>(f.isValid),       reinterpret_cast<std::uintptr_t>(f.getArm),
                                  reinterpret_cast<std::uintptr_t>(f.setArm),    reinterpret_cast<std::uintptr_t>(f.invoke),        reinterpret_cast<std::uintptr_t>(f.queue)};
  for (int i = 0; i < 9; ++i) CHECK(have[i] == kBase + want[i], "function %d at %llx, wanted %llx", i, static_cast<unsigned long long>(have[i]), static_cast<unsigned long long>(kBase + want[i]));
}

void TheSwitch() {  // 15
  g_lines.clear();
  nevr_quest::combat::tick::ResetForTest();
  wa::ResetForTest();
  wa::SetLegacyDirForTest(g_dir.c_str());
  const nevr_quest::combat::ModuleResult off = wa::InstallAt([](const char*, sentinel::ElfImage*) { return true; }, nullptr, false);
  CHECK(!off.installed && off.off && std::strcmp(off.status, "off_config") == 0 && wa::Phase() == 2 && nevr_quest::combat::tick::Count() == 0, "off result");
  CHECK(g_lines.size() == 1 && HasLine("combat_weapon_arm_install", "\"result\":\"off\",\"reason\":\"off_config\",\"switch\":\"combat.weapon_arm\"", kInfo), "off line");
  BuildWorld(1);
  for (int i = 0; i < 6; ++i) Pass();
  CHECK(g_setCalls == 0 && wa::CurrentStats().passes == 0, "an off module looked at the game");
}

void TheKillFileIsNotReadAndWarns() {  // 16
  Reset(1, 0, -1);
  std::FILE* m = std::fopen((g_dir + "/t26arm.off").c_str(), "w");
  std::fclose(m);
  g_lines.clear();
  nevr_quest::combat::tick::ResetForTest();
  wa::ResetForTest();
  wa::SetLegacyDirForTest(g_dir.c_str());
  wa::InstallAt([](const char*, sentinel::ElfImage*) { return false; }, nullptr, true);
  const std::vector<Line> legacy = Lines("combat_weapon_arm_legacy_switch");
  CHECK(legacy.size() == 1 && legacy[0].level == sentinel::LogLevel::kWarn && Has(legacy[0].text, "\"file\":\"t26arm.off\"") &&
            Has(legacy[0].text, "\"replaced_by\":\"combat.weapon_arm\""),
        "no legacy warning");
  Reset(1, 0, -1);
  m = std::fopen((g_dir + "/t26arm.off").c_str(), "w");
  std::fclose(m);
  for (int i = 0; i < 5; ++i) Pass();
  CHECK(g_setCalls == 1, "the plugin's kill file still turned the module off");
}


// The reads a level scan makes on this fake game (measured, not assumed), so a path that must read nothing but its scope test can be counted exactly.
std::uint32_t ReadsPerScan(std::uintptr_t game) {
  const std::uint64_t before = SafeReadCalls();
  nevr_quest::combat::level::Scan(game);
  return SafeReadCalls() - before;
}

void ArenaAndOtherLevelsAreLeftAlone() {  // 17
  // The combat lobby is quiet too: its script is shared with the arena lobby, and it is not established that an arena player never has it loaded.
  const char* quiet[] = {"mpl_arena_a", "mpl_arena_a_lowspec", "mpl_arena_b", "mpl_tutorial_a", "mpl_lobby_b", "mpl_lobby_b2", "mpl_lobby_b_social", "mpl_lobby_b_arena",
                         "mpl_lobby_b_combat"};
  for (const char* name : quiet) {
    Reset(1, 0, -1, name);
    const std::uint32_t perScan = ReadsPerScan(reinterpret_cast<std::uintptr_t>(g_game));
    const std::uint64_t reads0 = SafeReadCalls(), scans0 = nevr_quest::combat::level::ScanCalls();
    for (int i = 0; i < 12; ++i) Pass();
    CHECK(g_setCalls == 0 && g_queueCalls == 0 && wa::Phase() == 0, "%s: set %d queued %d phase %d", name, g_setCalls, g_queueCalls, wa::Phase());
    // By count: the only scans are the combat level answer's own looks (combat_scope.h), and nothing else reads: a pass reads no level, no equipment
    // system and no component.
    const std::uint64_t scans = nevr_quest::combat::level::ScanCalls() - scans0, reads = SafeReadCalls() - reads0;
    CHECK(scans == scope::Looks() && scans >= 11, "%s: %llu scans for %u looks", name, static_cast<unsigned long long>(scans), scope::Looks());
    CHECK(reads == scans * perScan, "%s: %llu reads for %llu scans of %u reads each", name, static_cast<unsigned long long>(reads), static_cast<unsigned long long>(scans), perScan);
    CHECK(!FileExists("t26arm.OVR-ORG-1234567890"), "%s: a marker was written", name);
    CHECK(Lines("combat_weapon_arm_waiting").empty() && Lines("combat_weapon_arm_set").empty() && Count("combat_weapon_arm_install") == 1, "%s: the module wrote a line", name);
    CHECK(wa::CurrentStats().scopedOut >= 11, "%s: scoped out %u", name, wa::CurrentStats().scopedOut);
  }
  // a transition holding an arena level beside a combat map: the arena level wins
  Reset(1, 0, -1, "mpl_combat_dyson", "mpl_arena_a");
  for (int i = 0; i < 8; ++i) Pass();
  CHECK(g_setCalls == 0 && !FileExists("t26arm.OVR-ORG-1234567890"), "a transition holding an arena level set the hand");
  // once the arena level is gone the module goes on, the hold starting then
  g_game[0x1f78 / 8] = 1;
  Pass();
  Pass();
  CHECK(g_setCalls == 0, "no hold after leaving the arena level");
  Pass();
  CHECK(g_setCalls == 1, "not set in the combat level");
  // every combat map goes on
  for (const char* name : {"mpl_combat_dyson", "mpl_combat_combustion", "mpl_combat_fission", "mpl_combat_gauss"}) {
    Reset(1, 0, -1, name);
    for (int i = 0; i < 4; ++i) Pass();
    CHECK(g_setCalls == 1, "%s: %d sets", name, g_setCalls);
  }
  // The tick stops (nothing refreshes the answer): a combat map seen a second ago is no longer believed, so nothing is set however long the module's own
  // callback keeps running (combat_scope.h, kStaleAfterMs).
  Reset(1, 0, -1, "mpl_combat_dyson");
  Pass();                                      // one look: a combat map; the hold starts
  for (int i = 0; i < 10; ++i) ModulePass();  // 10 s of the module's callback alone
  // The look is believed for one second (two passes of the 3-pass hold); after it every pass is scoped out, so the hold never ends in a set.
  CHECK(g_setCalls == 0 && g_queueCalls == 0 && wa::CurrentStats().scopedOut >= 8, "a stale answer acted: %d sets, %d queued, %u scoped out", g_setCalls,
        g_queueCalls, wa::CurrentStats().scopedOut);
}

void TheInstallOnAnImage() {  // 18
  combat_test::Image image;
  static combat_test::Image* sImage;
  sImage = &image;
  auto lookup = [](const char* module, sentinel::ElfImage* out) {
    if (!sImage->Present() || std::strcmp(module, sentinel::pinned::kLibR15) != 0) return false;
    *out = sImage->Elf();
    return true;
  };
  const nevr_quest::combat::pinned::Pins pins[] = {nevr_quest::combat::weapon_arm::targets::kPins, nevr_quest::combat::netgame::kPins,
                                                   nevr_quest::combat::gamespace::targets::kPins};
  auto put = [&] { combat_test::PutPins(image, pins, 3); };
  // The tick is installed on this image and the scope row registered its refresh (one tick callback), as the shipped table leaves it.
  auto begin = [&] {
    g_lines.clear();
    nevr_quest::combat::tick::ResetForTest();
    scope::ResetForTest();
    wa::ResetForTest();
    wa::SetLegacyDirForTest(g_dir.c_str());
    world::PutTickSlot(image);
    QCHECK(world::StartScope(lookup) && nevr_quest::combat::tick::Count() == 1);
  };
  static std::uintptr_t sBase;
  static const char* sWrong;
  auto symbols = [](const char* name) -> void* {
    namespace wt = nevr_quest::combat::weapon_arm::targets;
    const nevr_quest::combat::pinned::Symbol* all[] = {&nevr_quest::combat::netgame::kNetGameMTPin, &wt::kClientProfile, &wt::kTString, &wt::kJsonInt, &wt::kIsValid,
                                                       &wt::kGetArm, &wt::kSetArm, &wt::kInvoke, &wt::kQueue};
    for (const auto* s : all) {
      if (std::strcmp(name, s->symbol) == 0) return sWrong != nullptr && std::strcmp(name, sWrong) == 0 ? reinterpret_cast<void*>(sBase + 0x1000) : reinterpret_cast<void*>(sBase + s->vaddr);
    }
    return nullptr;
  };
  image.Build({world::TickRow()});
  put();
  sBase = image.Base();
  sWrong = nullptr;
  begin();
  nevr_quest::combat::ModuleResult r = wa::InstallAt(lookup, symbols, true);
  CHECK(r.installed && std::strcmp(r.status, "installed") == 0 && !r.off && wa::Phase() == 0 && nevr_quest::combat::tick::Count() == 2, "install on the pinned image");
  CHECK(HasLine("combat_weapon_arm_install", "\"named\":9"), "named count not logged");
  begin();
  r = wa::InstallAt(lookup, nullptr, true);
  CHECK(r.installed && HasLine("combat_weapon_arm_install", "\"named\":0"), "fixed addresses only");
  // each of the nine names at the wrong address refuses
  namespace wt = nevr_quest::combat::weapon_arm::targets;
  for (const char* sym : {wt::kClientProfile.symbol, wt::kTString.symbol, wt::kJsonInt.symbol, wt::kIsValid.symbol, wt::kGetArm.symbol, wt::kSetArm.symbol, wt::kInvoke.symbol,
                          wt::kQueue.symbol, nevr_quest::combat::netgame::kNetGameMTSymbol}) {
    sWrong = sym;
    begin();
    r = wa::InstallAt(lookup, symbols, true);
    CHECK(!r.installed && std::strcmp(r.status, "export_elsewhere") == 0 && nevr_quest::combat::tick::Count() == 1, "a name elsewhere installed: %s", sym);
  }
  sWrong = nullptr;
  const std::size_t bent = combat_test::ForEachBentPin(image, pins, 3, [&](const char* what) {
    begin();
    r = wa::InstallAt(lookup, symbols, true);
    CHECK(!r.installed && std::strcmp(r.status, "pinned_word_mismatch") == 0 && nevr_quest::combat::tick::Count() == 1 && HasLine("combat_weapon_arm_pinned_word", what), "pin %s: %s",
          what, r.status);
  });
  CHECK(bent >= 15, "only %zu pins were bent", bent);
  begin();
  for (std::size_t i = 0; i < nevr_quest::combat::tick::kMaxCallbacks; ++i) nevr_quest::combat::tick::Add([](std::uintptr_t) noexcept {});
  r = wa::InstallAt(lookup, symbols, true);
  CHECK(!r.installed && std::strcmp(r.status, "tick_table_full") == 0 && wa::Phase() == 2, "a full tick table installed");
  // another build id (the scope was started on the pinned build; the tick keeps running when the image changes)
  image.Build({world::TickRow()});
  put();
  begin();
  image.Build({}, "0123456789abcdef0123456789abcdef01234567");
  put();
  r = wa::InstallAt(lookup, symbols, true);
  CHECK(!r.installed && std::strcmp(r.status, "build_id_mismatch") == 0 && nevr_quest::combat::tick::Count() == 1, "another build id installed");
  nevr_quest::combat::tick::ResetForTest();
  image.Drop();
}

double Seconds(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

void TheCostInArena() {  // 19: printed (a host number; the module's header says so)
  Reset(1, 0, -1, "mpl_arena_a");
  const int passes = 100000;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < passes; ++i) {
    g_now += 2.0;
    wa::Tick(reinterpret_cast<std::uintptr_t>(g_game));
  }
  const auto t1 = std::chrono::steady_clock::now();
  const int frames = 2000000;
  for (int i = 0; i < frames; ++i) wa::Tick(reinterpret_cast<std::uintptr_t>(g_game));
  const auto t2 = std::chrono::steady_clock::now();
  std::printf("weapon_arm_test: arena pass (profile, the scope word's load and test) %.2f us at 1 Hz, frame with no pass due %.1f ns (x86 host)\n", Seconds(t0, t1) / passes * 1e6,
              Seconds(t1, t2) / frames * 1e9);
  CHECK(wa::CurrentStats().scopedOut >= static_cast<std::uint32_t>(passes) - 1, "the passes were not scoped out");
  // done: a frame is one load
  Reset(1, 1, -1);
  for (int i = 0; i < 4; ++i) Pass();
  CHECK(wa::Phase() == 2, "setup");
  const auto t3 = std::chrono::steady_clock::now();
  for (int i = 0; i < frames; ++i) wa::Tick(reinterpret_cast<std::uintptr_t>(g_game));
  const auto t4 = std::chrono::steady_clock::now();
  std::printf("weapon_arm_test: frame once done %.1f ns (x86 host)\n", Seconds(t3, t4) / frames * 1e9);
}

}  // namespace

int main() {
  nevr_quest::combat::SetReadCounterForTest(&g_safeReads);
  char tmpl[] = "/tmp/combat-wa-XXXXXX";
  if (mkdtemp(tmpl) == nullptr) return 2;
  g_dir = tmpl;
  sentinel::SetLogSink(&Capture);
  TheGunOnTheLeft();
  TheDeferredRoute();
  AlreadyRight();
  AMarkerThatCannotBeWrittenIsLoud();
  AProfileThatIsNotRightIsSetAndOneThatIsRightIsNot();
  AMarkerFromAnEarlierLaunch();
  GivingUp();
  AnInvalidComponent();
  NotFound();
  NoNetGame();
  NoLibrary();
  AScopeThatCannotChangeRefusesTheInstall();
  AnUnwritableDirectory();
  ASecondLogin();
  AMarkerThePluginWroteStopsTheModule();
  TheNineAddresses();
  TheSwitch();
  TheKillFileIsNotReadAndWarns();
  ArenaAndOtherLevelsAreLeftAlone();
  TheInstallOnAnImage();
  TheCostInArena();
  ClearDir();
  rmdir(g_dir.c_str());
  sentinel::SetLogSink(nullptr);
  if (quest_test::Failures() != 0) {
    std::fprintf(stderr, "weapon_arm_test: %d check(s) failed\n", quest_test::Failures());
    return 1;
  }
  std::printf("weapon_arm_test: all checks passed\n");
  return 0;
}
