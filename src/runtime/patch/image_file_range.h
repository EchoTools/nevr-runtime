#pragma once

// Where a range of a mapped PE image lies in the image's file (#2161). A plugin hands the runtime a pointer to its
// resource set as its image maps it; the game's reader reads the set's chunks from the plugin's file, so the runtime
// needs the file offset, and it needs the file to hold the same bytes there. That holds when the range lies in one
// section, inside the part of the section the file backs (SizeOfRawData; past it the loader fills zeroes), the section
// is not writable (a plugin could change the bytes after the check), and no base relocation writes into it (the loader
// patches those bytes in memory only).
//
// Pure: it reads the PE32+ headers from the mapped image (the loader maps them at the image base), with every read
// bounded by the image's SizeOfImage. Layout: Microsoft PE format, "Optional Header Data Directories", "Section
// Table", "The .reloc Section".

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace nevr_image_range {

enum class Problem {
  kNone = 0,
  kHeaders,          // the image has no readable PE32+ headers
  kNoSection,        // the range starts in no section
  kCrossesSection,   // the range runs past the end of the section it starts in
  kPastRawData,      // the range runs past the bytes the file backs in its section
  kRelocated,        // a base relocation writes into the range
  kWritable,         // the section the range lies in is writable (IMAGE_SCN_MEM_WRITE)
};

inline const char* ProblemName(Problem problem) {
  switch (problem) {
    case Problem::kNone: return "none";
    case Problem::kHeaders: return "headers";
    case Problem::kNoSection: return "no_section";
    case Problem::kCrossesSection: return "crosses_section";
    case Problem::kPastRawData: return "past_raw_data";
    case Problem::kRelocated: return "relocated";
    case Problem::kWritable: return "writable_section";
  }
  return "unknown";
}

struct Result {
  Problem problem = Problem::kNone;
  std::uint64_t file_offset = 0;
  bool ok() const { return problem == Problem::kNone; }
};

namespace detail {

class Reader {
 public:
  Reader(const unsigned char* base, std::uint64_t size) : base_(base), size_(size) {}
  bool Has(std::uint64_t at, std::uint64_t n) const { return at <= size_ && n <= size_ - at; }
  template <typename T>
  bool Get(std::uint64_t at, T* out) const {
    if (!Has(at, sizeof(T))) return false;
    std::memcpy(out, base_ + at, sizeof(T));
    return true;
  }

 private:
  const unsigned char* base_;
  std::uint64_t size_;
};

inline bool Overlap(std::uint64_t a, std::uint64_t aLen, std::uint64_t b, std::uint64_t bLen) {
  return a < b + bLen && b < a + aLen;
}

}  // namespace detail

// The file offset of [rva, rva + size) in the image mapped at `image`. `image` must point at a mapped image: the
// headers are read from it, and SizeOfImage bounds every read.
inline Result FileOffsetOf(const unsigned char* image, std::uint64_t rva, std::uint64_t size) {
  Result result;
  if (image == nullptr) return {Problem::kHeaders, 0};
  // The DOS header and the PE signature come first; SizeOfImage is not known until the optional header is read, so
  // these few reads are bounded by the smallest image that has them.
  detail::Reader head(image, 0x1000);
  std::uint32_t peAt = 0;
  std::uint32_t signature = 0;
  if (image[0] != 'M' || image[1] != 'Z' || !head.Get(0x3c, &peAt) || !head.Get(peAt, &signature) ||
      signature != 0x00004550u) {
    return {Problem::kHeaders, 0};
  }
  std::uint16_t sections = 0;
  std::uint16_t optionalBytes = 0;
  std::uint16_t magic = 0;
  std::uint32_t imageSize = 0;
  if (!head.Get(peAt + 6, &sections) || !head.Get(peAt + 20, &optionalBytes) || !head.Get(peAt + 24, &magic) ||
      magic != 0x20b || !head.Get(peAt + 24 + 56, &imageSize)) {
    return {Problem::kHeaders, 0};
  }
  const detail::Reader img(image, imageSize);
  const std::uint64_t optionalAt = std::uint64_t{peAt} + 24;
  const std::uint64_t sectionTable = optionalAt + optionalBytes;
  if (!img.Has(sectionTable, std::uint64_t{sections} * 40)) return {Problem::kHeaders, 0};

  bool found = false;
  for (std::uint16_t i = 0; i < sections; ++i) {
    const std::uint64_t at = sectionTable + std::uint64_t{i} * 40;
    std::uint32_t virtualSize = 0, virtualAddress = 0, rawSize = 0, rawPointer = 0, characteristics = 0;
    img.Get(at + 8, &virtualSize);
    img.Get(at + 12, &virtualAddress);
    img.Get(at + 16, &rawSize);
    img.Get(at + 20, &rawPointer);
    img.Get(at + 36, &characteristics);
    const std::uint64_t mapped = virtualSize != 0 ? virtualSize : rawSize;
    if (rva < virtualAddress || rva >= std::uint64_t{virtualAddress} + mapped) continue;
    found = true;
    if (size > std::uint64_t{virtualAddress} + mapped - rva) return {Problem::kCrossesSection, 0};
    if (rva + size > std::uint64_t{virtualAddress} + rawSize) return {Problem::kPastRawData, 0};
    if ((characteristics & 0x80000000u) != 0) return {Problem::kWritable, 0};  // IMAGE_SCN_MEM_WRITE
    result.file_offset = std::uint64_t{rawPointer} + (rva - virtualAddress);
    break;
  }
  if (!found) return {Problem::kNoSection, 0};

  std::uint32_t directories = 0;
  if (!img.Get(optionalAt + 108, &directories)) return {Problem::kHeaders, 0};
  if (directories <= 5) return result;  // no base relocation directory: nothing is patched
  std::uint32_t relocAt = 0, relocSize = 0;
  img.Get(optionalAt + 112 + 5 * 8, &relocAt);
  img.Get(optionalAt + 112 + 5 * 8 + 4, &relocSize);
  if (relocAt == 0 || relocSize == 0) return result;
  if (!img.Has(relocAt, relocSize)) return {Problem::kHeaders, 0};
  std::uint64_t block = relocAt;
  const std::uint64_t end = std::uint64_t{relocAt} + relocSize;
  while (block + 8 <= end) {
    std::uint32_t page = 0, blockSize = 0;
    img.Get(block, &page);
    img.Get(block + 4, &blockSize);
    if (blockSize < 8 || block + blockSize > end) return {Problem::kHeaders, 0};
    for (std::uint64_t e = block + 8; e + 2 <= block + blockSize; e += 2) {
      std::uint16_t entry = 0;
      img.Get(e, &entry);
      const unsigned type = entry >> 12;
      if (type == 0) continue;  // IMAGE_REL_BASED_ABSOLUTE: padding
      const std::uint64_t target = std::uint64_t{page} + (entry & 0x0fffu);
      // DIR64 writes 8 bytes, HIGHLOW 4, HIGH and LOW 2; 8 covers them all.
      if (detail::Overlap(target, 8, rva, size)) return {Problem::kRelocated, 0};
    }
    block += blockSize;
  }
  return result;
}

}  // namespace nevr_image_range
