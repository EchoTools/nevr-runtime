#pragma once

#include <cstddef>
#include <cstdint>

namespace CrashRecovery {

// Formats the first structured crash-dump line using caller-owned storage.
// It is deliberately allocation-free because WriteCrashDump can run in an
// exception context. Returns the number of bytes written, excluding NUL.
int FormatCrashExceptionSummary(char* buffer, size_t buffer_size,
                                uint32_t exception_code, uint64_t rip,
                                uint64_t game_base, uint32_t thread_id);

}  // namespace CrashRecovery
