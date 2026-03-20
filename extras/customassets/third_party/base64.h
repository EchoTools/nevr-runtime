#pragma once
// Simple base64 encoder/decoder
// Based on public domain implementation

#include <string>
#include <vector>

namespace base64 {

static const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

inline std::string Encode(const unsigned char* data, size_t len) {
  std::string result;
  result.reserve(((len + 2) / 3) * 4);

  for (size_t i = 0; i < len; i += 3) {
    unsigned char b1 = data[i];
    unsigned char b2 = (i + 1 < len) ? data[i + 1] : 0;
    unsigned char b3 = (i + 2 < len) ? data[i + 2] : 0;

    result.push_back(kBase64Chars[b1 >> 2]);
    result.push_back(kBase64Chars[((b1 & 0x03) << 4) | (b2 >> 4)]);
    result.push_back((i + 1 < len) ? kBase64Chars[((b2 & 0x0F) << 2) | (b3 >> 6)] : '=');
    result.push_back((i + 2 < len) ? kBase64Chars[b3 & 0x3F] : '=');
  }

  return result;
}

inline std::vector<unsigned char> Decode(const std::string& encoded) {
  std::vector<unsigned char> result;
  result.reserve((encoded.size() / 4) * 3);

  auto isBase64 = [](unsigned char c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/';
  };

  auto decode_char = [](unsigned char c) -> unsigned char {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return 0;
  };

  for (size_t i = 0; i < encoded.size(); i += 4) {
    unsigned char c1 = (i < encoded.size() && isBase64(encoded[i])) ? decode_char(encoded[i]) : 0;
    unsigned char c2 = (i + 1 < encoded.size() && isBase64(encoded[i + 1])) ? decode_char(encoded[i + 1]) : 0;
    unsigned char c3 = (i + 2 < encoded.size() && isBase64(encoded[i + 2])) ? decode_char(encoded[i + 2]) : 0;
    unsigned char c4 = (i + 3 < encoded.size() && isBase64(encoded[i + 3])) ? decode_char(encoded[i + 3]) : 0;

    result.push_back((c1 << 2) | (c2 >> 4));
    if (i + 2 < encoded.size() && encoded[i + 2] != '=') result.push_back((c2 << 4) | (c3 >> 2));
    if (i + 3 < encoded.size() && encoded[i + 3] != '=') result.push_back((c3 << 6) | c4);
  }

  return result;
}

}  // namespace base64
