#include "quest/combat/table_step.h"

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "quest/combat/combat_level.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/legacy_switch.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::table_step {

namespace {

std::atomic<GameFn> g_game{nullptr};
std::atomic<std::uint32_t> g_scopeReads{0};
std::atomic<bool> g_legacyDirSet{false};
char g_legacyDir[300] = "";  // tests only (SetLegacyDirForTest)

struct Registered {
  const char* event = nullptr;
  CountsFn fn = nullptr;
  std::uint32_t lastLeft = 0;
  std::uint32_t lastChanged = 0;
};
Registered g_counted[kMaxCounted];
std::atomic<std::size_t> g_countedN{0};
// The scope and level the last construction saw. A change writes the summaries. Written only by the thread that holds g_flushing.
std::atomic<std::uint64_t> g_lastLevel{0};
std::atomic<int> g_lastScope{static_cast<int>(Scope::kUnknown)};
std::atomic<bool> g_flushing{false};

void WriteCounts(Scope scope, std::uint64_t level) noexcept {
  const std::size_t n = g_countedN.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < n && i < kMaxCounted; ++i) {
    Registered& r = g_counted[i];
    const Counts now = r.fn();
    const std::uint32_t left = now.leftAlone - r.lastLeft;
    const std::uint32_t changed = now.changed - r.lastChanged;
    if (left == 0 && changed == 0) continue;
    r.lastLeft = now.leftAlone;
    r.lastChanged = now.changed;
    char levelText[19];
    const char* const name = level::LevelName(level);
    sentinel::LogFields(sentinel::LogLevel::kInfo, r.event,
                        {{"level", sentinel::HexString(levelText, level)}, {"level_name", name != nullptr ? name : "other"}, {"scope", ScopeName(scope)}, {"left_alone", left}, {"changed", changed},
                         {"msg", "objects built since the last level change, by the scope that was then loaded: left as the game made them, and changed"}});
  }
}

// Called with what a construction just saw. When the scope or the level differs from the last one seen, writes the summaries of the one that ended.
void NoteScope(Scope scope, std::uint64_t level) noexcept {
  if (g_lastScope.load(std::memory_order_relaxed) == static_cast<int>(scope) && g_lastLevel.load(std::memory_order_relaxed) == level) return;
  if (g_flushing.exchange(true, std::memory_order_acquire)) return;  // another thread is writing; the next change writes what is left
  const Scope oldScope = static_cast<Scope>(g_lastScope.load(std::memory_order_relaxed));
  const std::uint64_t oldLevel = g_lastLevel.load(std::memory_order_relaxed);
  if (oldScope != scope || oldLevel != level) {
    g_lastScope.store(static_cast<int>(scope), std::memory_order_relaxed);
    g_lastLevel.store(level, std::memory_order_relaxed);
    WriteCounts(oldScope, oldLevel);
  }
  g_flushing.store(false, std::memory_order_release);
}

std::uintptr_t GameNow() noexcept {
  const GameFn fn = g_game.load(std::memory_order_acquire);
  return fn != nullptr ? fn() : tick::Game();
}

// The data directory (legacy_switch.h, legacy::Directory), or the test's. False when none can be named.
bool LegacyDir(char* out, std::size_t n) noexcept {
  if (g_legacyDirSet.load(std::memory_order_acquire)) {
    std::snprintf(out, n, "%s", g_legacyDir);
    return true;
  }
  return legacy::Directory(out, n);
}

}  // namespace

const char* ScopeName(Scope scope) noexcept {
  switch (scope) {
    case Scope::kCombat: return "combat";
    case Scope::kOther: return "other";
    case Scope::kUnknown: return "unknown";
  }
  return "unknown";
}

Scope CurrentScope() noexcept {
  const std::uintptr_t game = GameNow();
  if (game == 0) return Scope::kUnknown;  // no read, no level: the scope this returns is never a change of level
  g_scopeReads.fetch_add(1, std::memory_order_relaxed);
  std::uint64_t level = 0;
  // The lobby rule is off: only a combat map counts, and an Echo Arena level that is loaded wins over it (combat_level.h).
  const Scope scope = level::Classify(game, /*lobby=*/false, &level, /*inFlight=*/true) == level::Kind::kCombatOrLobby ? Scope::kCombat : Scope::kOther;
  NoteScope(scope, level);
  return scope;
}

