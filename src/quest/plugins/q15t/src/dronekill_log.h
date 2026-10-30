// A small builder for one structured log line with up to 8 fields (drone modules). Values are copied into the builder, so a
// field may be a temporary. Not for per-call paths.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "plugin_log.h"

namespace q15t {

class LogKv {
 public:
  LogKv& S(const char* key, const char* value) {
    Put(key, value != nullptr ? value : "");
    return *this;
  }
  LogKv& U(const char* key, unsigned long long value) {
    char b[24];
    std::snprintf(b, sizeof b, "%llu", value);
    Put(key, b);
    return *this;
  }
  LogKv& X(const char* key, unsigned long long value) {
    char b[24];
    std::snprintf(b, sizeof b, "0x%llx", value);
    Put(key, b);
    return *this;
  }
  LogKv& D(const char* key, double value) {
    char b[32];
    std::snprintf(b, sizeof b, "%.6f", value);
    Put(key, b);
    return *this;
  }
  LogKv& V3(const char* key, const float* v) {
    char b[64];
    std::snprintf(b, sizeof b, "%.6f %.6f %.6f", static_cast<double>(v[0]), static_cast<double>(v[1]), static_cast<double>(v[2]));
    Put(key, b);
    return *this;
  }
  void Emit(std::uint32_t level, const char* message) const { Log(level, message, f_, static_cast<std::uint32_t>(n_)); }

 private:
  void Put(const char* key, const char* value) {
    if (n_ >= 8) return;
    std::snprintf(buf_[n_], sizeof buf_[n_], "%s", value);
    f_[n_] = {key, buf_[n_]};
    ++n_;
  }
  NevrLogField f_[8];
  char buf_[8][120];
  int n_ = 0;
};

}  // namespace q15t
