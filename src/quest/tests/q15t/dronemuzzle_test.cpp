// Host test of dronemuzzle.cpp (rows R49 / R50): the plain functions the arm64 thunks call, the install of both GOT words, and the
// frame-time tick, against a fake slot page and fake real functions (real mprotect, /proc/self/maps, SafeRead).
//
// The naked arm64 thunks cannot run on this host: here the two slots get HOST STAND-INS (below) that do what the thunks do in C++ (compare
// the return address with the thunk's word, then either call the plain function or jump to the next function). What the thunks themselves
// are is checked by src/quest/tests/q15t/mod/dronemuzzle.sh (NDK cross-compile; instruction words and relocations compared with the source build's).
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

#include "dronekill.h"
#include "dronemuzzle.h"
#include "dronemuzzle_core.h"
#include "nevr/plugin_interface.h"
#include "plugin_log.h"

using namespace q15t;

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
static void FakeLog(const char* id, uint32_t level, const char* msg, const NevrLogField* f, uint32_t n) {
  std::string t = std::string("plugin=") + id + " " + msg;
  for (uint32_t i = 0; i < n; ++i) t += std::string(" ") + f[i].key + "=" + f[i].value;
  g_lines.push_back({level, t});
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

// ================================================================ the core (ported from the source's host test)
namespace ref {  // upstream "df" code, with Pose -> float[3] and g_frame_dt -> an argument
constexpr float MUZZLE[3] = {0.000024f, 0.075634f, 0.147283f};
constexpr float BACK_MAX = 0.25f;
bool MuzzleTransform(float* p, const float* v, float g_frame_dt) {
  if (!v) return false;
  float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (len < 1e-4f) return false;
  float z[3] = {v[0] / len, v[1] / len, v[2] / len};
  float xl = sqrtf(z[2] * z[2] + z[0] * z[0]);
  float x[3] = {xl > 1e-4f ? z[2] / xl : 1.0f, 0.0f, xl > 1e-4f ? -z[0] / xl : 0.0f};
  float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
  float dt = g_frame_dt;
  dt = dt < 1.0f / 120.0f ? 1.0f / 120.0f : dt > 1.0f / 30.0f ? 1.0f / 30.0f : dt;
  float back = len * dt > BACK_MAX ? BACK_MAX / len : dt;
  for (int i = 0; i < 3; ++i) p[i] += MUZZLE[0] * x[i] + MUZZLE[1] * y[i] + MUZZLE[2] * z[i] - v[i] * back;
  return true;
}
float Tick(float g_frame_dt, float dt) {
  if (dt > 1.0f / 240.0f && dt < 0.1f) g_frame_dt = g_frame_dt * 0.9f + dt * 0.1f;
  return g_frame_dt;
}
}  // namespace ref

static void TestCore() {
  using namespace q15t::dm;
  unsigned x = 777;
  auto rnd = [&x]() {
    x = x * 1103515245u + 12345u;
    return static_cast<float>((x >> 8) & 0xffff) / 65535.0f;
  };
  int moved = 0;
  for (int i = 0; i < 200000; ++i) {
    float root[3] = {rnd() * 80 - 40, rnd() * 20 - 5, rnd() * 80 - 40};
    const float s = i % 50 == 0 ? 0.0f : i % 7 == 0 ? 1e-5f : rnd() * 200;
    float v[3] = {(rnd() - 0.5f) * s, (rnd() - 0.5f) * s, (rnd() - 0.5f) * s};
    if (i % 97 == 0) {
      v[0] = 0;
      v[2] = 0;
      v[1] = (rnd() - 0.5f) * 200;
    }
    const float dt = rnd() * 0.08f;
    float a[3], b[3];
    memcpy(b, root, sizeof b);
    const bool ok = MuzzleMove(root, v, dt, a);
    const bool rok = ref::MuzzleTransform(b, v, dt);
    if (ok != rok || (ok && memcmp(a, b, sizeof a) != 0)) {
      CHECK(false, "MuzzleMove differs from upstream at step %d", i);
      break;
    }
    moved += ok;
  }
  CHECK(moved > 150000, "moved %d", moved);
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

// the host stand-ins for the two thunks: the same decision in C++
__attribute__((noinline)) static uint64_t HostTpThunk(void* gs, uint64_t actor, const uint8_t* pose, uint64_t reason) {
  const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  if (ret == q15t_dm_tp_ret.load(std::memory_order_acquire)) return q15t_dm_teleport(gs, actor, pose, reason);
  return reinterpret_cast<TpFn>(q15t_dm_tp_next.load(std::memory_order_acquire))(gs, actor, pose, reason);
}
__attribute__((noinline)) static uint64_t HostFireThunk(uint8_t* bcs, uint64_t handle, const void* gun, float speed, const void* quat, const void* vel,
                                                       uint64_t remote, uint64_t flag) {
  const uintptr_t ret = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  if (ret == q15t_dm_fire_ret.load(std::memory_order_acquire)) return q15t_dm_fire(bcs, handle, gun, speed, quat, vel, remote, flag);
  return reinterpret_cast<FireFn>(q15t_dm_fire_next.load(std::memory_order_acquire))(bcs, handle, gun, speed, quat, vel, remote, flag);
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
static uintptr_t* g_w = nullptr;
static uintptr_t V(const void* fn) { return reinterpret_cast<uintptr_t>(fn) - g_base; }

static void NewPage(bool fire_in_exec_page) {
  void* p = mmap(nullptr, 2 * 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  g_w = static_cast<uintptr_t*>(p);
  g_base = reinterpret_cast<uintptr_t>(p);
  g_w[0x20 / 8] = reinterpret_cast<uintptr_t>(&FakeTeleport);
  if (fire_in_exec_page) g_w[0x1028 / 8] = reinterpret_cast<uintptr_t>(&FakeFireReal);
  else g_w[0x28 / 8] = reinterpret_cast<uintptr_t>(&FakeFireReal);
  mprotect(p, 4096, PROT_READ);
  mprotect(static_cast<char*>(p) + 4096, 4096, fire_in_exec_page ? (PROT_READ | PROT_EXEC) : PROT_READ);
}

static DroneMuzzleSlots Slots(bool fire_in_exec_page = false) {
  // calibrate: the return addresses of the "In" call sites
  TpIn(&RecTp, nullptr);
  FireIn(&RecFire);
  return {0x20, V(reinterpret_cast<void*>(&FakeTeleport)), g_rec_tp - g_base, fire_in_exec_page ? 0x1028u : 0x28u,
          V(reinterpret_cast<void*>(&FakeFireReal)), g_rec_fire - g_base};
}
static DroneMuzzleWrappers Wrappers() {
  return {reinterpret_cast<uintptr_t>(&HostTpThunk), reinterpret_cast<uintptr_t>(&HostFireThunk)};
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

static DroneMuzzleInstall Setup() {
  ResetDroneMuzzleForTest();
  ResetDroneKillForTest();
  g_lines.clear();
  g_tp.clear();
  g_fire.clear();
  g_now_ns = 5000000000ull;
  SetDroneMuzzleClockForTest(&ClkNs);
  BuildBullet();
  NewPage(false);
  SetDroneKillShotActiveForTest(true);
  return InstallDroneMuzzle(g_base, Slots(), Wrappers());
}

static TpFn TpSlot() { return reinterpret_cast<TpFn>(g_w[0x20 / 8]); }
static FireFn FireSlot() { return reinterpret_cast<FireFn>(g_w[0x28 / 8]); }
static void Shot(float x, float y, float z, int remote = 0) {
  const float v[3] = {x, y, z};
  SetDroneKillShotForTest(v, remote);
}
static void NoShot() { SetDroneKillShotForTest(nullptr, 0); }

alignas(16) static uint8_t g_pose[32];
static void SetPose(float px, float py, float pz) {
  for (int i = 0; i < 32; ++i) g_pose[i] = static_cast<uint8_t>(0x40 + i);  // quaternion / scale bytes the module must not touch
  const float p[3] = {px, py, pz};
  memcpy(g_pose + 0x10, p, 12);
}

// ================================================================ tests
static void TestInstallPinned() {
  ResetDroneMuzzleForTest();
  g_lines.clear();
  SetDroneKillShotActiveForTest(true);
  const size_t sz = 0x3800000;
  void* p = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  const uintptr_t base = reinterpret_cast<uintptr_t>(p);
  auto put = [&](uintptr_t slot, uintptr_t fn) {
    const uintptr_t v = base + fn;
    memcpy(static_cast<char*>(p) + slot, &v, 8);
  };
  put(0x36cfa18, 0x180a384);  // R49 WorldTeleport
  put(0x36f3cf8, 0x210c9c4);  // R50 FireBullet
  mprotect(p, sz, PROT_READ);
  const DroneMuzzleSlots& k = kDroneMuzzlePinned;
  CHECK(k.teleport_slot == 0x36cfa18 && k.teleport_fn == 0x180a384 && k.teleport_ret == 0x210c22c, "teleport constants");
  CHECK(k.fire_slot == 0x36f3cf8 && k.fire_fn == 0x210c9c4 && k.fire_ret == 0x210c2a4, "fire constants");
  const DroneMuzzleInstall r = InstallDroneMuzzle(base, kDroneMuzzlePinned, Wrappers());
  CHECK(r.ok && r.teleport.status == SwapStatus::kOk && r.fire.status == SwapStatus::kOk && r.shot_source, "pinned install");
  uintptr_t a, b;
  memcpy(&a, static_cast<char*>(p) + 0x36cfa18, 8);
  memcpy(&b, static_cast<char*>(p) + 0x36f3cf8, 8);
  CHECK(a == reinterpret_cast<uintptr_t>(&HostTpThunk) && b == reinterpret_cast<uintptr_t>(&HostFireThunk), "the words are not the wrappers");
  CHECK(q15t_dm_tp_ret.load() == base + 0x210c22c && q15t_dm_fire_ret.load() == base + 0x210c2a4, "return words: %llx %llx",
        (unsigned long long)q15t_dm_tp_ret.load(), (unsigned long long)q15t_dm_fire_ret.load());
  CHECK(q15t_dm_tp_next.load() == base + 0x180a384 && q15t_dm_fire_next.load() == base + 0x210c9c4, "next words");
  CHECK(CountLine("dronemuzzle hook install", 1) == 2 && HasLine("dronemuzzle: on (QA)", 1), "install lines");
  munmap(p, sz);
}

static void TestInstallRefusals() {
  // no thunk (the host's own default: only arm64 has them)
  ResetDroneMuzzleForTest();
  g_lines.clear();
  NewPage(false);
  DroneMuzzleInstall r = InstallDroneMuzzle(g_base, Slots(), DroneMuzzleThunks());
#if defined(__aarch64__)
  CHECK(DroneMuzzleThunks().teleport != 0 && DroneMuzzleThunks().fire != 0 && DroneMuzzleThunks().teleport != DroneMuzzleThunks().fire, "arm64 thunks");
#else
  CHECK(!r.ok && HasLine("no arm64 thunk on this architecture", 3), "host default wrappers: refusal not loud");
  CHECK(g_w[0x20 / 8] == reinterpret_cast<uintptr_t>(&FakeTeleport), "a word was swapped without a thunk");
#endif
  // a slot that does not hold the pinned function: nothing swapped
  ResetDroneMuzzleForTest();
  g_lines.clear();
  NewPage(false);
  DroneMuzzleSlots s = Slots();
  mprotect(g_w, 4096, PROT_READ | PROT_WRITE);
  g_w[0x28 / 8] = 0x4242420;
  mprotect(g_w, 4096, PROT_READ);
  r = InstallDroneMuzzle(g_base, s, Wrappers());
  CHECK(!r.ok && r.fire.status == SwapStatus::kUnexpectedOriginal && g_w[0x20 / 8] == reinterpret_cast<uintptr_t>(&FakeTeleport) && g_w[0x28 / 8] == 0x4242420,
        "foreign slot word: a word changed");
  CHECK(HasLine("dronemuzzle: OFF, nothing changed", 3) && q15t_dm_tp_next.load() == 0, "refusal not loud / next set");
  // the second word cannot be swapped (executable page): the first is put back
  ResetDroneMuzzleForTest();
  g_lines.clear();
  NewPage(true);
  r = InstallDroneMuzzle(g_base, Slots(true), Wrappers());
  CHECK(!r.ok && r.fire.status == SwapStatus::kExecutablePage, "exec page: %s", StatusName(r.fire.status));
  CHECK(g_w[0x20 / 8] == reinterpret_cast<uintptr_t>(&FakeTeleport), "WorldTeleport's word was not put back");
  CHECK(HasLine("dronemuzzle: the FireBullet swap failed") && HasLine("teleport_put_back=1"), "rollback not reported");
  CHECK(HasLine("dronemuzzle hook install", 3), "the failed swap was not an ERROR line");
  // only one of the two thunks: refused, nothing swapped
  for (int which = 0; which < 2; ++which) {
    ResetDroneMuzzleForTest();
    g_lines.clear();
    NewPage(false);
    DroneMuzzleWrappers w = Wrappers();
    if (which == 0) w.teleport = 0;
    else w.fire = 0;
    r = InstallDroneMuzzle(g_base, Slots(), w);
    CHECK(!r.ok && HasLine("no arm64 thunk on this architecture", 3) && g_w[0x20 / 8] == reinterpret_cast<uintptr_t>(&FakeTeleport) &&
              g_w[0x28 / 8] == reinterpret_cast<uintptr_t>(&FakeFireReal),
          "one thunk missing (%d): installed", which);
  }
  // WorldTeleport's word cannot be swapped (executable page): FireBullet's is not touched either
  ResetDroneMuzzleForTest();
  g_lines.clear();
  NewPage(true);
  {
    mprotect(g_w + 4096 / 8, 4096, PROT_READ | PROT_WRITE);
    g_w[0x1020 / 8] = reinterpret_cast<uintptr_t>(&FakeTeleport);
    mprotect(g_w + 4096 / 8, 4096, PROT_READ | PROT_EXEC);
    mprotect(g_w, 4096, PROT_READ | PROT_WRITE);
    g_w[0x28 / 8] = reinterpret_cast<uintptr_t>(&FakeFireReal);
    mprotect(g_w, 4096, PROT_READ);
    DroneMuzzleSlots sl = Slots(false);
    sl.teleport_slot = 0x1020;
    r = InstallDroneMuzzle(g_base, sl, Wrappers());
  }
  CHECK(!r.ok && r.teleport.status == SwapStatus::kExecutablePage, "teleport in an executable page: %s", StatusName(r.teleport.status));
  CHECK(g_w[0x28 / 8] == reinterpret_cast<uintptr_t>(&FakeFireReal), "FireBullet's word was swapped after WorldTeleport's failed");
  CHECK(HasLine("dronemuzzle hook install", 3) && HasLine("dronemuzzle: OFF, nothing changed", 3), "the failed WorldTeleport swap was not loud");
  // the plain functions before any install (no next function): return 0, call nothing
  ResetDroneMuzzleForTest();
  CHECK(q15t_dm_teleport(nullptr, 0, nullptr, 0) == 0 && q15t_dm_fire(nullptr, 0, nullptr, 0.0f, nullptr, nullptr, 0, 0) == 0, "a plain function ran with no next function");
  // the first word cannot be swapped: nothing else is touched
  ResetDroneMuzzleForTest();
  g_lines.clear();
  r = InstallDroneMuzzle(0, kDroneMuzzlePinned, Wrappers());
  CHECK(!r.ok && HasLine("no base address", 3), "base 0");
  // no shot source (dronekill off): installed, but loud
  ResetDroneMuzzleForTest();
  ResetDroneKillForTest();
  g_lines.clear();
  NewPage(false);
  r = InstallDroneMuzzle(g_base, Slots(), Wrappers());
  CHECK(r.ok && !r.shot_source && HasLine("dronemuzzle: on (QA)", 2) && HasLine("the thunks only pass through"), "no shot source not reported");
}

static void TestTeleport() {
  DroneMuzzleInstall r = Setup();
  CHECK(r.ok, "install");
  // not a drone shot (the accessor is null): the call goes through with its arguments, result and pose pointer untouched
  NoShot();
  SetPose(1, 2, 3);
  CHECK(TpIn(TpSlot(), g_pose) == 0x1234, "result");
  CHECK(g_tp.size() == 1 && g_tp[0].gs == reinterpret_cast<void*>(0xa0) && g_tp[0].actor == 0xb0 && g_tp[0].pose == g_pose && g_tp[0].reason == 0xc0,
        "not a drone shot: arguments");
  CHECK(GetDroneMuzzleStats().shots == 0, "counted a shot");
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
  CHECK(GetDroneMuzzleStats().shots == 1 && HasLine("dronemuzzle: shot") && HasLine("remote=1") && HasLine("velocity=0.000000 0.000000 120.000000"), "shot line");
  // the same drone shot from any OTHER caller: untouched
  g_tp.clear();
  CHECK(TpOut(TpSlot(), g_pose) == 0x1234 && g_tp.size() == 1 && g_tp[0].pose == g_pose, "another caller's pose was moved");
  CHECK(GetDroneMuzzleStats().shots == 1, "another caller counted");
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
  CHECK(CountLine("dronemuzzle: shot", 1) == 39, "shot lines after 45 more: %d", CountLine("dronemuzzle: shot", 1));  // shots 2..40 are written
  CHECK(CountLine("dronemuzzle: log cap reached", 1) == 1, "cap line");
  CHECK(GetDroneMuzzleStats().shots == 46, "shots %u", GetDroneMuzzleStats().shots);
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
    DroneMuzzleTick(0);
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
  DroneMuzzleTick(0);  // the first call only records the time
  for (int i = 0; i < 60; ++i) {
    g_now_ns += 16666667ull;  // 60 fps
    DroneMuzzleTick(0);
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
  DroneMuzzleTick(0);
  g_tp.clear();
  TpIn(TpSlot(), g_pose);
  memcpy(got, g_tp[0].copy + 0x10, 12);
  CHECK(std::fabs(got[2] - before) < 1e-7f, "a 0.5 s hitch fed the average");
  NoShot();
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
  CHECK(HasLine("dronemuzzle: drone bullet tracer starts at the drone") && HasLine("x=7.000000"), "tracer line");
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
  CHECK(CountLine("dronemuzzle: drone bullet tracer") == 3, "tracer lines %d", CountLine("dronemuzzle: drone bullet tracer"));
  CHECK(CountLine("dronemuzzle: log cap reached", 1) == 1 && HasLine("kind=tracer"), "tracer cap line");
  CHECK(GetDroneMuzzleStats().tracers == 8, "tracers counted %u", GetDroneMuzzleStats().tracers);
  NoShot();
}

int main() {
  NvrGameContext ctx{};
  ctx.ctx_size = sizeof ctx;
  ctx.plugin_id = "q15t";
  ctx.log = &FakeLog;
  SetLogContext(&ctx);
  TestCore();
  TestInstallPinned();
  Dump("pinned install");
  TestInstallRefusals();
  Dump("refusals (last scenario)");
  TestTeleport();
  TestFrameTick();
  TestFire();
  TestTracerCap();
  Dump("tracer cap scenario");
  if (g_fail == 0) std::printf("dronemuzzle_test: all checks passed\n");
  else std::printf("dronemuzzle_test: %d failure(s)\n", g_fail);
  return g_fail == 0 ? 0 : 1;
}
