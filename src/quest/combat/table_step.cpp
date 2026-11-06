#include "quest/combat/table_step.h"

#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>

#include "quest/combat/combat_level.h"
#include "quest/combat/combat_tick.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_log.h"
#include "quest/sentinel/pinned_targets.h"

namespace nevr_quest::combat::table_step {

namespace {

std::atomic<GameFn> g_game{nullptr};
std::atomic<std::uint32_t> g_scopeReads{0};
std::atomic<bool> g_legacyDirSet{false};
char g_legacyDir[300] = "";  // tests only (SetLegacyDirForTest)

std::uintptr_t GameNow() noexcept {
  const GameFn fn = g_game.load(std::memory_order_acquire);
  return fn != nullptr ? fn() : tick::Game();
}

// The plugin's data directory: /sdcard/Android/data/<package>/files/q15t, the package from /proc/self/cmdline (the plugin's own rule), or the
// test's. False when none can be named.
bool LegacyDir(char* out, std::size_t n) noexcept {
  if (g_legacyDirSet.load(std::memory_order_acquire)) {
    std::snprintf(out, n, "%s", g_legacyDir);
    return true;
  }
  char cmd[256] = {};
  std::FILE* f = std::fopen("/proc/self/cmdline", "re");
  if (f == nullptr) return false;
  const std::size_t got = std::fread(cmd, 1, sizeof(cmd) - 1, f);
  std::fclose(f);
  cmd[got] = '\0';
  if (char* colon = std::strchr(cmd, ':')) *colon = '\0';
  if (std::strchr(cmd, '.') == nullptr || std::strchr(cmd, '/') != nullptr || std::strlen(cmd) > 160) return false;
  std::snprintf(out, n, "/sdcard/Android/data/%.160s/files/q15t", cmd);
  return true;
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
  if (game == 0) return Scope::kUnknown;
  g_scopeReads.fetch_add(1, std::memory_order_relaxed);
  std::uint64_t level = 0;
  // The lobby rule is off: only a combat map counts, and an Echo Arena level that is loaded wins over it (combat_level.h).
  return level::Classify(game, /*lobby=*/false, &level) == level::Kind::kCombatOrLobby ? Scope::kCombat : Scope::kOther;
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
  g_legacyDirSet.store(false, std::memory_order_release);
  g_legacyDir[0] = '\0';
}

}  // namespace nevr_quest::combat::table_step
