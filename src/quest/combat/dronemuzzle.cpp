// dronemuzzle (dronemuzzle.h, docs/quest/combat/dronemuzzle.md): two reviewed assembly entries through the hook manager, their two
// continuations, and the frame-time tick.
#include "quest/combat/dronemuzzle.h"

#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "quest/combat/combat_tick.h"
#include "quest/combat/dronekill.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"
#include "runtime/combat/bullet_velocity_guard_rules.h"
#include "runtime/patch/dronemuzzle_rules.h"

extern "C" {
// The words the entries read with ldar. The manager publishes each next word (InstallAsmEntry's nextOut) before the slot changes; the install
// publishes each call-site word (release) before the slot changes, so the entry never runs before it (a 0 word cannot match x30).
__attribute__((visibility("hidden"))) std::uintptr_t nevr_combat_dm_tp_ret = 0;
__attribute__((visibility("hidden"))) void* nevr_combat_dm_tp_next = nullptr;
__attribute__((visibility("hidden"))) std::uintptr_t nevr_combat_dm_fire_ret = 0;
__attribute__((visibility("hidden"))) void* nevr_combat_dm_fire_next = nullptr;
}

namespace nevr_quest::combat::dronemuzzle {

namespace {

using namespace ::nevr_dronemuzzle;  // the muzzle rule, shared with PC (runtime/patch/dronemuzzle_rules.h, #2337)
namespace pinned = sentinel::pinned;
using sentinel::LogLevel;

constexpr std::uintptr_t kCsIndex = 0xc8, kCsRecords = 0xf8, kBulletStride = 0x298;
constexpr std::uintptr_t kRecShooterVel = 0x11c, kRecShooterPos = 0x128, kRecSpawn = 0x134;
constexpr std::uintptr_t kPosePos = 0x10;  // CTransfQ: quaternion +0, position +0x10, scale +0x1c
constexpr int kShotLines = 40, kTracerLines = 3;  // SHOT_LOG (df 56)

sentinel::GotHook g_teleportHook;
sentinel::GotHook g_fireHook;

std::atomic<int> g_shots{0}, g_tracers{0};
std::atomic<std::uint64_t> g_last_frame_ns{0};
constexpr float kDefaultFrameDt = 1.0f / 90.0f;
std::atomic<float> g_frame_dt{kDefaultFrameDt};
std::uint64_t (*g_clock_ns)() = nullptr;
std::atomic<bool> g_installed{false};
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};
std::atomic<const Pins*> g_pinsOverride{nullptr};

std::uint64_t NowNs() noexcept {
  if (g_clock_ns != nullptr) return g_clock_ns();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull + static_cast<std::uint64_t>(ts.tv_nsec);
}

std::uint32_t Tid() noexcept { return static_cast<std::uint32_t>(syscall(SYS_gettid)); }

const char* V3(char (&b)[64], const float* v) noexcept {
  std::snprintf(b, sizeof b, "%.6f %.6f %.6f", static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]));
  return b;
}
const char* F(char (&b)[32], double v) noexcept {
  std::snprintf(b, sizeof b, "%.6f", v);
  return b;
}

void CapLine(const char* kind, int cap) noexcept {
  sentinel::LogFields(LogLevel::kInfo, "combat_dronemuzzle_cap",
                      {{"kind", kind}, {"cap", cap}, {"msg", "this kind of dronemuzzle line reached its cap: later ones are counted, not written"}});
}

}  // namespace

using TeleportFn = std::uint64_t (*)(void* gs, std::uint64_t actor, const std::uint8_t* pose, std::uint64_t reason);
using FireFn = std::uint64_t (*)(std::uint8_t* bcs, std::uint64_t handle, const void* gun, float damage, const void* quat, const void* vel,
                                 std::uint64_t remote, std::uint64_t flag);

