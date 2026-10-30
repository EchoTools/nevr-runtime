// fxguard: see fxguard.h and docs/quest/q15t/modules/fxguard.md.
#include "fxguard.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "module_switch.h"
#include "plugin_log.h"
#include "safe_read.h"

namespace q15t {

// ELF vaddrs of the pinned build. Slots and functions: readelf -rW libr15.so (GLOB_DAT 0x37148e0 and JUMP_SLOT 0x36dd210 -> 0x1706904
// CSoundCS::PlaySound; JUMP_SLOT 0x3701090 -> 0x134b468 CSoundPlayer::Play3DSound; JUMP_SLOT 0x36f3cf8 -> 0x210c9c4
// CR15NetBulletCS::FireBullet). bullet_ret: the instruction after the `bl` at 0x21230d0; bullet_default: the word 0x2122fdc loads.
const FxGuardSlots kFxGuardPinned = {0x36dd210, 0x37148e0, 0x1706904, 0x3701090, 0x134b468, 0x21230d4, 0x37af76c, 0x36f3cf8, 0x210c9c4};

namespace {

// PlaySound returns a pair in x0, x1 (ReVault 0x1806a08..0x1806a18: x0 = the post result << 32 | the emitter id).
struct SoundPair {
  std::uint64_t id, ok;
};
using PlaySoundFn = SoundPair (*)(std::uintptr_t cs, std::uint64_t handle, std::uint64_t symbol, float volume, std::uint64_t debug0,
                                  std::uint64_t debug1);
using Play3DFn = std::uint32_t (*)(std::uintptr_t player, std::uint64_t symbol, std::uint32_t emitter, float volume);
// FireBullet(bcs, bullet handle, gun handle, float, const CQuaternion& (the muzzle transform), const C3Vector& velocity, u32, u32)
using FireFn = std::uint64_t (*)(std::uint8_t* bcs, std::uint64_t handle, const void* gun, float scale, const void* xform,
                                 const float* velocity, std::uint64_t a, std::uint64_t b);

// CSoundCS: the handle's index table and the pool (0xc0-byte entries: flags byte +3, emitter id +4).
constexpr std::uintptr_t kCsIndex = 0xc8, kCsPool = 0xf8, kEntryStride = 0xc0, kEntryFlags = 3, kEntryEmitter = 4;
constexpr std::uint8_t kEntryReleased = 0x2;
constexpr std::uint32_t kSiteSymbolOne = 1;  // the CSoundSymbol the invalid symbol's own row carries
constexpr float kMinSpeedSquared = 1e-6f;
constexpr int kFireLines = 10, kSiteLines = 4;
constexpr std::uint64_t kSummaryMs = 10000;

std::atomic<std::uintptr_t> g_play_real{0}, g_play_data_real{0}, g_play3d_real{0}, g_fire_real{0};
std::atomic<std::uintptr_t> g_site_ret{0};      // base + bullet_ret, 0 = site rule off
std::atomic<std::uintptr_t> g_site_default{0};  // base + bullet_default
std::atomic<bool> g_invalid_id{true};
std::atomic<std::uint32_t> g_sound_zero{0}, g_sound_invalid{0}, g_play3d_dropped{0}, g_site_calls{0}, g_site_dropped{0}, g_fire_skipped{0};
std::atomic<bool> g_changed{false};
std::uint64_t g_last_summary_ms = 0;  // game thread only
std::uint64_t (*g_clock_ms)() = nullptr;

std::uint64_t NowMs() {
  if (g_clock_ms != nullptr) return g_clock_ms();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000ull;
}

void Hex(char* out, std::size_t n, unsigned long long v) { std::snprintf(out, n, "0x%llx", v); }

// What PlaySound returns when its post fails, read from the same pool entry PlaySound reads. An unreadable entry reads as released.
SoundPair FailedPost(std::uintptr_t cs, std::uint64_t handle) {
  std::uintptr_t index = 0, pool = 0;
  std::uint16_t slot = 0;
  std::uint8_t flags = kEntryReleased;
  std::uint32_t emitter = 0;
  if (SafeRead(cs + kCsIndex, &index, sizeof index) && SafeRead(cs + kCsPool, &pool, sizeof pool) &&
      SafeRead(index + (handle & 0xffff) * 4, &slot, sizeof slot)) {
    const std::uintptr_t e = pool + static_cast<std::uintptr_t>(slot) * kEntryStride;
    if (!SafeRead(e + kEntryFlags, &flags, sizeof flags) || !SafeRead(e + kEntryEmitter, &emitter, sizeof emitter)) flags = kEntryReleased;
  }
  if ((flags & kEntryReleased) != 0) return {~0ull, 0};
  return {0xffffffff00000000ull | emitter, 1};
}

SoundPair PlaySoundGuard(std::uintptr_t cs, std::uint64_t handle, std::uint64_t symbol, float volume, std::uint64_t debug0,
                         std::uint64_t debug1) {
  if (!FxGuardDropsPlaySound(symbol, g_invalid_id.load(std::memory_order_relaxed))) {
    const auto real = reinterpret_cast<PlaySoundFn>(g_play_real.load(std::memory_order_acquire));
    return real(cs, handle, symbol, volume, debug0, debug1);
  }
  (symbol == kFxGuardInvalidSymbol ? g_sound_invalid : g_sound_zero).fetch_add(1, std::memory_order_relaxed);
  g_changed.store(true, std::memory_order_relaxed);
  return FailedPost(cs, handle);
}

__attribute__((noinline)) std::uint32_t Play3DGuard(std::uintptr_t player, std::uint64_t symbol, std::uint32_t emitter, float volume) {
  // the caller's return address: the PLT stub branches with x30 untouched, so a call from Update's site reads bullet_ret
  const auto ret = reinterpret_cast<std::uintptr_t>(__builtin_return_address(0));
  const std::uintptr_t site_ret = g_site_ret.load(std::memory_order_acquire);
  const bool site = site_ret != 0 && ret == site_ret;
  std::uint32_t def = 0;
  // the miss default, live per call (the game sets it at run time); a word in libr15's own .bss, mapped while libr15 is
  if (site) def = __atomic_load_n(reinterpret_cast<const std::uint32_t*>(g_site_default.load(std::memory_order_relaxed)), __ATOMIC_RELAXED);
  const bool drop = FxGuardDropsPlay3D(symbol, g_invalid_id.load(std::memory_order_relaxed), site, def);
  if (site) {
    const std::uint32_t n = g_site_calls.fetch_add(1, std::memory_order_relaxed);
    if (drop) g_site_dropped.fetch_add(1, std::memory_order_relaxed);
    g_changed.store(true, std::memory_order_relaxed);
    if (n < static_cast<std::uint32_t>(kSiteLines)) {
      char sym[24], d[16];
      Hex(sym, sizeof sym, symbol);
      std::snprintf(d, sizeof d, "0x%08x", def);
      const NevrLogField f[] = {{"symbol", sym}, {"site_default", d}, {"outcome", drop ? "dropped" : "passed"}};
      Log(NEVR_LOG_INFO, "fxguard: Play3DSound from the bullet update (so an unregistered bullet sound posts nothing)", f, 3);
    }
  }
  if (!drop) {
    const auto real = reinterpret_cast<Play3DFn>(g_play3d_real.load(std::memory_order_acquire));
    return real(player, symbol, emitter, volume);
  }
  g_play3d_dropped.fetch_add(1, std::memory_order_relaxed);
  g_changed.store(true, std::memory_order_relaxed);
  return 0;  // Play3DSound's own early-out value (ReVault 0x144b488 mov w0, wzr)
}

std::uint64_t FireGuard(std::uint8_t* bcs, std::uint64_t handle, const void* gun, float scale, const void* xform, const float* velocity,
                        std::uint64_t a, std::uint64_t b) {
  if (FxGuardSkipsFire(velocity)) {
    const std::uint32_t n = g_fire_skipped.fetch_add(1, std::memory_order_relaxed);
    g_changed.store(true, std::memory_order_relaxed);
    if (n < static_cast<std::uint32_t>(kFireLines)) {
      const float v[3] = {velocity != nullptr ? velocity[0] : 0.0f, velocity != nullptr ? velocity[1] : 0.0f,
                          velocity != nullptr ? velocity[2] : 0.0f};
      char h[24], vel[64];
      std::snprintf(h, sizeof h, "0x%llx", static_cast<unsigned long long>(handle & 0xffffffffull));
      std::snprintf(vel, sizeof vel, "%.3f,%.3f,%.3f", static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]));
      const NevrLogField f[] = {{"bullet", h}, {"velocity", vel}};
      Log(NEVR_LOG_INFO, "fxguard: fire skipped, the velocity is zero or not finite (a NaN bullet direction would stall the game loop)", f, 2);
    }
    return 0;  // FireBullet returns nothing the caller reads
  }
  const auto next = reinterpret_cast<FireFn>(g_fire_real.load(std::memory_order_acquire));
#if defined(__aarch64__) && defined(__clang__)
  // A tail call: x30 stays the game caller's, which dronemuzzle's thunk behind this compares with FireBulletLocal's call site.
  __attribute__((musttail)) return next(bcs, handle, gun, scale, xform, velocity, a, b);
#else
  return next(bcs, handle, gun, scale, xform, velocity, a, b);
#endif
}

