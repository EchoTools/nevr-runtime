#pragma once

// The PC edge of the self-update's shared engine (core/update/engine_check.h, core/update/worker.h): what a Win32 path needs and
// the game's log bits. The engine is platform-free and takes UTF-8 paths and a UTF-8 root; this header is how the PC binding
// (update_process.cpp) and the PC tests get from wide text to it. Implemented in update_run.cpp.

#include <cstdint>
#include <functional>
#include <string>

#include "core/update/engine_check.h"

namespace nevr_update {

/// The game's LogLevel bit (abi/echovr.h: Debug 1, Info 2, Warning 4, Error 8) for a level above; an unknown level is Info.
/// update_process.cpp static_asserts these against the enum, so the mapping is tested here and cannot drift from it.
constexpr int EchoLevelBits(int level) noexcept {
  return level == kDebug ? 0x1 : level == kWarning ? 0x4 : level == kError ? 0x8 : 0x2;
}

/// UTF-16 to UTF-8, strictly: a lone surrogate does not convert, and the function returns false with `utf8` empty. An empty string
/// converts to an empty string. The engine takes the root in UTF-8, so a root that does not convert is `root_unconvertible`.
bool RootToUtf8(const std::wstring& wide, std::string* utf8);

/// The PC verifier takes the wide path (Authenticode, opened with CreateFileW); the engine hands it the UTF-8 path. A path that does not
/// convert is not a verdict on the bytes: it is refused as `path_unconvertible`, with ok and refused both false, and `verify` is not run.
std::function<VerifyOutcome(const std::string& utf8_path, const ExpectedFile& expected)> WidenVerifier(
    std::function<VerifyOutcome(const std::wstring& path)> verify);

}  // namespace nevr_update
