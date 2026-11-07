// Host test of dronemuzzle (src/quest/combat/dronemuzzle.cpp, #2187): the continuations the two entries call, the install of both entries by the
// real GotHook into two JUMP_SLOTs of a libr15 image built in memory, the frame-time tick, both orders against another handler on a slot, and the
// step's refusals.
//
// On the host the entries are the C++ stand-ins dronemuzzle.cpp builds there (the same decision: the return address, then the continuation or the
// next word); the arm64 entries are the top-level asm() of dronemuzzle_entry.inc, which src/quest/combat/check/dronemuzzle.sh disassembles,
// checks for ldar and runs under qemu (src/quest/tests/dronemuzzle_qemu.cpp).
//
// Moved from the q15t plugin's dronemuzzle_test: the core's oracle, the teleport, the frame tick, the bullet record and the tracer cap are the same
// checks. What changed is the fixture (an image and the manager's install, not a slot page and a swap; the call sites the entries act on are
// calibrated as before), the log lines (sentinel records, rendered as in dronekill_test), and the install cases: the plugin's "no arm64 thunk"
// refusals have no counterpart (the entries are always built), and a slot the manager cannot write is outside the image's writable segment. New
// here: the entry above a handler that was on the slot first, a handler refused above the entry (#2219), and the step's needs_dronekill_shot.
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "quest/combat/combat_level.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/dronekill.h"
#include "quest/combat/dronemuzzle.h"
#include "quest/combat/dronemuzzle_core.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"
#include "quest/tests/combat_libr15_image.h"

// The arena path's read count, through safe_read.h's test seam: every SafeRead after StartReadCount adds one.
namespace {
std::atomic<std::uint64_t> g_arenaReads{0};
[[maybe_unused]] void StartReadCount() {
  g_arenaReads.store(0);
  nevr_quest::combat::SetReadCounterForTest(&g_arenaReads);
}
[[maybe_unused]] std::uint64_t ReadCount() { return g_arenaReads.load(); }
}  // namespace

using namespace nevr_quest::combat::dronemuzzle;
namespace dk = nevr_quest::combat::dronekill;
using sentinel::GotStatus;

static int g_fail = 0;
#define CHECK(cond, ...)                               \
  do {                                                 \
    if (!(cond)) {                                     \
      ++g_fail;                                        \
      std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
      std::printf(__VA_ARGS__);                        \
      std::printf("\n");                               \
    }                                                  \
  } while (0)

struct Line {
  uint32_t level;
  std::string text;
};
static std::vector<Line> g_lines;
// A sentinel record as the plugin test's lines read: `<event> key=value ...` (string values unquoted), at the plugin's level numbers (1 Info,
// 2 Warning, 3 Error, 0 Debug), so a check names an event or a field.
static std::string Render(const char* json) {
  std::string out, key;
  const char* p = json;
  bool first = true;
  while (*p != 0) {
    if (*p != '"') {
      ++p;
      continue;
    }
    std::string k;
    for (++p; *p != 0 && *p != '"'; ++p) k += *p;
    if (*p == '"') ++p;
    while (*p == ':' || *p == ' ') ++p;
    std::string v;
    if (*p == '"') {
      for (++p; *p != 0 && *p != '"'; ++p) {
        if (*p == '\\' && p[1] != 0) ++p;
        v += *p;
      }
      if (*p == '"') ++p;
    } else {
      while (*p != 0 && *p != ',' && *p != '}') v += *p++;
    }
    if (k == "ts_ms" || k == "level" || k == "ts") continue;
    if (k == "event") {
      out = v + out;
      first = false;
      continue;
    }
    out += " " + k + "=" + v;
  }
  (void)first;
  return out;
}
static void Sink(sentinel::LogLevel level, const char* line) {
  const uint32_t lv = level == sentinel::LogLevel::kInfo ? 1 : level == sentinel::LogLevel::kWarn ? 2 : level == sentinel::LogLevel::kError ? 3 : 0;
  g_lines.push_back({lv, Render(line)});
}
static int CountLine(const char* needle, int level = -1) {
  int n = 0;
  for (const auto& l : g_lines)
    if (l.text.find(needle) != std::string::npos && (level < 0 || (int)l.level == level)) ++n;
  return n;
}
static bool HasLine(const char* needle, int level = -1) { return CountLine(needle, level) > 0; }
static void Dump(const char* title) {  // Q15T_TEST_VERBOSE=1: the module's own log records of that scenario
  if (std::getenv("Q15T_TEST_VERBOSE") == nullptr) return;
  std::printf("---- %s\n", title);
  for (const auto& l : g_lines) std::printf("[%u] %s\n", l.level, l.text.c_str());
}

// ================================================================ the core: a trigonometric oracle, and hand values
namespace ref {  // the frame-time average, written out again from its rule
float Tick(float g_frame_dt, float dt) {
  if (dt > 1.0f / 240.0f && dt < 0.1f) g_frame_dt = g_frame_dt * 0.9f + dt * 0.1f;
  return g_frame_dt;
}
// The spawn from the aim's yaw and pitch, with the basis from trigonometry rather than from the core's formulas. The aim is
// Z = (cos p sin y, sin p, cos p cos y); X = (cos y, 0, -sin y); Y = (-sin p sin y, cos p, -sin p cos y), which is Z x X for that X.
// When cos p < 1e-4 the rule's fallback applies: X = (1, 0, 0) and Y = Z x X = (0, 0, -sin p). The back-off is min(|v| dt, 0.25) m
// along -Z, with dt clamped to 1/120..1/30 s. The result is in double; the test compares it with the float result.
constexpr double kPi = 3.14159265358979323846;
void TrigSpawn(const float* root, double yaw, double pitch, double speed, double frame_dt, double* out) {
  const double cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(yaw), sy = std::sin(yaw);
  const double Z[3] = {cp * sy, sp, cp * cy};
  double X[3] = {cy, 0.0, -sy};
  double Y[3] = {-sp * sy, cp, -sp * cy};
  if (cp < 1e-4) {
    X[0] = 1.0;
    X[1] = 0.0;
    X[2] = 0.0;
    Y[0] = 0.0;
    Y[1] = 0.0;
    Y[2] = -sp;
  }
  const double dt = frame_dt < 1.0 / 120.0 ? 1.0 / 120.0 : frame_dt > 1.0 / 30.0 ? 1.0 / 30.0 : frame_dt;
  const double back = std::fmin(speed * dt, 0.25);
  for (int k = 0; k < 3; ++k)
    out[k] = root[k] + dm::kMuzzle[0] * X[k] + dm::kMuzzle[1] * Y[k] + dm::kMuzzle[2] * Z[k] - back * Z[k];
}
}  // namespace ref