void LogHook(const char* hook, std::uintptr_t slot_vaddr, const InstallResult& r) {
  char s[24];
  Hex(s, sizeof s, slot_vaddr);
  const NevrLogField f[] = {{"hook", hook}, {"slot_vaddr", s}, {"status", StatusName(r.status)}, {"reason", r.reason}};
  Log(r.status == SwapStatus::kOk ? NEVR_LOG_INFO : NEVR_LOG_ERROR, "fxguard hook install", f, 4);
}

InstallResult Off() {
  InstallResult r;
  r.reason = "off";
  return r;
}

bool InstallSound(std::uintptr_t base, const FxGuardSlots& s, FxGuardInstall* out) {
  if (!CheckWordPins("fxguard sound", base, kFxGuardSoundPins, sizeof kFxGuardSoundPins / sizeof kFxGuardSoundPins[0],
                     "the sound guard is off")) {
    out->play.reason = out->play_data.reason = out->play3d.reason = "word_pin_mismatch";
    return false;
  }
  g_invalid_id.store(!ModuleSwitchPresent("fxsndguardid.off"), std::memory_order_relaxed);
  // the bullet site, published before the Play3DSound slot is swapped; its pins failing turns off the site rule only
  if (g_invalid_id.load(std::memory_order_relaxed) &&
      CheckWordPins("fxguard bullet site", base, kFxGuardBulletSitePins, sizeof kFxGuardBulletSitePins / sizeof kFxGuardBulletSitePins[0],
                    "the bullet-site rule is off")) {
    g_site_default.store(base + s.bullet_default, std::memory_order_relaxed);
    g_site_ret.store(base + s.bullet_ret, std::memory_order_release);
    out->bullet_site_on = true;
  }
  out->play = InstallImportHook("CSoundCS::PlaySound", base, s.play_slot, reinterpret_cast<std::uintptr_t>(&PlaySoundGuard), &g_play_real,
                                s.play_fn);
  LogHook("CSoundCS::PlaySound (JUMP_SLOT)", s.play_slot, out->play);
  if (out->play.status != SwapStatus::kOk) {
    if (out->play.status == SwapStatus::kRestoreFailed) SwapWord(base + s.play_slot, reinterpret_cast<std::uintptr_t>(&PlaySoundGuard), base + s.play_fn);
    g_site_ret.store(0, std::memory_order_release);
    out->bullet_site_on = false;
    return false;
  }
  // the queued path: the same wrapper, which calls g_play_real (the same function: both words hold base + play_fn)
  out->play_data = InstallImportHook("CSoundCS::PlaySound", base, s.play_data_slot, reinterpret_cast<std::uintptr_t>(&PlaySoundGuard),
                                     &g_play_data_real, s.play_fn);
  LogHook("CSoundCS::PlaySound (GLOB_DAT, DeferredPlaySound's queue)", s.play_data_slot, out->play_data);
  out->play3d = InstallImportHook("CSoundPlayer::Play3DSound", base, s.play3d_slot, reinterpret_cast<std::uintptr_t>(&Play3DGuard),
                                  &g_play3d_real, s.play3d_fn);
  LogHook("CSoundPlayer::Play3DSound (JUMP_SLOT)", s.play3d_slot, out->play3d);
  if (out->play3d.status != SwapStatus::kOk) {
    g_site_ret.store(0, std::memory_order_release);
    out->bullet_site_on = false;
  }
  // on: PlaySound's direct path is guarded; a refused queued or Play3DSound word is its own ERROR line above and fails the summary
  return true;
}

}  // namespace

