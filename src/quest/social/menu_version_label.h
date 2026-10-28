#pragma once

#include <cstddef>
#include <string>

#include "pinned_targets.h"

// The runtime version on the Quest main menu (#747), the twin of the PC hook (src/runtime/patch/menu_version_label.cpp).
// The stock versions panel sets its lobby_version text item through CCanvasUICS::SetText; this redirects that call and
// draws "nEVR <project version>" in place of the lobby build time. The decisions are the PC rules header
// (src/runtime/patch/menu_version_label_rules.h), reused unchanged.
//
// The redirect is a GOT hook on libr15's own slot for SetText (pinned_targets.h, LibR15CanvasSetText: JUMP_SLOT 0x36ff908).
// Every SetText2 script node calls the function through that slot and nothing calls it directly, so the slot is the whole
// call path. The slot is a data page: the install changes it and puts its protection back, and no code page is written, so
// the Horizon policy that denies execmod on a written libr15 code page (docs/adr/0021) is not met.
namespace quest_social::menu_version {

using Thunk = sentinel::pinned::LibR15CanvasSetTextThunk;

// What is drawn for a project version: "nEVR <version>" fitted to the lobby_version slot, as on PC
// (nevr_build_identity::MenuVersionLabel and nevr_menu_version_label::FitLabel). Label() is the running build's.
std::string LabelFor(const char* version);
std::string Label();

// Installs the hook into libr15's SetText slot. Returns 1 when the slot holds the thunk, 0 when the install is refused (the
// game then draws its own text). Logs one line either way.
std::size_t InstallMenuVersionLabelHook() noexcept;

// The install on any target, with `label` already fitted. InstallMenuVersionLabelHook is this on the pinned target.
bool InstallAt(const sentinel::GotTarget& target, const char* label) noexcept;

// How many calls drew the label.
unsigned long long Replaced() noexcept;

// Test support: arms the handler without touching a GOT slot; ResetForTest disarms and clears the count.
void ArmForTest(const char* label) noexcept;
void ResetForTest() noexcept;

}  // namespace quest_social::menu_version