static void TestCore() {
  using namespace dm;
  unsigned x = 777;
  auto rnd = [&x]() {
    x = x * 1103515245u + 12345u;
    return static_cast<float>((x >> 8) & 0xffff) / 65535.0f;
  };
  // 200000 random aims given by yaw and pitch: the spawn against ref::TrigSpawn to 1e-5 m (float rounding is about 2e-6 m at |root| <= 60 m).
  // Every 97th aim is vertical (pitch +-pi/2), which takes the rule's X fallback.
  int checked = 0;
  for (int i = 0; i < 200000; ++i) {
    const double yaw = (rnd() * 2.0 - 1.0) * ref::kPi;
    const double pitch = i % 97 == 0 ? (i % 2 == 0 ? 1.0 : -1.0) * ref::kPi / 2 : (rnd() * 2.0 - 1.0) * 1.5;
    const double speed = 0.5 + rnd() * 299.5;
    const double cp = std::cos(pitch), sp = std::sin(pitch), cy = std::cos(yaw), sy = std::sin(yaw);
    const float v[3] = {static_cast<float>(speed * cp * sy), static_cast<float>(speed * sp), static_cast<float>(speed * cp * cy)};
    const float root[3] = {rnd() * 80 - 40, rnd() * 20 - 5, rnd() * 80 - 40};
    const float dt = rnd() * 0.08f;
    double want[3];
    ref::TrigSpawn(root, yaw, pitch, speed, dt, want);
    float got[3];
    if (!MuzzleMove(root, v, dt, got)) {
      CHECK(false, "a random aim was refused at %d", i);
      break;
    }
    bool same = true;
    for (int k = 0; k < 3; ++k) same = same && std::fabs(got[k] - want[k]) < 1e-5;
    if (!same) {
      CHECK(false, "aim %d: got %f %f %f want %f %f %f", i, got[0], got[1], got[2], want[0], want[1], want[2]);
      break;
    }
    ++checked;
  }
  CHECK(checked == 200000, "checked %d aims", checked);
  float avg = 1.0f / 90.0f, avg_ref = avg;
  for (int i = 0; i < 100000; ++i) {
    const float dt = rnd() * 0.12f;
    avg = FrameDt(avg, dt);
    avg_ref = ref::Tick(avg_ref, dt);
    if (avg != avg_ref) {
      CHECK(false, "FrameDt differs at step %d", i);
      break;
    }
  }
  const float root[3] = {0, 0, 0};
  float out[3];
  const float v[3] = {0, 0, 120};
  CHECK(MuzzleMove(root, v, 1.0f / 90.0f, out), "muzzle");
  CHECK(std::fabs(out[0] - 0.000024f) < 1e-6f && std::fabs(out[1] - 0.075634f) < 1e-6f && std::fabs(out[2] - (0.147283f - 0.25f)) < 1e-6f, "muzzle shape");
  const float slow[3] = {3, 0, 0};
  CHECK(MuzzleMove(root, slow, 0.5f, out) && std::fabs(out[0] - (0.147283f - 0.1f)) < 1e-5f, "slow: one clamped frame back");
  const float none[3] = {0, 0, 0};
  CHECK(!MuzzleMove(root, none, 0.01f, out), "zero velocity moved");
  const float nanv[3] = {NAN, 0, 0};
  CHECK(!MuzzleMove(root, nanv, 0.01f, out), "a NaN velocity moved");
  const float tiny[3] = {0, 0, 1e-5f};
  CHECK(!MuzzleMove(root, tiny, 0.01f, out), "a 1e-5 velocity moved");
  const float small[3] = {0, 0, 2e-4f};
  CHECK(MuzzleMove(root, small, 0.01f, out), "a 2e-4 velocity was refused");
  CHECK(kMuzzle[0] == 0.000024f && kMuzzle[1] == 0.075634f && kMuzzle[2] == 0.147283f && kBackMax == 0.25f, "muzzle constants");
  CHECK(ClampDt(0.001f) == 1.0f / 120.0f && ClampDt(1.0f) == 1.0f / 30.0f && ClampDt(0.02f) == 0.02f, "ClampDt");
  CHECK(FrameDt(0.5f, 0.2f) == 0.5f && FrameDt(0.5f, 0.001f) == 0.5f, "FrameDt bounds");
}

// ================================================================ fakes
using TpFn = uint64_t (*)(void*, uint64_t, const uint8_t*, uint64_t);
using FireFn = uint64_t (*)(uint8_t*, uint64_t, const void*, float, const void*, const void*, uint64_t, uint64_t);

struct TpCall {
  void* gs;
  uint64_t actor;
  const uint8_t* pose;
  uint8_t copy[32];
  uint64_t reason;
};
static std::vector<TpCall> g_tp;
static uint64_t FakeTeleport(void* gs, uint64_t actor, const uint8_t* pose, uint64_t reason) {
  TpCall c{gs, actor, pose, {}, reason};
  if (pose != nullptr && reinterpret_cast<uintptr_t>(pose) > 0x10000) memcpy(c.copy, pose, sizeof c.copy);
  g_tp.push_back(c);
  return 0x1234;
}

