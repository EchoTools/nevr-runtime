#pragma once

// What the crash report reads out of a PE image that is mapped in memory (nevr_crash::Location, #1605): the identity
// a symbol server keys on (TimeDateStamp and SizeOfImage), and the nearest named export at or below an address.
// Bytes only: no windows.h, no allocation, no lock, so a vectored handler may call it (N70). A PE has no GNU build id;
// the identity is the pair a debugger matches a binary to its symbols with.

#include <cstddef>
#include <cstdint>

namespace nevr_crash_pe {

// True when [address, address + length) can be read. The device passes a VirtualQuery probe, a test passes nullptr.
using ReadableFn = bool (*)(const void* address, std::size_t length);

struct ImageIdentity {
  std::uint32_t time_date_stamp;
  std::uint32_t size_of_image;
};

// Reads the identity of the image mapped at `image`. False when the headers do not parse as a PE image.
bool ReadIdentity(const std::uint8_t* image, ReadableFn readable, ImageIdentity* out);

// The named export of the image with the highest address that is not above `rva` (forwarded exports are skipped).
// Copies its name into `name` (`capacity` bytes, NUL-terminated) and stores `rva` less the export's address in
// `*offset`. False when the image exports no name at or below `rva`.
bool NearestExport(const std::uint8_t* image, ReadableFn readable, std::uint32_t rva, char* name,
                   std::size_t capacity, std::uint64_t* offset);

// Writes `pe-<time_date_stamp hex>-<size_of_image hex>`, the build identity printed for a PE module.
void FormatIdentity(const ImageIdentity& identity, char* out, std::size_t capacity);

}  // namespace nevr_crash_pe
