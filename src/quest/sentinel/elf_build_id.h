#pragma once

// The GNU build id of an ELF image that is mapped in memory, read from its PT_NOTE segments. The crash report
// (core/crash_report.h) prints it next to a module+offset so a report names the exact libr15.so build it came from.
// Reads memory only: no allocation, no lock, no file, so it is safe in a signal handler.

#include <cstddef>
#include <cstdint>

namespace sentinel {

// True when [address, address + length) can be read. The signal-handler seam: the device build passes a mincore
// probe, the host test passes nullptr (every read allowed).
using ReadableFn = bool (*)(const void* address, std::size_t length);

// Copies the build id of the image whose first mapping is at `base` into `out` (at most `capacity` bytes) and
// stores its length in `*length`. Returns false for an image with no GNU build id note, a header that does not
// parse as 64-bit ELF, or a read `readable` refuses.
bool ReadElfBuildId(const void* base, ReadableFn readable, std::uint8_t* out, std::size_t capacity,
                    std::size_t* length);

}  // namespace sentinel
