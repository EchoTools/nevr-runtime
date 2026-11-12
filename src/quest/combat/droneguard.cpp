#include "quest/combat/droneguard.h"

#include <dlfcn.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "quest/combat/line_budget.h"
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
// combat.droneguard_stage1 as the step was handed it (InstallAt's `stage1`), for the refusal lines: InstallAt stores it before any line.
std::atomic<bool> g_stage1Asked{false};
std::atomic<unsigned> g_skips{0}, g_totals{0}, g_idxSkips{0}, g_idxTotals{0}, g_stateLines{0};
// The lines written of each kind (a budget each, line_budget.h).
std::atomic<unsigned> g_skipLines{0}, g_targetSetLines{0}, g_idxSkipLines{0}, g_idxResolvedLines{0};

// The components whose follow update is being skipped right now, one table for the null-target runs and one for the stage-1 runs: a run belongs to the component
// (a drone with no target skipped every frame beside a drone with one is one run, not a line a frame).
//
// When a slot is released: when the component's target arrives (End) or when a later run needs a slot and this one began more than kRunStaleSeconds ago (Begin). The game
// gives no signal that a follow component died, so a run whose component is gone would otherwise hold its slot for the life of the process: a run older than the window is
// stale, its slot is reclaimed and counted (runsStale), and its target-set line is not written (a component at a reused address never had those skips). A table that is
// full even of fresh runs says so once (combat_droneguard_run_table_full), counts every refused run and writes no line for it.
//
// Cost (ADR 0038): `open` counts the runs held. End, which every call with a target makes, returns at once when it is 0 (the common case: no drone is without its target), so the
// table is scanned only while `open` is above 0: while a run is open, and for at most kRunStaleSeconds after the last run that never ended (a dead component), because the scan
// releases every stale slot it passes. The clock is read only inside that scan (for this component's slot, and for the others on every 32nd scan) and to open a run, never on the common path.
using ClockFn = std::int64_t (*)() noexcept;
std::int64_t MonotonicMs() noexcept {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}
std::atomic<ClockFn> g_clock{&MonotonicMs};
std::int64_t NowMs() noexcept { return g_clock.load(std::memory_order_relaxed)(); }
std::atomic<unsigned> g_runScans{0};  // table scans End made (the by-count test of the early-out)
std::atomic<unsigned> g_runs{0}, g_targetSets{0}, g_runsStale{0}, g_runTableFull{0};
std::atomic<bool> g_fullSaid{false};

enum class Began { kNew, kInRun, kFull };
enum class Ended { kNone, kEnded, kStale };
constexpr unsigned kSweepEvery = 32;  // a power of two
constexpr std::int64_t kStaleMs = static_cast<std::int64_t>(kRunStaleSeconds) * 1000;

