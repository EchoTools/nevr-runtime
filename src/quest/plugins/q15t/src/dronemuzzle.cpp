// Port of the source build's dronemuzzle (rows R49 / R50). See dronemuzzle.h and docs/quest/q15t/modules/dronemuzzle.md.
#include "dronemuzzle.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "dronekill.h"
#include "dronekill_log.h"
#include "dronemuzzle_core.h"
#include "got_swap.h"
#include "safe_read.h"

namespace q15t {

// ELF vaddrs of the pinned build. Slots: readelf -rW (JUMP_SLOT 0x36cfa18 -> fn 0x180a384; 0x36f3cf8 -> fn 0x210c9c4). The two return
// addresses are the instructions after FireBulletLocal's `bl` to each PLT stub (disassembly 0x220c228 bl 0xff0660 / 0x220c2a0 bl 0x1038c20).
const DroneMuzzleSlots kDroneMuzzlePinned = {0x36cfa18, 0x180a384, 0x210c22c, 0x36f3cf8, 0x210c9c4, 0x210c2a4};

namespace {

using namespace dm;

constexpr std::uintptr_t kCsIndex = 0xc8, kCsRecords = 0xf8, kBulletStride = 0x298;
constexpr std::uintptr_t kRecShooterVel = 0x11c, kRecShooterPos = 0x128, kRecSpawn = 0x134;
constexpr std::uintptr_t kPosePos = 0x10;  // CTransfQ: quaternion +0, position +0x10, scale +0x1c
constexpr int kShotLines = 40, kTracerLines = 3;  // SHOT_LOG

// the arguments as FireBulletLocal passes them (x0..x3 / x0..x6 + s0): integer registers whole, the speed in s0
using TeleportFn = std::uint64_t (*)(void* gs, std::uint64_t actor, const std::uint8_t* pose, std::uint64_t reason);
using FireFn = std::uint64_t (*)(std::uint8_t* bcs, std::uint64_t handle, const void* gun, float speed, const void* quat, const void* vel,
                                 std::uint64_t remote, std::uint64_t flag);

std::atomic<int> g_shots{0}, g_tracers{0};
std::atomic<std::uint64_t> g_last_frame_ns{0};
constexpr float kDefaultFrameDt = 1.0f / 90.0f;
std::atomic<float> g_frame_dt{kDefaultFrameDt};
std::uint64_t (*g_clock_ns)() = nullptr;

std::uint64_t NowNs() {
  if (g_clock_ns != nullptr) return g_clock_ns();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

std::uint32_t Tid() { return static_cast<std::uint32_t>(syscall(SYS_gettid)); }

}  // namespace
}  // namespace q15t

extern "C" {
// The words the two thunks read. A std::atomic<uintptr_t> is one aligned lock-free 8-byte word: the thunk's `ldar` is its acquire load.
std::atomic<std::uintptr_t> q15t_dm_tp_ret __attribute__((visibility("hidden"))){0};
std::atomic<std::uintptr_t> q15t_dm_tp_next __attribute__((visibility("hidden"))){0};
std::atomic<std::uintptr_t> q15t_dm_fire_ret __attribute__((visibility("hidden"))){0};
std::atomic<std::uintptr_t> q15t_dm_fire_next __attribute__((visibility("hidden"))){0};

// F4. Reached only from the thunk, only for a call that returns into FireBulletLocal.
std::uint64_t q15t_dm_teleport(void* gs, std::uint64_t actor, const std::uint8_t* pose, std::uint64_t reason) {
  using namespace q15t;
  using namespace q15t::dm;
  const auto next = reinterpret_cast<TeleportFn>(q15t_dm_tp_next.load(std::memory_order_acquire));
  if (next == nullptr) return 0;
  int remote = 0;
  const float* v = DroneKillShotVelocity(&remote);
  if (v == nullptr) return next(gs, actor, pose, reason);  // not a drone shot (AUD-7 DF-6): untouched
  alignas(16) std::uint8_t moved[32];
  if (!SafeRead(reinterpret_cast<std::uintptr_t>(pose), moved, sizeof moved)) return next(gs, actor, pose, reason);
  float root[3], spawn[3];
  std::memcpy(root, moved + kPosePos, sizeof root);
  const float dt = g_frame_dt.load(std::memory_order_relaxed);
  if (!MuzzleMove(root, v, dt, spawn)) return next(gs, actor, pose, reason);  // |v| < 1e-4: untouched
  std::memcpy(moved + kPosePos, spawn, sizeof spawn);
  const std::uint64_t r = next(gs, actor, moved, reason);  // the copy: the caller's pose is never written
  const int n = g_shots.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n == kShotLines + 1) LogKv().S("kind", "shot").U("cap", kShotLines).Emit(NEVR_LOG_INFO, "dronemuzzle: log cap reached, more are counted not written");
  if (n <= kShotLines)
    LogKv().U("shot", static_cast<unsigned long long>(n)).S("remote", remote != 0 ? "1" : "0").V3("root", root).V3("spawn", spawn).V3("velocity", v)
        .D("frame_ms", ClampDt(dt) * 1000.0f).U("tid", Tid()).Emit(NEVR_LOG_INFO, "dronemuzzle: shot");
  return r;
}

// F6. Reached only from the thunk, only for a call that returns into FireBulletLocal.
std::uint64_t q15t_dm_fire(std::uint8_t* bcs, std::uint64_t handle, const void* gun, float speed, const void* quat, const void* vel,
                           std::uint64_t remote, std::uint64_t flag) {
  using namespace q15t;
  using namespace q15t::dm;
  const auto next = reinterpret_cast<FireFn>(q15t_dm_fire_next.load(std::memory_order_acquire));
  if (next == nullptr) return 0;
  const std::uint64_t r = next(bcs, handle, gun, speed, quat, vel, remote, flag);
  int rem = 0;
  const float* v = DroneKillShotVelocity(&rem);
  const float v2 = v != nullptr ? v[0] * v[0] + v[1] * v[1] + v[2] * v[2] : 0.0f;
  if (v == nullptr || !(v2 > 1e-6f) || !std::isfinite(v2)) return r;  // not a drone shot / F46-6, DM-2: v ~ 0 or not finite (= fireguard)
  // FireBullet's own lookup of the record it has just filled (r15+0x210c9e4 .. 0x210ca04), every read through SafeRead
  const auto b = reinterpret_cast<std::uintptr_t>(bcs);
  std::uintptr_t index = 0, records = 0;
  std::uint16_t slot = 0;
  if (!SafeRead(b + kCsIndex, &index, sizeof index) || index == 0 || !SafeRead(b + kCsRecords, &records, sizeof records) || records == 0 ||
      !SafeRead(index + (handle & 0xffff) * 4, &slot, sizeof slot))
    return r;
  const std::uintptr_t rec = records + static_cast<std::uintptr_t>(slot) * kBulletStride;
  float p[3], probe[3];
  // the two fields about to be written must be readable heap first
  if (!SafeRead(rec + kRecSpawn, p, sizeof p) || !SafeRead(rec + kRecShooterVel, probe, sizeof probe) ||
      !SafeRead(rec + kRecShooterPos, probe, sizeof probe))
    return r;
  const float zero[3] = {0, 0, 0};
  std::memcpy(reinterpret_cast<void*>(rec + kRecShooterVel), zero, sizeof zero);
  std::memcpy(reinterpret_cast<void*>(rec + kRecShooterPos), p, sizeof p);
  const int t = g_tracers.fetch_add(1, std::memory_order_relaxed);
  if (t == kTracerLines) LogKv().S("kind", "tracer").U("cap", kTracerLines).Emit(NEVR_LOG_INFO, "dronemuzzle: log cap reached, more are counted not written");
  if (t < kTracerLines)
    LogKv().D("x", p[0]).D("y", p[1]).D("z", p[2]).U("index", handle & 0xffff).U("tid", Tid())
        .Emit(NEVR_LOG_INFO, "dronemuzzle: drone bullet tracer starts at the drone");
  return r;
}
}  // extern "C"

