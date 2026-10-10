#pragma once

#include <windows.h>

namespace nevr_game_image_guard {

constexpr DWORD kUnsupportedImageExitCode = 0xE0420003;

/// Validate readable PE header spans independently of the image's claimed lengths.
bool IsSupportedGameModule(const void* module) noexcept;

/// End the current process with a stable nonzero code if its game module is unsupported.
void RequireSupportedGameModule(HMODULE module) noexcept;

/// Shared production entry used by both DLL-load and launcher initialization routes.
void RunWithSupportedGameModule(HMODULE module, void (*initialize)(HMODULE)) noexcept;

}  // namespace nevr_game_image_guard