struct RunTable {
  std::atomic<std::uintptr_t> cs[kRunSlots];
  std::atomic<std::int64_t> since[kRunSlots];
  std::atomic<unsigned> open;
  // A slot is claimed and released by compare_exchange, so if Follow ever runs on two threads one claims a slot once and one release decrements `open` once.
  Began Begin(std::uintptr_t c) noexcept {
    for (int attempt = 0; attempt < 3; ++attempt) {
      int freeAt = -1;
      for (unsigned i = 0; i < kRunSlots; ++i) {
        const std::uintptr_t v = cs[i].load(std::memory_order_relaxed);
        if (v == c) return Began::kInRun;
        if (v == 0 && freeAt < 0) freeAt = static_cast<int>(i);
      }
      const std::int64_t now = NowMs();
      if (freeAt >= 0) {
        std::uintptr_t none = 0;
        if (!cs[freeAt].compare_exchange_strong(none, c, std::memory_order_relaxed)) continue;  // another thread took it: look again
        since[freeAt].store(now, std::memory_order_relaxed);
        open.fetch_add(1, std::memory_order_relaxed);
        return Began::kNew;
      }
      for (unsigned i = 0; i < kRunSlots; ++i) {  // full: reclaim the first slot whose run is stale (the slot stays counted in `open`)
        std::uintptr_t v = cs[i].load(std::memory_order_relaxed);
        if (v == 0 || now - since[i].load(std::memory_order_relaxed) <= kStaleMs) continue;
        if (!cs[i].compare_exchange_strong(v, c, std::memory_order_relaxed)) continue;
        g_runsStale.fetch_add(1, std::memory_order_relaxed);
        since[i].store(now, std::memory_order_relaxed);
        return Began::kNew;
      }
      return Began::kFull;
    }
    return Began::kFull;
  }
  // kEnded: `c` was in a run and now is not (the caller writes the run-ended line). kStale: it was, and the run is older than the window (no line).
  // The scan also releases every other slot whose run is stale (a component that died mid-run never calls End), so `open` drains to 0 within
  // kRunStaleSeconds of the last death and the common path is free again.
  Ended End(std::uintptr_t c) noexcept {
    if (open.load(std::memory_order_relaxed) == 0) return Ended::kNone;
    // The clock costs more than the scan, so it is read for a slot that is this component's, and for the other slots only on every kSweepEvery-th scan.
    const bool sweep = (g_runScans.fetch_add(1, std::memory_order_relaxed) & (kSweepEvery - 1)) == 0;
    std::int64_t now = 0;
    bool haveNow = false;
    Ended result = Ended::kNone;
    for (unsigned i = 0; i < kRunSlots; ++i) {
      std::uintptr_t v = cs[i].load(std::memory_order_relaxed);
      if (v == 0) continue;
      bool stale = false;
      if (v == c || sweep) {
        if (!haveNow) {
          now = NowMs();
          haveNow = true;
        }
        stale = now - since[i].load(std::memory_order_relaxed) > kStaleMs;
      }
      if (v != c && !stale) continue;
      if (!cs[i].compare_exchange_strong(v, 0, std::memory_order_relaxed)) continue;
      open.fetch_sub(1, std::memory_order_relaxed);
      if (stale) g_runsStale.fetch_add(1, std::memory_order_relaxed);
      if (v == c) result = stale ? Ended::kStale : Ended::kEnded;
    }
    return result;
  }
  void Clear() noexcept {
    for (unsigned i = 0; i < kRunSlots; ++i) {
      cs[i].store(0, std::memory_order_relaxed);
      since[i].store(0, std::memory_order_relaxed);
    }
    open.store(0, std::memory_order_relaxed);
  }
};
RunTable g_skipRuns, g_idxRuns;
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
                       {"runs", g_runs.load(std::memory_order_relaxed)},
                       {"target_set", g_targetSets.load(std::memory_order_relaxed)},
                       {"runs_stale", g_runsStale.load(std::memory_order_relaxed)},
                       {"run_table_full", g_runTableFull.load(std::memory_order_relaxed)},
                       {"final", final ? 1 : 0},
                       {"consequence", "calls is every Follow call the game made, skips those not made because the target was null, idx_skips those stage 1 "
                                       "did not make; runs and target_set count the null-target runs begun and ended by a target (runs_stale those dropped as older than the stale window, "
                                       "run_table_full those refused a slot); their sum over the run's time is the call rate of Follow"}});
}

// One line on the first call and on every change of (raw index word, flag byte, target null, handle system null), at most kStateLines. Read-only: the
// values are the previous frame's cache (Follow refreshes it itself). Written before the game's function is called.
void StateLine(std::uintptr_t cs, const Snap& s) noexcept {
  const std::uint64_t key = s.idx | (static_cast<std::uint64_t>(s.flags) << 32) | (static_cast<std::uint64_t>(s.target != 0) << 40) |
                            (static_cast<std::uint64_t>(s.sys != 0) << 41);
  if (g_state.exchange(key, std::memory_order_relaxed) == key) return;
  if (!LineBudget(&g_stateLines, kStateLines, "droneguard", "state", "further state lines are not written; the stats line still counts every call and skip")) return;
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

// The first skip of a component's run, and every kTotalEvery-th skip of the process (at most kTotalLines of them).
void NoteTableFull(const char* table) noexcept {
  g_runTableFull.fetch_add(1, std::memory_order_relaxed);
  if (g_fullSaid.exchange(true, std::memory_order_relaxed)) return;
  sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_droneguard_run_table_full",
                      {{"table", table}, {"slots", kRunSlots},
                       {"consequence", "every slot holds a run younger than the stale window, so this component's run writes no line; its skips are still made and counted, and every "
                                       "refused run is counted in run_table_full in the stats line"}});
}

void LogSkipRun(std::uintptr_t cs, unsigned n) noexcept {
  const Began began = g_skipRuns.Begin(cs);
  if (began == Began::kFull) NoteTableFull("skip");
  if (began == Began::kNew) {
    g_runs.fetch_add(1, std::memory_order_relaxed);
    if (LineBudget(&g_skipLines, kRunLines, "droneguard", "skip", "further first-skip lines are not written; combat_droneguard_skips and the stats line count the skips")) {
      char c[19];
      sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_droneguard_skip",
                          {{"cs", sentinel::HexString(c, cs)},
                           {"consequence", "the follow target is null: this component's follow update is skipped, as the game's would fault on it"}});
    }
  } else if (n % kTotalEvery == 0 &&
             LineBudget(&g_totals, kTotalLines, "droneguard", "skips", "no further skip totals are written; the stats line still counts every skip")) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_droneguard_skips", {{"skips", n}});
  }
}

