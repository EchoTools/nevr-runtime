#include "runtime/host/file_digest.h"

#include <wincrypt.h>

#include <algorithm>
#include <cstddef>
#include <iterator>

namespace nevr_host {

bool ReadWholeFile(HANDLE file, std::vector<unsigned char>* out) {
  LARGE_INTEGER size{};
  LARGE_INTEGER start{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0) return false;
  if (!SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) return false;
  out->resize(static_cast<std::size_t>(size.QuadPart));
  std::size_t done = 0;
  while (done < out->size()) {
    const DWORD want = static_cast<DWORD>(std::min<std::size_t>(out->size() - done, std::size_t{1} << 20));
    DWORD got = 0;
    if (!ReadFile(file, out->data() + done, want, &got, nullptr) || got == 0) return false;
    done += got;
  }
  return true;
}

std::string Sha256Hex(const std::vector<unsigned char>& bytes) {
  HCRYPTPROV provider = 0;
  if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) return {};
  std::string hex;
  HCRYPTHASH hash = 0;
  if (CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash)) {
    bool ok = true;
    std::size_t done = 0;
    while (ok && done < bytes.size()) {
      const std::size_t chunk = std::min<std::size_t>(bytes.size() - done, std::size_t{1} << 20);
      ok = CryptHashData(hash, bytes.data() + done, static_cast<DWORD>(chunk), 0) != FALSE;
      done += chunk;
    }
    BYTE digest[32] = {};
    DWORD length = sizeof(digest);
    if (ok && CryptGetHashParam(hash, HP_HASHVAL, digest, &length, 0) && length == sizeof(digest)) {
      constexpr char kDigits[] = "0123456789abcdef";
      for (const BYTE b : digest) {
        hex.push_back(kDigits[b >> 4]);
        hex.push_back(kDigits[b & 0x0f]);
      }
    }
    CryptDestroyHash(hash);
  }
  CryptReleaseContext(provider, 0);
  return hex;
}

std::string BuildVersion(const std::vector<unsigned char>& bytes) {
  static constexpr unsigned char kPrefix[] = {0, 'N', 'E', 'V', 'R', '-', 'B', 'U', 'I', 'L', 'D', ' '};
  const auto found = std::search(bytes.begin(), bytes.end(), std::begin(kPrefix), std::end(kPrefix));
  if (found == bytes.end()) return {};
  std::string version;
  for (auto at = found + static_cast<std::ptrdiff_t>(sizeof(kPrefix));
       at != bytes.end() && *at != 0 && *at != ' ' && version.size() < 64; ++at) {
    version.push_back(static_cast<char>(*at));
  }
  return version;
}

}  // namespace nevr_host
