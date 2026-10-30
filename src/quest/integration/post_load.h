// What the sentinel does right after the game loads a module: the installs that need a library which
// is not mapped when the sentinel's constructor runs (docs/adr/0003, "Hook activation").
//
//   login hook          needs libpnsovr.so (loaded by CSysModule::Load -> dlopen)
//   matchmaking hook    needs libpnsradmatchmaking.so (loaded by CNSLobby::LoadMatchmakingSupport)
//   login title         needs the login-failed page's script library, lib7f2363b8976491aa.so (#1137)
//   plugin pass         needs nothing but a running game: the first non-null dlopen is its moment (#438, ADR 0019
//                       section 3), after the two installs above have had theirs
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
  // The normal plugin pass (#438). Runs once, on the first non-null dlopen, after the actions above and OUTSIDE the lock
  // that guards them, because a plugin's code may call dlopen (which reaches AfterDlopen again on this thread). A call that
  // arrives while it runs does not start it again. It settles whatever it answers (a plugin that fails is a record, not a
  // retry). null: no plugin waits for it.
  ActionFn plugins = nullptr;
};

// Replaces the actions and forgets every settled state. Call before the dlopen hook is installed (or
// from a test). Not for use while dlopen can run on another thread.
void SetPostLoadActions(const PostLoadActions& actions);

// A function the dlopen hook calls for every non-null handle, before the actions above run and whether or not any is wanted.
// It must not throw and must not call game code. The crash-first hook uses it to hook the provider libraries' sigaction and
// signal slots when they load (sentinel/crash_first_hook.h, #1605). Set once at install; null: none. Not cleared by
// SetPostLoadActions.
using ModuleLoadedFn = void (*)(const char* name, void* handle) noexcept;
void SetModuleLoadedObserver(ModuleLoadedFn fn) noexcept;

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
