/* The one way to install a hook: through a thunk.
 *
 * GotHook::Install, which takes an arbitrary function pointer, is private. A hook function
 * that is not a CallbackThunk entry would not be a root of the frame sensor (tests/quest
 * TestHookFramesCarryNoPersonality), so it could hold a personality-bearing frame across the
 * call into game code unseen. InstallThunk installs the thunk's entry and nothing else.
 *
 * A reviewed assembly entry (callback_thunk.h rule 7) is the one other hook; InstallAsmEntry installs it from the record that
 * declares it.
 *
 * Including this header pulls in callback_thunk.h, so a translation unit that installs a hook
 * must be built with -fno-exceptions.
 */
#pragma once

#include "callback_thunk.h"
#include "got_hook.h"
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

// Befriended by GotHook so it can reach the private entry install (callback_thunk.h rule 7). It takes the record that
// NEVR_ASM_ENTRY_RECORD made, which is the frame sensor's root for the entry and its continuation, and nothing else. Only
// src/quest/combat/ and the tests name it (tests/quest TestRawInstallOnlyInTests).
struct AsmEntryInstaller {
  template <typename Entry, typename Continuation>
  static GotStatus Install(GotHook& hook, const GotTarget& target, const AsmEntryRecord<Entry, Continuation>& record,
                           void** nextOut, ImageLookup lookup) {
    if (!IsRecordedAsmEntry(record)) {
      // A record built at run time is not in the section the sensor reads, so its continuation would not be walked.
      LogFields(LogLevel::kError, "got_hook",
                {{"op", "install"}, {"status", "bad_argument"}, {"reason", "record_not_in_nevr_hook_records"},
                 {"kind", "asm_entry"}, {"module", target.module != nullptr ? target.module : "(null)"},
                 {"symbol", target.symbol != nullptr ? target.symbol : "(null)"},
                 {"owner", target.owner != nullptr ? target.owner : "(unnamed)"}});
      return GotStatus::kBadArgument;
    }
    return hook.InstallEntry(target, reinterpret_cast<void*>(record.entry), nextOut, lookup);
  }
};

// Installs a reviewed assembly entry into `target`'s slot. `nextOut` is the word the entry (or its continuation) loads with
// acquire semantics at every call: it holds the function the entry calls in place of the game's (the game's, or the newest hook
// already on the slot) before the slot changes, and the manager republishes it when that hook leaves. The slot's newest hook
// is the entry from then until it leaves the chain (its Remove or Forget): a later install on the slot is refused
// (kSlotHeldByEntry).
template <typename Entry, typename Continuation>
GotStatus InstallAsmEntry(GotHook& hook, const GotTarget& target, const AsmEntryRecord<Entry, Continuation>& record,
                          void** nextOut, ImageLookup lookup = FindLoadedImage) {
  return AsmEntryInstaller::Install(hook, target, record, nextOut, lookup);
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
// or re-protected. Same rule as InstallThunk: the detour is always a thunk entry, so the frame sensor walks it.
template <typename Thunk>
nevr_quest::libr15_patch::ActivateStatus InstallRowThunk(const char* rowId) {
  static_assert(IsCallbackThunk<Thunk>::value,
                "InstallRowThunk requires a CallbackThunk<Tag, Ret(Args...)> instantiation");
  return RowThunkInstaller::Install<Thunk>(rowId);
}

}  // namespace sentinel
