#pragma once

// Validated binary patch: write only when the site holds the bytes we know, read the write
// back, and say which of four things happened.  Pure byte logic with an injected writer so it
// is unit-tested without a game image (src/runtime/tests/test_checked_patch.cpp); production
// passes ProcessMemcpy.  Header-only and free of Windows and game headers on purpose.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace nevr_checked_patch {

enum class Result {
  Applied,         // expected bytes were present; the replacement was written and read back
  AlreadyApplied,  // the replacement is already there; nothing was written
  Mismatch,        // neither expected nor replacement bytes; nothing was written
  WriteFailed,     // the writer failed or the read-back is not the replacement, and the site holds no replacement
  WrittenWithError,  // the writer reported a failure, but the read-back is the replacement: the bytes are in
};

inline const char* ResultName(Result r) {
  switch (r) {
    case Result::Applied: return "applied";
    case Result::AlreadyApplied: return "already_applied";
    case Result::Mismatch: return "mismatch";
    case Result::WriteFailed: return "write_failed";
    case Result::WrittenWithError: return "written_with_error";
  }
  return "unknown";
}

struct Site {
  const uint8_t* expected;  // bytes the unpatched binary holds
  const uint8_t* patched;   // bytes to write
  size_t size;
};

using WriteFn = bool (*)(void* dest, const void* src, size_t n);

/// The Win32 error of the last failed write (logging Rule 10). 0 when no write failed.
using WriteErrorFn = uint32_t (*)();

/// Remembers the error of a failed write for WriteErrorFn: the last failure wins, a success changes nothing. A
/// WriteFn is a plain function pointer, so a writer keeps its error here, and the patch resets it before Apply.
struct WriteErrors {
  uint32_t last = 0;
  bool Note(bool ok, uint32_t error) {
    if (!ok) last = error;
    return ok;
  }
};

// The site is read back whatever the writer said: a writer may report a failure after its bytes went in (a protection
// restore that failed), and the caller needs to know what the site holds, not only what the writer claimed.
inline Result Apply(uint8_t* site, const Site& spec, WriteFn write) {
  if (std::memcmp(site, spec.patched, spec.size) == 0) return Result::AlreadyApplied;
  if (std::memcmp(site, spec.expected, spec.size) != 0) return Result::Mismatch;
  const bool reported = write(site, spec.patched, spec.size);
  if (std::memcmp(site, spec.patched, spec.size) != 0) return Result::WriteFailed;
  return reported ? Result::Applied : Result::WrittenWithError;
}

// SPECTATORSTREAM_CHECK (runtime/hook/addresses.h): the 6-byte JE inside PreprocessCommandLine
// that branches around the spectator-stream setup, and the 6 NOPs that fall through it.
inline constexpr uint8_t kSpectatorStreamExpected[6] = {0x0f, 0x84, 0xdf, 0x00, 0x00, 0x00};
inline constexpr uint8_t kSpectatorStreamPatched[6] = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90};

// The one place the {expected, patched} pair is assembled, so a swap is a unit-test failure.
inline Site SpectatorStreamSite() {
  return Site{kSpectatorStreamExpected, kSpectatorStreamPatched, sizeof(kSpectatorStreamExpected)};
}

// One key=value record for a patch site: the bytes the unpatched binary should hold, the bytes
// found there BEFORE the write, and the outcome. Written to `out` (NUL-terminated, truncated at
// `cap`); returns snprintf's result. Both the boot-tee and Log() paths print this same text. A write_failed record
// ends with the writer's Win32 error (`writeError`, from WriteErrorFn), so a failed write names its cause.
inline int FormatSpectatorStreamRecord(char* out, size_t cap, const char* phase, uintptr_t addr,
                                       const uint8_t* actual, uint32_t writeError, Result result) {
  constexpr size_t kBytes = sizeof(kSpectatorStreamExpected);
  char expectedHex[2 * kBytes + 1];
  char actualHex[2 * kBytes + 1];
  for (size_t i = 0; i < kBytes; ++i) {
    std::snprintf(expectedHex + 2 * i, 3, "%02x", kSpectatorStreamExpected[i]);
    std::snprintf(actualHex + 2 * i, 3, "%02x", actual[i]);
  }
  int n = std::snprintf(out, cap,
                        "[NEVR.PATCH] spectatorstream_check phase=%s addr=0x%llx expected=%s actual=%s result=%s",
                        phase, static_cast<unsigned long long>(addr), expectedHex, actualHex, ResultName(result));
  if (result == Result::WriteFailed && n >= 0 && static_cast<size_t>(n) < cap) {
    n += std::snprintf(out + n, cap - static_cast<size_t>(n), " write_error=%u", writeError);
  }
  return n;
}

}  // namespace nevr_checked_patch
