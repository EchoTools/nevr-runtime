// The social facade's hook install, behind a header a translation unit WITH exceptions may include.
// social_install.h includes callback_thunk.h, which refuses to compile with exceptions on; this shim
// (social_shim.cpp, built -fno-exceptions) is the only place the integration touches it.
#pragma once

namespace nevr_quest::integration {

// quest_social::RegisterSocialReportCounters(): 10 of the reporter's 32 counters.
bool RegisterSocialCounters() noexcept;

// quest_social::InstallSocialHook(true): builds the facade, arms the callback, redirects the slot.
// False when the hook was refused (the game keeps the Oculus social object).
bool InstallSocialHook() noexcept;

}  // namespace nevr_quest::integration
