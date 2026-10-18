#pragma once

// The game function the host hooks, and what must be there before it writes a
// detour. Shared by the rendezvous (which validates it) and the diagnostics (which
// report it).

#include <cstddef>
#include <cstdint>

namespace nevr_host {

// echovr.exe facts for the rendezvous. The host hooks
// `game_module + kGameMainWrapperRva` and validates the pinned prologue before writing
// the detour (never blind-write). RVA: GAME_MAIN_WRAPPER in src/runtime/hook/addresses.h
// (echovr.exe 0x1400CD510).
// Prologue: the first 13 bytes of the wrapper, every instruction before the CALL rel32
// to GameMain: `sub rsp,0x38` and `mov qword [rsp+0x20],0`. They are position-independent,
// which is what lets the hook's trampoline relocate them.
// tools/hook_identity_manifest.json pins the first 8 (0x4883ec3848c74424).
inline constexpr std::uintptr_t kGameMainWrapperRva = 0xCD510;
inline constexpr std::size_t kGameMainWrapperPrologueSize = 13;
inline constexpr unsigned char kGameMainWrapperPrologue[kGameMainWrapperPrologueSize] = {
    0x48, 0x83, 0xec, 0x38, 0x48, 0xc7, 0x44, 0x24, 0x20, 0x00, 0x00, 0x00, 0x00};

}  // namespace nevr_host
