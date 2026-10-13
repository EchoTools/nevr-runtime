#pragma once

// Validated binary patch: write only when the site holds the bytes we know, read the write
// back, and say which of four things happened.  Pure byte logic with an injected writer so it
// is unit-tested without a game image (src/runtime/tests/test_checked_patch.cpp); production
// passes ProcessMemcpy.  Header-only and free of Windows and game headers on purpose.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nevr_checked_patch {

enum class Result {
  Applied,         // expected bytes were present; the replacement was written and read back
  AlreadyApplied,  // the replacement is already there; nothing was written
  Mismatch,        // neither expected nor replacement bytes; nothing was written
  WriteFailed,     // the writer failed, or the read-back is not the replacement
};

inline const char* ResultName(Result r) {
  switch (r) {
    case Result::Applied: return "applied";
    case Result::AlreadyApplied: return "already_applied";
    case Result::Mismatch: return "mismatch";
    case Result::WriteFailed: return "write_failed";
  }
  return "unknown";
}

struct Site {
  const uint8_t* expected;  // bytes the unpatched binary holds
  const uint8_t* patched;   // bytes to write
  size_t size;
};

using WriteFn = bool (*)(void* dest, const void* src, size_t n);

inline Result Apply(uint8_t* site, const Site& spec, WriteFn write) {
  if (std::memcmp(site, spec.patched, spec.size) == 0) return Result::AlreadyApplied;
  if (std::memcmp(site, spec.expected, spec.size) != 0) return Result::Mismatch;
  if (!write(site, spec.patched, spec.size)) return Result::WriteFailed;
  return std::memcmp(site, spec.patched, spec.size) == 0 ? Result::Applied : Result::WriteFailed;
}

// SPECTATORSTREAM_CHECK (runtime/hook/addresses.h): the 6-byte JE inside PreprocessCommandLine
// that branches around the spectator-stream setup, and the 6 NOPs that fall through it.
inline constexpr uint8_t kSpectatorStreamExpected[6] = {0x0f, 0x84, 0xdf, 0x00, 0x00, 0x00};
inline constexpr uint8_t kSpectatorStreamPatched[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

}  // namespace nevr_checked_patch
