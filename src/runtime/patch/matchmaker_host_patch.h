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
#include <optional>
#include <utility>

namespace nevr_matchmaker_host_patch {

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

// ntdll's LDR_DLL_NOTIFICATION_REASON_LOADED / _UNLOADED, the `reason` of an LdrRegisterDllNotification callback.
constexpr unsigned kNotificationLoaded = 1;
constexpr unsigned kNotificationUnloaded = 2;
constexpr char kModuleName[] = "pnsradmatchmaking.dll";

/// Whether the loader's BaseDllName (`nameChars` UTF-16 code units, not NUL-terminated) is the matchmaking
/// module: ASCII case-insensitive, whole name.
template <typename Ch>
bool IsMatchmakingModule(const Ch* name, std::size_t nameChars) {
  constexpr std::size_t kLength = sizeof(kModuleName) - 1;
  if (name == nullptr || nameChars != kLength) return false;
  for (std::size_t i = 0; i < kLength; ++i) {
    auto c = static_cast<unsigned>(name[i]);
    if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
    if (c != static_cast<unsigned char>(kModuleName[i])) return false;
  }
  return true;
}

/// One loader notification. Only a LOAD of the matchmaking module acts: the image at `base` is rewritten
/// (Apply) and the outcome returned. Every other notification returns nullopt and writes nothing, an unload
/// included: the freed image takes its patch with it, so nothing is held per module and nothing needs
/// undoing. There is no once-only guard: each load is a fresh image (#18).
template <typename Ch, typename Write>
std::optional<Result> OnModuleNotification(unsigned reason, const Ch* name, std::size_t nameChars, std::uint8_t* base,
                                           std::uint16_t port, Write&& write) {
  if (reason != kNotificationLoaded || base == nullptr || !IsMatchmakingModule(name, nameChars)) return std::nullopt;
  return Apply(base, port, std::forward<Write>(write));
}

}  // namespace nevr_matchmaker_host_patch
