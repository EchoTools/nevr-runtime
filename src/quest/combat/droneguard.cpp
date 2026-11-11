#include "quest/combat/droneguard.h"

#include <dlfcn.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::droneguard {

namespace {

sentinel::GotHook g_hook;

constexpr std::size_t kSpan = kOffHandleIdx + 4 - kOffTarget;  // one read covers every field the guard looks at

struct Snap {  // the span [cs+0x140, cs+0x194)
  std::uintptr_t target;
  std::uintptr_t sys;
  std::uint32_t idx;
  std::uint8_t flags;
};

// Stage 1's engine functions, set before the thunk is armed and read-only after; whether stage 1 runs.
EngineFns g_f{};
std::atomic<bool> g_idxOn{false};
std::atomic<unsigned> g_skips{0}, g_totals{0}, g_idxSkips{0}, g_idxTotals{0}, g_stateLines{0};
std::atomic<bool> g_skipping{false}, g_idxSkipping{false};
std::atomic<std::uint64_t> g_state{~0ULL};  // no real key has bits above 41 set
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};

constexpr const char* kConsequence = "the drone's follow update is skipped while its target is null instead of crashing the game, in every level";
constexpr const char* kRefusedConsequence = "a drone with no orbit target faults the game's Follow";

// The fast path: four plain loads from the follow component. Follow loads [cs+0xc8] as its first data access, so the game dereferences the same
// object on every call and these loads are as safe as the stock call; a null component goes to the game's function. No system call.
bool ReadSnapPlain(std::uintptr_t cs, Snap* out) noexcept {
  if (cs == 0) return false;
  out->target = *reinterpret_cast<const volatile std::uintptr_t*>(cs + kOffTarget);
  out->sys = *reinterpret_cast<const volatile std::uintptr_t*>(cs + kOffHandleSys);
  out->idx = *reinterpret_cast<const volatile std::uint32_t*>(cs + kOffHandleIdx);
  out->flags = *reinterpret_cast<const volatile std::uint8_t*>(cs + kOffHandleFlags);
  return true;
}

// The stage-1 read after the engine's own refresh: the never-faulting read (that cache can be torn down under it), and stage 1 is off by default.
bool ReadSnap(std::uintptr_t cs, Snap* out) noexcept {
  std::uint8_t buf[kSpan];
  if (!SafeRead(cs + kOffTarget, buf, sizeof(buf))) return false;
  std::memcpy(&out->target, buf, sizeof(out->target));
  std::memcpy(&out->sys, buf + (kOffHandleSys - kOffTarget), sizeof(out->sys));
  std::memcpy(&out->idx, buf + (kOffHandleIdx - kOffTarget), sizeof(out->idx));
  std::memcpy(&out->flags, buf + (kOffHandleFlags - kOffTarget), sizeof(out->flags));
  return true;
}

void LogStats(bool final) noexcept {
  sentinel::LogFields(final ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kDebug, "combat_droneguard_stats",
                      {{"calls", FollowThunk::Calls()},
                       {"skips", g_skips.load(std::memory_order_relaxed)},
                       {"idx_skips", g_idxSkips.load(std::memory_order_relaxed)},
                       {"final", final ? 1 : 0},
                       {"consequence", "calls is every Follow call the game made, skips those not made because the target was null, idx_skips those stage 1 "
                                       "did not make; their sum over the run's time is the call rate of Follow"}});
}

// One line on the first call and on every change of (raw index word, flag byte, target null, handle system null), at most kStateLines. Read-only: the
// values are the previous frame's cache (Follow refreshes it itself). Written before the game's function is called.
void StateLine(std::uintptr_t cs, const Snap& s) noexcept {
  const std::uint64_t key = s.idx | (static_cast<std::uint64_t>(s.flags) << 32) | (static_cast<std::uint64_t>(s.target != 0) << 40) |
                            (static_cast<std::uint64_t>(s.sys != 0) << 41);
  if (g_state.exchange(key, std::memory_order_relaxed) == key) return;
  if (g_stateLines.fetch_add(1, std::memory_order_relaxed) >= kStateLines) return;
  char c[19];
  char raw[16];
  char fl[8];
  std::snprintf(raw, sizeof(raw), "%08x", s.idx);
  std::snprintf(fl, sizeof(fl), "%02x", s.flags);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_droneguard_state",
                      {{"target", s.target != 0 ? "set" : "0"},
                       {"handle_sys", s.sys != 0 ? "set" : "0"},
                       {"idx_unresolved", (s.idx & 0xffff) == 0xffff ? "yes" : "no"},
                       {"raw190", raw},
                       {"flags16c", fl},
                       {"cs", sentinel::HexString(c, cs)}});
}