bool FxGuardDropsPlaySound(std::uint64_t symbol, bool invalid_id_on) {
  return static_cast<std::uint32_t>(symbol) == 0 || (invalid_id_on && symbol == kFxGuardInvalidSymbol);
}

bool FxGuardDropsPlay3D(std::uint64_t symbol, bool invalid_id_on, bool from_bullet_site, std::uint32_t site_default) {
  if (!invalid_id_on) return false;
  if (symbol == kFxGuardInvalidSymbol) return true;
  const auto low = static_cast<std::uint32_t>(symbol);
  return from_bullet_site && (low == site_default || low == kSiteSymbolOne);
}

// Called once per shot: a plain read, no syscall. FireBullet itself reads the same three floats right after (ReVault 0x220cca4), so the
// read is no less safe than the call it guards.
bool FxGuardSkipsFire(const float* velocity) {
  if (velocity == nullptr) return true;
  const float v2 = velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2];
  return !(v2 > kMinSpeedSquared) || !std::isfinite(v2);
}

FxGuardInstall InstallFxGuard(std::uintptr_t base, const FxGuardSlots& s) {
  FxGuardInstall out;
  out.play = out.play_data = out.play3d = out.fire = Off();
  if (ModuleSwitchPresent("fxguard.off")) {
    out.summary.reason = "off_file";
    const NevrLogField f[] = {{"module", "fxguard"}, {"outcome", "off"}, {"switch", "fxguard.off"}};
    Log(NEVR_LOG_INFO, "q15t_fxguard_install", f, 3);
    return out;
  }
  bool failed = false;
  if (ModuleSwitchPresent("fxsndguard.off")) {
    out.sound_on = false;
  } else {
    out.sound_on = InstallSound(base, s, &out);
    failed = !out.sound_on || out.play_data.status != SwapStatus::kOk || out.play3d.status != SwapStatus::kOk;
  }
  if (!ModuleSwitchPresent("fxfireguard.off")) {
    if (!CheckWordPins("fxguard fire", base, kFxGuardFirePins, sizeof kFxGuardFirePins / sizeof kFxGuardFirePins[0], "the fire guard is off")) {
      out.fire.reason = "word_pin_mismatch";
    } else {
      // after dronemuzzle (plugin.cpp): the slot holds the game's FireBullet or dronemuzzle's thunk, and this calls whichever it is
      out.fire = InstallChainedImportHook(base, s.fire_slot, reinterpret_cast<std::uintptr_t>(&FireGuard), &g_fire_real, s.fire_fn);
      LogHook("CR15NetBulletCS::FireBullet (JUMP_SLOT, chained)", s.fire_slot, out.fire);
    }
    out.fire_on = out.fire.status == SwapStatus::kOk;
    failed = failed || !out.fire_on;
  }
  if (!out.sound_on && !out.fire_on && !failed) {
    out.summary.reason = "off_file";  // both guards switched off by their own files
  } else if (failed) {
    const InstallResult* hooks[] = {&out.play, &out.play_data, &out.play3d, &out.fire};
    for (const InstallResult* h : hooks)
      if (h->status != SwapStatus::kOk && std::strcmp(h->reason, "off") != 0) {
        out.summary = *h;
        break;
      }
    if (out.summary.reason[0] == 0 && out.summary.status == SwapStatus::kNullSlot) out.summary.reason = "hook_failed";
  } else {
    out.summary.status = SwapStatus::kOk;
  }
  const NevrLogField f[] = {{"module", "fxguard"},
                            {"outcome", out.summary.status == SwapStatus::kOk ? "on" : (failed ? "partial_or_refused" : "off")},
                            {"sound_guard", out.sound_on ? "on" : "off"},
                            {"invalid_id_drop", out.sound_on && g_invalid_id.load(std::memory_order_relaxed) ? "on" : "off"},
                            {"bullet_site", out.bullet_site_on ? "on" : "off"},
                            {"fire_guard", out.fire_on ? "on" : "off"},
                            {"reason", out.summary.reason}};
  Log(failed ? NEVR_LOG_ERROR : NEVR_LOG_INFO, "q15t_fxguard_install", f, 7);
  return out;
}

