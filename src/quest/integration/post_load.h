// What the sentinel does right after the game loads a module: the installs that need a library which
// is not mapped when the sentinel's constructor runs (docs/adr/0003, "Hook activation").
//
//   login hook          needs libpnsovr.so (loaded by CSysModule::Load -> dlopen)
//   matchmaking hook    needs libpnsradmatchmaking.so (loaded by CNSLobby::LoadMatchmakingSupport)
//   login title         needs the login-failed page's script library, lib7f2363b8976491aa.so (#1137)
//   plugin pass         needs nothing but a running game: the first non-null dlopen is its moment (#438, ADR 0019
//                       section 3), after the two installs above have had theirs
//   combat step         needs nothing but a running game too (#2171, docs/adr/0046 rule 6): it runs on the thread that ran the
//                       plugin pass, right after the pass has returned, or at the first non-null dlopen when no plugin is planned
//
// dlopen_hook.cpp calls AfterDlopen from the handler of the hook on libr15's dlopen slot, after the
// real dlopen has returned a non-null handle. AfterDlopen runs entirely outside any game call: it
// calls no game code (the installs patch GOT slots through libc and read the loader's tables), so it
// may allocate, lock and catch, and it is annotated NEVR_OUTSIDE_GAME_CALL for the frame sensor.
//
// Each action reports whether it is finished. An action that answers kRetryLater (the module it needs
// is not mapped yet) is tried again after the next successful dlopen; kDone and kGiveUp end it. The
// actions are injected so the host test drives the policy with fakes; production binds the real
// installs in production_steps.cpp.
#pragma once

#include <cstdint>

namespace nevr_quest::integration {

enum class Settle : std::uint8_t {
  kDone,        // installed (or already installed): never run again
  kRetryLater,  // the module it needs is not loaded yet: run again after the next dlopen
  kGiveUp,      // refused for a reason a retry cannot change (unknown build, bad slot): never run again
};

struct ActionResult {
  Settle settle = Settle::kGiveUp;
  const char* status = "unset";  // a fixed token for the log, never a value
};

// Must not throw and must not call game code.
using ActionFn = ActionResult (*)() noexcept;

struct PostLoadActions {
  ActionFn login = nullptr;        // null: the feature is off, nothing is tried
  ActionFn matchmaking = nullptr;  // null: same
  ActionFn loginTitle = nullptr;   // null: same
  // The peer timeout log (peer_timeout_log.h): libr15's DestroyPeer slot, once libr15 is loaded. null: not wanted.
  ActionFn peerTimeoutLog = nullptr;
  // The normal plugin pass (#438). Runs once, on the first non-null dlopen, after the actions above and OUTSIDE the lock
  // that guards them, because a plugin's code may call dlopen (which reaches AfterDlopen again on this thread). A call that
  // arrives while it runs does not start it again. It settles whatever it answers (a plugin that fails is a record, not a
  // retry). null: no plugin waits for it.
  ActionFn plugins = nullptr;
  // The combat area's normal install step (#2171, combat/combat_install.h). It shares the plugin pass's runner: one thread at a time
  // is in it, the combat action starts only after the plugin action has returned and its settled flag is set (a plugin's hook on a slot a
  // combat module also takes is in first, and the combat hook joins it through the manager's first-handler rule, ADR 0046 rule 7), on the thread that ran the plugin action, outside the lock above, and a call that
  // arrives while either runs returns without starting anything. With no plugin action it runs at the first non-null dlopen, behind the
  // same guard. Runs once and settles whatever it answers. null: combat does not run in this process, nothing is tried.
  ActionFn combat = nullptr;
};

// Replaces the actions and forgets every settled state. Call before the dlopen hook is installed (or
// from a test). Not for use while dlopen can run on another thread.
void SetPostLoadActions(const PostLoadActions& actions);

// The functions the dlopen hook calls for every non-null handle, before the actions above run and whether or not any is wanted, in
// a fixed table (module_observers.cpp): the first slot is SetModuleLoadedObserver's, the others are added with
// AddModuleLoadedObserver, and every observer is called in slot order. Each must not throw and must not call game code. The
// crash-first hook sets the first to hook the provider libraries' sigaction and signal slots when they load
// (sentinel/crash_first_hook.h, #1605); the EC client adds one to hook their WriteLogV slots (combat/ec.h, #2187). Not cleared by
// SetPostLoadActions.
using ModuleLoadedFn = void (*)(const char* name, void* handle) noexcept;
inline constexpr int kModuleLoadedObservers = 4;
// Sets the first slot (null: none).
void SetModuleLoadedObserver(ModuleLoadedFn fn) noexcept;
// Adds `fn` to the first free slot after the first. True when it is in the table (added now, or already there); false for null or
// a full table. Safe from any thread: a dlopen that races the add sees the observer or not, never a torn slot.
bool AddModuleLoadedObserver(ModuleLoadedFn fn) noexcept;
// Calls every observer with `name` and `handle` (AfterDlopen does, for a non-null handle).
void NotifyModuleLoaded(const char* name, void* handle) noexcept;
// Empties every slot (tests).
void ClearModuleLoadedObserversForTest() noexcept;

// True while at least one action is still wanted and unsettled.
bool PostLoadPending() noexcept;

// Counters of what AfterDlopen did, for tests and diagnostics.
struct PostLoadStats {
  std::uint64_t calls = 0;         // non-null dlopen handles seen
  std::uint64_t attempts = 0;      // action runs
  std::uint64_t loginSettled = 0;  // 1 when the login action settled
  std::uint64_t matchmakingSettled = 0;
  std::uint64_t loginTitleSettled = 0;  // 1 when the login title action settled
  std::uint64_t pluginsSettled = 0;  // 1 when the plugin pass ran
  std::uint64_t combatSettled = 0;   // 1 when the combat action ran
};
PostLoadStats PostLoadStatsView() noexcept;

// How many distinct libpnsradmatchmaking images the game's dlopen has returned (a handle that differs from the
// last one counts): 1 after the library loads once, 2 when the game unloaded it and mapped it again at another
// address. The redirect hook installs once per process, so this is what the self-check compares it against
// (src/quest/integration/production_steps.cpp, "matchmaking_reload_redirect").
std::uint64_t MatchmakingImages() noexcept;

}  // namespace nevr_quest::integration

// Declared where the hook TU (built -fno-exceptions) can see it without pulling in exceptions
// machinery: the definition carries NEVR_OUTSIDE_GAME_CALL (post_load.cpp).
namespace nevr_quest::integration {
void AfterDlopen(const char* name, void* handle) noexcept;
}