// nullptr: run Follow; else the skip reason. Calls the engine's RefreshCache on the handle first.
const char* IdxSkip(std::uintptr_t cs) noexcept {
  g_f.refresh(reinterpret_cast<void*>(cs + kOffHandle));
  Snap s{};
  if (!ReadSnap(cs, &s)) return nullptr;  // doubt: Follow runs
  if ((s.idx & 0xffff) == 0xffff) return "idx ffff";
  if (s.sys == 0) return "handle-sys 0";
  if (!g_f.isValid(reinterpret_cast<const void*>(s.target), s.idx)) return "refused but cached";
  return nullptr;
}

void LogSkipRun(std::uintptr_t cs, unsigned n) noexcept {
  if (!g_skipping.exchange(true, std::memory_order_relaxed)) {
    char c[19];
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_droneguard_skip",
                        {{"cs", sentinel::HexString(c, cs)},
                         {"consequence", "the follow target is null: this component's follow update is skipped, as the game's would fault on it"}});
  } else if (n % kTotalEvery == 0 && g_totals.fetch_add(1, std::memory_order_relaxed) < kTotalLines) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_droneguard_skips", {{"skips", n}});
  }
}

void LogIdxSkipRun(std::uintptr_t cs, const char* why, unsigned n) noexcept {
  if (!g_idxSkipping.exchange(true, std::memory_order_relaxed)) {
    char c[19];
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_droneguard_idx_skip",
                        {{"why", why}, {"cs", sentinel::HexString(c, cs)},
                         {"consequence", "the followed player's cache is stale: this component's follow update is skipped"}});
  } else if (n % kTotalEvery == 0 && g_idxTotals.fetch_add(1, std::memory_order_relaxed) < kTotalLines) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_droneguard_idx_skips", {{"idx_skips", n}});
  }
}

// Every log below is written before the game's Follow is called or instead of calling it, so no game frame is live above it.
void FollowHandler(FollowThunk::Fn original, void* self, std::uint64_t handle) noexcept {
  const std::uint64_t calls = FollowThunk::Calls();
  if ((calls & (kStatsEvery - 1)) == 0) LogStats(false);
  const auto cs = reinterpret_cast<std::uintptr_t>(self);
  Snap s{};
  if (!ReadSnapPlain(cs, &s)) {  // a null component: Follow decides
    original(self, handle);
    return;
  }
  StateLine(cs, s);
  if (s.target == 0) {
    const unsigned n = g_skips.fetch_add(1, std::memory_order_relaxed) + 1;
    LogSkipRun(cs, n);
    return;
  }
  if (g_skipping.exchange(false, std::memory_order_relaxed)) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_droneguard_target_set", {{"skips", g_skips.load(std::memory_order_relaxed)}});
  }
  if (g_idxOn.load(std::memory_order_relaxed)) {
    if (const char* why = IdxSkip(cs)) {
      const unsigned n = g_idxSkips.fetch_add(1, std::memory_order_relaxed) + 1;
      LogIdxSkipRun(cs, why, n);
      return;
    }
    if (g_idxSkipping.exchange(false, std::memory_order_relaxed)) {
      sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_droneguard_idx_resolved", {{"idx_skips", g_idxSkips.load(std::memory_order_relaxed)}});
    }
  }
  original(self, handle);
}

NEVR_HOOK_RECORD(kFollowRecord, FollowThunk, &FollowHandler);

ModuleResult Refused(const char* token, const pinned::Failure* word = nullptr) noexcept {
  if (word != nullptr) {
    char at[19], haveText[19], wantText[19];
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_droneguard_install",
                        {{"result", "refused"}, {"reason", token}, {"pin", word->what}, {"vaddr", sentinel::HexString(at, word->vaddr)},
                         {"found", word->readable ? sentinel::HexString(haveText, word->have) : "unreadable"},
                         {"expected", sentinel::HexString(wantText, word->want)}, {"consequence", kRefusedConsequence}});
  } else {
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_droneguard_install",
                        {{"result", "refused"}, {"reason", token}, {"consequence", kRefusedConsequence}});
  }
  return {false, token};
}

// Stage 1's preconditions: its own words, then both engine functions. Returns the stage-1 state token and writes the stage-1 line.
const char* PrepareStageOne(std::uintptr_t base, const EngineFns* fns) noexcept {
  pinned::Failure word;
  if (pinned::FirstFailure(base, &kIdxWordPins, 1, &word)) {
    char at[19], haveText[19], wantText[19];
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_droneguard_stage1",
                        {{"result", "null_half_only"}, {"reason", "pinned_word_mismatch"}, {"pin", word.what}, {"vaddr", sentinel::HexString(at, word.vaddr)},
                         {"found", word.readable ? sentinel::HexString(haveText, word.have) : "unreadable"},
                         {"expected", sentinel::HexString(wantText, word.want)},
                         {"consequence", "the cache guard is off; only the null-target guard runs"}});
    return "null_half_only";
  }
  if (fns == nullptr || fns->refresh == nullptr || fns->isValid == nullptr) {
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_droneguard_stage1",
                        {{"result", "null_half_only"}, {"reason", "engine_functions_missing"},
                         {"consequence", "RefreshCache and IsComponentValid are not exported at the pinned addresses: the cache guard is off; only the null-target "
                                         "guard runs"}});
    return "null_half_only";
  }
  g_f = *fns;
  g_idxOn.store(true, std::memory_order_relaxed);  // before the swap: the handler never sees a half-set stage
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_droneguard_stage1",
                      {{"result", "on"}, {"reason", "ok"},
                       {"consequence", "RefreshCache runs first, then a stale cached player skips the follow update"}});
  return "on";
}

}  // namespace