alignas(16) static uint8_t g_bcs[0x200];
alignas(16) static uint16_t g_index[0x40];
alignas(16) static uint8_t g_records[0x298 * 4];
static const uint64_t kHandle = 0x00030002;  // index 2 -> slot 1
struct FireCallRec {
  uint8_t* bcs;
  uint64_t handle;
  const void *gun, *quat, *vel;
  float speed;
  uint64_t remote, flag;
};
static std::vector<FireCallRec> g_fire;
static uint8_t* Rec() { return g_records + 1 * 0x298; }
static uint64_t FakeFireReal(uint8_t* bcs, uint64_t handle, const void* gun, float speed, const void* quat, const void* vel, uint64_t remote, uint64_t flag) {
  g_fire.push_back({bcs, handle, gun, quat, vel, speed, remote, flag});
  const float v[3] = {1, 2, 3}, p[3] = {4, 5, 6}, sp[3] = {7, 8, 9};  // FireBullet fills the record with the local player's values
  memcpy(Rec() + 0x11c, v, 12);
  memcpy(Rec() + 0x128, p, 12);
  memcpy(Rec() + 0x134, sp, 12);
  return 0x5678;
}

// the return-address recorders used to calibrate the "FireBulletLocal" call sites below
static uintptr_t g_rec_tp = 0, g_rec_fire = 0;
__attribute__((noinline)) static uint64_t RecTp(void*, uint64_t, const uint8_t*, uint64_t) {
  g_rec_tp = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  return 0;
}
__attribute__((noinline)) static uint64_t RecFire(uint8_t*, uint64_t, const void*, float, const void*, const void*, uint64_t, uint64_t) {
  g_rec_fire = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  return 0;
}
// two call sites per function: "In" = the site the thunks act on (FireBulletLocal's), "Out" = any other caller
__attribute__((noinline)) static uint64_t TpIn(TpFn f, const uint8_t* pose) {
  volatile uint64_t r = f(reinterpret_cast<void*>(0xa0), 0xb0, pose, 0xc0);
  return r;
}
__attribute__((noinline)) static uint64_t TpOut(TpFn f, const uint8_t* pose) {
  volatile uint64_t r = f(reinterpret_cast<void*>(0xa0), 0xb0, pose, 0xc0);
  return r;
}
__attribute__((noinline)) static uint64_t FireIn(FireFn f) {
  volatile uint64_t r = f(g_bcs, kHandle, reinterpret_cast<void*>(0x1), 2.5f, reinterpret_cast<void*>(0x3), reinterpret_cast<void*>(0x4), 5, 6);
  return r;
}
__attribute__((noinline)) static uint64_t FireOut(FireFn f) {
  volatile uint64_t r = f(g_bcs, kHandle, reinterpret_cast<void*>(0x1), 2.5f, reinterpret_cast<void*>(0x3), reinterpret_cast<void*>(0x4), 5, 6);
  return r;
}

static uint64_t g_now_ns = 5000000000ull;
static uint64_t ClkNs() { return g_now_ns; }

static uintptr_t g_base = 0;
using combat_test::Image;
static Image g_image;
static constexpr uint64_t kRw = Image::kRwStart;
static constexpr uint64_t kSlotTp = kRw + 0x20, kSlotFire = kRw + 0x28, kExecFire = 0x2028, kExecTp = 0x2020;
static uintptr_t& W(uint64_t vaddr) { return *reinterpret_cast<uintptr_t*>(g_base + vaddr); }
static uintptr_t V(const void* fn) { return reinterpret_cast<uintptr_t>(fn) - g_base; }
static bool Lookup(const char* module, sentinel::ElfImage* out) {
  if (!g_image.Present() || std::strcmp(module, sentinel::pinned::kLibR15) != 0) return false;
  *out = g_image.Elf();
  return true;
}
static uint64_t g_tpSlot = kSlotTp, g_fireSlot = kSlotFire;

// A fresh libr15 image whose two JUMP_SLOTs hold the fakes; a slot `outside` the writable segment the manager refuses to write.
static void NewPage(bool fire_outside, bool tp_outside = false) {
  g_tpSlot = tp_outside ? kExecTp : kSlotTp;
  g_fireSlot = fire_outside ? kExecFire : kSlotFire;
  g_image.Build({{kTeleportSymbol, g_tpSlot, false, false, 0}, {kFireSymbol, g_fireSlot, false, false, 0}});
  g_base = g_image.Base();
  W(g_tpSlot) = reinterpret_cast<uintptr_t>(&FakeTeleport);
  W(g_fireSlot) = reinterpret_cast<uintptr_t>(&FakeFireReal);
}

static Pins Slots() {
  // calibrate: the return addresses of the "In" call sites
  TpIn(&RecTp, nullptr);
  FireIn(&RecFire);
  return {g_tpSlot, V(reinterpret_cast<void*>(&FakeTeleport)), g_rec_tp - g_base, g_fireSlot, V(reinterpret_cast<void*>(&FakeFireReal)), g_rec_fire - g_base};
}

static void BuildBullet() {
  memset(g_bcs, 0, sizeof g_bcs);
  memset(g_index, 0, sizeof g_index);
  memset(g_records, 0xA5, sizeof g_records);  // a canary everywhere
  const uintptr_t idx = reinterpret_cast<uintptr_t>(g_index), recs = reinterpret_cast<uintptr_t>(g_records);
  memcpy(g_bcs + 0xc8, &idx, 8);
  memcpy(g_bcs + 0xf8, &recs, 8);
  g_index[(kHandle & 0xffff) * 2] = 1;  // slot 1
}

static Result Setup() {
  ResetForTest();
  dk::ResetForTest();
  g_lines.clear();
  g_tp.clear();
  g_fire.clear();
  g_now_ns = 5000000000ull;
  SetClockForTest(&ClkNs);
  BuildBullet();
  NewPage(false);
  dk::SetShotActiveForTest(true);
  dk::SetScopeForTest(true);  // a combat map is loaded: the frame tick times frames
  return InstallAt(&Lookup, Slots());
}

