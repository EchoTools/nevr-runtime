#pragma once
// The HMD serial field the bridge's LoginRequest carries, chosen the way the stock client chooses it (#83).
//
// Stock path, CR15NetGame::LogIn (echovr.exe 0x14017ef10): when the game's "No VR" flag (bit 20 of the
// flags at game+0x7ae0) is set, it sends "N/A" (0x1416dcce8). Otherwise FUN_14072d2a0 copies 0x18
// bytes into the global at 0x1420c7834 and sends that value; FUN_14072e1d0 returns the same global.
// ReVault's raw decompilation establishes this game-buffer path, but not the value's upstream hardware
// source. The getter runs as part of login before the game connects to the login service, so the bridge
// can read the populated value when it builds its login request.
//
// "N/A" and "unknown" are values the game service's alt detection ignores (nakama IgnoredLoginValues);
// a game-provided serial value is a strong alt signal, so relay it instead of a shared placeholder.

#include <cstddef>
#include <string>

namespace nevr_hmd_serial {

constexpr std::size_t kSerialBytes = 24;  // the game's 0x18-byte serial buffer
constexpr unsigned long long kNoVrFlag = 0x100000ULL;  // game+0x7ae0 bit 20

enum class Source { GameBuffer, NoVr, Unavailable };

struct Choice {
  std::string value;
  Source source;
};

// noVr: the game's flag. serial: the game's 24-byte buffer (nullptr when it cannot be read).
inline Choice Select(bool noVr, const char* serial) {
  if (noVr) return {"N/A", Source::NoVr};
  if (serial == nullptr) return {"unknown", Source::Unavailable};
  std::string value;
  for (std::size_t i = 0; i < kSerialBytes && serial[i] != '\0'; ++i) {
    const unsigned char c = static_cast<unsigned char>(serial[i]);
    if (c < 0x21 || c > 0x7e) return {"unknown", Source::Unavailable};  // not a serial: no spaces, no control bytes
    value.push_back(static_cast<char>(c));
  }
  if (value.empty()) return {"unknown", Source::Unavailable};
  return {value, Source::GameBuffer};
}

inline const char* SourceName(Source s) {
  switch (s) {
    case Source::GameBuffer: return "game-buffer";
    case Source::NoVr: return "no-vr";
    default: return "unavailable";
  }
}

}  // namespace nevr_hmd_serial
