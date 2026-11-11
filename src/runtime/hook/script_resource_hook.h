#pragma once

// The game's script load and free, wrapped so script_hooks.h can hook each script as it loads and unhook it before it
// is freed (#600). Both are CScriptResource virtual functions in echovr.exe:
//  - vslot 7, Load (0x1412ebb50): loads scripts\<hash>.dll by the resource's name (res+0x38) into res+0x70, finds
//    setup_bindings, fills the resource's engine callbacks and calls it; returns 0 on success. One global lock
//    (0x1420e0dc0) serialises it.
//  - vslot 6 (0x1412ece90, 39 bytes): frees res+0x70 through the game's FreeLibrary wrapper (0x1400eb1c0) when set,
//    then clears it. CResourceManager::Unload (0x140fa3000) reaches it when the resource's reference count reaches 0.

#include <cstdint>

namespace nevr_script_resource_hook {

inline constexpr std::uint64_t kLoadVa = 0x1412ebb50;
inline constexpr std::uint64_t kUnloadVa = 0x1412ece90;
inline constexpr std::uintptr_t kNameOffset = 0x38;    // the script's hash, the DLL's file name
inline constexpr std::uintptr_t kModuleOffset = 0x70;  // the loaded DLL

/// The first bytes of each, as the shipped echovr.exe has them (the hooks are installed only over these).
inline constexpr std::uint8_t kLoadPrologue[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x74,
                                                 0x24, 0x10, 0x48, 0x89, 0x7c, 0x24, 0x18};
inline constexpr std::uint8_t kUnloadPrologue[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48,
                                                   0x8b, 0xd9, 0x48, 0x8b, 0x49, 0x70};

/// Hooks both, or neither, in the game image at `game_base`; logs the outcome once. Once per process.
void Install(std::uintptr_t game_base);

/// What the wraps do around the original calls, for tests: after a Load that returned `result`, and before a free.
void AfterLoad(void* resource, std::uint64_t result);
void BeforeUnload(void* resource);

#ifdef NEVR_TEST_HOOKS
/// Lets a test install again. It does not remove installed hooks.
void ResetForTest();
#endif

}  // namespace nevr_script_resource_hook