// F4. Reached only from the entry, only for a call that returns into FireBulletLocal. Inlined into the continuation, so the continuation's own
// text loads the next word (check/dronemuzzle.sh).
__attribute__((always_inline)) inline std::uint64_t Teleport(void* gs, std::uint64_t actor, const std::uint8_t* pose, std::uint64_t reason) noexcept {
  const auto next = reinterpret_cast<TeleportFn>(__atomic_load_n(&nevr_combat_dm_tp_next, __ATOMIC_ACQUIRE));
  if (next == nullptr) return 0;
  int remote = 0;
  const float* v = dronekill::ShotVelocity(&remote);
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
  if (n == kShotLines + 1) CapLine("shot", kShotLines);
  if (n <= kShotLines) {
    char a[64], b[64], c[64], ms[32];
    sentinel::LogFields(LogLevel::kInfo, "combat_dronemuzzle_shot",
                        {{"shot", n}, {"remote", remote != 0 ? 1 : 0}, {"root", V3(a, root)}, {"spawn", V3(b, spawn)}, {"velocity", V3(c, v)},
                         {"frame_ms", F(ms, ClampDt(dt) * 1000.0f)}, {"tid", Tid()},
                         {"msg", "a drone's bullet starts at the drone's gun, not at the drone's root"}});
  }
  return r;
}

// F6. Reached only from the entry, only for a call that returns into FireBulletLocal. Inlined into the continuation.
__attribute__((always_inline)) inline std::uint64_t Fire(std::uint8_t* bcs, std::uint64_t handle, const void* gun, float damage, const void* quat, const void* vel, std::uint64_t remote,
                   std::uint64_t flag) noexcept {
  const auto next = reinterpret_cast<FireFn>(__atomic_load_n(&nevr_combat_dm_fire_next, __ATOMIC_ACQUIRE));
  if (next == nullptr) return 0;
  const std::uint64_t r = next(bcs, handle, gun, damage, quat, vel, remote, flag);
  int rem = 0;
  const float* v = dronekill::ShotVelocity(&rem);
  // not a drone shot / F46-6, DM-2: v ~ 0 or not finite; the bullet-velocity guard's rule, one definition (runtime/combat/bullet_velocity_guard_rules.h)
  if (::nevr_combat::bullet_velocity_guard::Skips(v)) return r;
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
  if (t == kTracerLines) CapLine("tracer", kTracerLines);
  if (t < kTracerLines) {
    char x[32], y[32], z[32];
    sentinel::LogFields(LogLevel::kInfo, "combat_dronemuzzle_tracer",
                        {{"x", F(x, p[0])}, {"y", F(y, p[1])}, {"z", F(z, p[2])}, {"index", handle & 0xffff}, {"tid", Tid()},
                         {"msg", "a drone bullet's tracer starts at the drone: its shooter velocity is 0 and its shooter position is its spawn"}});
  }
  return r;
}

}  // namespace nevr_quest::combat::dronemuzzle

namespace dmz = nevr_quest::combat::dronemuzzle;

extern "C" std::uint64_t nevr_combat_dm_teleport(void* gs, std::uint64_t actor, const std::uint8_t* pose, std::uint64_t reason) noexcept {
  return dmz::Teleport(gs, actor, pose, reason);
}
extern "C" std::uint64_t nevr_combat_dm_fire(std::uint8_t* bcs, std::uint64_t handle, const void* gun, float damage, const void* quat, const void* vel,
                                             std::uint64_t remote, std::uint64_t flag) noexcept {
  return dmz::Fire(bcs, handle, gun, damage, quat, vel, remote, flag);
}

#if defined(__aarch64__)
#include "quest/combat/dronemuzzle_entry.inc"
#else
// The host stand-ins: the same decision in C++ (the return address, then the continuation or the next word), all a host test can stand in for.
extern "C" __attribute__((visibility("hidden"), noinline)) std::uint64_t nevr_combat_dm_tp_entry(void* gs, std::uint64_t actor, const std::uint8_t* pose,
                                                                                                std::uint64_t reason) {
  const auto ret = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
  if (ret == __atomic_load_n(&nevr_combat_dm_tp_ret, __ATOMIC_ACQUIRE)) return nevr_combat_dm_teleport(gs, actor, pose, reason);
  return reinterpret_cast<dmz::TeleportFn>(__atomic_load_n(&nevr_combat_dm_tp_next, __ATOMIC_ACQUIRE))(gs, actor, pose, reason);
}
extern "C" __attribute__((visibility("hidden"), noinline)) std::uint64_t nevr_combat_dm_fire_entry(std::uint8_t* bcs, std::uint64_t handle, const void* gun,
                                                                                                  float damage, const void* quat, const void* vel,
                                                                                                  std::uint64_t remote, std::uint64_t flag) {
  const auto ret = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
  if (ret == __atomic_load_n(&nevr_combat_dm_fire_ret, __ATOMIC_ACQUIRE)) return nevr_combat_dm_fire(bcs, handle, gun, damage, quat, vel, remote, flag);
  return reinterpret_cast<dmz::FireFn>(__atomic_load_n(&nevr_combat_dm_fire_next, __ATOMIC_ACQUIRE))(bcs, handle, gun, damage, quat, vel, remote, flag);
}
#endif

