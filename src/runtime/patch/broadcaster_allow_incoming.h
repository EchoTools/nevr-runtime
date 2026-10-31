#pragma once

#include <array>
#include <cstdint>

// P13 (#1774): the detour on CBroadcaster::Initialize (0x140F7FE50), the one function that takes the netconfig mask.
// The hook sets allow_incoming (bit 0) in the mask and calls the original with all 15 parameters. The install and the
// PatchDetour call live in mode_patches.cpp (PatchEnableServer), where the identity manifest scanner finds them.
namespace nevr_broadcaster_allow_incoming {

// CBroadcaster::Initialize: Win64, four register parameters and eleven stack arguments (p5..p15 at [RSP+0x20..0x70]).
using BroadcasterInitializeFn = std::uint64_t (*)(std::int64_t* self, std::uint64_t nameHash, std::int16_t port,
                                                  std::uint32_t* mask, std::uint64_t p5, std::uint64_t p6,
                                                  std::uint64_t p7, std::uint64_t p8, std::uint64_t p9,
                                                  std::uint64_t p10, std::uint64_t p11, std::uint64_t p12,
                                                  std::uint64_t p13, std::uint64_t p14, std::uint64_t p15);

// The first 11 bytes of CBroadcaster::Initialize at 0x140F7FE50: MOV RAX,RSP; MOV [RAX+0x10],RBX; MOV [RAX+0x20],R9.
// All relocatable, no RIP-relative operand.
inline constexpr std::array<std::uint8_t, 11> kBroadcasterInitializePrologue{
    0x48, 0x8B, 0xC4, 0x48, 0x89, 0x58, 0x10, 0x4C, 0x89, 0x48, 0x20};

// The game's function as the hook found it, set by mode_patches.cpp before PatchDetour.
extern BroadcasterInitializeFn OriginalBroadcasterInitialize;

/// The detour: sets allow_incoming in *mask (if it was clear), logs the mask in and out, and forwards every parameter
/// to the original. Returns the original's RAX unchanged.
std::uint64_t BroadcasterInitializeHook(std::int64_t* self, std::uint64_t nameHash, std::int16_t port,
                                        std::uint32_t* mask, std::uint64_t p5, std::uint64_t p6, std::uint64_t p7,
                                        std::uint64_t p8, std::uint64_t p9, std::uint64_t p10, std::uint64_t p11,
                                        std::uint64_t p12, std::uint64_t p13, std::uint64_t p14, std::uint64_t p15);

}  // namespace nevr_broadcaster_allow_incoming
