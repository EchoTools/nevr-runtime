// The Android OBB footer (measured on the store OBB, docs/adr/0042): the file ends with
//   [footer version 4][package version 4][flags 4][salt 8][name length 4][name]  then  [footer size 4][tag 4]
// (little-endian). The footer size counts the bytes before the tag; the tag is 0x01059983. Everything before the footer is the
// encrypted image. The salted flag (0x2) says the salt is present; an unsalted OBB has no salt to derive a key from.
#pragma once

#include <array>
#include <cstdint>
#include <string>

#include "quest/obb/byte_source.h"
#include "quest/obb/status.h"

namespace nevr_quest::obb {

constexpr std::uint32_t kObbFooterTag = 0x01059983;
constexpr std::uint32_t kObbFlagSalted = 0x2;
constexpr std::size_t kObbSaltBytes = 8;
constexpr std::size_t kObbSectorBytes = 512;
// The footer sizes Android accepts at most (its read buffer); a larger value is a damaged tail.
constexpr std::uint32_t kObbFooterMaxBytes = 32768;

struct ObbFooter {
  std::uint32_t footer_version = 0;
  std::uint32_t package_version = 0;
  std::uint32_t flags = 0;
  std::array<std::uint8_t, kObbSaltBytes> salt{};
  std::string package_name;
  std::uint32_t footer_bytes = 0;  // the footer size field: bytes between the image and the tag
  std::uint64_t image_bytes = 0;   // file size - footer - tag
};

// Reads and checks the footer. Codes: obb_too_short, obb_bad_tag, obb_bad_footer_size, obb_bad_footer_version, obb_unsalted,
// obb_bad_name_length, obb_image_not_sector_aligned, and the source's own read errors.
Status ParseObbFooter(const ByteSource& obb, ObbFooter* out);

}  // namespace nevr_quest::obb
