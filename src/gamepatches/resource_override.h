#pragma once

/// Install a hook on Resource_InitFromBuffers (0x140fa2510) to replace
/// the CComponentSpaceResource for mpl_arena_a with a modified version
/// from _overrides/mpl_arena_a. No-op if the override file doesn't exist.
void InstallResourceOverride();
