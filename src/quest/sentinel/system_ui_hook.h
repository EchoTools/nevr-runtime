/* The main-menu QUIT on the headset (#392): the system UI the game asks for when the player quits.
 *
 * What the game does (pinned libr15; ReVault names in pinned_targets.h): the QUIT path reaches CVR::ShowSystemUI, which
 * calls vrapi_ShowSystemUI(java, 1). The type is 1, the headset's global menu. That menu is the Meta home surface, so
 * the player lands in the library instead of a quit confirmation (issue #392: the expected result is a confirmation,
 * then the game exits).
 *
 * One GOT hook, on libr15's vrapi_ShowSystemUI slot. The handler rewrites the type from the global menu to the
 * confirm-quit menu and calls the original with that; any other type passes through unchanged. The constants are
 * VrApi.h's ovrSystemUiType, from memory of the Mobile SDK and NOT checked against a header in this tree: the headset
 * test in the PR body is what confirms them.
 *
 * It never logs on the game's call path (callback_thunk.h rule 1). Install logs one line:
 *   {"level":"info","event":"system_ui","result":"installed|refused","from":1,"to":0,"slot":"vrapi_ShowSystemUI",...}
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include "pinned_targets.h"

namespace nevr_quest::system_ui {

// ovrSystemUiType (VrApi.h, unverified here): the quit-confirmation menu, and the global menu the QUIT path asks for.
inline constexpr int kConfirmQuitMenu = 0;
inline constexpr int kGlobalMenu = 1;

// Installs the hook into libr15.so's vrapi_ShowSystemUI slot. True when the slot holds the thunk. A refused install is
// logged by GotHook and by the hook's own line, and leaves the game's call intact.
bool Install() noexcept;

}  // namespace nevr_quest::system_ui
