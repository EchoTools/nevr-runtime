#include "quest/combat/levelname.h"

#include <atomic>
#include <cstdio>
#include <cstring>

#include "quest/combat/legacy_switch.h"
#include "quest/combat/safe_read.h"
#include "quest/sentinel/hook_install.h"
#include "quest/sentinel/hook_log.h"

namespace nevr_quest::combat::levelname {

namespace {

namespace pin = targets;

constexpr std::size_t kHookCount = 4;
static_assert(kHookCount == pin::kSlotCount, "one handle per vtable word");
sentinel::GotHook g_hooks[kHookCount];

constexpr std::uint64_t kLogFirstKept = 16;

using PlainFn = std::uint64_t (*)(void* game, std::uint64_t name);
std::atomic<std::uintptr_t> g_plain{0};
std::atomic<std::uint64_t> g_kept{0};
std::atomic<bool> g_installed{false};
std::atomic<sentinel::ImageLookup> g_lookup{nullptr};

// One line for the first kKeptLogFirst substitutions; written after the plain function returned, never while game code is on the stack above
// the handler (callback_thunk.h rule 1).
void NoteKept(std::uint64_t name) noexcept {
  const std::uint64_t n = g_kept.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n > kLogFirstKept) return;
  char nameText[19];
  sentinel::LogFields(sentinel::LogLevel::kInfo, "combat_levelname_kept",
                      {{"name", sentinel::HexString(nameText, name)}, {"n", n}, {"lowspec", 1}, {"answer", "plain name"}});
}

// this = x0 (the CR15Game), name = x1 (CSymbol64); the result (x0) is the name to load. Neither callee reads x2 or beyond.
std::uint64_t Answer(pin::Sig* original, void* game, std::uint64_t name) noexcept {
  if (name == pin::kGlobalMp) {
    bool ok = true;
    const auto flags = SafeRead<std::uint32_t>(reinterpret_cast<std::uintptr_t>(game) + pin::kFlagOffset, &ok);
    const auto plain = reinterpret_cast<PlainFn>(g_plain.load(std::memory_order_acquire));
    if (ok && (flags & pin::kLowspecMask) != 0 && plain != nullptr) {
      const std::uint64_t answer = plain(game, name);
      NoteKept(name);
      return answer;
    }
  }
  return original(game, name);
}

template <std::size_t I>
std::uint64_t Handle(typename pin::Thunk<I>::Fn original, void* game, std::uint64_t name) noexcept {
  return Answer(original, game, name);
}

// A hook is defined by a record (callback_thunk.h): one per word.
NEVR_HOOK_RECORD(kRecord0, pin::Thunk<0>, &Handle<0>);
NEVR_HOOK_RECORD(kRecord1, pin::Thunk<1>, &Handle<1>);
NEVR_HOOK_RECORD(kRecord2, pin::Thunk<2>, &Handle<2>);
NEVR_HOOK_RECORD(kRecord3, pin::Thunk<3>, &Handle<3>);

template <std::size_t I>
void ArmSlot() noexcept;
template <> void ArmSlot<0>() noexcept { pin::Thunk<0>::Arm(kRecord0); }
template <> void ArmSlot<1>() noexcept { pin::Thunk<1>::Arm(kRecord1); }
template <> void ArmSlot<2>() noexcept { pin::Thunk<2>::Arm(kRecord2); }
template <> void ArmSlot<3>() noexcept { pin::Thunk<3>::Arm(kRecord3); }

template <std::size_t I>
void DisarmSlot() noexcept {
  pin::Thunk<I>::Disarm();
}

template <std::size_t I>
sentinel::GotStatus PutSlot(const sentinel::GotTarget& target, sentinel::ImageLookup lookup) noexcept {
  return sentinel::InstallThunk<pin::Thunk<I>>(g_hooks[I], target, lookup);
}

template <std::size_t I>
void ResetSlot() noexcept {
  pin::Thunk<I>::Reset();
}

struct SlotOps {
  void (*arm)() noexcept;
  void (*disarm)() noexcept;
  sentinel::GotStatus (*put)(const sentinel::GotTarget&, sentinel::ImageLookup) noexcept;
  void (*reset)() noexcept;
};
constexpr SlotOps kOps[kHookCount] = {{&ArmSlot<0>, &DisarmSlot<0>, &PutSlot<0>, &ResetSlot<0>},
                                      {&ArmSlot<1>, &DisarmSlot<1>, &PutSlot<1>, &ResetSlot<1>},
                                      {&ArmSlot<2>, &DisarmSlot<2>, &PutSlot<2>, &ResetSlot<2>},
                                      {&ArmSlot<3>, &DisarmSlot<3>, &PutSlot<3>, &ResetSlot<3>}};

// The plugin's kill file `levelname.off` is not read: a file there turns nothing off (docs/adr/0046 rule 4). One found is a Warning that names the
// `combat:` key that replaces it, so an operator who relied on it learns why the full level is loading.
void WarnLegacyKillFile() noexcept {
  if (!legacy::Present("levelname.off")) return;
  sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_levelname_legacy_switch",
                      {{"file", "levelname.off"},
                       {"replaced_by", "combat.levelname: false"},
                       {"msg", "the plugin's kill file is no longer read and changes nothing; set the combat key in config.yaml instead"}});
}

struct Result {
  const char* result;
  const char* reason;
  std::size_t installed;
  sentinel::GotStatus status[pin::kSlotCount];
  bool tried[pin::kSlotCount];
  int failedSlot;
  std::size_t rolledBack;
  std::size_t rollbackFailed;
};