bool EngineExportsMatch(std::uintptr_t base, const void* refresh, const void* valid, EngineFns* out) noexcept {
  if (reinterpret_cast<std::uintptr_t>(refresh) != base + kRefreshCacheVaddr || reinterpret_cast<std::uintptr_t>(valid) != base + kIsValidVaddr) return false;
  out->refresh = reinterpret_cast<void (*)(void*)>(const_cast<void*>(refresh));
  out->isValid = reinterpret_cast<bool (*)(const void*, std::uint64_t)>(const_cast<void*>(valid));
  return true;
}

bool ResolveEngineFns(std::uintptr_t base, EngineFns* out) noexcept {
  void* const h = dlopen("libr15.so", RTLD_NOW | RTLD_NOLOAD);
  if (h == nullptr) return false;
  void* const refresh = dlsym(h, kRefreshCacheSymbol);
  void* const valid = dlsym(h, kIsValidSymbol);
  dlclose(h);
  return EngineExportsMatch(base, refresh, valid, out);
}

ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins, bool on, bool stage1, const EngineFns* fns) noexcept {
  if (!on) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_droneguard_install",
                        {{"result", "off"}, {"reason", "off_config"},
                         {"consequence", "combat.droneguard is false: the game's own Follow runs for every component, and a drone with no orbit target faults it"}});
    return {false, "off_config", true};
  }
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) return Refused("module_not_loaded");
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused("no_build_id");
  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return Refused("build_id_mismatch");
  pinned::Failure word;
  if (pinned::FirstFailure(image.base, &kWordPins, 1, &word)) return Refused("pinned_word_mismatch", &word);
  const char* const stageOne = stage1 ? PrepareStageOne(image.base, fns) : "off";
  sentinel::GotTarget target(sentinel::pinned::kLibR15, kFollowSymbol, sentinel::RelocKind::kJumpSlot, sentinel::pinned::LibR15PatchedBuildId(), pins.slot);
  target.expectedOriginal = reinterpret_cast<const void*>(image.base + pins.fn);
  target = target.Owned("combat/droneguard");
  FollowThunk::Arm(kFollowRecord);
  const sentinel::GotStatus status = sentinel::InstallThunk<FollowThunk>(g_hook, target, lookup);
  const bool installed = status == sentinel::GotStatus::kOk;
  // A refused slot keeps the game's call as it is, and stage 1 goes off with it (no later install without it inherits it).
  if (!installed && !g_hook.installed()) g_idxOn.store(false, std::memory_order_relaxed);
  sentinel::LogFields(installed ? sentinel::LogLevel::kInfo : sentinel::LogLevel::kError, "combat_droneguard_install",
                      {{"result", installed ? "installed" : "refused"}, {"reason", installed ? "ok" : "slot_refused"},
                       {"hook", sentinel::GotStatusName(status)}, {"stage1", installed ? stageOne : "off"},
                       {"consequence", installed ? kConsequence : kRefusedConsequence}});
  if (!installed) return {false, sentinel::GotStatusName(status)};
  return {true, stage1 && std::strcmp(stageOne, "on") != 0 ? "installed_null_half" : "installed"};
}

ModuleResult Install(bool on, bool stage1) noexcept {
  EngineFns fns{};
  bool haveFns = false;
  if (on && stage1) {
    const sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
    sentinel::ElfImage image;
    haveFns = (lookup != nullptr ? lookup : &sentinel::FindLoadedImage)(sentinel::pinned::kLibR15, &image) && ResolveEngineFns(image.base, &fns);
  }
  const sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
  return InstallAt(lookup != nullptr ? lookup : &sentinel::FindLoadedImage, kPins, on, stage1, haveFns ? &fns : nullptr);
}

Stats CurrentStats() noexcept {
  return {FollowThunk::Calls(), g_skips.load(std::memory_order_relaxed), g_idxSkips.load(std::memory_order_relaxed)};
}

void NoteEnd() noexcept {
  if (g_hook.installed()) LogStats(true);
}

void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept { g_lookup.store(lookup, std::memory_order_release); }

void ResetForTest() noexcept {
  g_hook.Forget();
  FollowThunk::Reset();
  g_f = EngineFns{};
  g_idxOn.store(false);
  g_skips.store(0);
  g_totals.store(0);
  g_idxSkips.store(0);
  g_idxTotals.store(0);
  g_stateLines.store(0);
  g_skipping.store(false);
  g_idxSkipping.store(false);
  g_state.store(~0ULL);
  g_lookup.store(nullptr);
}

}  // namespace nevr_quest::combat::droneguard
