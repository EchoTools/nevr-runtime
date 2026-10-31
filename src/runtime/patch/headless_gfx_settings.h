#pragma once

#include <cstdint>

// The headless replacement of ApplyRuntimeGfxSettings (echovr.exe 0x140c31870).
//
// The apply calls about 66 renderer setters, which need a live renderer; a headless run creates none. Its two callers
// are the boot call in FUN_1401090c0 (0x140109209) and the menu/JSON settings translator fcn.140a26be0 (0x140a26ca1).
// Both ignore the result, so on a headless run the detour returns 0 and never enters the original. The detour is
// installed from PatchEnableHeadless and nowhere else; nothing is installed on a client or a server.
namespace nevr_headless_gfx_settings {

/// Detours ApplyRuntimeGfxSettings at `gameBase + 0xC31870`, after the 27-byte prologue matches. Returns true when the
/// detour is installed. On a prologue mismatch nothing is installed and a Warning names the expected and actual bytes;
/// the game's apply then runs unhooked on this run.
bool Install(std::uintptr_t gameBase);

#ifdef NEVR_HEADLESS_GFX_TESTING
/// Removes the detour and clears the call count (test seam).
void ResetForTest();
/// True when the detour is installed (test seam).
bool ActiveForTest();
#endif

}  // namespace nevr_headless_gfx_settings
