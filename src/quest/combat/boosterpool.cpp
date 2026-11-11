#include "quest/combat/boosterpool.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "quest/combat/line_budget.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::boosterpool {

namespace {

namespace pin = targets;
using Thunk = pin::Thunk;
using ActorPair = pin::ActorPair;

sentinel::GotHook g_hook;

constexpr unsigned kLogFirstKept = 16;

// What the handler calls and reads, published before the word changes.
std::atomic<AppendFn> g_append{nullptr};
std::atomic<std::uintptr_t> g_plainAddress{0};
// The two names, cached once the plain name has been read (non-zero): the plain name, then the suffixed one. A zero suffixed name is "not known yet".
std::atomic<std::uint64_t> g_plainName{0};
std::atomic<std::uint64_t> g_suffixedName{0};
std::atomic<std::uint64_t> g_kept{0};
std::atomic<std::uint64_t> g_unreadable{0};
std::atomic<unsigned> g_keptLines{0};  // the kept lines written, against kLogFirstKept (line_budget.h)
std::atomic<bool> g_installed{false};
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};

// One line for the first kLogFirstKept substitutions, then one Warning at the cap (line_budget.h).
void NoteKept(std::uint64_t name) noexcept {
  const std::uint64_t n = g_kept.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!LineBudget(&g_keptLines, kLogFirstKept, "boosterpool", "kept", "no further kept lines are written; combat_boosterpool_stats still counts every substitution")) {
    return;
  }
  char nameText[19];
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_boosterpool_kept",
                      {{"name", sentinel::HexString(nameText, name)}, {"n", n}, {"answer", "plain name"}});
}

// Reads the game's plain name and builds the suffixed one the way the stock does, `Append(plain, "_lowspec", 1, 1)`. False (nothing cached, the call
// is passed through) when the plain name cannot be read. A plain name of 0 is the game's static not set yet: used for this call, not cached.
bool ResolveNames(std::uint64_t* plain, std::uint64_t* suffixed) noexcept {
  const AppendFn append = g_append.load(std::memory_order_acquire);
  if (append == nullptr) return false;
  if (!SafeRead(g_plainAddress.load(std::memory_order_acquire), plain, sizeof(*plain))) {  // on any doubt the real call, untouched
    g_unreadable.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  *suffixed = append(*plain, pin::kSuffix, 1, 1);
  if (*plain != 0 && *suffixed != 0) {
    g_plainName.store(*plain, std::memory_order_release);
    g_suffixedName.store(*suffixed, std::memory_order_release);  // last: a reader that sees this sees the plain name
  }
  return true;
}

// this = st (the game space's ST), name = the CSymbol64 the caller asks for. Only the one name the stock builds under the lowspec bit is changed.
ActorPair Handle(Thunk::Fn original, void* st, std::uint64_t name) noexcept {
  std::uint64_t suffixed = g_suffixedName.load(std::memory_order_acquire);
  std::uint64_t plain = 0;
  if (suffixed != 0) {
    plain = g_plainName.load(std::memory_order_acquire);
  } else if (!ResolveNames(&plain, &suffixed)) {
    return original(st, name);
  }
  if (name != suffixed) return original(st, name);
  const ActorPair actor = original(st, plain);
  NoteKept(name);
  return actor;
}

NEVR_HOOK_RECORD(kRecord, Thunk, &Handle);

// The module's end-of-process line: every substitution and every unreadable plain name counted, not only the first kLogFirstKept lines.
void LogStats() noexcept {
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_boosterpool_stats",
                      {{"kept", g_kept.load(std::memory_order_relaxed)},
                       {"unreadable", g_unreadable.load(std::memory_order_relaxed)},
                       {"final", 1},
                       {"consequence", "kept is every allocation whose suffixed name was given the plain one since install; unreadable the calls "
                                       "that passed through because the plain name could not be read"}});
}

