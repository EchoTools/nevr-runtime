#pragma once
// The login prompt the game shows in its own login-error text (issue #239).
//
// The token-auth worker publishes the text while it waits for the player; the sentinel's hook on
// CR15NetGame::SetDelimitedErrorMessage (src/quest/sentinel/login_prompt_hook.h) copies it into
// the game's error message. The two run on different threads and the hook runs on the game's
// call path, so:
//   - Copy() takes no lock, allocates nothing and cannot throw. It is a sequence-lock read of a
//     fixed buffer and gives up after a few attempts rather than wait for a writer.
//   - Publish() and Withdraw() serialise against each other with a pthread mutex and never run on
//     a game call path.
// This file and prompt_board.cpp are built with -fno-exceptions (src/quest/CMakeLists.txt), so
// every frame the hook reaches through Copy() is personality-free (docs/adr/0003, hook contract
// rule 5). Nothing here logs: the callers do.

#include <cstddef>
#include <cstdint>

namespace nevr::quest_auth::prompt_board {

// The game keeps four lines of 63 characters (CR15NetGame::SetErrorMessage stores four 64-byte
// buffers); four such lines and three '\n' separators fit.
inline constexpr std::size_t kMaxLines = 4;
inline constexpr std::size_t kMaxLineChars = 63;
inline constexpr std::size_t kCapacity = kMaxLines * kMaxLineChars + (kMaxLines - 1);

// Makes `text` (`len` bytes, no NUL inside) the published prompt. Returns false and leaves the
// board unchanged when `text` is null, `len` is 0 or above kCapacity, or `text` holds a NUL.
bool Publish(const char* text, std::size_t len) noexcept;

// Removes the published prompt. Returns whether one was published.
bool Withdraw() noexcept;

// Copies the published prompt, NUL-terminated, into `out`. Returns false (and writes "" when
// cap > 0) when nothing is published, `cap` is below kCapacity + 1, or a writer kept the buffer
// busy through every attempt.
bool Copy(char* out, std::size_t cap) noexcept;

// How many times the board has changed (a publish or a withdraw). For tests and logging.
std::uint64_t Version() noexcept;

}  // namespace nevr::quest_auth::prompt_board
