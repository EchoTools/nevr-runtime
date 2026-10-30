// A structured log record with up to 8 fields, numbers formatted into the record's own buffers (no heap, no constructor
// at namespace scope). Used by the powerboost, hudmeter and shieldgamma modules.
#pragma once

#include <cstdint>
#include <cstdio>

#include "plugin_log.h"

namespace q15t {

class Rec {
 public:
  Rec& S(const char* key, const char* value) { return Put(key, value); }
  Rec& U(const char* key, unsigned long long v) {
    char* b = Buf();
    if (b != nullptr) std::snprintf(b, kLen, "%llu", v);
    return Done(key, b);
  }
  Rec& I(const char* key, long long v) {
    char* b = Buf();
    if (b != nullptr) std::snprintf(b, kLen, "%lld", v);
    return Done(key, b);
  }
  Rec& X(const char* key, unsigned long long v) {
    char* b = Buf();
    if (b != nullptr) std::snprintf(b, kLen, "0x%llx", v);
    return Done(key, b);
  }
  Rec& F(const char* key, double v) {
    char* b = Buf();
    if (b != nullptr) std::snprintf(b, kLen, "%g", v);
    return Done(key, b);
  }
  void Emit(std::uint32_t level, const char* message) const { Log(level, message, f_, static_cast<std::uint32_t>(n_)); }

 private:
  static constexpr int kMax = 8, kLen = 32;
  char* Buf() { return n_ < kMax ? buf_[n_] : nullptr; }
  Rec& Done(const char* key, const char* value) { return Put(key, value); }
  Rec& Put(const char* key, const char* value) {
    if (n_ < kMax) {
      f_[n_].key = key;
      f_[n_].value = value;
      ++n_;
    }
    return *this;
  }
  NevrLogField f_[kMax] = {};
  char buf_[kMax][kLen] = {};
  int n_ = 0;
};

}  // namespace q15t
