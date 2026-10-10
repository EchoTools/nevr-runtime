#pragma once
// What the game's login-error screen should say while token auth deals with the player (issue #239).
//
// The token-auth worker publishes text here; the sentinel's login-prompt hooks
// (src/quest/sentinel/login_prompt_hook.h) copy it into the game's error-message buffers. The two
// run on different threads and the hooks run on the game's call path, so:
//   - Read() takes no lock, allocates nothing, cannot throw and never waits for a writer: it is a
//     sequence-lock read of a fixed buffer that gives up (kBusy) after kReadAttempts tries, with a
//     CPU relax hint (no syscall, no sleep) between them.
//   - Publish() and Withdraw() serialise against each other with a pthread mutex and never run on
//     a game call path.
// This file and prompt_board.cpp are built with -fno-exceptions (src/quest/CMakeLists.txt), so
// every frame a hook reaches through Read() is personality-free (docs/adr/0003, hook contract
// rule 5). Nothing here logs: the callers do.

#include <cstddef>
#include <cstdint>

namespace nevr::quest_auth::prompt_board {

// The game keeps four lines of 63 characters (CR15NetGame::SetErrorMessage stores four 64-byte
// buffers); four such lines and three '\n' separators fit.
inline constexpr std::size_t kMaxLines = 4;
inline constexpr std::size_t kMaxLineChars = 63;
inline constexpr std::size_t kCapacity = kMaxLines * kMaxLineChars + (kMaxLines - 1);
inline constexpr int kReadAttempts = 16;

enum class Mode : std::uint8_t {
  // Replaces the game's message when its login fails, and refreshes a screen that shows a prompt.
  kPrompt = 1,
  // Only refreshes a screen that already shows a prompt (e.g. "signed in"); a new login failure
  // shows the game's own message.
  kNotice = 2,
};

enum class ReadResult {
  kEmpty,   // nothing published (out is "")
  kCopied,  // `out`, `mode` and `version` hold the published text
  kBusy,    // a writer kept the buffer busy through every attempt (out is "")
};

// Makes `text` (`len` bytes, no NUL inside) the published text. Returns false and leaves the
// board unchanged when `text` is null, `len` is 0 or above kCapacity, or `text` holds a NUL.
bool Publish(const char* text, std::size_t len, Mode mode) noexcept;

// Removes the published text and overwrites the buffer with zeros. Returns whether text was
// published.
bool Withdraw() noexcept;

// Copies the published text, NUL-terminated, into `out` (`cap` must be at least kCapacity + 1;
// smaller reads as kEmpty). `mode` and `version` may be null.
ReadResult Read(char* out, std::size_t cap, Mode* mode, std::uint64_t* version) noexcept;

// Changes so far (each publish or withdraw adds one). Read() reports the version it copied, so a
// reader can tell whether what it applied is still current.
std::uint64_t Version() noexcept;

// Test support: how many bytes of the text buffer are not zero (a withdrawn board holds none).
std::size_t NonZeroTextBytesForTest() noexcept;

// Test support: holds the board in the "writer inside" state, so a reader deterministically gets
// kBusy, until EndWriteForTest. Nothing in production calls these.
void BeginWriteForTest() noexcept;
void EndWriteForTest() noexcept;

}  // namespace nevr::quest_auth::prompt_board
