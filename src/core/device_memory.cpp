#include "core/device_memory.h"

#ifdef _WIN32
#include <winsock2.h>  // before windows.h (tools/tests/test_header_include_order.py)
#include <windows.h>
#endif

#include <cstdio>
#include <cstring>
#include <limits>

namespace nevr_device {

namespace {

constexpr const char* kMemInfoPath = "/proc/meminfo";
constexpr const char* kMemAvailableKey = "MemAvailable:";
constexpr const char* kMemInfoUnit = " kB";
constexpr std::uint64_t kBytesPerKibibyte = 1024;
constexpr unsigned kDecimalBase = 10;
constexpr std::size_t kMemInfoLineBytes = 256;  // a /proc/meminfo line is far shorter; a longer one is read in parts and never matches the key twice

}  // namespace

bool ParseMemAvailable(const char* meminfo_text, std::uint64_t* bytes) noexcept {
  if (meminfo_text == nullptr) return false;
  const std::size_t keyLength = std::strlen(kMemAvailableKey);
  const char* line = meminfo_text;
  while (*line != '\0') {
    if (std::strncmp(line, kMemAvailableKey, keyLength) == 0) {
      const char* p = line + keyLength;
      while (*p == ' ' || *p == '\t') ++p;
      if (*p < '0' || *p > '9') return false;
      std::uint64_t kibibytes = 0;
      for (; *p >= '0' && *p <= '9'; ++p) {
        const std::uint64_t digit = static_cast<std::uint64_t>(*p - '0');
        if (kibibytes > (std::numeric_limits<std::uint64_t>::max() - digit) / kDecimalBase) return false;
        kibibytes = kibibytes * kDecimalBase + digit;
      }
      if (std::strncmp(p, kMemInfoUnit, std::strlen(kMemInfoUnit)) != 0) return false;
      if (kibibytes > std::numeric_limits<std::uint64_t>::max() / kBytesPerKibibyte) return false;
      *bytes = kibibytes * kBytesPerKibibyte;
      return true;
    }
    const char* next = std::strchr(line, '\n');
    if (next == nullptr) break;
    line = next + 1;
  }
  return false;
}

#ifdef _WIN32
// Windows has no /proc/meminfo: the same figure is GlobalMemoryStatusEx's available physical memory, the memory a process can still
// be given without the system paging out.
bool MemoryAvailable(std::uint64_t* bytes) noexcept {
  MEMORYSTATUSEX status = {};
  status.dwLength = sizeof(status);
  if (!GlobalMemoryStatusEx(&status)) return false;
  *bytes = status.ullAvailPhys;
  return true;
}
#else
bool MemoryAvailable(std::uint64_t* bytes) noexcept {
  std::FILE* f = std::fopen(kMemInfoPath, "r");
  if (f == nullptr) return false;
  char line[kMemInfoLineBytes];
  bool found = false;
  while (std::fgets(line, sizeof(line), f) != nullptr) {
    if (ParseMemAvailable(line, bytes)) {
      found = true;
      break;
    }
  }
  std::fclose(f);
  return found;
}
#endif

}  // namespace nevr_device
