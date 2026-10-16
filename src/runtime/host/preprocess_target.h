#pragma once

// The game function the host hooks, and what must be there before it writes a
// detour. Shared by the rendezvous (which validates it) and the diagnostics (which
// report it).

#include <cstddef>
#include <cstdint>

namespace nevr_host {

// echovr.exe facts for the rendezvous. The host hooks
// `game_module + kPreprocessRva` and validates the pinned prologue before writing
// the detour (never blind-write). RVA: PreprocessCommandLine in src/abi/echovr_functions.cpp.
// Prologue: the first 18 bytes of echovr.exe 0x140116720 before the first
// position-dependent CALL rel32; tools/hook_identity_manifest.json pins the first
// 8 (0x405557488d6c24b1).
inline constexpr std::uintptr_t kPreprocessRva = 0x116720;
inline constexpr std::size_t kPreprocessPrologueSize = 18;
inline constexpr unsigned char kPreprocessPrologue[kPreprocessPrologueSize] = {
    0x40, 0x55, 0x57, 0x48, 0x8d, 0x6c, 0x24, 0xb1, 0x48,
    0x81, 0xec, 0xa8, 0x00, 0x00, 0x00, 0x48, 0x8b, 0xf9};

}  // namespace nevr_host