static TpFn TpSlot() { return reinterpret_cast<TpFn>(W(g_tpSlot)); }
static FireFn FireSlot() { return reinterpret_cast<FireFn>(W(g_fireSlot)); }
static void Shot(float x, float y, float z, int remote = 0) {
  const float v[3] = {x, y, z};
  dk::SetShotForTest(v, remote);
}
static void NoShot() { dk::SetShotForTest(nullptr, 0); }

alignas(16) static uint8_t g_pose[32];
static void SetPose(float px, float py, float pz) {
  for (int i = 0; i < 32; ++i) g_pose[i] = static_cast<uint8_t>(0x40 + i);  // quaternion / scale bytes the module must not touch
  const float p[3] = {px, py, pz};
  memcpy(g_pose + 0x10, p, 12);
}

// ================================================================ tests
static void TestInstallPinned() {
  const Pins& k = kPins;
  CHECK(k.teleportSlot == 0x36cfa18 && k.teleportFn == 0x180a384 && k.teleportRet == 0x210c22c, "teleport constants");
  CHECK(k.fireSlot == 0x36f3cf8 && k.fireFn == 0x210c9c4 && k.fireRet == 0x210c2a4, "fire constants");
  CHECK(std::strcmp(kTeleportSymbol, "_ZN10NRadEngine13CncaGameSpace13WorldTeleportENS_6SActorENS_8CTransfQENS_24EComponentTeleportReasonE") == 0 &&
            std::strcmp(kFireSymbol, "_ZN10NRadEngine8NRadGame15CR15NetBulletCS10FireBulletENS_18SCompactPoolHandleENS_16CComponentHandleINS0_12CR15NetGunCSEEEfRKNS_11CQuaternionERKNS_8C3VectorEjj") == 0,
        "the symbols are the readelf names");
  // the production slots on an image holding them; the functions are the fakes (an original must be code)
  ResetForTest();
  g_lines.clear();
  g_image.Build({{kTeleportSymbol, k.teleportSlot, false, false, 0}, {kFireSymbol, k.fireSlot, false, false, 0}});
  g_base = g_image.Base();
  W(k.teleportSlot) = reinterpret_cast<uintptr_t>(&FakeTeleport);
  W(k.fireSlot) = reinterpret_cast<uintptr_t>(&FakeFireReal);
  Pins pins = k;
  pins.teleportFn = V(reinterpret_cast<void*>(&FakeTeleport));
  pins.fireFn = V(reinterpret_cast<void*>(&FakeFireReal));
  const Result r = InstallAt(&Lookup, pins);
  CHECK(r.ok && r.teleport == GotStatus::kOk && r.fire == GotStatus::kOk, "pinned install");
  CHECK(W(k.teleportSlot) == reinterpret_cast<uintptr_t>(&nevr_combat_dm_tp_entry) && W(k.fireSlot) == reinterpret_cast<uintptr_t>(&nevr_combat_dm_fire_entry),
        "the words are not the entries");
  const Words w = WordsForTest();
  CHECK(w.teleportRet == g_base + 0x210c22c && w.fireRet == g_base + 0x210c2a4, "return words: %llx %llx", (unsigned long long)w.teleportRet,
        (unsigned long long)w.fireRet);
  CHECK(w.teleportNext == reinterpret_cast<uintptr_t>(&FakeTeleport) && w.fireNext == reinterpret_cast<uintptr_t>(&FakeFireReal), "next words");
  CHECK(CountLine("combat_dronemuzzle_hook", 1) == 2 && HasLine("result=installed", 1), "install lines");
  CHECK(HasLine("muzzle_x=0.000024") && HasLine("muzzle_y=0.075634") && HasLine("muzzle_z=0.147283") && HasLine("back_max_m=0.250000"),
        "the install line lacks a muzzle constant or the back-off cap");
  ResetForTest();
}

