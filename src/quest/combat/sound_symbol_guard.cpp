// The sound-symbol guard (sound_symbol_guard.h, docs/quest/combat/sound_symbol_guard.md): a thunk on Play3DSound's slot that, in a combat level,
// does not make a post from PlaySound or the bullet site whose symbol can only fail.
#include "quest/combat/sound_symbol_guard.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#include "core/burst_then_count.h"
#include "quest/combat/combat_scope.h"
#include "quest/combat/dronemuzzle_targets.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"
#include "runtime/combat/sound_symbol_guard_rules.h"

namespace nevr_quest::combat::sound_symbol_guard {

namespace {

namespace rules = ::nevr_combat::sound_symbol_guard;
using sentinel::LogLevel;

sentinel::GotHook g_hook;
std::atomic<std::uint64_t> g_calls{0}, g_droppedZero{0}, g_droppedUnregistered{0}, g_keptOther{0};
std::atomic<std::uint64_t> g_lastCaller{0};
// What the installed hook compares a return address with: the image base and the two sites' offsets (written before the slot changes).
std::atomic<std::uintptr_t> g_base{0};
Pins g_activePins = kPins;
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};
std::atomic<const Pins*> g_pinsOverride{nullptr};
std::uint64_t (*g_clockMs)() = nullptr;
// The drop lines' budget (core/burst_then_count.h: no lock of its own). Taken only on the drop path, which calls nothing of the game's.
std::mutex g_budgetLock;
nevr::BurstThenCount g_budget{kDropLines, kDropEveryMs};

// Every word the guard relies on: the bullet site's path to Play3DSound and its default symbol (kWordPins), and what a drop leaves undone,
// PlaySound's handling of the answer and the invalid-id path (kDropPins).
constexpr pinned::Pins kInstallPins[] = {targets::kWordPins, targets::kDropPins};

constexpr const char* kDropMsg =
    "a sound post whose symbol no bank defines was not made: Play3DSound would fail it in PostEvent and hand the error to OutputError";
constexpr const char* kInstalledMsg =
    "in a combat level, a post from PlaySound or the bullet site whose symbol is 0 or unregistered is not made; every other post reaches "
    "Play3DSound unchanged";
constexpr const char* kRefusedMsg = "the guard is not installed: every sound post reaches Play3DSound, an unplayable symbol included";

std::uint64_t NowMs() noexcept {
  if (g_clockMs != nullptr) return g_clockMs();
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::uint64_t>(ts.tv_sec) * 1000ull + static_cast<std::uint64_t>(ts.tv_nsec) / 1000000ull;
}

// Written instead of the call to Play3DSound, so no game frame is live above it.
[[gnu::noinline]] void LogDrop(std::uint32_t symbol, rules::Drop d, Caller caller, std::uint64_t n) noexcept {
  g_budgetLock.lock();
  const nevr::BurstThenCount::Line l = g_budget.Event(NowMs());
  g_budgetLock.unlock();
  if (l.verdict == nevr::BurstThenCount::Verdict::kWrite) {
    char s[11];
    std::snprintf(s, sizeof s, "0x%08x", static_cast<unsigned>(symbol));
    sentinel::LogFields(LogLevel::kDebug, "combat_sound_symbol_guard_drop",
                        {{"symbol", s}, {"class", rules::DropName(d)}, {"caller", CallerName(caller)}, {"n", n},
                         {"msg", kDropMsg}});
  } else if (l.verdict == nevr::BurstThenCount::Verdict::kCapped) {
    sentinel::LogFields(LogLevel::kDebug, "combat_sound_symbol_guard_drops",
                        {{"n", static_cast<long long>(l.total)}, {"since_last", 0},
                         {"msg", "later dropped posts are counted, not logged one by one; a count follows at most every 10 s while they continue"}});
  } else if (l.verdict == nevr::BurstThenCount::Verdict::kCount) {
    sentinel::LogFields(LogLevel::kDebug, "combat_sound_symbol_guard_drops",
                        {{"n", static_cast<long long>(l.total)}, {"since_last", static_cast<long long>(l.sinceLine)},
                         {"msg", "posts whose symbol no bank defines were not made since the last line"}});
  }
}

// The combat level case. Not inlined into the handler, so the arena path stays a load, a test and a tail call (README rule 9).
[[gnu::noinline]] std::int32_t ActInCombat(Play3DThunk::Fn original, const void* caller, void* player, std::uint64_t symbol, std::uint32_t emitter,
                                           float volume) noexcept {
  if (!scope::LookIsFresh()) return original(player, symbol, emitter, volume);  // the tick stopped: the answer is not believed
  g_calls.fetch_add(1, std::memory_order_relaxed);
  const std::uintptr_t base = g_base.load(std::memory_order_acquire);
  const std::uintptr_t ret = reinterpret_cast<std::uintptr_t>(caller);
  g_lastCaller.store(ret - base, std::memory_order_relaxed);
  const std::uint32_t sym = static_cast<std::uint32_t>(symbol);
  const rules::Drop d = rules::Classify(sym);
  if (d == rules::Drop::kNone) return original(player, symbol, emitter, volume);
  const Caller site = CallerOf(ret, base, g_activePins);
  if (site == Caller::kOther) {  // a caller whose emitter may carry the destroy bit (TriggerSingle): the game's own failed post
    g_keptOther.fetch_add(1, std::memory_order_relaxed);
    return original(player, symbol, emitter, volume);
  }
  std::atomic<std::uint64_t>& counter = d == rules::Drop::kZero ? g_droppedZero : g_droppedUnregistered;
  const std::uint64_t n = counter.fetch_add(1, std::memory_order_relaxed) + 1;
  LogDrop(sym, d, site, n);
  return 0;  // Play3DSound's own answer with audio disabled (0x134b488): PlaySound's -1 branch is not taken; the bullet site ignores it
}

std::int32_t Play3DHandler(Play3DThunk::Fn original, const void* caller, void* player, std::uint64_t symbol, std::uint32_t emitter,
                           float volume) noexcept {
  // Outside a combat level (an Echo Arena level, a lobby arena players use, anywhere else) the game's function with the same arguments, and
  // nothing else.
  if (!scope::SeenCombatOrLobby()) return original(player, symbol, emitter, volume);
  return ActInCombat(original, caller, player, symbol, emitter, volume);
}

NEVR_HOOK_RECORD(kPlay3DRecord, Play3DThunk, &Play3DHandler);

ModuleResult Refused(const char* status, std::uint64_t slot, const pinned::Failure* word = nullptr) noexcept {
  char s[19];
  if (word != nullptr) {
    char at[19], want[19], have[19];
    sentinel::LogFields(LogLevel::kError, "combat_sound_symbol_guard_install",
                        {{"result", "refused"}, {"reason", status}, {"slot", sentinel::HexString(s, slot)}, {"pin", word->what},
                         {"vaddr", sentinel::HexString(at, word->vaddr)}, {"found", word->readable ? sentinel::HexString(have, word->have) : "unreadable"},
                         {"expected", sentinel::HexString(want, word->want)}, {"msg", kRefusedMsg}});
  } else {
    sentinel::LogFields(LogLevel::kError, "combat_sound_symbol_guard_install",
                        {{"result", "refused"}, {"reason", status}, {"slot", sentinel::HexString(s, slot)}, {"msg", kRefusedMsg}});
  }
  return {false, status};
}

}  // namespace

