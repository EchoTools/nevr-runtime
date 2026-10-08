/* The one way to install a hook: through a thunk.
 *
 * GotHook::Install, which takes an arbitrary function pointer, is private. A hook function
 * that is not a CallbackThunk entry would not be a root of the frame sensor (tests/quest
 * TestHookFramesCarryNoPersonality), so it could hold a personality-bearing frame across the
 * call into game code unseen. InstallThunk installs the thunk's entry and nothing else.
 *
 * Including this header pulls in callback_thunk.h, so a translation unit that installs a hook
 * must be built with -fno-exceptions.
 */
#pragma once

#include "callback_thunk.h"
#include "got_hook.h"

namespace sentinel {

// Befriended by GotHook so it can reach the private raw Install.
struct ThunkInstaller {
  template <typename Thunk>
  static GotStatus Install(GotHook& hook, const GotTarget& target, ImageLookup lookup) {
    return hook.Install(target, Thunk::EntryAddress(), Thunk::OriginalOut(), lookup);
  }
};

// Installs `Thunk`'s entry into `target`'s slot; the original lands in Thunk's original
// pointer before the slot changes.
template <typename Thunk>
GotStatus InstallThunk(GotHook& hook, const GotTarget& target,
                       ImageLookup lookup = FindLoadedImage) {
  return ThunkInstaller::Install<Thunk>(hook, target, lookup);
}

}  // namespace sentinel