namespace nevr_quest::combat::dronemuzzle {

namespace {

NEVR_ASM_ENTRY_RECORD(kTeleportEntry, ::nevr_combat_dm_tp_entry, ::nevr_combat_dm_teleport);
NEVR_ASM_ENTRY_RECORD(kFireEntry, ::nevr_combat_dm_fire_entry, ::nevr_combat_dm_fire);

const char* StatusOf(sentinel::GotStatus status) noexcept {
  return status == sentinel::GotStatus::kOk ? "installed" : sentinel::GotStatusName(status);
}

void LogHook(const char* hook, sentinel::GotStatus status) noexcept {
  sentinel::LogFields(status == sentinel::GotStatus::kOk ? LogLevel::kInfo : LogLevel::kError, "combat_dronemuzzle_hook",
                      {{"hook", hook}, {"status", StatusOf(status)}});
}

Result Refuse(Result out, const char* token, const char* why) noexcept {
  out.ok = false;
  out.status = token;
  sentinel::LogFields(LogLevel::kError, "combat_dronemuzzle_install",
                      {{"result", "refused"}, {"reason", token}, {"why", why},
                       {"consequence", "dronemuzzle is not installed: a drone's bullets start where the game puts them"}});
  return out;
}

void ClearWords() noexcept {
  __atomic_store_n(&nevr_combat_dm_tp_ret, static_cast<std::uintptr_t>(0), __ATOMIC_RELEASE);
  __atomic_store_n(&nevr_combat_dm_fire_ret, static_cast<std::uintptr_t>(0), __ATOMIC_RELEASE);
}

}  // namespace

Result InstallAt(sentinel::ImageLookup lookup, const Pins& pins) noexcept {
  Result out;
  if (g_installed.load(std::memory_order_acquire)) return Refuse(out, "already_ran", "a second step installs nothing");
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(pinned::kLibR15, &image)) return Refuse(out, "module_not_loaded", "libr15.so is not loaded");
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refuse(out, "no_build_id", "libr15.so has no build ID");
  if (std::strcmp(id, pinned::LibR15PatchedBuildId()) != 0) return Refuse(out, "build_id_mismatch", "libr15.so is not the pinned build");
  const std::uintptr_t base = image.base;
  // both call-site words before any slot changes (DM-1)
  __atomic_store_n(&nevr_combat_dm_tp_ret, base + pins.teleportRet, __ATOMIC_RELEASE);
  __atomic_store_n(&nevr_combat_dm_fire_ret, base + pins.fireRet, __ATOMIC_RELEASE);
  out.teleport = sentinel::InstallAsmEntry(g_teleportHook, Target(kTeleportSymbol, pins.teleportSlot, base, pins.teleportFn), kTeleportEntry,
                                           &nevr_combat_dm_tp_next, lookup);
  LogHook("CncaGameSpace::WorldTeleport", out.teleport);
  if (out.teleport != sentinel::GotStatus::kOk) {
    ClearWords();
    return Refuse(out, "hook_failed", "the WorldTeleport entry was not installed");
  }
  out.fire = sentinel::InstallAsmEntry(g_fireHook, Target(kFireSymbol, pins.fireSlot, base, pins.fireFn), kFireEntry, &nevr_combat_dm_fire_next, lookup);
  LogHook("CR15NetBulletCS::FireBullet", out.fire);
  if (out.fire != sentinel::GotStatus::kOk) {
    const sentinel::GotStatus back = g_teleportHook.Remove();
    ClearWords();
    sentinel::LogFields(back == sentinel::GotStatus::kOk ? LogLevel::kWarn : LogLevel::kError, "combat_dronemuzzle_rollback",
                        {{"failed_hook", "CR15NetBulletCS::FireBullet"}, {"teleport_removed", back == sentinel::GotStatus::kOk ? 1 : 0},
                         {"consequence", back == sentinel::GotStatus::kOk ? "the WorldTeleport entry was removed: dronemuzzle is not installed"
                                                                          : "the WorldTeleport entry could not be removed: it passes every call on"}});
    return Refuse(out, "hook_failed", "the FireBullet entry was not installed");
  }
  g_installed.store(true, std::memory_order_release);
  out.ok = true;
  out.status = "installed";
  char tr[19], fr[19], mx[32], my[32], mz[32], bm[32];
  sentinel::LogFields(LogLevel::kInfo, "combat_dronemuzzle_install",
                      {{"result", "installed"}, {"reason", "ok"}, {"teleport_ret", sentinel::HexString(tr, base + pins.teleportRet)},
                       {"fire_ret", sentinel::HexString(fr, base + pins.fireRet)}, {"muzzle_x", F(mx, kMuzzle[0])}, {"muzzle_y", F(my, kMuzzle[1])},
                       {"muzzle_z", F(mz, kMuzzle[2])}, {"back_max_m", F(bm, kBackMax)},
                       {"consequence", "a drone's bullets leave its gun during a drone shot; every other call passes through"}});
  return out;
}