static void TestInstallRefusals() {
  // a slot that does not hold the pinned function: nothing installed
  ResetForTest();
  g_lines.clear();
  NewPage(false);
  Pins s = Slots();
  W(g_fireSlot) = reinterpret_cast<uintptr_t>(&FakeTeleport);  // code, but not FireBullet
  Result r = InstallAt(&Lookup, s);
  CHECK(!r.ok && r.fire == GotStatus::kOriginalMismatch && W(g_tpSlot) == reinterpret_cast<uintptr_t>(&FakeTeleport) &&
            W(g_fireSlot) == reinterpret_cast<uintptr_t>(&FakeTeleport),
        "foreign slot word: a word changed (%s)", sentinel::GotStatusName(r.fire));
  CHECK(HasLine("result=refused", 3) && WordsForTest().teleportRet == 0 && WordsForTest().fireRet == 0, "refusal not loud / words left set");
  // the second entry cannot be installed (outside the writable segment): the first is removed
  ResetForTest();
  g_lines.clear();
  NewPage(true);
  r = InstallAt(&Lookup, Slots());
  CHECK(!r.ok && r.fire == GotStatus::kSlotOutsideImage, "outside: %s", sentinel::GotStatusName(r.fire));
  CHECK(W(g_tpSlot) == reinterpret_cast<uintptr_t>(&FakeTeleport), "WorldTeleport's entry was not removed");
  CHECK(HasLine("combat_dronemuzzle_rollback") && HasLine("teleport_removed=1"), "rollback not reported");
  CHECK(HasLine("combat_dronemuzzle_hook", 3), "the failed install was not an Error line");
  // WorldTeleport's entry cannot be installed: FireBullet's is not touched either
  ResetForTest();
  g_lines.clear();
  NewPage(false, true);
  r = InstallAt(&Lookup, Slots());
  CHECK(!r.ok && r.teleport == GotStatus::kSlotOutsideImage, "teleport outside: %s", sentinel::GotStatusName(r.teleport));
  CHECK(W(g_fireSlot) == reinterpret_cast<uintptr_t>(&FakeFireReal) && r.fire == GotStatus::kNotInstalled, "FireBullet's entry went in after WorldTeleport's failed");
  CHECK(HasLine("combat_dronemuzzle_hook", 3) && HasLine("result=refused", 3), "the failed WorldTeleport install was not loud");
  // no libr15 loaded: refused, nothing set
  ResetForTest();
  g_lines.clear();
  NewPage(false);
  r = InstallAt([](const char*, sentinel::ElfImage*) { return false; }, Slots());
  CHECK(!r.ok && std::strcmp(r.status, "module_not_loaded") == 0 && W(g_tpSlot) == reinterpret_cast<uintptr_t>(&FakeTeleport) &&
            WordsForTest().teleportRet == 0,
        "no libr15: %s", r.status);
  // a build that is not the pinned one: refused, nothing set
  ResetForTest();
  g_lines.clear();
  g_image.Build({{kTeleportSymbol, kSlotTp, false, false, 0}, {kFireSymbol, kSlotFire, false, false, 0}}, "0000000000000000000000000000000000000000");
  g_base = g_image.Base();
  W(kSlotTp) = reinterpret_cast<uintptr_t>(&FakeTeleport);
  W(kSlotFire) = reinterpret_cast<uintptr_t>(&FakeFireReal);
  r = InstallAt(&Lookup, Slots());
  CHECK(!r.ok && std::strcmp(r.status, "build_id_mismatch") == 0 && W(kSlotTp) == reinterpret_cast<uintptr_t>(&FakeTeleport), "another build: %s", r.status);
  // the continuations before any install (no next function): return 0, call nothing
  ResetForTest();
  g_tp.clear();
  g_fire.clear();
  CHECK(nevr_combat_dm_teleport(nullptr, 0, nullptr, 0) == 0 && nevr_combat_dm_fire(nullptr, 0, nullptr, 0.0f, nullptr, nullptr, 0, 0) == 0 &&
            g_tp.empty() && g_fire.empty(),
        "a continuation ran with no next function");
  // the step: off, dronekill's shot missing, a second install
  ResetForTest();
  dk::ResetForTest();
  nevr_quest::combat::tick::ResetForTest();
  g_lines.clear();
  NewPage(false);
  const Pins pins = Slots();
  SetInstallForTest(&Lookup, &pins);
  nevr_quest::combat::ModuleResult m = Install(false);
  CHECK(!m.installed && std::strcmp(m.status, "off") == 0 && HasLine("combat_dronemuzzle_install result=off", 1) &&
            W(g_tpSlot) == reinterpret_cast<uintptr_t>(&FakeTeleport),
        "off");
  m = Install(true);
  CHECK(!m.installed && std::strcmp(m.status, "needs_dronekill_shot") == 0 && W(g_tpSlot) == reinterpret_cast<uintptr_t>(&FakeTeleport) &&
            nevr_quest::combat::tick::Count() == 0,
        "no dronekill shot: %s", m.status);
  dk::SetShotActiveForTest(true);
  m = Install(true);
  CHECK(m.installed && std::strcmp(m.status, "installed") == 0 && nevr_quest::combat::tick::Count() == 1, "on: %s", m.status);
  m = Install(true);
  CHECK(!m.installed && std::strcmp(m.status, "already_ran") == 0, "a second step: %s", m.status);
  nevr_quest::combat::tick::ResetForTest();
  ResetForTest();
  dk::ResetForTest();
}

// Another handler on WorldTeleport's slot (a CallbackThunk, as another module's would be). Installed first: the entry goes in above it, acts
// on FireBulletLocal's call and calls it as the next word for every call. Installed after the entry: refused (slot_held_by_entry), the entry
// stays the newest and keeps acting.
struct OtherTag {};
using OtherThunk = sentinel::CallbackThunk<OtherTag, uint64_t(void*, uint64_t, const uint8_t*, uint64_t)>;
static int g_otherCalls = 0;
static uint64_t OtherHandler(OtherThunk::Fn original, void* gs, uint64_t actor, const uint8_t* pose, uint64_t reason) noexcept {
  ++g_otherCalls;
  return original(gs, actor, pose, reason);
}
NEVR_HOOK_RECORD(kOtherHook, OtherThunk, &OtherHandler);

static void TestAnotherHandlerOnTheSlot() {
  for (const bool otherFirst : {true, false}) {
    ResetForTest();
    dk::ResetForTest();
    OtherThunk::Reset();
    g_lines.clear();
    g_tp.clear();
    NewPage(false);
    const Pins pins = Slots();
    sentinel::GotHook other;
    sentinel::GotTarget target = Target(kTeleportSymbol, pins.teleportSlot, g_base, pins.teleportFn);
    target.owner = "test_other";
    OtherThunk::Arm(kOtherHook);
    if (otherFirst) CHECK(sentinel::InstallThunk<OtherThunk>(other, target, &Lookup) == GotStatus::kOk, "the other handler did not install first");
    const Result r = InstallAt(&Lookup, pins);
    CHECK(r.ok, "%s: the entry did not install (%s)", otherFirst ? "other first" : "entry first", sentinel::GotStatusName(r.teleport));
    if (!otherFirst) {
      const GotStatus refused = sentinel::InstallThunk<OtherThunk>(other, target, &Lookup);
      CHECK(refused == GotStatus::kSlotHeldByEntry, "a handler joined above the entry: %s", sentinel::GotStatusName(refused));
    }
    CHECK(W(g_tpSlot) == reinterpret_cast<uintptr_t>(&nevr_combat_dm_tp_entry), "the entry is not the newest");
    g_otherCalls = 0;
    SetPose(10, 20, 30);
    Shot(0, 0, 120);
    CHECK(TpIn(TpSlot(), g_pose) == 0x1234 && g_tp.size() == 1 && g_tp[0].pose != g_pose, "the entry did not act on FireBulletLocal's call");
    CHECK(TpOut(TpSlot(), g_pose) == 0x1234 && g_tp.size() == 2 && g_tp[1].pose == g_pose, "another caller's call was changed");
    CHECK(g_otherCalls == (otherFirst ? 2 : 0), "the other handler ran %d times", g_otherCalls);
    NoShot();
    other.Forget();
    OtherThunk::Reset();
    ResetForTest();
  }
}

