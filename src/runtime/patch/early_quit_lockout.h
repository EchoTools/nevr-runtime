#pragma once
// The game client's early quit lockout, made to work (owner, 2026-10-02: "now we enable it as part of this
// build"). The shipped client cannot show it whatever the game service sends: the countdown expiry is only
// ever written -1, the scripts' lockoutcountdownactive is a constant 0, and the feature-flag callback masks
// away bit 1, which the matchmaking script's lockout path needs. Evidence and the live proof:
// echovr-reconstruction docs/earlyquit_field_analysis.md (dc719052). Decisions: early_quit_lockout_rules.h.

#include <cstdint>

namespace EarlyQuitLockout {

// Hooks CR15NetGame::SetEarlyQuitPenaltyLevel and CR15NetEarlyQuitPenaltyExpression::operator(), and keeps bit
// 1 of the feature flags. Each piece is installed only after its bytes are validated; a mismatch is logged
// and that piece is skipped. Call after the hooking engine is up; logs through the boot tee (DllMain time).
void Install(std::uintptr_t gameBase);

}  // namespace EarlyQuitLockout
