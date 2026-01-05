#include "base64.h"

#include <string>
#include <vector>

static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const uint8_t* data, size_t len) {
  std::string out;
  int val = 0, valb = -6;
  for (size_t i = 0; i < len; ++i) {
    val = (val << 8) + data[i];
    valb += 8;
    while (valb >= 0) {
      out.push_back(b64_table[(val >> valb) & 0x3F]);
      valb -= 6;
    }
  }
  if (valb > -6) out.push_back(b64_table[((val << 8) >> (valb + 8)) & 0x3F]);
  while (out.size() % 4) out.push_back('=');
  return out;
}

std::string base64_encode(const std::vector<uint8_t>& data) { return base64_encode(data.data(), data.size()); }