const char* StatusOf(const Result& r, std::size_t i) noexcept { return r.tried[i] ? sentinel::GotStatusName(r.status[i]) : "not_tried"; }

void LogInstall(const Result& r, const char* consequence) noexcept {
  const bool bad = std::strcmp(r.result, "refused") == 0;
  sentinel::LogFields(bad ? sentinel::LogLevel::kError : sentinel::LogLevel::kInfo, "combat_levelname_install",
                      {{"result", r.result},
                       {"reason", r.reason},
                       {"key", "combat.levelname"},
                       {"slots", r.installed},
                       {"failed_slot", r.failedSlot >= 0 ? pin::kSlotNames[r.failedSlot] : "none"},
                       {pin::kSlotNames[0], StatusOf(r, 0)},
                       {pin::kSlotNames[1], StatusOf(r, 1)},
                       {pin::kSlotNames[2], StatusOf(r, 2)},
                       {pin::kSlotNames[3], StatusOf(r, 3)},
                       {"rolled_back", r.rolledBack},
                       {"rollback_failed", r.rollbackFailed},
                       {"consequence", consequence}});
}

constexpr const char* kKeepsGame = "global_mp loads as the game's cut level (global_mp_lowspec) when the lowspec flag is set; the combat bot body is not in it";

Result Fresh(const char* result, const char* reason) noexcept {
  Result r{};
  r.result = result;
  r.reason = reason;
  r.failedSlot = -1;
  return r;
}

}  // namespace

ModuleResult InstallAt(sentinel::ImageLookup lookup, bool on) noexcept {
  WarnLegacyKillFile();
  if (!on) {
    LogInstall(Fresh("off", "off_config"), "combat.levelname is false: global_mp loads as the game's cut level when the lowspec flag is set");
    return {false, "off"};
  }
  if (g_installed.load(std::memory_order_acquire)) {
    sentinel::LogFields(sentinel::LogLevel::kWarn, "combat_levelname_refused",
                        {{"why", "already_ran"}, {"consequence", "a second step would find its words already ours; nothing was written"}});
    return {false, "already_ran"};
  }
  sentinel::ElfImage image;
  if (lookup == nullptr || !lookup(sentinel::pinned::kLibR15, &image)) {
    LogInstall(Fresh("refused", "module_not_loaded"), kKeepsGame);
    return {false, "module_not_loaded"};
  }
  g_plain.store(image.base + pin::kPlainStubVaddr, std::memory_order_release);  // published before any word changes
  Result r = Fresh("installed", "ok");
  for (std::size_t i = 0; i < pin::kSlotCount; ++i) kOps[i].arm();
  for (std::size_t i = 0; i < pin::kSlotCount; ++i) {
    sentinel::GotTarget target = pin::Target(i);
    target.expectedOriginal = reinterpret_cast<const void*>(image.base + pin::kFnVaddr);  // a word that holds anything else is refused, not chained over
    r.tried[i] = true;
    r.status[i] = kOps[i].put(target, lookup);
    if (r.status[i] != sentinel::GotStatus::kOk) {
      r.result = "refused";
      r.reason = sentinel::GotStatusName(r.status[i]);
      r.failedSlot = static_cast<int>(i);
      break;
    }
    ++r.installed;
  }
  if (r.failedSlot >= 0) {
    // All four or none: the words already ours go back, and every thunk passes through whatever is left.
    for (std::size_t i = 0; i < pin::kSlotCount; ++i) {
      if (!g_hooks[i].installed()) continue;
      if (g_hooks[i].Remove() == sentinel::GotStatus::kOk) {
        ++r.rolledBack;
      } else {
        ++r.rollbackFailed;
      }
    }
    for (std::size_t i = 0; i < pin::kSlotCount; ++i) kOps[i].disarm();
    g_plain.store(0, std::memory_order_release);
    r.installed = 0;
    LogInstall(r, kKeepsGame);
    return {false, "refused"};
  }
  g_installed.store(true, std::memory_order_release);
  LogInstall(r, "global_mp keeps its plain name when the lowspec flag is set: the full-size level loads, on a map or a lobby of either game");
  return {true, "installed"};
}

ModuleResult Install(bool on) noexcept {
  const sentinel::ImageLookup lookup = g_lookup.load(std::memory_order_acquire);
  return InstallAt(lookup != nullptr ? lookup : &sentinel::FindLoadedImage, on);
}

bool Installed() noexcept { return g_installed.load(std::memory_order_acquire); }
std::uint64_t KeptPlainCount() noexcept { return g_kept.load(std::memory_order_relaxed); }

void SetImageLookupForTest(sentinel::ImageLookup lookup) noexcept { g_lookup.store(lookup, std::memory_order_release); }
void SetPlainForTest(std::uintptr_t plain) noexcept { g_plain.store(plain, std::memory_order_release); }
std::uintptr_t PlainForTest() noexcept { return g_plain.load(std::memory_order_acquire); }

void ResetForTest() noexcept {
  for (sentinel::GotHook& hook : g_hooks) {
    if (hook.installed() && hook.Remove() != sentinel::GotStatus::kOk) hook.Forget();
  }
  for (std::size_t i = 0; i < pin::kSlotCount; ++i) {
    kOps[i].disarm();
    kOps[i].reset();
  }
  g_plain.store(0);
  g_kept.store(0);
  g_installed.store(false);
}

}  // namespace nevr_quest::combat::levelname