ModuleResult InstallAt(sentinel::ImageLookup lookup, const Pins& pins) noexcept {
  if (!scope::Live()) return Refused("scope_not_live", pins.slot);
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) return Refused("module_not_loaded", pins.slot);
  char id[64] = {};
  if (!sentinel::ReadBuildId(image, id, sizeof(id))) return Refused("no_build_id", pins.slot);
  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return Refused("build_id_mismatch", pins.slot);
  pinned::Failure word;
  if (pinned::FirstFailure(image.base, kInstallPins, sizeof(kInstallPins) / sizeof(kInstallPins[0]), &word)) {
    return Refused("pinned_word_mismatch", pins.slot, &word);
  }
  // The slot must hold Play3DSound itself (base + fn), or the chain's oldest original; owned by this module.
  sentinel::GotTarget target = dronemuzzle::Target(targets::kPlay3DSymbol, pins.slot, image.base, pins.fn).Owned("combat_sound_symbol_guard");
  g_activePins = pins;
  g_base.store(image.base, std::memory_order_release);
  Play3DThunk::Arm(kPlay3DRecord);
  const sentinel::GotStatus status = sentinel::InstallThunk<Play3DThunk>(g_hook, target, lookup);
  if (status != sentinel::GotStatus::kOk) {
    if (!g_hook.installed()) Play3DThunk::Disarm();
    return Refused(sentinel::GotStatusName(status), pins.slot);
  }
  char s[19];
  sentinel::LogFields(LogLevel::kInfo, "combat_sound_symbol_guard_install",
                      {{"result", "installed"}, {"reason", "ok"}, {"slot", sentinel::HexString(s, pins.slot)}, {"msg", kInstalledMsg}});
  return {true, "installed"};
}

