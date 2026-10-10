#pragma once

// pnsradmatchmaking.dll carries a compiled matchmaker host default
// ("wss://matchmaker.readyatdawn.com/rad/rad15_live", a 48-byte slot at RVA 0x1c84d8). The game
// unloads and reloads the module during a session (CNSLobby::StartSessionCBHost loads it,
// ~CR15NetLobby frees it), and every load maps a fresh unpatched image, so the rewrite is a pure
// function of one image and runs on every load notification (#18). It is separate from the loader
// callback so a test can drive it over fake images.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace MatchmakerHostPatch {

constexpr std::uintptr_t kHostRva = 0x1c84d8;
constexpr char kHostExpected[] = "wss://matchmaker.readyatdawn.com/rad/rad15_live";
// The slot is the string and its NUL; the next byte belongs to other data.
constexpr std::size_t kHostSlotSize = sizeof(kHostExpected);

/// Whether a replacement of `length` characters plus its NUL stays inside the slot.
constexpr bool FitsInHostSlot(int length) {
  return length > 0 && static_cast<std::size_t>(length) + 1 <= kHostSlotSize;
}

enum class Result {
  Patched,
  NoPort,         // the matchmaker listener never bound a port
  DoesNotFit,     // the replacement would overflow the 48-byte slot
  BytesMismatch,  // the slot is not the original string (already patched, or another build)
  WriteFailed,
};

/// Rewrites the host slot of one pnsradmatchmaking image at `base` to ws://127.0.0.1:<port>.
/// `write(dst, src, len)` performs the memory write and returns false on failure.
template <typename Write>
Result Apply(std::uint8_t* base, std::uint16_t port, Write&& write) {
  if (port == 0) return Result::NoPort;
  char replacement[32];
  const int length = std::snprintf(replacement, sizeof(replacement), "ws://127.0.0.1:%u", static_cast<unsigned>(port));
  if (!FitsInHostSlot(length)) return Result::DoesNotFit;
  std::uint8_t* site = base + kHostRva;
  if (std::memcmp(site, kHostExpected, sizeof(kHostExpected) - 1) != 0) return Result::BytesMismatch;
  return write(site, replacement, static_cast<std::size_t>(length) + 1) ? Result::Patched : Result::WriteFailed;
}

}  // namespace MatchmakerHostPatch
