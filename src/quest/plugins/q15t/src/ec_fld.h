// A structured log line builder for the ec / ecfx modules: up to 8 fields; numbers are formatted into the builder's own buffers.
// Strings are NOT copied: they must outlive the Emit call (the builder is used as one expression).
#pragma once

#include <cstdint>
#include <cstdio>

#include "plugin_log.h"

namespace q15t {

// A structured line: up to 8 fields; numbers are formatted into the builder's own buffers.
struct EcFld {
  NevrLogField f[8];
  char buf[8][40];
  std::uint32_t n = 0;
  EcFld& S(const char* k, const char* v) {
    if (n < 8) f[n++] = {k, v != nullptr ? v : ""};
    return *this;
  }
  EcFld& N(const char* k, long long v) {
    if (n < 8) {
      std::snprintf(buf[n], sizeof buf[n], "%lld", v);
      f[n] = {k, buf[n]};
      ++n;
    }
    return *this;
  }
  EcFld& X(const char* k, unsigned long long v) {
    if (n < 8) {
      std::snprintf(buf[n], sizeof buf[n], "0x%llx", v);
      f[n] = {k, buf[n]};
      ++n;
    }
    return *this;
  }
  void Emit(std::uint32_t level, const char* msg) const { Log(level, msg, f, n); }
};

}  // namespace q15t
