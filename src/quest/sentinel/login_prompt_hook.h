/* The in-headset sign-in prompt (issue #239): the game's own login-error text.
 *
 * When the game's login fails it hands the failure message to
 * CR15NetGame::SetDelimitedErrorMessage, which stores up to four '\n'-separated lines for its
 * error screen (pinned_targets.h has the measured call chain). A GOT hook on libr15's slot for
 * that function replaces the message with the prompt on the prompt board
 * (src/quest/auth/prompt_board.h) while token auth has one published, i.e. while it waits for
 * the player to sign in; otherwise the game's own message passes through unchanged.
 *
 * Frequency: the three callers are the login error callbacks (LogInFailedCB, LoginRemovedCB,
 * LocalUserProfileErrorCB), so the hook runs once per failed login attempt, never per frame.
 * The handler does not block: a lock-free board read into a stack buffer, then the original.
 *
 * It never logs (hook contract, docs/adr/0003). Its counters are reported by hook_report.h:
 *   login_prompt_text_shown    the game's message was replaced by the prompt
 *   login_prompt_text_passed   no prompt was published; the game's message went through
 *   login_prompt_thunk_faults  the thunk had no original to call (never expected)
 * The install itself is logged by GotHook as one JSON line.
 *
 * Built with -fno-exceptions, like every translation unit that includes callback_thunk.h.
 */
#pragma once

#include <cstdint>

#include "pinned_targets.h"

namespace nevr_quest::login_prompt {

using Thunk = sentinel::pinned::LibR15SetDelimitedErrorMessageThunk;

// Registers the three counters. Call before sentinel::StartReporter.
bool RegisterCounters() noexcept;

// Arms the handler and installs the thunk in libr15.so's slot. A refused install is logged by
// GotHook and leaves the game's call intact. Returns whether the slot now holds the thunk.
bool Install() noexcept;

// Arms the handler without touching a GOT slot: for the host test, which publishes an original
// through Thunk::OriginalOut() and calls Thunk::EntryFn() directly.
void ArmForTest() noexcept;

// The counters' current values.
std::uint64_t ShownCount() noexcept;
std::uint64_t PassedCount() noexcept;

}  // namespace nevr_quest::login_prompt