ModuleResult Install(bool on) noexcept {
  if (!on) {
    sentinel::LogFields(LogLevel::kInfo, "combat_dronemuzzle_install",
                        {{"result", "off"}, {"reason", "off_config"},
                         {"consequence", "combat.dronemuzzle is false: a drone's bullets start where the game puts them"}});
    return {false, "off", true};  // off by its key: counted apart on the phase line, neither installed nor failed
  }
  if (!dronekill::ShotActive()) {
    return {false, Refuse(Result{}, "needs_dronekill_shot", "dronekill's FireBulletLocal hook is not installed, so no drone shot is ever seen").status};
  }
  if (!tick::Add(&Tick)) return {false, Refuse(Result{}, "tick_table_full", "the combat tick has no room for the frame time").status};
  const sentinel::ImageLookup override = g_lookup.load(std::memory_order_acquire);
  const Pins* const pins = g_pinsOverride.load(std::memory_order_acquire);
  const Result r = InstallAt(override != nullptr ? override : &sentinel::FindLoadedImage, pins != nullptr ? *pins : kPins);
  return {r.ok, r.status};
}

// once a frame (the game thread, after ExclusiveUpdateGame). Out of dronekill's scope (no combat map) it reads no clock and forgets the last frame,
// so the first frame back in a combat map only records the time.
void Tick(std::uintptr_t) noexcept {
  if (!dronekill::InCombatScope()) {
    g_last_frame_ns.store(0, std::memory_order_relaxed);
    return;
  }
  const std::uint64_t now = NowNs(), last = g_last_frame_ns.exchange(now, std::memory_order_relaxed);
  if (last != 0)
    g_frame_dt.store(FrameDt(g_frame_dt.load(std::memory_order_relaxed), static_cast<float>(now - last) * 1e-9f), std::memory_order_relaxed);
}

Stats CurrentStats() noexcept { return {static_cast<std::uint32_t>(g_shots.load()), static_cast<std::uint32_t>(g_tracers.load())}; }

Words WordsForTest() noexcept {
  return {__atomic_load_n(&nevr_combat_dm_tp_ret, __ATOMIC_ACQUIRE), reinterpret_cast<std::uintptr_t>(__atomic_load_n(&nevr_combat_dm_tp_next, __ATOMIC_ACQUIRE)),
          __atomic_load_n(&nevr_combat_dm_fire_ret, __ATOMIC_ACQUIRE), reinterpret_cast<std::uintptr_t>(__atomic_load_n(&nevr_combat_dm_fire_next, __ATOMIC_ACQUIRE))};
}

void SetClockForTest(std::uint64_t (*now_ns)()) noexcept { g_clock_ns = now_ns; }
void SetInstallForTest(sentinel::ImageLookup lookup, const Pins* pins) noexcept {
  g_lookup.store(lookup, std::memory_order_release);
  g_pinsOverride.store(pins, std::memory_order_release);
}

void ResetForTest() noexcept {
  g_teleportHook.Forget();
  g_fireHook.Forget();
  g_installed.store(false);
  g_shots.store(0);
  g_tracers.store(0);
  g_last_frame_ns.store(0);
  g_frame_dt.store(kDefaultFrameDt);
  g_clock_ns = nullptr;
  ClearWords();
  __atomic_store_n(&nevr_combat_dm_tp_next, static_cast<void*>(nullptr), __ATOMIC_RELEASE);
  __atomic_store_n(&nevr_combat_dm_fire_next, static_cast<void*>(nullptr), __ATOMIC_RELEASE);
  SetInstallForTest(nullptr, nullptr);
}

}  // namespace nevr_quest::combat::dronemuzzle
