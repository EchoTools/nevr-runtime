#pragma once

// The one mapping from a log level to the name a record carries (#2245).
//
// The game's CLog levels are a bit each (echovr.exe CLog::DefaultHandler 0x1400eba20 prints "[DEBUG] " for 1,
// "[WARNING] " for 4 and "[ERROR] " for 8; info is 2), and EchoVR::LogLevel (abi/echovr.h) is the same numbering, so
// the runtime's own Log() levels and the game's arrive at CLog::PrintfImpl as one number. A level that is none of the
// four has no name: it is written "unknown" with its raw value beside it, never as "info".

#include <cstdint>

namespace nevr_log_level {

inline constexpr std::uint32_t kDebug = 1;
inline constexpr std::uint32_t kInfo = 2;
inline constexpr std::uint32_t kWarning = 4;
inline constexpr std::uint32_t kError = 8;

/// The record's name for a level, or nullptr when the level is not one of the four.
inline const char* Name(std::uint32_t level) {
  switch (level) {
    case kDebug:   return "debug";
    case kInfo:    return "info";
    case kWarning: return "warn";
    case kError:   return "error";
    default:       return nullptr;
  }
}

/// Name(), with "unknown" for a level that has none. For a writer that cannot carry the raw value beside it, such as
/// the console line; a record carries it (level_raw).
inline const char* NameOrUnknown(std::uint32_t level) {
  const char* name = Name(level);
  return name != nullptr ? name : "unknown";
}

/// The level a line is recorded at. The game's error dialog (echovr.exe 0x1400ea730, the box that tells the player the
/// game cannot start) logs its title and message at info (LEA ECX,[RDX+2] at 0x1400ea778 and 0x1400ea791) before it
/// shows the box: a line logged from inside it is an error whatever level the game gave it, so it is recorded as one.
inline std::uint32_t Effective(std::uint32_t gameLevel, bool insideErrorDialog) {
  return insideErrorDialog ? kError : gameLevel;
}

}  // namespace nevr_log_level