#if defined(__aarch64__)
extern "C" {
// AUD-7 F46-1 / F46-4 (the source's thunks, symbol names changed): each thunk = bti c, the RETURN address compare, and for every other
// caller a `br` to the next function with x0..x7 / s0 / x30 / sp as the caller left them (x16 only). FireBulletLocal's call goes through C.
// DM-1: the words are loaded with ldar; they are all published (release) before the slot store, so the thunk never runs before them (a 0
// return word cannot match x30).
__attribute__((naked, visibility("hidden"))) void q15t_dm_tp_thunk() {
  asm volatile(
      "hint #34\n"  // bti c
      "adrp x16, q15t_dm_tp_ret\n"
      "add x16, x16, :lo12:q15t_dm_tp_ret\n"
      "ldar x16, [x16]\n"
      "cmp x30, x16\n"
      "b.eq 1f\n"
      "adrp x16, q15t_dm_tp_next\n"
      "add x16, x16, :lo12:q15t_dm_tp_next\n"
      "ldar x16, [x16]\n"
      "br x16\n"
      "1:\n"
      "stp x29, x30, [sp, #-16]!\n"
      "mov x29, sp\n"
      "bl q15t_dm_teleport\n"
      "ldp x29, x30, [sp], #16\n"
      "ret\n");
}
__attribute__((naked, visibility("hidden"))) void q15t_dm_fire_thunk() {
  asm volatile(
      "hint #34\n"  // bti c
      "adrp x16, q15t_dm_fire_ret\n"
      "add x16, x16, :lo12:q15t_dm_fire_ret\n"
      "ldar x16, [x16]\n"
      "cmp x30, x16\n"
      "b.eq 1f\n"
      "adrp x16, q15t_dm_fire_next\n"
      "add x16, x16, :lo12:q15t_dm_fire_next\n"
      "ldar x16, [x16]\n"
      "br x16\n"
      "1:\n"
      "stp x29, x30, [sp, #-16]!\n"
      "mov x29, sp\n"
      "bl q15t_dm_fire\n"
      "ldp x29, x30, [sp], #16\n"
      "ret\n");
}
}  // extern "C"
#endif

