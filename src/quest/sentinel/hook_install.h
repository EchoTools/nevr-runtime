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
#include "inline_hook.h"
#include "libr15_attach.h"

namespace sentinel {

// Befriended by GotHook so it can reach the private raw Install.
struct ThunkInstaller {
  template <typename Thunk>
  static GotStatus Install(GotHook& hook, const GotTarget& target, ImageLookup lookup) {
    static_assert(IsCallbackThunk<Thunk>::value,
                  "InstallThunk requires a CallbackThunk<Tag, Ret(Args...)> instantiation");
    return hook.Install(target, Thunk::EntryAddress(), Thunk::OriginalOut(), lookup);
  }
};

// Installs `Thunk`'s entry into `target`'s slot; the original lands in Thunk's original
// pointer before the slot changes.
template <typename Thunk>
GotStatus InstallThunk(GotHook& hook, const GotTarget& target,
                       ImageLookup lookup = FindLoadedImage) {
  static_assert(IsCallbackThunk<Thunk>::value,
                "InstallThunk requires a CallbackThunk<Tag, Ret(Args...)> instantiation");
  return ThunkInstaller::Install<Thunk>(hook, target, lookup);
}

// Befriended by InlineHook so it can reach the private inline Install.
struct InlineThunkInstaller {
  template <typename Thunk>
  static InlineStatus Install(const void* target, const InlinePrologue& prologue) {
    static_assert(IsCallbackThunk<Thunk>::value,
                  "InstallInlineThunk requires a CallbackThunk<Tag, Ret(Args...)> instantiation");
    return InlineHook::Install(target, prologue, Thunk::EntryAddress(), Thunk::OriginalOut());
  }
};

// Patches `target` (an arm64 function entry, inline_hook.h) so that it runs `Thunk`'s entry. The
// thunk's original pointer is the trampoline, which runs the target's displaced prologue. Same rule as
// InstallThunk: the detour is always a thunk entry, so the frame sensor walks it.
template <typename Thunk>
InlineStatus InstallInlineThunk(const void* target, const InlinePrologue& prologue) {
  static_assert(IsCallbackThunk<Thunk>::value,
                "InstallInlineThunk requires a CallbackThunk<Tag, Ret(Args...)> instantiation");
  return InlineThunkInstaller::Install<Thunk>(target, prologue);
}

// Befriended by RowActivation so it can reach the private activation (libr15_attach.h).
struct RowThunkInstaller {
  template <typename Thunk>
  static nevr_quest::libr15_patch::ActivateStatus Install(const char* rowId) {
    static_assert(IsCallbackThunk<Thunk>::value,
                  "InstallRowThunk requires a CallbackThunk<Tag, Ret(Args...)> instantiation");
    return nevr_quest::libr15_patch::RowActivation::ActivateRow(rowId, Thunk::EntryAddress(), Thunk::OriginalOut());
  }
};

// Activates the build-time patch's row `rowId` (ADR 0021): `Thunk`'s entry goes into the row's slot with one atomic
// store, and the thunk's original pointer is the row's trampoline, published before the store. No page is written
// or re-protected. Same rule as InstallInlineThunk: the detour is always a thunk entry, so the frame sensor walks it.
template <typename Thunk>
nevr_quest::libr15_patch::ActivateStatus InstallRowThunk(const char* rowId) {
  static_assert(IsCallbackThunk<Thunk>::value,
                "InstallRowThunk requires a CallbackThunk<Tag, Ret(Args...)> instantiation");
  return RowThunkInstaller::Install<Thunk>(rowId);
}

}  // namespace sentinel
