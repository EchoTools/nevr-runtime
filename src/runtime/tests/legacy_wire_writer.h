#pragma once
// Test-only helpers for the legacy translator tests: a byte writer and zlib/zstd helpers that share no code with
// compat/legacy_codec, so a layout mistake in the codec cannot cancel out. Used by test_legacy_codec.cpp and
// test_legacy_session.cpp.

#include <gtest/gtest.h>
#include <zlib.h>
#include <zstd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace nevr_legacy_test {

// ---- independent byte writer and readers -----------------------------------------------------

class Bytes {
 public:
  Bytes& U8(uint8_t v) {
    s_.push_back(static_cast<char>(v));
    return *this;
  }
  Bytes& U16(uint16_t v) { return Le(v, 2); }
  Bytes& U32(uint32_t v) { return Le(v, 4); }
  Bytes& U64(uint64_t v) { return Le(v, 8); }
  Bytes& I16(int16_t v) { return Le(static_cast<uint16_t>(v), 2); }
  Bytes& Be16(uint16_t v) {
    U8(static_cast<uint8_t>(v >> 8));
    return U8(static_cast<uint8_t>(v & 0xff));
  }
  Bytes& Pad(std::size_t n) {
    s_.append(n, '\0');
    return *this;
  }
  Bytes& Raw(std::string_view v) {
    s_.append(v);
    return *this;
  }
  Bytes& CStr(std::string_view v) {
    s_.append(v);
    s_.push_back('\0');
    return *this;
  }
  Bytes& Guid(uint8_t seed) {
    for (int i = 0; i < 16; ++i) s_.push_back(static_cast<char>(seed + i));
    return *this;
  }
  Bytes& User(uint64_t platform, uint64_t account) { return U64(platform).U64(account); }
  const std::string& Str() const { return s_; }

 private:
  Bytes& Le(uint64_t v, int n) {
    for (int i = 0; i < n; ++i) s_.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
    return *this;
  }
  std::string s_;
};

inline uint64_t U64At(const std::string& b, std::size_t off) {
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | static_cast<unsigned char>(b.at(off + i));
  return v;
}

inline uint32_t U32At(const std::string& b, std::size_t off) {
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | static_cast<unsigned char>(b.at(off + i));
  return v;
}

inline std::string ZlibCompress(const std::string& raw) {
  uLongf n = compressBound(static_cast<uLong>(raw.size()));
  std::string out(n, '\0');
  EXPECT_EQ(compress(reinterpret_cast<Bytef*>(out.data()), &n, reinterpret_cast<const Bytef*>(raw.data()),
                     static_cast<uLong>(raw.size())),
            Z_OK);
  out.resize(n);
  return out;
}

inline std::string ZlibUncompress(const std::string& z, std::size_t rawSize) {
  std::string out(rawSize, '\0');
  uLongf n = static_cast<uLongf>(rawSize);
  EXPECT_EQ(uncompress(reinterpret_cast<Bytef*>(out.data()), &n, reinterpret_cast<const Bytef*>(z.data()),
                       static_cast<uLong>(z.size())),
            Z_OK);
  out.resize(n);
  return out;
}

inline std::string ZstdCompress(const std::string& raw) {
  std::string out(ZSTD_compressBound(raw.size()), '\0');
  const std::size_t n = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), 3);
  EXPECT_FALSE(ZSTD_isError(n));
  out.resize(n);
  return out;
}

inline std::string ZstdUncompress(const std::string& z, std::size_t rawSize) {
  std::string out(rawSize, '\0');
  const std::size_t n = ZSTD_decompress(out.data(), out.size(), z.data(), z.size());
  EXPECT_FALSE(ZSTD_isError(n));
  out.resize(n);
  return out;
}

inline std::array<uint8_t, 16> GuidOf(uint8_t seed) {
  std::array<uint8_t, 16> g{};
  for (std::size_t i = 0; i < g.size(); ++i) g[i] = static_cast<uint8_t>(seed + i);
  return g;
}

}  // namespace nevr_legacy_test
