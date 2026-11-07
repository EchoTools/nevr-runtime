#pragma once

// Hooks into the game's script DLLs (bin\win10\scripts\<16 hex>.dll), per script, installed each time the game loads
// the script and removed before it frees it (#600, EchoCombat slice 7; the plan's D1).
//
// The game loads a script through CScriptResource::Load (echovr.exe 0x1412ebb50: LoadLibraryW of
// scripts\<hash>.dll, then the DLL's setup_bindings) and frees it through the resource's vslot 6 (0x1412ece90, which
// calls the game's FreeLibrary wrapper 0x1400eb1c0) when the resource's last reference goes. A script is freed and
// loaded again as levels change, so a hook installed once would point into a freed image: script_resource_hook.cpp
// wraps both and calls OnLoaded after the original Load returns (so after setup_bindings, whose accessor slots some
// hooks replace) and OnUnloading before the original frees the module.
//
// A row names the script by its hash (the DLL's file name), the function by its RVA in that DLL, and the bytes the hook
// was written against. Every row of a script is checked before any is installed: one row whose bytes differ refuses
// the whole script, with a Warning per row that names its address, the bytes it expected and the bytes it found
// (logging.md Rule 6), so a script is hooked as written or runs stock, never half hooked. A module the game loads twice
// (two resources of one script share the DLL) is hooked once and unhooked when the last of them is freed.

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nevr_script_hooks {

/// A row with no detour (and no original) is a pin: its bytes are checked with the script's other rows and nothing is
/// installed there. It pins code in the middle of a function that a hook elsewhere depends on.
struct Hook {
  std::uint64_t script;          // the script's hash: scripts\<016llx>.dll
  std::uint32_t rva;             // the hooked function, from the DLL's base
  const std::uint8_t* expected;  // the bytes at the function the detour was written against
  std::size_t expected_len;
  void* detour;
  void** original;               // set to the function's address, then the trampoline, on each install
  const char* name;              // for the log and HookGuard
};

/// A pointer slot in the script's own data that the engine fills during setup_bindings (an engine callback the script
/// calls through, `call [rip+slot]`): replaced after the load, so the script calls `replacement`, which can call the
/// engine's function through `*original`. Checked with the script's detour rows: the slot must hold a pointer into the
/// game executable, else the whole script is refused. Put back before the game frees the script.
struct Slot {
  std::uint64_t script;
  std::uint32_t rva;    // the slot, from the DLL's base
  void* replacement;
  void** original;      // set to the engine's function on each install
  std::atomic<void*>* module;  // the script's base while the slot is replaced, null otherwise (read by the replacement)
  const char* name;
};

/// Adds rows to the table. Called at boot, before the game loads its first script; a row added after a script is
/// loaded applies from that script's next load.
void Register(const Hook* rows, std::size_t count);
void Register(const Slot* slots, std::size_t count);

/// The game loaded script `script` at `base` (setup_bindings has run). Installs the script's rows, once per module.
void OnLoaded(std::uint64_t script, void* base);

/// The game is about to free its reference to script `script` at `base`. Removes the module's hooks when it is the
/// last reference this table saw loaded.
void OnUnloading(std::uint64_t script, void* base);

/// The number of modules the table holds as loaded, and how many of them carry hooks (for tests and the summary).
std::size_t LoadedModules();
std::size_t HookedModules();

/// Forgets every row and module. Tests only: it does not remove installed hooks.
void ResetForTest();

}  // namespace nevr_script_hooks
