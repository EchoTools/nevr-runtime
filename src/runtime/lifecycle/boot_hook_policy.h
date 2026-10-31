#pragma once

#include <cstdio>

// What a partial LoadLibrary hook install means for the boot. Pure, so the verdict is unit-tested
// (test_dll_load_hook.cpp) without running InitializeAfterGameImageGuard.
//
// The four LoadLibrary hooks (nevr_dll_load_hook::Install) run every load filter, post-load observer and
// DLL-load callback the runtime registers; loads through an unhooked variant run none of them. They are
// REQUIRED (#632): a partial install fails the boot like any required boot hook, so a
// server refuses to start (boot.cpp's g_bootHookFailed check) and a client's last boot line says ok=false.

namespace nevr_boot_hook_policy {

inline constexpr bool kLoadLibraryHooksRequired = true;

struct LoadLibraryVerdict {
  bool complete;    // every variant is hooked
  bool failsBoot;   // the install is partial and the hooks are required
  char cause[96];   // for NoteBootHookFailureCause: "LoadLibrary hooks 3/4 installed"; empty when nothing failed
};

inline LoadLibraryVerdict JudgeLoadLibraryHooks(int installed, int total) {
  LoadLibraryVerdict verdict{};
  verdict.complete = installed == total;
  verdict.failsBoot = !verdict.complete && kLoadLibraryHooksRequired;
  if (verdict.failsBoot) {
    std::snprintf(verdict.cause, sizeof(verdict.cause), "LoadLibrary hooks %d/%d installed", installed, total);
  }
  return verdict;
}

}  // namespace nevr_boot_hook_policy
