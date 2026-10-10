// The social facade's hook install, behind a header a translation unit WITH exceptions may include.
// social_install.h includes callback_thunk.h, which refuses to compile with exceptions on; this shim
// (social_shim.cpp, built -fno-exceptions) is the only place the integration touches it.
#pragma once

namespace nevr_quest::integration {

// quest_social::RegisterSocialReportCounters(): 19 of the reporter's counters (hook_report.h).
bool RegisterSocialCounters() noexcept;

// quest_social::InstallSocialHook(true): builds the facade, arms the callback, redirects the slot.
// False when the hook was refused (the game keeps the Oculus social object). `detail` receives a fixed
// token naming the outcome (an InstallStatus or GotStatus name). `presenceNames` is the effective
// presence_names feature (#393): answer the rich presence's destination lookup from the built-in table.
bool InstallSocialHook(const char** detail, bool presenceNames) noexcept;

}  // namespace nevr_quest::integration