void FxGuardTick(std::uintptr_t) {
  const std::uint64_t now = NowMs();
  if (g_last_summary_ms == 0) g_last_summary_ms = now;
  if (now - g_last_summary_ms < kSummaryMs) return;
  g_last_summary_ms = now;
  if (!g_changed.exchange(false, std::memory_order_relaxed)) return;
  const FxGuardStats s = GetFxGuardStats();
  const KV f[] = {{"playsound_symbol_zero", s.sound_zero},   {"playsound_invalid_id", s.sound_invalid}, {"play3d_dropped", s.play3d_dropped},
                  {"bullet_site_calls", s.site_calls},        {"bullet_site_dropped", s.site_dropped},   {"fire_skipped", s.fire_skipped}};
  LogNums(NEVR_LOG_INFO, "fxguard: totals (sound posts answered as failed without the game's callstack print; shots skipped)", f, 6);
}

FxGuardStats GetFxGuardStats() {
  return {g_sound_zero.load(), g_sound_invalid.load(), g_play3d_dropped.load(), g_site_calls.load(), g_site_dropped.load(), g_fire_skipped.load()};
}

void SetFxGuardClockForTest(std::uint64_t (*now_ms)()) { g_clock_ms = now_ms; }

void ResetFxGuardForTest() {
  g_play_real = g_play_data_real = g_play3d_real = g_fire_real = 0;
  g_site_ret = g_site_default = 0;
  g_invalid_id = true;
  g_sound_zero = g_sound_invalid = g_play3d_dropped = g_site_calls = g_site_dropped = g_fire_skipped = 0;
  g_changed = false;
  g_last_summary_ms = 0;
  g_clock_ms = nullptr;
}

}  // namespace q15t