void LogInstall(const char* result, const char* reason, const char* slotStatus, const char* consequence) noexcept {
  sentinel::LogFields(std::strcmp(result, "refused") == 0 ? sentinel::LogLevel::kError : sentinel::LogLevel::kInfo, "combat_boosterpool_install",
                      {{"result", result}, {"reason", reason}, {"key", "combat.levelname"}, {"slot", slotStatus}, {"consequence", consequence}});
}

constexpr const char* kKeepsGame = "the game keeps its own allocations; the booster pool asks for the actor the cut global_mp carries";

}  // namespace

ModuleResult InstallAt(sentinel::ImageLookup lookup, bool on, bool levelnameInstalled) noexcept {
  if (!on) {
    LogInstall("off", "off_config", "not_tried", "combat.levelname is false: the game keeps its own allocations");
    return {false, "off", true};
  }
  if (!levelnameInstalled) {
    LogInstall("refused", "needs_levelname", "not_tried",
               "levelname did not install, so the cut global_mp is the loaded level and the plain actor does not exist: the game keeps its own allocations");
    return {false, "needs_levelname"};
  }
  if (g_installed.load(std::memory_order_acquire)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_boosterpool_refused",
                        {{"why", "already_ran"}, {"consequence", "a second step would find its word already ours; nothing was written"}});
    return {false, "already_ran"};
  }
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) {
    LogInstall("refused", "module_not_loaded", "not_tried", kKeepsGame);
    return {false, "module_not_loaded"};
  }
  if (g_append.load(std::memory_order_acquire) == nullptr) {
    g_append.store(reinterpret_cast<AppendFn>(image.base + pin::kAppendStubVaddr), std::memory_order_release);
    g_plainAddress.store(image.base + pin::kPlainNameVaddr, std::memory_order_release);
  }
  Thunk::Arm(kRecord);
  sentinel::GotTarget target = pin::Target();
  target.expectedOriginal = reinterpret_cast<const void*>(image.base + pin::kFnVaddr);  // a word that holds anything else is refused, not chained over
  const sentinel::GotStatus status = sentinel::InstallThunk<Thunk>(g_hook, target, lookup);
  if (status != sentinel::GotStatus::kOk) {
    Thunk::Disarm();
    LogInstall("refused", sentinel::GotStatusName(status), sentinel::GotStatusName(status), kKeepsGame);
    return {false, "refused"};
  }
  g_installed.store(true, std::memory_order_release);
  LogInstall("installed", "ok", sentinel::GotStatusName(status),
             "an allocation for the suffixed booster actor asks for the plain one: the booster works on the full-size global_mp");
  return {true, "installed"};
}

ModuleResult Install(bool on, bool levelnameInstalled) noexcept {
  const sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
  return InstallAt(lookup != nullptr ? lookup : &sentinel::FindLoadedImage, on, levelnameInstalled);
}

std::uint64_t KeptCount() noexcept { return g_kept.load(std::memory_order_relaxed); }
std::uint64_t UnreadableCount() noexcept { return g_unreadable.load(std::memory_order_relaxed); }

void NoteEnd() noexcept {
  if (g_installed.load(std::memory_order_acquire)) LogStats();
}

void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept { g_lookup.store(lookup, std::memory_order_release); }

void TargetsForTest(AppendFn* append, std::uintptr_t* plainAddress) noexcept {
  *append = g_append.load();
  *plainAddress = g_plainAddress.load();
}

void SetTargetsForTest(AppendFn append, std::uintptr_t plainAddress) noexcept {
  g_suffixedName.store(0);
  g_plainName.store(0);
  g_append.store(append);
  g_plainAddress.store(plainAddress);
}

void ResetForTest() noexcept {
  if (g_hook.installed() && g_hook.Remove() != sentinel::GotStatus::kOk) g_hook.Forget();
  Thunk::Disarm();
  Thunk::Reset();
  g_append.store(nullptr);
  g_plainAddress.store(0);
  g_plainName.store(0);
  g_suffixedName.store(0);
  g_kept.store(0);
  g_unreadable.store(0);
  g_keptLines.store(0);
  g_installed.store(false);
}

}  // namespace nevr_quest::combat::boosterpool
