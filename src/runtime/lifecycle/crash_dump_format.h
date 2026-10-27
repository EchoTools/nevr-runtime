#pragma once

#include <cstddef>
#include <cstdint>

namespace nevr_crash_recovery {

// Formats the first structured crash-dump line using caller-owned storage.
// It is deliberately allocation-free because WriteCrashDump can run in an
// exception context. Returns the number of bytes written, excluding NUL.
int FormatCrashExceptionSummary(char* buffer, size_t buffer_size,
                                uint32_t exception_code, uint64_t rip,
                                uint64_t game_base, uint32_t thread_id);

// Names the module whose image holds `address`, from a caller-owned table of
// {base, end, name} entries (the init-time snapshot in crash_recovery.cpp).
// Returns nullptr when no entry contains it. Reads only the table: no
// enumeration, no allocation, so it is safe in the crash path (N70). A module
// loaded after the snapshot is not in the table and comes back as nullptr.
template <typename Module>
const char* ModuleNameForAddress(const Module* modules, int count, uint64_t address) {
  for (int i = 0; i < count; i++) {
    if (address >= modules[i].base && address < modules[i].end) return modules[i].name;
  }
  return nullptr;
}

}  // namespace nevr_crash_recovery