namespace q15t {

DroneMuzzleWrappers DroneMuzzleThunks() {
#if defined(__aarch64__)
  return {reinterpret_cast<std::uintptr_t>(&q15t_dm_tp_thunk), reinterpret_cast<std::uintptr_t>(&q15t_dm_fire_thunk)};
#else
  return {0, 0};
#endif
}

namespace {
void LogSwap(const char* hook, const InstallResult& r) {
  LogKv().S("hook", hook).S("status", StatusName(r.status)).S("reason", r.reason).Emit(r.status == SwapStatus::kOk ? NEVR_LOG_INFO : NEVR_LOG_ERROR, "dronemuzzle hook install");
}
}  // namespace

DroneMuzzleInstall InstallDroneMuzzle(std::uintptr_t base, const DroneMuzzleSlots& slots, const DroneMuzzleWrappers& wrappers) {
  DroneMuzzleInstall out;
  auto refuse = [&out](const char* why) {
    LogKv().S("why", why).Emit(NEVR_LOG_ERROR, "dronemuzzle: OFF, nothing changed");
    return out;
  };
  if (base == 0) return refuse("no base address");
  if (wrappers.teleport == 0 || wrappers.fire == 0) return refuse("no arm64 thunk on this architecture");
  // both slots are checked before the first swap (all or none)
  const std::uintptr_t want[2][2] = {{slots.teleport_slot, slots.teleport_fn}, {slots.fire_slot, slots.fire_fn}};
  const char* names[2] = {"CncaGameSpace::WorldTeleport", "CR15NetBulletCS::FireBullet"};
  InstallResult* results[2] = {&out.teleport, &out.fire};
  for (int i = 0; i < 2; ++i) {
    bool ok = true;
    const auto word = Rd<std::uintptr_t>(base + want[i][0], &ok);
    if (ok && word == base + want[i][1]) continue;
    results[i]->status = SwapStatus::kUnexpectedOriginal;
    results[i]->reason = "slot_word_not_the_pinned_function";
    LogSwap(names[i], *results[i]);
    return refuse("a slot does not hold the pinned function");
  }
  // all four words before any swap (DM-1)
  q15t_dm_tp_ret.store(base + slots.teleport_ret, std::memory_order_release);
  q15t_dm_fire_ret.store(base + slots.fire_ret, std::memory_order_release);
  out.teleport = InstallImportHook(names[0], base, slots.teleport_slot, wrappers.teleport, &q15t_dm_tp_next, slots.teleport_fn);
  LogSwap(names[0], out.teleport);
  if (out.teleport.status != SwapStatus::kOk) {
    if (out.teleport.status == SwapStatus::kRestoreFailed) SwapWord(base + slots.teleport_slot, wrappers.teleport, base + slots.teleport_fn);
    return refuse("the WorldTeleport swap failed");
  }
  out.fire = InstallImportHook(names[1], base, slots.fire_slot, wrappers.fire, &q15t_dm_fire_next, slots.fire_fn);
  LogSwap(names[1], out.fire);
  if (out.fire.status != SwapStatus::kOk) {
    if (out.fire.status == SwapStatus::kRestoreFailed) SwapWord(base + slots.fire_slot, wrappers.fire, base + slots.fire_fn);
    const SwapResult back = SwapWord(base + slots.teleport_slot, wrappers.teleport, base + slots.teleport_fn);
    LogKv().S("teleport_put_back", back.status == SwapStatus::kOk ? "1" : "0")
        .Emit(back.status == SwapStatus::kOk ? NEVR_LOG_WARNING : NEVR_LOG_ERROR, "dronemuzzle: the FireBullet swap failed");
    return out;
  }
  out.ok = true;
  out.shot_source = DroneKillShotActive();
  LogKv().X("teleport_ret", base + slots.teleport_ret).X("fire_ret", base + slots.fire_ret)
      .S("shot_source", out.shot_source ? "dronekill FireBulletLocal word swapped" : "NONE (dronekill off): the thunks only pass through")
      .D("muzzle_x", kMuzzle[0]).D("muzzle_y", kMuzzle[1]).D("muzzle_z", kMuzzle[2]).D("back_max_m", kBackMax)
      .Emit(out.shot_source ? NEVR_LOG_INFO : NEVR_LOG_WARNING, "dronemuzzle: on (QA)");
  return out;
}

// once a frame (the game thread, after ExclusiveUpdateGame)
void DroneMuzzleTick(std::uintptr_t) {
  const std::uint64_t now = NowNs(), last = g_last_frame_ns.exchange(now, std::memory_order_relaxed);
  if (last != 0)
    g_frame_dt.store(FrameDt(g_frame_dt.load(std::memory_order_relaxed), static_cast<float>(now - last) * 1e-9f), std::memory_order_relaxed);
}

DroneMuzzleStats GetDroneMuzzleStats() { return {static_cast<std::uint32_t>(g_shots.load()), static_cast<std::uint32_t>(g_tracers.load())}; }

void SetDroneMuzzleClockForTest(std::uint64_t (*now_ns)()) { g_clock_ns = now_ns; }

void ResetDroneMuzzleForTest() {
  g_shots.store(0);
  g_tracers.store(0);
  g_last_frame_ns.store(0);
  g_frame_dt.store(kDefaultFrameDt);
  g_clock_ns = nullptr;
  q15t_dm_tp_ret.store(0);
  q15t_dm_tp_next.store(0);
  q15t_dm_fire_ret.store(0);
  q15t_dm_fire_next.store(0);
}

}  // namespace q15t
