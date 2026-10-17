#pragma once
// SHA-1 and SHA-256 as lowercase hex, for tests that must prove a secret (or its digest) never reaches a log line.
// Test support only: nothing in the shipped runtime includes this.

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace nevr_test_digest {

namespace detail {
inline std::uint32_t Rol(std::uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
inline std::uint32_t Ror(std::uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }
inline std::string Hex(const std::uint32_t* words, int count) {
  static const char kDigits[] = "0123456789abcdef";
  std::string out;
  for (int i = 0; i < count; ++i) {
    for (int shift = 28; shift >= 0; shift -= 4) out.push_back(kDigits[(words[i] >> shift) & 0xf]);
  }
  return out;
}
inline std::vector<std::uint8_t> Pad(const std::string& text) {
  std::vector<std::uint8_t> data(text.begin(), text.end());
  const std::uint64_t bits = static_cast<std::uint64_t>(text.size()) * 8;
  data.push_back(0x80);
  while (data.size() % 64 != 56) data.push_back(0);
  for (int i = 7; i >= 0; --i) data.push_back(static_cast<std::uint8_t>(bits >> (8 * i)));
  return data;
}
}  // namespace detail

inline std::string Sha1Hex(const std::string& text) {
  std::uint32_t h[5] = {0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u};
  const std::vector<std::uint8_t> data = detail::Pad(text);
  for (std::size_t chunk = 0; chunk < data.size(); chunk += 64) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
      const std::uint8_t* p = &data[chunk + 4 * i];
      w[i] = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
    }
    for (int i = 16; i < 80; ++i) w[i] = detail::Rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
      std::uint32_t f, k;
      if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999u; }
      else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
      else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
      else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
      const std::uint32_t t = detail::Rol(a, 5) + f + e + k + w[i];
      e = d; d = c; c = detail::Rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
  }
  return detail::Hex(h, 5);
}

inline std::string Sha256Hex(const std::string& text) {
  static const std::uint32_t K[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
      0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
      0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
      0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
      0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
      0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
      0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
  std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  const std::vector<std::uint8_t> data = detail::Pad(text);
  for (std::size_t chunk = 0; chunk < data.size(); chunk += 64) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
      const std::uint8_t* p = &data[chunk + 4 * i];
      w[i] = (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
    }
    for (int i = 16; i < 64; ++i) {
      const std::uint32_t s0 = detail::Ror(w[i - 15], 7) ^ detail::Ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
      const std::uint32_t s1 = detail::Ror(w[i - 2], 17) ^ detail::Ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
      const std::uint32_t S1 = detail::Ror(e, 6) ^ detail::Ror(e, 11) ^ detail::Ror(e, 25);
      const std::uint32_t ch = (e & f) ^ (~e & g);
      const std::uint32_t t1 = hh + S1 + ch + K[i] + w[i];
      const std::uint32_t S0 = detail::Ror(a, 2) ^ detail::Ror(a, 13) ^ detail::Ror(a, 22);
      const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t t2 = S0 + maj;
      hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
  }
  return detail::Hex(h, 8);
}

}  // namespace nevr_test_digest
