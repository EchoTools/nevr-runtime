#pragma once

// Whether the built-in filter may hand a game line to the engine's own PrintfImpl (passthrough_to_engine, #548).
// The engine reads varargs only when the format holds a conversion, so a format with no conversion is safe to
// forward with null varargs; a format with one and null varargs is not (nevr_game_line_format::Capture copies
// such a format verbatim, so the engine would read an argument that is not there).

#include <cstdint>
#include <cstring>

namespace nevr_passthrough_forward {

// The character that starts a printf conversion.
constexpr char kConversionLead = '%';

inline bool CanForward(const char* fmt, const std::int64_t* varargs) {
  if (fmt == nullptr) return false;
  if (varargs != nullptr) return true;
  return std::strchr(fmt, kConversionLead) == nullptr;
}

}  // namespace nevr_passthrough_forward
