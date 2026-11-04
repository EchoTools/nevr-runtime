#include "quest/obb/obb_footer.h"

#include <cstdio>

namespace nevr_quest::obb {

namespace {
// The footer before the name: version, package version, flags, and (salted) the salt, then the name length.
constexpr std::uint32_t kUnsaltedFixedBytes = 16;  // version 4, package version 4, flags 4, name length 4
constexpr std::uint32_t kSaltedFixedBytes = 24;    // ... plus the 8-byte salt
constexpr std::uint32_t kTrailerBytes = 8;         // footer size 4, tag 4
constexpr std::uint32_t kSupportedFooterVersion = 1;

std::uint32_t Le32(const std::uint8_t* p) {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) | (static_cast<std::uint32_t>(p[2]) << 16) |
         (static_cast<std::uint32_t>(p[3]) << 24);
}
std::string Hex32(std::uint32_t v) {
  char buf[16];
  std::snprintf(buf, sizeof buf, "0x%08x", v);
  return buf;
}
}  // namespace

Status ParseObbFooter(const ByteSource& obb, ObbFooter* out) {
  const std::uint64_t total = obb.Size();
  if (total < kTrailerBytes) {
    return Status::Fail("obb_too_short", "the file is " + std::to_string(total) + " bytes; the footer's trailer alone is 8");
  }
  std::uint8_t trailer[kTrailerBytes];
  if (Status s = obb.ReadAt(total - kTrailerBytes, trailer, sizeof trailer); !s.ok) return s;
  const std::uint32_t footer_bytes = Le32(trailer);
  const std::uint32_t tag = Le32(trailer + 4);
  if (tag != kObbFooterTag) {
    return Status::Fail("obb_bad_tag", "the file ends with " + Hex32(tag) + ", not the OBB footer tag " + Hex32(kObbFooterTag));
  }
  if (footer_bytes < kUnsaltedFixedBytes || footer_bytes > kObbFooterMaxBytes || footer_bytes > total - kTrailerBytes) {
    return Status::Fail("obb_bad_footer_size", "the footer size is " + std::to_string(footer_bytes) + " in a file of " + std::to_string(total) +
                                                   " bytes (at least " + std::to_string(kUnsaltedFixedBytes) + ", at most " +
                                                   std::to_string(kObbFooterMaxBytes) + ", and inside the file)");
  }
  std::vector<std::uint8_t> footer(footer_bytes);
  if (Status s = obb.ReadAt(total - kTrailerBytes - footer_bytes, footer.data(), footer.size()); !s.ok) return s;

  ObbFooter f;
  f.footer_version = Le32(&footer[0]);
  f.package_version = Le32(&footer[4]);
  f.flags = Le32(&footer[8]);
  f.footer_bytes = footer_bytes;
  if (f.footer_version != kSupportedFooterVersion) {
    return Status::Fail("obb_bad_footer_version", "the footer version is " + std::to_string(f.footer_version) + "; only " +
                                                      std::to_string(kSupportedFooterVersion) + " is read");
  }
  if ((f.flags & kObbFlagSalted) == 0) {
    return Status::Fail("obb_unsalted", "the OBB's flags are " + Hex32(f.flags) + ": not salted, and there is no salt to derive the volume key from");
  }
  if (footer_bytes < kSaltedFixedBytes) {
    return Status::Fail("obb_bad_footer_size", "a salted footer is at least " + std::to_string(kSaltedFixedBytes) + " bytes; this one is " + std::to_string(footer_bytes));
  }
  for (std::size_t i = 0; i < kObbSaltBytes; ++i) f.salt[i] = footer[12 + i];
  const std::uint32_t name_length = Le32(&footer[20]);
  if (static_cast<std::uint64_t>(kSaltedFixedBytes) + name_length != footer_bytes) {
    return Status::Fail("obb_bad_name_length", "the package name is declared " + std::to_string(name_length) + " bytes in a footer of " +
                                                   std::to_string(footer_bytes) + " (" + std::to_string(kSaltedFixedBytes) + " before the name)");
  }
  f.package_name.assign(reinterpret_cast<const char*>(&footer[kSaltedFixedBytes]), name_length);
  f.image_bytes = total - kTrailerBytes - footer_bytes;
  if (f.image_bytes == 0 || f.image_bytes % kObbSectorBytes != 0) {
    return Status::Fail("obb_image_not_sector_aligned", "the image before the footer is " + std::to_string(f.image_bytes) +
                                                            " bytes, not a positive whole number of " + std::to_string(kObbSectorBytes) + "-byte sectors");
  }
  *out = f;
  return Status::Ok();
}

}  // namespace nevr_quest::obb
