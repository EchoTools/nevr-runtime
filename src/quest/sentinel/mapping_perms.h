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

// True when every byte of [addr, addr + length) lies in a mapping whose permission at `write` is set: "r" for a read, "w" for
// a write. The per-access check of a game-object read or write (the Quest's counterpart of the PC's IsReadableMemory and
// IsWritableMemory). False for a zero length, for a byte no mapping holds, and when /proc/self/maps cannot be read. It never
// dereferences the range.
inline bool MappingsAllow(std::uintptr_t addr, std::size_t length, bool write) {
  const std::uintptr_t end = addr + length;
  if (length == 0 || end < addr) return false;
  std::FILE* f = std::fopen("/proc/self/maps", "re");
  if (f == nullptr) return false;
  char line[512];
  std::uintptr_t cursor = addr;
  bool allowed = true;
  while (allowed && cursor < end && std::fgets(line, sizeof line, f) != nullptr) {
    unsigned long lo = 0;
    unsigned long hi = 0;
    char p[5] = {};
    if (std::sscanf(line, "%lx-%lx %4s", &lo, &hi, p) != 3) continue;
    if (hi <= cursor) continue;
    if (lo > cursor) break;  // no mapping holds this byte
    allowed = p[write ? 1 : 0] == (write ? 'w' : 'r');
    cursor = hi;
  }
  std::fclose(f);
  return allowed && cursor >= end;
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