static void TestTeleport() {
  Result r = Setup();
  CHECK(r.ok, "install");
  // not a drone shot (the accessor is null): the call goes through with its arguments, result and pose pointer untouched
  NoShot();
  SetPose(1, 2, 3);
  CHECK(TpIn(TpSlot(), g_pose) == 0x1234, "result");
  CHECK(g_tp.size() == 1 && g_tp[0].gs == reinterpret_cast<void*>(0xa0) && g_tp[0].actor == 0xb0 && g_tp[0].pose == g_pose && g_tp[0].reason == 0xc0,
        "not a drone shot: arguments");
  CHECK(CurrentStats().shots == 0, "counted a shot");
  // a drone shot from FireBulletLocal's call site: the pose goes on as a moved COPY
  g_tp.clear();
  Shot(0, 0, 120, 1);
  SetPose(10, 20, 30);
  uint8_t before[32];
  memcpy(before, g_pose, 32);
  CHECK(TpIn(TpSlot(), g_pose) == 0x1234, "result");
  CHECK(g_tp.size() == 1 && g_tp[0].pose != g_pose, "the caller's pose was passed on, not a copy");
  CHECK(memcmp(before, g_pose, 32) == 0, "the caller's pose was written");
  const float root[3] = {10, 20, 30};
  float spawn[3];
  const float vv[3] = {0, 0, 120};
  CHECK(dm::MuzzleMove(root, vv, 1.0f / 90.0f, spawn), "expected spawn");
  float got[3];
  memcpy(got, g_tp[0].copy + 0x10, 12);
  CHECK(std::fabs(got[0] - spawn[0]) < 1e-6f && std::fabs(got[1] - spawn[1]) < 1e-6f && std::fabs(got[2] - spawn[2]) < 1e-6f, "spawn (%f %f %f) want (%f %f %f)", got[0],
        got[1], got[2], spawn[0], spawn[1], spawn[2]);
  CHECK(memcmp(g_tp[0].copy, before, 0x10) == 0 && memcmp(g_tp[0].copy + 0x1c, before + 0x1c, 4) == 0, "the quaternion / scale bytes changed");
  CHECK(g_tp[0].gs == reinterpret_cast<void*>(0xa0) && g_tp[0].actor == 0xb0 && g_tp[0].reason == 0xc0, "other arguments");
  CHECK(CurrentStats().shots == 1 && HasLine("combat_dronemuzzle_shot") && HasLine("remote=1") && HasLine("velocity=0.000000 0.000000 120.000000"), "shot line");
  // the same drone shot from any OTHER caller: untouched
  g_tp.clear();
  CHECK(TpOut(TpSlot(), g_pose) == 0x1234 && g_tp.size() == 1 && g_tp[0].pose == g_pose, "another caller's pose was moved");
  CHECK(CurrentStats().shots == 1, "another caller counted");
  // |v| ~ 0: untouched
  g_tp.clear();
  Shot(0, 0, 5e-5f);
  CHECK(TpIn(TpSlot(), g_pose) == 0x1234 && g_tp.size() == 1 && g_tp[0].pose == g_pose, "a zero-velocity shot moved the pose");
  // an unreadable pose pointer: goes through untouched
  g_tp.clear();
  Shot(0, 0, 120);
  CHECK(TpIn(TpSlot(), reinterpret_cast<const uint8_t*>(0x1000)) == 0x1234 && g_tp.size() == 1 && g_tp[0].pose == reinterpret_cast<const uint8_t*>(0x1000),
        "unreadable pose");
  Dump("teleport scenario so far");
  // shot lines are capped at 40, the counter goes on
  g_lines.clear();
  for (int i = 0; i < 45; ++i) TpIn(TpSlot(), g_pose);
  CHECK(CountLine("combat_dronemuzzle_shot", 1) == 39, "shot lines after 45 more: %d", CountLine("combat_dronemuzzle_shot", 1));  // shots 2..40 are written
  CHECK(CountLine("combat_dronemuzzle_cap", 1) == 1, "cap line");
  CHECK(CurrentStats().shots == 46, "shots %u", CurrentStats().shots);
  NoShot();
}

static float SlowShotZ() {
  Shot(0, 0, 3);
  SetPose(0, 0, 0);
  g_tp.clear();
  TpIn(TpSlot(), g_pose);
  float got[3] = {0, 0, 0};
  if (!g_tp.empty()) memcpy(got, g_tp[0].copy + 0x10, 12);
  NoShot();
  return got[2];
}

