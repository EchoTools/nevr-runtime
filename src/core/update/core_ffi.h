#pragma once

// The core install method (installer/, ADR 0023) bound to the engine's Core (core/update/engine_check.h): the one binding the PC runtime
// and the Quest runtime share (docs/adr/0024-pc-self-update.md, section 3). Every call goes through the C ABI in
// installer/include/nevr_install.h; this unit adds no rule of its own.

#include "core/update/engine_check.h"

namespace nevr_update {

/// The Core over the linked installer library, one session per check. `profile_json` is the stage's profile: nullptr or an empty
/// string stages the PC release through NevrPcStage (the windows-zip profile); any other text is passed to NevrStage as the profile
/// (the Quest runtime passes {"kind":"quest-runtime","members":["libnevr-runtime.so"]}, installer/src/stage.rs QUEST_PROFILE).
/// A build that does not link the library returns a Core whose open fails with the reason, so the check ends in
/// `update check action=failed reason=core_open_failed`.
Core MakeFfiCore(const char* profile_json);

}  // namespace nevr_update
