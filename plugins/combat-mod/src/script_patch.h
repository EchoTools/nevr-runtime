/* SYNTHESIS -- custom tool code, not from binary */
#pragma once

#include <cstdint>
#include <vector>

namespace combat_mod {

/*
 * InstallScriptPatch -- hook the engine's script DLL loader.
 *
 * When the game loads a script module by path, the hook intercepts the
 * call and patches known scripts in-memory immediately after load.
 * Script identification is by UUID substring in the file path.
 *
 * Patched scripts:
 *   - f808c072bb49da47  (streaming) -- lobby channel UUID rewrite
 *   - d44f62ba114f0cde  (event)     -- event channel UUID rewrite
 *   - 12220d647ad3f5d3  (arena)     -- disable forced arena chassis swap
 *
 * Appends the hook target to `hooks` for shutdown cleanup.
 */
void InstallScriptPatch(uintptr_t base, std::vector<void*>& hooks);

} // namespace combat_mod
