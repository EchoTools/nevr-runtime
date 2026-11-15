#pragma once
// The HMD serial field the bridge's LoginRequest carries, chosen the way the stock client chooses it (#83).
//
// Stock path, CR15NetGame::LogIn (echovr.exe 0x14017ef10): when the game's "No VR" flag (bit 20 of the
// flags at game+0x7ae0) is set, it sends "N/A" (0x1416dcce8). Otherwise FUN_14072d2a0 copies 0x18
// bytes into the global at 0x1420c7834 and sends that value; FUN_14072e1d0 returns the same global.
// The raw decompilation establishes this game-buffer path, but not the value's upstream hardware
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

// The login platform follows the same flag the stock login reads for the serial (#1082): a client the
// game itself marks No VR is a different session identity from the same player's VR client. A dedicated
// server keeps the platform it has always logged in with.
inline bool LoginIsNoVr(bool isServer, unsigned long long gameFlags) {
  return !isServer && (gameFlags & kNoVrFlag) != 0;
}

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

// The headset type the login's system_info carries. The game's HMD descriptor (an ovrHmdDesc copy at 0x1420c77a8, ProductName[64]
// at +8; src/runtime/patch/render_config.h kHmdDescVa) holds the LibOVR product name of the headset in use, such as "Oculus Rift
// CV1"; the stock client sends that string [UNVERIFIED: the stock login's own read of it was not disassembled; the value
// matches what a stock Rift CV1 login carries in the game service's logs]. "No VR" only when the game's No VR flag is set;
// "unknown" when the game is in VR mode but the descriptor is empty or not text, so a real headset is never reported as No VR.
constexpr std::size_t kHmdProductNameRva = 0x20C77A8 + 8;  // image base + 0x1420c77a8 + 8
constexpr std::size_t kProductNameBytes = 64;
constexpr const char* kNoVrHeadset = "No VR";
constexpr const char* kUnknownHeadset = "unknown";

inline std::string HeadsetType(bool noVr, const char* productName) {
  if (noVr) return kNoVrHeadset;
  if (productName == nullptr) return kUnknownHeadset;
  std::string value;
  for (std::size_t i = 0; i < kProductNameBytes && productName[i] != '\0'; ++i) {
    const unsigned char c = static_cast<unsigned char>(productName[i]);
    if (c < 0x20 || c > 0x7e) return kUnknownHeadset;  // not text
    value.push_back(static_cast<char>(c));
  }
  const std::size_t first = value.find_first_not_of(' ');
  if (first == std::string::npos) return kUnknownHeadset;
  return value.substr(first, value.find_last_not_of(' ') - first + 1);
}

inline const char* SourceName(Source s) {
  switch (s) {
    case Source::GameBuffer: return "game-buffer";
    case Source::NoVr: return "no-vr";
    default: return "unavailable";
  }
}

}  // namespace nevr_hmd_serial
