// Built with -fno-exceptions (callback_thunk.h refuses otherwise). The work that allocates and catches lives in
// config_alias_levels.cpp, reached only through AliasFor, which runs before the game's lookup is called.
#include "quest/sentinel/config_alias_hook.h"

#include <atomic>

#include "callback_thunk.h"
#include "core/config_alias.h"
#include "got_hook.h"
#include "hook_install.h"
#include "hook_log.h"
#include "pinned_targets.h"
#include "quest/invite/frame_dispatch.h"

namespace nevr_quest::config_alias_hook {

// config_alias_levels.cpp: the alias for `key` (the key itself when none is live), after a reconcile on the tick thread.
std::uint64_t AliasFor(std::uint64_t key) noexcept;
// config_alias_levels.cpp: the frame work that reconciles once per tick.
bool ReconcileWork(void* context) noexcept;

namespace {

using LookupSig = void*(std::uint64_t key);
struct LookupTag {};
using LookupThunk = sentinel::CallbackThunk<LookupTag, LookupSig>;

sentinel::GotHook g_lookupHook;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_aliasMissLogged{false};

constexpr const char kLookupSymbol[] = "_ZN10NRadEngine8NRadGame24CR15NetBalanceSettingsCS4JsonENS_9CSymbol64E";

sentinel::GotTarget BalanceLookup() {
  return sentinel::GotTarget{sentinel::pinned::kLibR15, kLookupSymbol, sentinel::RelocKind::kJumpSlot,
                             sentinel::pinned::LibR15PatchedBuildId(), sentinel::pinned::kBalanceLookupSlot};
}

template <typename Original>
void* Lookup(Original original, std::uint64_t key) noexcept {
  const std::uint64_t alias = AliasFor(key);
  if (alias != key) {
    if (void* record = original(alias)) return record;
    if (!g_aliasMissLogged.exchange(true)) {
      char keyHex[19];
      char aliasHex[19];
      sentinel::LogFields(sentinel::LogLevel::kWarn, "config_alias",
                          {{"reason", "alias_not_found"}, {"key", sentinel::HexString(keyHex, key)},
                           {"alias", sentinel::HexString(aliasHex, alias)},
                           {"consequence", "the game holds no record under the alias, so the stock one is read"}});
    }
  }
  return original(key);
}

void* HookedLookup(LookupThunk::Fn original, std::uint64_t key) noexcept { return Lookup(original, key); }

NEVR_HOOK_RECORD(kLookupHook, LookupThunk, &HookedLookup);

}  // namespace

void* RunLookupForTest(LookupFn original, std::uint64_t key) noexcept { return Lookup(original, key); }

}  // namespace nevr_quest::config_alias_hook

namespace nevr_config_alias {

// Declared in core/config_alias.h; the PC defines it in the runtime hook. Boot thread, after Stage.
bool InstallIfStaged() {
  namespace hook = nevr_quest::config_alias_hook;
  if (hook::g_installed.load(std::memory_order_acquire)) return true;
  if (!Global().AnyStaged()) return false;
  hook::LookupThunk::Arm(hook::kLookupHook);
  const sentinel::GotStatus got = sentinel::InstallThunk<hook::LookupThunk>(hook::g_lookupHook, hook::BalanceLookup());
  if (got != sentinel::GotStatus::kOk) {
    hook::LookupThunk::Disarm();
    sentinel::LogFields(sentinel::LogLevel::kError, "config_alias",
                        {{"status", "off"}, {"reason", "hook_refused"},
                         {"consequence", "maps read the stock arena rules on this headset"}});
    return false;
  }
  hook::g_installed.store(true, std::memory_order_release);
  const bool ticking = nevr_quest::invite::frame::RegisterFrameWork(&hook::ReconcileWork, nullptr);
  sentinel::LogFields(sentinel::LogLevel::kInfo, "config_alias",
                      {{"status", "installed"}, {"slot", "0x36c2750"}, {"source", "game_space_tables"},
                       {"refresh", "unavailable"}, {"tick", ticking ? "frame_work" : "lookup_only"}});
  return true;
}

}  // namespace nevr_config_alias
