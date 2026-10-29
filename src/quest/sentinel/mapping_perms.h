// The permissions /proc/self/maps shows for the mapping that holds an address ("r-xp"). Evidence in a log line that a
// protection did or did not change (libr15_attach.cpp reads it after the activation store); header-only so a second
// reader does not copy the parser. An unreadable map never reads as writable or executable.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace sentinel {

// Writes the four permission characters and a NUL into `perms`. False when /proc/self/maps cannot be read or no
// mapping holds the address.
inline bool MappingPermsOf(std::uintptr_t addr, char (&perms)[5]) {
  std::FILE* f = std::fopen("/proc/self/maps", "re");
  if (f == nullptr) return false;
  char line[512];
  bool found = false;
  while (!found && std::fgets(line, sizeof line, f) != nullptr) {
    unsigned long lo = 0;
    unsigned long hi = 0;
    char p[5] = {};
    if (std::sscanf(line, "%lx-%lx %4s", &lo, &hi, p) != 3) continue;
    if (addr >= lo && addr < hi) {
      std::memcpy(perms, p, sizeof perms);
      found = true;
    }
  }
  std::fclose(f);
  return found;
}

// The text of those permissions, or "unknown".
struct PermsText {
  char text[8];
};
inline PermsText PermsTextOf(std::uintptr_t addr) {
  PermsText out = {};
  char perms[5] = {};
  if (MappingPermsOf(addr, perms)) {
    std::memcpy(out.text, perms, sizeof perms);
  } else {
    std::memcpy(out.text, "unknown", sizeof "unknown");
  }
  return out;
}

}  // namespace sentinel