static void TestFrameTick() {
  Setup();
  {  // before any tick the frame time is the 1/90 s default
    const float root0[3] = {0, 0, 0}, v0[3] = {0, 0, 3};
    float d0[3];
    CHECK(dm::MuzzleMove(root0, v0, 1.0f / 90.0f, d0), "expected default");
    CHECK(std::fabs(SlowShotZ() - d0[2]) < 1e-7f, "the frame time before any tick is not 1/90 s");
  }
  {  // the first tick only records the time: a 20 ms "frame" since the epoch is not a frame
    Setup();
    g_now_ns = 20000000ull;
    Tick(0);
    const float root0[3] = {0, 0, 0}, v0[3] = {0, 0, 3};
    float d0[3];
    CHECK(dm::MuzzleMove(root0, v0, 1.0f / 90.0f, d0), "expected default");
    CHECK(std::fabs(SlowShotZ() - d0[2]) < 1e-7f, "the first tick fed the frame time");
  }
  Setup();
  Shot(0, 0, 3);  // slow: the "back by one frame of travel" depends on the frame time
  SetPose(0, 0, 0);
  const float root[3] = {0, 0, 0}, v[3] = {0, 0, 3};
  float avg = 1.0f / 90.0f;
  Tick(0);  // the first call only records the time
  for (int i = 0; i < 60; ++i) {
    g_now_ns += 16666667ull;  // 60 fps
    Tick(0);
    avg = dm::FrameDt(avg, 0.016666667f);
  }
  float want[3];
  CHECK(dm::MuzzleMove(root, v, avg, want), "expected");
  g_tp.clear();
  TpIn(TpSlot(), g_pose);
  float got[3];
  memcpy(got, g_tp[0].copy + 0x10, 12);
  CHECK(std::fabs(got[2] - want[2]) < 1e-6f, "frame average not used: got %f want %f", got[2], want[2]);
  float old[3];
  CHECK(dm::MuzzleMove(root, v, 1.0f / 90.0f, old) && std::fabs(old[2] - want[2]) > 1e-4f, "the test cannot tell the default from the average");
  // a hitch (>= 0.1 s) leaves the average alone
  const float before = got[2];
  g_now_ns += 500000000ull;
  Tick(0);
  g_tp.clear();
  TpIn(TpSlot(), g_pose);
  memcpy(got, g_tp[0].copy + 0x10, 12);
  CHECK(std::fabs(got[2] - before) < 1e-7f, "a 0.5 s hitch fed the average");
  NoShot();
}

// Out of a combat map (dronekill's scope) the tick reads no clock and forgets the last frame: the first frame back only records the time.
static int g_clockReads = 0;
static uint64_t CountingClk() {
  ++g_clockReads;
  return g_now_ns;
}
static void TestFrameTickOutOfScope() {
  Setup();
  SetClockForTest(&CountingClk);
  g_clockReads = 0;
  dk::SetScopeForTest(false);
  for (int i = 0; i < 10; ++i) {
    g_now_ns += 16666667ull;
    Tick(0);
  }
  CHECK(g_clockReads == 0, "the tick read the clock %d times out of scope", g_clockReads);
  dk::SetScopeForTest(true);
  Tick(0);  // records the time only
  g_now_ns += 16666667ull;
  Tick(0);
  CHECK(g_clockReads == 2, "in scope: %d clock reads", g_clockReads);
  dk::SetScopeForTest(false);
  Tick(0);
  dk::SetScopeForTest(true);
  g_now_ns += 50000000ull;  // 50 ms out of scope: a frame time the average would take, but not one frame
  Tick(0);
  g_now_ns += 16666667ull;
  Tick(0);
  Shot(0, 0, 3);
  SetPose(0, 0, 0);
  g_tp.clear();
  TpIn(TpSlot(), g_pose);
  float got[3] = {0, 0, 0};
  if (!g_tp.empty()) memcpy(got, g_tp[0].copy + 0x10, 12);
  const float root[3] = {0, 0, 0}, v[3] = {0, 0, 3};
  float want[3];
  CHECK(dm::MuzzleMove(root, v, dm::FrameDt(dm::FrameDt(1.0f / 90.0f, 0.016666667f), 0.016666667f), want), "expected");
  CHECK(std::fabs(got[2] - want[2]) < 1e-6f, "the frames around the time out of scope: got %f want %f", got[2], want[2]);
  NoShot();
}

// Arena (no combat map, so dronekill makes no shot frame): every WorldTeleport and FireBullet call, from FireBulletLocal's call site or any other,
// and the frame tick read no game memory and scan no level, counted; each call reaches the game's function unchanged.
static void TestArenaPaysNothing() {
  Setup();
  BuildBullet();
  NoShot();
  dk::SetScopeForTest(false);
  SetPose(1, 2, 3);
  g_tp.clear();
  g_fire.clear();
  StartReadCount();
  nevr_quest::combat::level::ResetScanCallsForTest();
  CHECK(TpIn(TpSlot(), g_pose) == 0x1234 && TpOut(TpSlot(), g_pose) == 0x1234 && FireIn(FireSlot()) == 0x5678 && FireOut(FireSlot()) == 0x5678, "results");
  for (int i = 0; i < 10; ++i) Tick(0);
  CHECK(ReadCount() == 0 && nevr_quest::combat::level::ScanCallsForTest() == 0, "arena: %llu reads and %llu scans",
        static_cast<unsigned long long>(ReadCount()), static_cast<unsigned long long>(nevr_quest::combat::level::ScanCallsForTest()));
  CHECK(g_tp.size() == 2 && g_tp[0].pose == g_pose && g_tp[1].pose == g_pose && g_fire.size() == 2, "arena: a call was changed");
  float v3[3];
  memcpy(v3, Rec() + 0x11c, 12);
  CHECK(v3[0] == 1 && v3[1] == 2 && v3[2] == 3, "arena: the bullet record was written");
  CHECK(CurrentStats().shots == 0 && CurrentStats().tracers == 0, "arena: a shot or tracer was counted");
}