ModuleResult Install(bool on) noexcept {
  if (!on) {
    char s[19];
    sentinel::LogFields(LogLevel::kInfo, "combat_sound_symbol_guard_install",
                        {{"result", "off"}, {"reason", "off_config"}, {"slot", sentinel::HexString(s, kPins.slot)},
                         {"msg", "combat.sound_symbol_guard is false: every sound post reaches Play3DSound, an unplayable symbol included"}});
    return {false, "off", true};
  }
  const sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
  const Pins* const pins = g_pinsOverride.load(std::memory_order_acquire);
  return InstallAt(lookup != nullptr ? lookup : &sentinel::FindLoadedImage, pins != nullptr ? *pins : kPins);
}

void NoteEnd() noexcept {
  if (!g_hook.installed()) return;
  sentinel::LogFields(LogLevel::kInfo, "combat_sound_symbol_guard_total",
                      {{"dropped_zero", g_droppedZero.load(std::memory_order_relaxed)},
                       {"dropped_unregistered", g_droppedUnregistered.load(std::memory_order_relaxed)},
                       {"kept_other_caller", g_keptOther.load(std::memory_order_relaxed)}, {"calls", g_calls.load(std::memory_order_relaxed)},
                       {"msg", "calls is every sound post seen in a combat level; the dropped ones were not made, kept_other_caller ones had an "
                               "unplayable symbol but came from another caller and reached Play3DSound"}});
}

Counts CurrentCounts() noexcept {
  return {g_calls.load(std::memory_order_relaxed), g_droppedZero.load(std::memory_order_relaxed),
          g_droppedUnregistered.load(std::memory_order_relaxed), g_keptOther.load(std::memory_order_relaxed)};
}

void SetInstallForTest(sentinel::ImageLookup lookup, const Pins* pins) noexcept {
  g_lookup.store(lookup, std::memory_order_release);
  g_pinsOverride.store(pins, std::memory_order_release);
}
void SetClockForTest(std::uint64_t (*nowMs)()) noexcept { g_clockMs = nowMs; }
std::uint64_t LastCallerForTest() noexcept { return g_lastCaller.load(std::memory_order_relaxed); }

void ResetForTest() noexcept {
  g_hook.Forget();
  Play3DThunk::Reset();
  g_calls.store(0);
  g_droppedZero.store(0);
  g_droppedUnregistered.store(0);
  g_keptOther.store(0);
  g_lastCaller.store(0);
  g_base.store(0);
  g_activePins = kPins;
  g_budget = nevr::BurstThenCount{kDropLines, kDropEveryMs};
  g_clockMs = nullptr;
  SetInstallForTest(nullptr, nullptr);
}

}  // namespace nevr_quest::combat::sound_symbol_guard
