#include "quest/data/data_host_roots.h"

#include <cstdint>
#include <cstring>
#include <vector>

#include "quest/data/sha256.h"

namespace nevr_quest::data {

namespace {

constexpr const char kBegin[] = "-----BEGIN CERTIFICATE-----";
constexpr const char kEnd[] = "-----END CERTIFICATE-----";

int Sextet(char c) noexcept {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// The DER inside one PEM block; false when the text is not exactly one certificate block.
bool DecodePem(const char* pem, std::vector<unsigned char>* der) {
  const char* begin = std::strstr(pem, kBegin);
  if (begin == nullptr || begin != pem) return false;
  const char* body = begin + sizeof(kBegin) - 1;
  const char* end = std::strstr(body, kEnd);
  if (end == nullptr) return false;
  if (std::strstr(end + sizeof(kEnd) - 1, kBegin) != nullptr) return false;  // a second certificate
  std::uint32_t accumulator = 0;
  int bits = 0;
  int padding = 0;
  for (const char* at = body; at < end; ++at) {
    if (*at == '\n' || *at == '\r') continue;
    if (*at == '=') {
      ++padding;
      continue;
    }
    const int value = Sextet(*at);
    if (value < 0 || padding != 0) return false;
    accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      der->push_back(static_cast<unsigned char>((accumulator >> bits) & 0xff));
    }
  }
  return padding <= 2 && !der->empty();
}

// A DER SEQUENCE whose header's length is exactly the rest of the buffer.
bool IsOneSequence(const std::vector<unsigned char>& der) {
  if (der.size() < 4 || der[0] != 0x30) return false;
  std::size_t length = 0;
  std::size_t header = 2;
  if ((der[1] & 0x80) == 0) {
    length = der[1];
  } else {
    const std::size_t octets = der[1] & 0x7f;
    if (octets == 0 || octets > 4 || der.size() < 2 + octets) return false;
    for (std::size_t i = 0; i < octets; ++i) length = (length << 8) | der[2 + i];
    header = 2 + octets;
  }
  return header + length == der.size();
}

}  // namespace

std::string DataHostRootsPem() {
  std::string pem;
  for (const DataHostRoot& root : kDataHostRoots) pem += root.pem;
  return pem;
}

RootsCheck CheckRoots(const DataHostRoot* roots, std::size_t count) {
  RootsCheck result;
  if (count == 0) {
    result.detail = "no_roots";
    return result;
  }
  for (std::size_t at = 0; at < count; ++at) {
    const DataHostRoot& root = roots[at];
    std::vector<unsigned char> der;
    if (!DecodePem(root.pem, &der)) {
      result.detail = std::string("not_one_pem_certificate:") + root.name;
      return result;
    }
    if (!IsOneSequence(der)) {
      result.detail = std::string("not_a_der_certificate:") + root.name;
      return result;
    }
    if (Sha256Hex(der.data(), der.size()) != root.sha256) {
      result.detail = std::string("fingerprint_mismatch:") + root.name;
      return result;
    }
    ++result.count;
  }
  result.ok = true;
  return result;
}

RootsCheck CheckDataHostRoots() { return CheckRoots(kDataHostRoots, kDataHostRootCount); }

}  // namespace nevr_quest::data