static void TestFire() {
  Setup();
  BuildBullet();
  // not a drone shot: the record keeps what FireBullet filled in; arguments and result pass through
  NoShot();
  g_fire.clear();
  CHECK(FireIn(FireSlot()) == 0x5678, "result");
  CHECK(g_fire.size() == 1 && g_fire[0].bcs == g_bcs && g_fire[0].handle == kHandle && g_fire[0].gun == reinterpret_cast<void*>(0x1) && g_fire[0].speed == 2.5f &&
            g_fire[0].quat == reinterpret_cast<void*>(0x3) && g_fire[0].vel == reinterpret_cast<void*>(0x4) && g_fire[0].remote == 5 && g_fire[0].flag == 6,
        "arguments");
  float v3[3], p3[3];
  memcpy(v3, Rec() + 0x11c, 12);
  memcpy(p3, Rec() + 0x128, 12);
  CHECK(v3[0] == 1 && v3[1] == 2 && v3[2] == 3 && p3[0] == 4 && p3[1] == 5 && p3[2] == 6, "record changed for a non-drone shot");
  // a drone shot, FireBulletLocal's call site: shooter velocity 0, shooter position = the spawn
  BuildBullet();
  Shot(0, 0, 120);
  CHECK(FireIn(FireSlot()) == 0x5678, "result 2");
  memcpy(v3, Rec() + 0x11c, 12);
  memcpy(p3, Rec() + 0x128, 12);
  float sp[3];
  memcpy(sp, Rec() + 0x134, 12);
  CHECK(v3[0] == 0 && v3[1] == 0 && v3[2] == 0, "shooter velocity not zeroed");
  CHECK(p3[0] == 7 && p3[1] == 8 && p3[2] == 9 && sp[0] == 7 && sp[1] == 8 && sp[2] == 9, "shooter position is not the spawn");
  // nothing else in the record or its neighbours changed
  bool canary = true;
  for (size_t i = 0; i < sizeof g_records; ++i) {
    const bool ours = i >= 0x298 + 0x11c && i < 0x298 + 0x140;  // shooter velocity, shooter position, spawn: the fake FireBullet filled them
    if (!ours && g_records[i] != 0xA5) canary = false;
  }
  CHECK(canary, "bytes outside the three fields were written");
  CHECK(HasLine("combat_dronemuzzle_tracer") && HasLine("x=7.000000"), "tracer line");
  // another caller of FireBullet with the same drone shot: untouched
  BuildBullet();
  CHECK(FireOut(FireSlot()) == 0x5678, "result 3");
  memcpy(v3, Rec() + 0x11c, 12);
  CHECK(v3[0] == 1 && v3[2] == 3, "another caller's record was changed");
  // v^2 just over 1e-6 acts, just under does not
  BuildBullet();
  Shot(0, 0, 0.01f);
  FireIn(FireSlot());
  memcpy(v3, Rec() + 0x11c, 12);
  CHECK(v3[0] == 0 && v3[1] == 0 && v3[2] == 0, "a slow drone shot (0.01 m/s) was not fixed");
  // a (near) zero or non-finite velocity: untouched
  const float bad[][3] = {{0, 0, 1e-4f}, {0, 0, 5e-4f}, {NAN, 0, 0}, {INFINITY, 0, 0}, {0, 0, 0}};
  for (const auto& b : bad) {
    BuildBullet();
    Shot(b[0], b[1], b[2]);
    FireIn(FireSlot());
    memcpy(v3, Rec() + 0x11c, 12);
    CHECK(v3[0] == 1 && v3[1] == 2 && v3[2] == 3, "record changed for v = (%f %f %f)", (double)b[0], (double)b[1], (double)b[2]);
  }
  // an unreadable record table: the call goes through, nothing is written, no crash
  BuildBullet();
  Shot(0, 0, 120);
  const uintptr_t zero = 0;
  memcpy(g_bcs + 0xc8, &zero, 8);
  CHECK(FireIn(FireSlot()) == 0x5678, "result 4");
  memcpy(v3, Rec() + 0x11c, 12);
  CHECK(v3[0] == 1, "written through a null index table");
  BuildBullet();
  const uintptr_t wild = 0x1000;
  memcpy(g_bcs + 0xf8, &wild, 8);
  CHECK(FireIn(FireSlot()) == 0x5678, "result 5");
  // the record's shooter velocity is on an unreadable page (the spawn and the shooter position are readable): nothing is written, no crash
  {
    const size_t pg = 4096;
    uint8_t* m = static_cast<uint8_t*>(mmap(nullptr, 2 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
    memset(m + pg, 0xA5, pg);
    mprotect(m, pg, PROT_NONE);
    BuildBullet();
    const uintptr_t rec = reinterpret_cast<uintptr_t>(m + pg) - 0x124;  // +0x11c..+0x124 lie in the PROT_NONE page
    const uintptr_t idx = reinterpret_cast<uintptr_t>(g_index);
    memcpy(g_bcs + 0xc8, &idx, 8);
    memcpy(g_bcs + 0xf8, &rec, 8);
    g_index[(kHandle & 0xffff) * 2] = 0;
    Shot(0, 0, 120);
    CHECK(FireIn(FireSlot()) == 0x5678, "result 6");
    bool same = true;
    for (size_t i = 0; i < pg; ++i) same &= m[pg + i] == 0xA5;
    CHECK(same, "bytes were written next to an unreadable shooter velocity");
    munmap(m, 2 * pg);
  }
  NoShot();
}

static void TestTracerCap() {
  Setup();
  Shot(0, 0, 120);
  for (int i = 0; i < 8; ++i) {
    BuildBullet();
    FireIn(FireSlot());
  }
  CHECK(CountLine("combat_dronemuzzle_tracer") == 3, "tracer lines %d", CountLine("combat_dronemuzzle_tracer"));
  CHECK(CountLine("combat_dronemuzzle_cap", 1) == 1 && HasLine("kind=tracer"), "tracer cap line");
  CHECK(CurrentStats().tracers == 8, "tracers counted %u", CurrentStats().tracers);
  NoShot();
}

int main() {
  sentinel::SetLogSink(&Sink);
  TestCore();
  TestInstallPinned();
  Dump("pinned install");
  TestInstallRefusals();
  Dump("refusals (last scenario)");
  TestAnotherHandlerOnTheSlot();
  TestTeleport();
  TestFrameTick();
  TestFrameTickOutOfScope();
  TestArenaPaysNothing();
  TestFire();
  TestTracerCap();
  Dump("tracer cap scenario");
  if (g_fail == 0) std::printf("dronemuzzle_test: all checks passed\n");
  else std::printf("dronemuzzle_test: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