void RegisterCounts(const char* event, CountsFn fn) noexcept {
  if (event == nullptr || fn == nullptr) return;
  const std::size_t n = g_countedN.load(std::memory_order_acquire);
  for (std::size_t i = 0; i < n && i < kMaxCounted; ++i) {
    if (g_counted[i].event == event || std::strcmp(g_counted[i].event, event) == 0) {  // a second install of the module keeps its row
      g_counted[i].fn = fn;
      return;
    }
  }
  if (n >= kMaxCounted) return;
  g_counted[n].event = event;
  g_counted[n].fn = fn;
  g_countedN.store(n + 1, std::memory_order_release);
}

const PinWord* FirstPinMismatch(std::uintptr_t base, const PinWord* pins, std::size_t count, std::uint32_t* have, bool* readable) noexcept {
  for (std::size_t i = 0; i < count; ++i) {
    std::uint32_t word = 0;
    const bool ok = SafeRead(base + pins[i].vaddr, &word, sizeof(word));
    if (!ok || word != pins[i].word) {
      *have = ok ? word : 0;
      *readable = ok;
      return &pins[i];
    }
  }
  return nullptr;
}

const char* FindPinnedLibR15(sentinel::ImageLookup lookup, sentinel::ElfImage* out) noexcept {
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, out)) return "module_not_loaded";
  char id[64] = {};
  if (!sentinel::ReadBuildId(*out, id, sizeof(id))) return "no_build_id";
  if (std::strcmp(id, sentinel::pinned::LibR15PatchedBuildId()) != 0) return "build_id_mismatch";
  return nullptr;
}

StepFix ApplyShaderSetStep(void* obj, std::uint64_t* found) noexcept {
  if (obj == nullptr) return StepFix::kUnreadable;
  const std::uintptr_t at = reinterpret_cast<std::uintptr_t>(obj) + kShaderSetStepOffset;
  bool ok = true;  // SafeRead only ever clears it
  const std::uint64_t have = SafeRead<std::uint64_t>(at, &ok);
  if (!ok) return StepFix::kUnreadable;
  if (found != nullptr) *found = have;
  if (have != kShaderSetStockStep) return StepFix::kNotStock;
  const std::uint64_t one = kShaderSetStep;
  std::memcpy(reinterpret_cast<void*>(at), &one, sizeof one);
  bool backOk = true;
  const std::uint64_t now = SafeRead<std::uint64_t>(at, &backOk);
  return backOk && now == kShaderSetStep ? StepFix::kSet : StepFix::kUnreadable;
}

void WarnLegacySwitch(const char* event, const char* file, const char* key) noexcept {
  char dir[300] = {};
  if (!LegacyDir(dir, sizeof(dir))) return;
  char path[400];
  std::snprintf(path, sizeof(path), "%s/%s", dir, file);
  if (access(path, F_OK) != 0) return;
  sentinel::LogFields(sentinel::LogLevel::kWarn, event,
                      {{"file", file},
                       {"replaced_by", key},
                       {"msg", "the plugin's kill file is no longer read and changes nothing; set the combat key in config.yaml instead"}});
}

void SetGameForTest(GameFn fn) noexcept { g_game.store(fn, std::memory_order_release); }

void SetLegacyDirForTest(const char* dir) noexcept {
  std::snprintf(g_legacyDir, sizeof(g_legacyDir), "%s", dir != nullptr ? dir : "");
  g_legacyDirSet.store(dir != nullptr && dir[0] != '\0', std::memory_order_release);
}

std::uint32_t ScopeReadsForTest() noexcept { return g_scopeReads.load(std::memory_order_relaxed); }

void ResetForTest() noexcept {
  g_game.store(nullptr, std::memory_order_release);
  g_scopeReads.store(0, std::memory_order_relaxed);
  g_countedN.store(0, std::memory_order_release);
  for (Registered& r : g_counted) r = Registered{};
  g_lastLevel.store(0, std::memory_order_relaxed);
  g_lastScope.store(static_cast<int>(Scope::kUnknown), std::memory_order_relaxed);
  g_flushing.store(false, std::memory_order_relaxed);
  g_legacyDirSet.store(false, std::memory_order_release);
  g_legacyDir[0] = '\0';
}

}  // namespace nevr_quest::combat::table_step
