#pragma once

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace nevr_game_line_format {

inline int Capture(char* out, std::size_t capacity, const char* fmt, std::int64_t* varargs) {
  if (out == nullptr || capacity == 0 || fmt == nullptr) return 0;
  int len = 0;
  if (varargs == nullptr || std::strchr(fmt, '%') == nullptr) {
    const std::size_t sourceLen = std::strlen(fmt);
    len = static_cast<int>(sourceLen < capacity ? sourceLen : capacity - 1);
    std::memcpy(out, fmt, static_cast<std::size_t>(len));
  } else {
    len = std::vsnprintf(out, capacity, fmt, reinterpret_cast<va_list>(varargs));
    if (len < 0) len = 0;
    if (len >= static_cast<int>(capacity)) len = static_cast<int>(capacity) - 1;
  }
  out[len] = '\0';
  return len;
}

}  // namespace nevr_game_line_format
