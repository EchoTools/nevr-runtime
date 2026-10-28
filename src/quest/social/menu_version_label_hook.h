#pragma once

#include "pinned_targets.h"
#include "quest/social/menu_version_label.h"

// The thunk and the seams of the menu version label (menu_version_label.cpp). Included by -fno-exceptions code only: it
// pulls in callback_thunk.h.
namespace quest_social::menu_version {

using Thunk = sentinel::pinned::LibR15CanvasSetTextThunk;

// The install on any target, with `label` already fitted. InstallMenuVersionLabelHook is this on the pinned target.
bool InstallAt(const sentinel::GotTarget& target, const char* label) noexcept;

// How many calls drew the label.
unsigned long long Replaced() noexcept;

// Test support: arms the handler without touching a GOT slot; ResetForTest disarms and clears the count.
void ArmForTest(const char* label) noexcept;
void ResetForTest() noexcept;

}  // namespace quest_social::menu_version