void LogIdxSkipRun(std::uintptr_t cs, const char* why, unsigned n) noexcept {
  const Began began = g_idxRuns.Begin(cs);
  if (began == Began::kFull) NoteTableFull("idx_skip");
  if (began == Began::kNew) {
    if (LineBudget(&g_idxSkipLines, kRunLines, "droneguard", "idx_skip", "further first-skip lines are not written; combat_droneguard_idx_skips and the stats line count the skips")) {
      char c[19];
      sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_droneguard_idx_skip",
                          {{"why", why}, {"cs", sentinel::HexString(c, cs)},
                           {"consequence", "the followed player's cache is stale: this component's follow update is skipped"}});
    }
  } else if (n % kTotalEvery == 0 &&
             LineBudget(&g_idxTotals, kTotalLines, "droneguard", "idx_skips", "no further idx skip totals are written; the stats line still counts every skip")) {
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
  const Ended ended = g_skipRuns.End(cs);
  if (ended == Ended::kEnded) g_targetSets.fetch_add(1, std::memory_order_relaxed);
  if (ended == Ended::kEnded &&
      LineBudget(&g_targetSetLines, kRunLines, "droneguard", "target_set", "further target-set lines are not written; the stats line still counts every skip")) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_droneguard_target_set", {{"skips", g_skips.load(std::memory_order_relaxed)}});
  }
  if (g_idxOn.load(std::memory_order_relaxed)) {
    if (const char* why = IdxSkip(cs)) {
      const unsigned n = g_idxSkips.fetch_add(1, std::memory_order_relaxed) + 1;
      LogIdxSkipRun(cs, why, n);
      return;
    }
    if (g_idxRuns.End(cs) == Ended::kEnded &&
        LineBudget(&g_idxResolvedLines, kRunLines, "droneguard", "idx_resolved", "further idx-resolved lines are not written; the stats line still counts every skip")) {
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
                         {"droneguard_stage1", g_stage1Asked.load(std::memory_order_relaxed) ? 1 : 0},
                         {"expected", sentinel::HexString(wantText, word->want)}, {"consequence", kRefusedConsequence}});
  } else {
    sentinel::LogFields(sentinel::LogLevel::kError, "combat_droneguard_install",
                        {{"result", "refused"}, {"reason", token}, {"droneguard_stage1", g_stage1Asked.load(std::memory_order_relaxed) ? 1 : 0},
                         {"consequence", kRefusedConsequence}});
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
  g_stage1Asked.store(stage1, std::memory_order_relaxed);
  if (!on) {
    sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_droneguard_install",
                        {{"result", "off"}, {"reason", "off_config"}, {"droneguard_stage1", stage1 ? 1 : 0},
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
                       {"hook", sentinel::GotStatusName(status)}, {"stage1", installed ? stageOne : "off"}, {"droneguard_stage1", stage1 ? 1 : 0},
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
  return {FollowThunk::Calls(), g_skips.load(std::memory_order_relaxed), g_idxSkips.load(std::memory_order_relaxed),
          g_runs.load(std::memory_order_relaxed), g_targetSets.load(std::memory_order_relaxed), g_runsStale.load(std::memory_order_relaxed),
          g_runTableFull.load(std::memory_order_relaxed)};
}

unsigned RunScansForTest() noexcept { return g_runScans.load(std::memory_order_relaxed); }
void SetClockForTest(std::int64_t (*now)() noexcept) noexcept { g_clock.store(now != nullptr ? now : &MonotonicMs, std::memory_order_relaxed); }

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
  g_skipRuns.Clear();
  g_idxRuns.Clear();
  g_runScans.store(0);
  g_runs.store(0);
  g_targetSets.store(0);
  g_runsStale.store(0);
  g_runTableFull.store(0);
  g_fullSaid.store(false);
  g_clock.store(&MonotonicMs);
  g_skipLines.store(0);
  g_targetSetLines.store(0);
  g_idxSkipLines.store(0);
  g_idxResolvedLines.store(0);
  g_state.store(~0ULL);
  g_lookup.store(nullptr);
}

}  // namespace nevr_quest::combat::droneguard
