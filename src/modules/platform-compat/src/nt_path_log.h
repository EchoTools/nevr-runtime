// #1845: the level of the line the CreateDirectoryW hook writes when the game passes a relative path behind
// the \\?\ prefix. Header only and pure, so a host test compiles it without the Windows headers.

#pragma once

namespace nevr_platform_compat {

enum class NtPathFixLevel { Debug, Warning };

// The game passes such a path itself on native Windows too, so the line is Debug on every platform.
inline NtPathFixLevel RelativeNtPathLogLevel() {
  return NtPathFixLevel::Debug;
}

}  // namespace nevr_platform_compat
